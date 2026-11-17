// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/call.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace shannond::ims {

namespace {

constexpr const char *kMmtelService = "urn:urn-7:3gpp-service.ims.icsi.mmtel";
constexpr const char *kMmtelTag = "+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel\"";
constexpr const char *kAllow = "INVITE, ACK, CANCEL, BYE, UPDATE, PRACK, OPTIONS, INFO";

bool isSdp(const SipMessage &m)
{
    auto type = m.header("Content-Type");
    return type && !m.body.empty() && iequals(trim(type->substr(0, type->find(';'))), "application/sdp");
}

std::optional<Sdp> sdpOf(const SipMessage &m)
{
    if (!isSdp(m))
        return std::nullopt;
    return Sdp::parse(m.body);
}

std::string tagOf(const std::optional<std::string> &header)
{
    if (!header)
        return {};
    auto n = NameAddr::parse(*header);
    return n ? n->params.get("tag").value_or("") : std::string();
}

// The name-addr without its parameters (the tag goes on separately)
std::string bareNameAddr(const std::optional<std::string> &header)
{
    if (!header)
        return {};
    auto n = NameAddr::parse(*header);
    return n ? "<" + n->uri + ">" : std::string();
}

std::string uriOf(const std::optional<std::string> &header)
{
    if (!header)
        return {};
    auto n = NameAddr::parse(*header);
    return n ? n->uri : std::string();
}

// A number from a tel: or sip: URI, else the URI
std::string numberOf(const std::string &uri)
{
    auto u = Uri::parse(uri);
    if (!u)
        return uri;
    std::string user = u->user.substr(0, u->user.find(';'));
    if (u->scheme == "tel" || u->params.get("user") == "phone" ||
        (!user.empty() && user.find_first_not_of("+0123456789*#") == std::string::npos))
        return user;
    return uri;
}

bool hasSend(Direction d)
{
    return d == Direction::SendRecv || d == Direction::SendOnly;
}

bool hasRecv(Direction d)
{
    return d == Direction::SendRecv || d == Direction::RecvOnly;
}

Direction makeDirection(bool send, bool recv)
{
    if (send && recv)
        return Direction::SendRecv;
    if (send)
        return Direction::SendOnly;
    if (recv)
        return Direction::RecvOnly;
    return Direction::Inactive;
}

// The other side's resources, as its SDP says (its "local" is its side)
bool resourcesMet(const Sdp &theirs)
{
    return !theirs.qos.present || theirs.qos.localCurrent || theirs.qos.localDesired != "mandatory";
}

const Codec *matching(const Sdp &sdp, const Codec &like)
{
    for (const Codec &c : sdp.codecs)
        if (iequals(c.name, like.name) && c.rate == like.rate && c.octetAlign() == like.octetAlign())
            return &c;
    return nullptr;
}

const Codec *telephoneEventAt(const Sdp &sdp, unsigned rate)
{
    const Codec *any = nullptr;
    for (const Codec &c : sdp.codecs)
        if (iequals(c.name, "telephone-event")) {
            if (c.rate == rate)
                return &c;
            if (!any)
                any = &c;
        }
    return any;
}

uint64_t sdpSessionId()
{
    static std::mt19937_64 random{std::random_device{}()};
    return random() % 4000000000ull + 1;
}

}  // namespace

const char *callStateName(ImsCallState state)
{
    switch (state) {
    case ImsCallState::Setup: return "setup";
    case ImsCallState::Dialing: return "dialing";
    case ImsCallState::Alerting: return "alerting";
    case ImsCallState::Incoming: return "incoming";
    case ImsCallState::Active: return "active";
    case ImsCallState::Held: return "held";
    case ImsCallState::Ended: return "ended";
    }
    return "?";
}

struct ImsCalls::Session {
    ImsCallInfo info;

    // The dialog (RFC 3261 12)
    std::string callId, localTag, remoteTag;
    std::string localUri, remoteUri;  // name-addrs without tags
    std::string remoteTarget;
    std::vector<std::string> routeSet;
    uint32_t localCseq = 0;
    uint32_t inviteCseq = 0;  // of our last INVITE
    bool confirmed = false;
    std::optional<SipMessage> lastAck;  // sent again for a retransmitted 2xx

    // Our initial INVITE
    std::string inviteBranch;
    bool inviteOpen = false;  // its transaction runs
    bool provisional = false;
    bool cancelWanted = false;
    uint32_t lastRseq = 0;
    bool prackPending = false;
    EventLoop::TimerId ringbackTimer = 0;

    // An incoming INVITE
    std::optional<SipMessage> inviteIn;
    Source source;
    uint32_t rseq = 0;
    std::optional<SipMessage> reliable;  // our reliable 1xx, until its PRACK
    std::chrono::milliseconds relInterval{0}, relElapsed{0};
    EventLoop::TimerId relTimer = 0;
    bool answerSentReliably = false;
    EventLoop::TimerId ringTimer = 0;

    // Our 2xx to an INVITE, until its ACK
    std::optional<SipMessage> ok;
    std::optional<SipMessage> okRequest;
    Source okSource;
    std::chrono::milliseconds okInterval{0}, okElapsed{0};
    EventLoop::TimerId okTimer = 0;
    bool answerInAck = false;  // our 2xx made the offer

    // SDP
    uint64_t sdpId = 0, sdpVersion = 0;
    uint16_t port = 0;
    std::optional<Sdp> local, remote;
    bool preconditions = false;
    bool remoteResources = false;
    bool qosUpdateSent = false;
    bool localHold = false;
    bool reinvitePending = false;

    // Media
    std::optional<int> channel;
    bool mediaBusy = false, mediaDirty = false;

    // The session timer (RFC 4028)
    EventLoop::TimerId sessionTimer = 0;
    unsigned sessionInterval = 0;
    bool weRefresh = false;
};

ImsCalls::ImsCalls(EventLoop &loop, MediaEngine &media) : loop_(loop), media_(media)
{
    MediaEvents events;
    events.bearer = [this](int id, int qci, bool added) { bearer(id, qci, added); };
    events.timeout = [this](int channel) {
        for (auto &[id, s] : sessions_)
            if (s->channel == channel && s->info.state != ImsCallState::Ended) {
                log("call " + std::to_string(id) + ": no RTP, ending it");
                sendBye(*s);
                end(*s, 0, "RTP timeout");
                return;
            }
    };
    media_.onEvents(std::move(events));
}

ImsCalls::~ImsCalls()
{
    *alive_ = false;
    for (auto &[id, s] : sessions_)
        for (auto t : {s->ringbackTimer, s->relTimer, s->ringTimer, s->okTimer, s->sessionTimer})
            loop_.cancel(t);
}

void ImsCalls::log(const std::string &line)
{
    if (log_)
        log_("ims: " + line);
}

ImsCalls::Session *ImsCalls::find(int id)
{
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

ImsCalls::Session *ImsCalls::byCallId(const std::string &callId)
{
    for (auto &[id, s] : sessions_)
        if (s->callId == callId)
            return s.get();
    return nullptr;
}

std::vector<ImsCallInfo> ImsCalls::calls() const
{
    std::vector<ImsCallInfo> out;
    for (const auto &[id, s] : sessions_)
        if (s->info.state != ImsCallState::Setup && s->info.state != ImsCallState::Ended)
            out.push_back(s->info);
    return out;
}

bool ImsCalls::any() const
{
    for (const auto &[id, s] : sessions_)
        if (s->info.state != ImsCallState::Ended)
            return true;
    return false;
}

void ImsCalls::notify(Session &s)
{
    if (s.info.state == ImsCallState::Setup)
        return;
    const ImsCallInfo info = s.info;
    for (auto &c : changed_)
        c(info);
}

void ImsCalls::setState(Session &s, ImsCallState state)
{
    if (s.info.state == state)
        return;
    s.info.state = state;
    log("call " + std::to_string(s.info.id) + ": " + callStateName(state));
    notify(s);
    updateAudio();
}

void ImsCalls::end(Session &s, int status, const std::string &reason, bool retryOnCs)
{
    if (s.info.state == ImsCallState::Ended)
        return;
    const bool shown = s.info.state != ImsCallState::Setup;
    for (auto *t : {&s.ringbackTimer, &s.relTimer, &s.ringTimer, &s.okTimer, &s.sessionTimer}) {
        loop_.cancel(*t);
        *t = 0;
    }
    s.reliable.reset();
    s.ok.reset();
    if (s.channel && !s.mediaBusy) {
        media_.close(*s.channel, {});
        s.channel.reset();
    }
    if (s.port && !s.mediaBusy) {
        media_.releasePort(s.port);
        s.port = 0;
    }
    s.info.endStatus = status;
    s.info.endReason = reason;
    s.info.retryOnCs = retryOnCs;
    s.info.state = ImsCallState::Ended;
    log("call " + std::to_string(s.info.id) + ": ended (" + (status ? std::to_string(status) + " " : "") + reason +
        (retryOnCs ? ", retry on CS" : "") + ")");
    if (shown)
        for (auto &c : changed_)
            c(s.info);
    updateAudio();
    // Kept a while for late answers (a 2xx after our CANCEL needs ACK + BYE)
    const int id = s.info.id;
    auto alive = alive_;
    loop_.after(std::chrono::seconds(32), [this, alive, id]() {
        if (!*alive)
            return;
        Session *s = find(id);
        if (s && s->info.state == ImsCallState::Ended && !s->mediaBusy)
            sessions_.erase(id);
    });
}

void ImsCalls::updateAudio()
{
    // Audio while a call has its voice channel (an outgoing call's early
    // media included; an incoming one only once answered)
    bool on = false;
    for (const auto &[id, s] : sessions_)
        if (s->info.state != ImsCallState::Ended && (s->channel || s->mediaBusy))
            on = true;
    if (on == audioOn_)
        return;
    audioOn_ = on;
    if (audio_)
        audio_(on);
}

// Attach and detach

void ImsCalls::attach(SipEndpoint &endpoint, ImsRegistration &registration, const std::string &pdnName)
{
    endpoint_ = &endpoint;
    registration_ = &registration;
    profile_ = registration.params().profile;
    pdnName_ = pdnName;
    auto alive = alive_;
    endpoint.onStrayResponse([this, alive](const SipMessage &r) {
        if (*alive)
            handleStrayResponse(r);
    });
    media_.prepare(endpoint.localAddress(), pdnName, [this, alive](bool ok) {
        if (*alive && !ok)
            log("the voice engine did not take the PDN: calls will have no audio");
    });
}

void ImsCalls::detach()
{
    for (auto &[id, s] : sessions_)
        end(*s, 0, "IMS registration lost", s->info.state == ImsCallState::Dialing);
    endpoint_ = nullptr;
    registration_ = nullptr;
}

// Building messages

std::string ImsCalls::contact() const
{
    return registration_->info().contact;
}

void ImsCalls::addCommon(SipMessage &m, Session &s, bool body)
{
    m.add("Contact", contact());
    m.add("Allow", kAllow);
    if (body && s.local) {
        m.add("Content-Type", "application/sdp");
        m.body = s.local->str();
    }
}

SipMessage ImsCalls::inDialog(Session &s, const std::string &method)
{
    SipMessage m = SipMessage::request(method, s.remoteTarget);
    for (const auto &r : s.routeSet)
        m.add("Route", r);
    m.add("Max-Forwards", "70");
    m.add("From", s.localUri + ";tag=" + s.localTag);
    m.add("To", s.remoteUri + (s.remoteTag.empty() ? "" : ";tag=" + s.remoteTag));
    m.add("Call-ID", s.callId);
    m.add("CSeq", std::to_string(++s.localCseq) + " " + method);
    registration_->decorate(m, false);
    return m;
}

Sdp ImsCalls::nextSdp(Session &s)
{
    Sdp n = *s.local;
    n.version = ++s.sdpVersion;
    return n;
}

void ImsCalls::sendInDialog(Session &s, SipMessage m, SipEndpoint::ResponseHandler handler)
{
    (void)s;
    endpoint_->send(std::move(m), registration_->route(), std::move(handler));
}

void ImsCalls::ack(Session &s, uint32_t cseq)
{
    SipMessage a = SipMessage::request("ACK", s.remoteTarget);
    Via via;
    via.transport = "UDP";
    via.host = endpoint_->localAddress();
    via.port = endpoint_->port(registration_->route().flow);
    via.params.set("branch", newBranch());
    via.params.set("rport");
    a.add("Via", via.str());
    for (const auto &r : s.routeSet)
        a.add("Route", r);
    a.add("Max-Forwards", "70");
    a.add("From", s.localUri + ";tag=" + s.localTag);
    a.add("To", s.remoteUri + ";tag=" + s.remoteTag);
    a.add("Call-ID", s.callId);
    a.add("CSeq", std::to_string(cseq) + " ACK");
    endpoint_->sendStateless(a, registration_->route());
    s.lastAck = a;
}

void ImsCalls::sendBye(Session &s)
{
    if (!endpoint_ || s.remoteTag.empty())
        return;
    SipMessage bye = inDialog(s, "BYE");
    const int id = s.info.id;
    sendInDialog(s, std::move(bye), [this, id](const SipMessage *r) {
        if (r && r->status() >= 300)
            log("call " + std::to_string(id) + ": BYE answered " + std::to_string(r->status()));
    });
}

std::string ImsCalls::targetUri(const std::string &number, const std::string &domain)
{
    if (number.starts_with("sip:") || number.starts_with("sips:") || number.starts_with("tel:"))
        return number;
    if (number.find('@') != std::string::npos)
        return "sip:" + number;
    std::string n;
    for (char c : number)
        if (!std::strchr(" -().", c))
            n += c;
    // 24.229 5.1.2A.1.5: a global number as itself, a local one in the
    // context of the home domain; SIP URIs, as the stock profile's
    // remote_uri_type says
    if (n.starts_with("+"))
        return "sip:" + n + "@" + domain + ";user=phone";
    return "sip:" + n + ";phone-context=" + domain + "@" + domain + ";user=phone";
}

// Outgoing calls

std::optional<int> ImsCalls::dial(const std::string &number)
{
    if (!attached() || registration_->state() != RegistrationState::Registered || number.empty())
        return std::nullopt;
    const RegistrationInfo &reg = registration_->info();
    const std::string target = targetUri(number, registration_->params().identity.domain);
    const uint16_t port = media_.allocatePort();
    if (!port) {
        log("no free RTP port for a call");
        return std::nullopt;
    }

    auto owned = std::make_unique<Session>();
    Session &s = *owned;
    s.info.id = nextId_++;
    s.info.incoming = false;
    s.info.number = number;
    s.info.state = ImsCallState::Dialing;
    s.callId = randomHex(32);
    s.localTag = randomHex(10);
    s.localUri = "<" + reg.defaultImpu + ">";
    s.remoteUri = "<" + target + ">";
    s.remoteTarget = target;
    s.port = port;
    s.sdpId = s.sdpVersion = sdpSessionId();
    s.preconditions = profile_.preconditions;
    MediaCapabilities caps = profile_.media;
    caps.preconditions = profile_.preconditions;
    Sdp offer = makeOffer(caps, endpoint_->localAddress(), port, s.sdpId);
    setPreconditions(offer, voiceBearer_, false);
    s.local = offer;

    SipMessage m = SipMessage::request("INVITE", target);
    m.add("Max-Forwards", "70");
    registration_->decorate(m);
    m.add("From", s.localUri + ";tag=" + s.localTag);
    m.add("To", s.remoteUri);
    m.add("Call-ID", s.callId);
    s.inviteCseq = ++s.localCseq;
    m.add("CSeq", std::to_string(s.inviteCseq) + " INVITE");
    m.add("Contact", contact());
    m.add("Accept-Contact", std::string("*;") + kMmtelTag);
    m.add("P-Preferred-Service", kMmtelService);
    m.add("P-Preferred-Identity", "<" + reg.defaultImpu + ">");
    m.add("P-Early-Media", "supported");
    m.add("Supported", profile_.preconditions ? "100rel, precondition, timer" : "100rel, timer");
    m.add("Allow", kAllow);
    if (profile_.sessionExpires) {
        m.add("Session-Expires", std::to_string(profile_.sessionExpires));
        m.add("Min-SE", std::to_string(profile_.minSe));
    }
    m.add("Accept", "application/sdp, application/3gpp-ims+xml");
    m.add("Content-Type", "application/sdp");
    m.body = s.local->str();

    const int id = s.info.id;
    sessions_[id] = std::move(owned);
    log("call " + std::to_string(id) + ": dialing");
    notify(s);
    auto alive = alive_;
    s.inviteOpen = true;
    s.inviteBranch = endpoint_->send(std::move(m), registration_->route(), [this, alive, id](const SipMessage *r) {
        if (*alive)
            inviteAnswered(id, r);
    });
    return id;
}

void ImsCalls::earlyDialog(Session &s, const SipMessage &r)
{
    const std::string tag = tagOf(r.header("To"));
    if (tag.empty())
        return;
    const bool final = r.status() >= 200;
    if (s.remoteTag.empty() || (final && tag != s.remoteTag)) {
        // Forks: the first early dialog is ours; a 2xx from another fork
        // takes over
        s.remoteTag = tag;
        auto rr = r.headers("Record-Route");
        s.routeSet.assign(rr.rbegin(), rr.rend());
    } else if (tag != s.remoteTag) {
        return;
    }
    const std::string target = uriOf(r.header("Contact"));
    if (!target.empty())
        s.remoteTarget = target;
}

void ImsCalls::prack(Session &s, const SipMessage &r)
{
    auto rseqText = r.header("RSeq");
    if (!rseqText)
        return;
    uint32_t rseq = 0;
    try {
        rseq = static_cast<uint32_t>(std::stoul(*rseqText));
    } catch (...) {
        return;
    }
    if (rseq <= s.lastRseq)
        return;  // a retransmission: its PRACK is under way
    s.lastRseq = rseq;
    SipMessage p = inDialog(s, "PRACK");
    p.add("RAck", std::to_string(rseq) + " " + std::to_string(s.inviteCseq) + " INVITE");
    s.prackPending = true;
    const int id = s.info.id;
    auto alive = alive_;
    sendInDialog(s, std::move(p), [this, alive, id](const SipMessage *answer) {
        if (!*alive || (answer && answer->status() < 200))
            return;
        Session *s = find(id);
        if (!s)
            return;
        s->prackPending = false;
        if (answer && isSdp(*answer))
            if (auto sdp = sdpOf(*answer))
                remoteAnswer(*s, *sdp);
        sendQosUpdate(*s);
    });
}

void ImsCalls::sendQosUpdate(Session &s)
{
    // Our resources are up: tell the other side with an UPDATE (RFC 3312
    // 5.1), once the offer/answer of the INVITE is done
    if (s.info.state == ImsCallState::Ended || !s.preconditions || !voiceBearer_ || s.qosUpdateSent ||
        s.prackPending || s.reliable || !s.local || !s.remote || !s.remote->qos.present || s.confirmed ||
        s.local->qos.localCurrent || s.remoteTag.empty())
        return;
    Sdp sdp = nextSdp(s);
    setPreconditions(sdp, true, s.remote->qos.localCurrent);
    s.local = sdp;
    s.qosUpdateSent = true;
    SipMessage u = inDialog(s, "UPDATE");
    addCommon(u, s, true);
    const int id = s.info.id;
    auto alive = alive_;
    log("call " + std::to_string(id) + ": voice bearer up, UPDATE");
    sendInDialog(s, std::move(u), [this, alive, id](const SipMessage *r) {
        if (!*alive || (r && r->status() < 200))
            return;
        Session *s = find(id);
        if (!s || s->info.state == ImsCallState::Ended)
            return;
        if (!r || r->status() >= 300) {
            log("call " + std::to_string(id) + ": UPDATE failed");
            return;
        }
        if (auto sdp = sdpOf(*r))
            remoteAnswer(*s, *sdp);
        checkIncomingReady(*s);
    });
}

void ImsCalls::inviteAnswered(int id, const SipMessage *r)
{
    Session *sp = find(id);
    if (!sp)
        return;
    Session &s = *sp;
    if (!r || r->status() >= 200)
        s.inviteOpen = false;
    const bool ended = s.info.state == ImsCallState::Ended;
    if (!r) {
        if (!ended)
            end(s, 408, "No answer from the network", !s.provisional);
        return;
    }
    const int status = r->status();
    if (status == 100)
        return;
    if (status < 200) {
        s.provisional = true;
        if (s.cancelWanted) {
            cancel(s);
            return;
        }
        if (ended)
            return;
        earlyDialog(s, *r);
        if (tagOf(r->header("To")) != s.remoteTag)
            return;
        if (r->hasOption("Require", "100rel"))
            prack(s, *r);
        if (auto sdp = sdpOf(*r))
            if (!s.remote)
                remoteAnswer(s, *sdp);
        if (status == 180 && s.info.state == ImsCallState::Dialing) {
            setState(s, ImsCallState::Alerting);
            auto alive = alive_;
            s.ringbackTimer = loop_.after(std::chrono::seconds(profile_.ringbackTimer), [this, alive, id]() {
                if (!*alive)
                    return;
                Session *s = find(id);
                if (!s)
                    return;
                s->ringbackTimer = 0;
                log("call " + std::to_string(id) + ": no answer in time");
                cancel(*s);
                end(*s, 0, "No answer");
            });
        }
        return;
    }
    if (status < 300) {
        earlyDialog(s, *r);
        ack(s, s.inviteCseq);
        if (ended) {
            // Hung up while the answer was on its way
            sendBye(s);
            return;
        }
        s.confirmed = true;
        loop_.cancel(s.ringbackTimer);
        s.ringbackTimer = 0;
        if (auto sdp = sdpOf(*r))
            if (!s.remote || sdp->version != s.remote->version)
                remoteAnswer(s, *sdp);
        if (!s.remote) {
            log("call " + std::to_string(id) + ": answered without SDP");
            sendBye(s);
            end(s, 0, "No media");
            return;
        }
        sessionTimer(s, *r, true);
        setState(s, ImsCallState::Active);
        syncMedia(s);
        return;
    }
    if (ended)
        return;  // 487 after our CANCEL
    const auto &codes = profile_.csfbCodes;
    const bool cs = std::find(codes.begin(), codes.end(), status) != codes.end();
    end(s, status, r->reason(), cs);
}

void ImsCalls::cancel(Session &s)
{
    s.cancelWanted = false;
    if (!s.inviteOpen)
        return;
    if (!s.provisional) {
        // RFC 3261 9.1: a CANCEL only after a provisional answer
        s.cancelWanted = true;
        return;
    }
    endpoint_->cancel(s.inviteBranch, [](const SipMessage *) {});
}

// Incoming calls

void ImsCalls::incoming(const SipMessage &request, const Source &source)
{
    endpoint_->respond(source, request, request.makeResponse(100, "Trying"));
    auto offer = sdpOf(request);
    if (!offer || !offer->voice() || offer->port == 0) {
        // No offer (or none we can take): offerless INVITEs are not done
        log("incoming call without a usable SDP offer, rejected");
        endpoint_->respond(source, request, request.makeResponse(488, "Not Acceptable Here"));
        return;
    }
    const uint16_t port = media_.allocatePort();
    if (!port) {
        endpoint_->respond(source, request, request.makeResponse(486, "Busy Here"));
        return;
    }

    auto owned = std::make_unique<Session>();
    Session &s = *owned;
    s.info.id = nextId_++;
    s.info.incoming = true;
    s.info.state = ImsCallState::Setup;
    std::string caller = uriOf(request.header("P-Asserted-Identity"));
    if (caller.empty())
        caller = uriOf(request.header("From"));
    // 24.229: a tel URI in the asserted identities is the number
    for (const auto &pai : request.headers("P-Asserted-Identity"))
        if (uriOf(pai).starts_with("tel:"))
            caller = uriOf(pai);
    if (request.header("Privacy").value_or("").find("id") != std::string::npos ||
        caller.find("anonymous") != std::string::npos)
        caller.clear();
    s.info.number = caller.empty() ? std::string() : numberOf(caller);
    s.callId = request.callId().value_or("");
    s.remoteTag = tagOf(request.header("From"));
    s.localTag = randomHex(10);
    s.remoteUri = bareNameAddr(request.header("From"));
    s.localUri = bareNameAddr(request.header("To"));
    s.remoteTarget = uriOf(request.header("Contact"));
    s.routeSet = request.headers("Record-Route");
    s.inviteIn = request;
    s.source = source;
    s.port = port;
    s.sdpId = s.sdpVersion = sdpSessionId();

    MediaCapabilities caps = profile_.media;
    caps.preconditions = profile_.preconditions;
    auto answer = makeAnswer(caps, *offer, endpoint_->localAddress(), port, s.sdpId);
    if (!answer || s.remoteTarget.empty()) {
        media_.releasePort(port);
        endpoint_->respond(source, request, request.makeResponse(488, "Not Acceptable Here"));
        return;
    }
    s.remote = *offer;
    s.remoteResources = resourcesMet(*offer);
    s.preconditions = offer->qos.present && profile_.preconditions &&
                      (request.hasOption("Supported", "100rel") || request.hasOption("Require", "100rel"));
    if (s.preconditions) {
        // Ask the other side to confirm when its resources are there
        setPreconditions(*answer, voiceBearer_, s.remoteResources, !s.remoteResources);
    } else {
        answer->qos.present = false;
    }
    answer->direction = makeDirection(hasRecv(offer->direction), hasSend(offer->direction));
    s.local = *answer;
    s.info.remoteHeld = !hasRecv(offer->direction);

    const int id = s.info.id;
    sessions_[id] = std::move(owned);
    log("call " + std::to_string(id) + ": incoming" + (s.preconditions ? " (preconditions)" : ""));
    if (s.preconditions) {
        respondInvite(s, 183, "Session Progress", true, true);
        s.answerSentReliably = true;
    }
    checkIncomingReady(s);
}

void ImsCalls::respondInvite(Session &s, int status, const std::string &reason, bool reliable, bool body)
{
    SipMessage r = s.inviteIn->makeResponse(status, reason);
    r.set("To", s.inviteIn->header("To").value_or("") + ";tag=" + s.localTag);
    if (status > 100 && status < 300) {
        for (const auto &rr : s.inviteIn->headers("Record-Route"))
            r.add("Record-Route", rr);
        addCommon(r, s, body);
        r.add("Supported", "100rel, precondition, timer");
    }
    if (reliable) {
        r.add("Require", s.preconditions ? "100rel, precondition" : "100rel");
        r.add("RSeq", std::to_string(++s.rseq));
    }
    if (status >= 200 && status < 300 && s.sessionInterval) {
        r.add("Session-Expires",
              std::to_string(s.sessionInterval) + ";refresher=" + (s.weRefresh ? "uas" : "uac"));
        r.add("Require", "timer");
    }
    if (reliable) {
        // Sent again until its PRACK (RFC 3262 3)
        s.reliable = r;
        s.relInterval = profile_.timers.t1;
        s.relElapsed = std::chrono::milliseconds(0);
        const int id = s.info.id;
        auto alive = alive_;
        loop_.cancel(s.relTimer);
        s.relTimer = loop_.after(s.relInterval, [this, alive, id]() {
            if (*alive)
                retransmitReliable(id);
        });
    }
    if (status >= 200 && status < 300) {
        s.ok = r;
        s.okRequest = s.inviteIn;
        s.okSource = s.source;
        startOkRetransmit(s);
    }
    endpoint_->respond(s.source, *s.inviteIn, r);
}

void ImsCalls::retransmitReliable(int id)
{
    Session *s = find(id);
    if (!s || !s->reliable || !endpoint_)
        return;
    s->relTimer = 0;
    s->relElapsed += s->relInterval;
    if (s->relElapsed >= profile_.timers.t1 * 64) {
        // RFC 3262 3: no PRACK, the INVITE fails with a 5xx
        log("call " + std::to_string(id) + ": no PRACK");
        s->reliable.reset();
        respondInvite(*s, 504, "Server Time-out", false, false);
        end(*s, 0, "No PRACK");
        return;
    }
    endpoint_->respond(s->source, *s->inviteIn, *s->reliable);
    s->relInterval *= 2;
    auto alive = alive_;
    s->relTimer = loop_.after(s->relInterval, [this, alive, id]() {
        if (*alive)
            retransmitReliable(id);
    });
}

void ImsCalls::startOkRetransmit(Session &s)
{
    loop_.cancel(s.okTimer);
    s.okInterval = profile_.timers.t1;
    s.okElapsed = std::chrono::milliseconds(0);
    const int id = s.info.id;
    auto alive = alive_;
    s.okTimer = loop_.after(s.okInterval, [this, alive, id]() {
        if (*alive)
            retransmitOk(id);
    });
}

void ImsCalls::retransmitOk(int id)
{
    // RFC 3261 13.3.1.4: our 2xx again until the ACK, at most 64*T1
    Session *s = find(id);
    if (!s || !s->ok || !endpoint_)
        return;
    s->okTimer = 0;
    s->okElapsed += s->okInterval;
    if (s->okElapsed >= profile_.timers.t1 * 64) {
        log("call " + std::to_string(id) + ": no ACK");
        s->ok.reset();
        sendBye(*s);
        end(*s, 0, "No ACK");
        return;
    }
    endpoint_->respond(s->okSource, *s->okRequest, *s->ok);
    s->okInterval = std::min(s->okInterval * 2, std::chrono::milliseconds(profile_.timers.t2));
    auto alive = alive_;
    s->okTimer = loop_.after(s->okInterval, [this, alive, id]() {
        if (*alive)
            retransmitOk(id);
    });
}

void ImsCalls::checkIncomingReady(Session &s)
{
    if (!s.info.incoming || s.info.state != ImsCallState::Setup)
        return;
    if (s.preconditions) {
        sendQosUpdate(s);
        if (s.reliable || !s.remoteResources || !voiceBearer_)
            return;
    }
    respondInvite(s, 180, "Ringing", false, false);
    setState(s, ImsCallState::Incoming);
    const int id = s.info.id;
    auto alive = alive_;
    s.ringTimer = loop_.after(std::chrono::seconds(profile_.ringingTimer), [this, alive, id]() {
        if (!*alive)
            return;
        Session *s = find(id);
        if (!s || s->info.state != ImsCallState::Incoming)
            return;
        s->ringTimer = 0;
        respondInvite(*s, 480, "Temporarily Unavailable", false, false);
        end(*s, 0, "Not answered");
    });
}

bool ImsCalls::answer(int id)
{
    Session *s = find(id);
    if (!s || !endpoint_ || !s->info.incoming || s->info.state != ImsCallState::Incoming)
        return false;
    loop_.cancel(s->ringTimer);
    s->ringTimer = 0;
    // The session timer: as the caller asked, else none; the caller
    // refreshes if it can
    if (auto se = s->inviteIn->header("Session-Expires")) {
        Params p = Params::parse(*se);
        unsigned interval = 0;
        try {
            interval = static_cast<unsigned>(std::stoul(*se));
        } catch (...) {
        }
        if (interval) {
            interval = std::max(interval, profile_.minSe);
            auto refresher = p.get("refresher");
            const bool theyCan = s->inviteIn->hasOption("Supported", "timer");
            s->weRefresh = refresher ? *refresher == "uas" : !theyCan;
            s->sessionInterval = interval;
        }
    }
    if (!s->answerSentReliably && s->local)
        s->local->qos.present = false;
    respondInvite(*s, 200, "OK", false, !s->answerSentReliably);
    s->confirmed = true;
    if (s->sessionInterval) {
        SipMessage fake = SipMessage::response(200, "OK");
        fake.add("Session-Expires",
                 std::to_string(s->sessionInterval) + ";refresher=" + (s->weRefresh ? "uas" : "uac"));
        sessionTimer(*s, fake, false);
    }
    setState(*s, ImsCallState::Active);
    syncMedia(*s);
    return true;
}

// Requests from the network

bool ImsCalls::handleRequest(const SipMessage &request, const Source &source)
{
    const std::string &method = request.method();
    static const char *const kOurs[] = {"INVITE", "ACK", "BYE", "CANCEL", "PRACK", "UPDATE", "INFO", "OPTIONS"};
    if (std::find_if(std::begin(kOurs), std::end(kOurs), [&](const char *m) { return method == m; }) ==
        std::end(kOurs))
        return false;
    if (!endpoint_)
        return false;
    Session *s = byCallId(request.callId().value_or(""));
    const bool inDialog = !tagOf(request.header("To")).empty();

    if (method == "OPTIONS" && !s)
        return false;
    if (method == "INVITE" && !inDialog) {
        if (s)
            return true;  // a retransmission the transaction layer did not catch
        if (!registration_ || registration_->state() != RegistrationState::Registered) {
            endpoint_->respond(source, request, request.makeResponse(480, "Temporarily Unavailable"));
            return true;
        }
        incoming(request, source);
        return true;
    }
    if (method == "ACK") {
        if (s && s->ok && s->okRequest) {
            auto a = request.cseq(), o = s->okRequest->cseq();
            if (a && o && a->first == o->first) {
                s->ok.reset();
                loop_.cancel(s->okTimer);
                s->okTimer = 0;
                if (s->answerInAck) {
                    s->answerInAck = false;
                    if (auto sdp = sdpOf(request))
                        remoteAnswer(*s, *sdp);
                }
            }
        }
        return true;
    }
    if (method == "CANCEL") {
        if (!s || !s->inviteIn || s->inviteIn->branch() != request.branch()) {
            endpoint_->respond(source, request, request.makeResponse(481, "Call/Transaction Does Not Exist"));
            return true;
        }
        endpoint_->respond(source, request, request.makeResponse(200, "OK"));
        if (s->info.state == ImsCallState::Setup || s->info.state == ImsCallState::Incoming) {
            s->reliable.reset();
            respondInvite(*s, 487, "Request Terminated", false, false);
            end(*s, 487, "Cancelled");
        }
        return true;
    }
    if (!s || s->info.state == ImsCallState::Ended) {
        endpoint_->respond(source, request, request.makeResponse(481, "Call/Transaction Does Not Exist"));
        return true;
    }
    inDialogRequest(*s, request, source);
    return true;
}

void ImsCalls::inDialogRequest(Session &s, const SipMessage &request, const Source &source)
{
    const std::string &method = request.method();
    SipMessage ok = request.makeResponse(200, "OK");
    if (method == "BYE") {
        endpoint_->respond(source, request, ok);
        end(s, 0, "Remote hangup");
        return;
    }
    if (method == "PRACK") {
        if (s.reliable) {
            const std::string rack = request.header("RAck").value_or("");
            if (rack.starts_with(std::to_string(s.rseq) + " ")) {
                s.reliable.reset();
                loop_.cancel(s.relTimer);
                s.relTimer = 0;
            }
        }
        endpoint_->respond(source, request, ok);
        checkIncomingReady(s);
        return;
    }
    if (method == "UPDATE" || method == "INVITE") {
        offerReceived(s, request, source);
        return;
    }
    // INFO, OPTIONS
    endpoint_->respond(source, request, ok);
}

void ImsCalls::offerReceived(Session &s, const SipMessage &request, const Source &source)
{
    const bool invite = request.method() == "INVITE";
    if (invite && (s.reinvitePending || s.ok)) {
        // RFC 3261 14.2: an INVITE while ours is pending
        SipMessage r = request.makeResponse(491, "Request Pending");
        endpoint_->respond(source, request, r);
        return;
    }
    if (invite)
        endpoint_->respond(source, request, request.makeResponse(100, "Trying"));
    const std::string target = uriOf(request.header("Contact"));
    if (!target.empty())
        s.remoteTarget = target;

    SipMessage r = request.makeResponse(200, "OK");
    r.set("To", request.header("To").value_or(""));
    bool body = false;
    if (isSdp(request)) {
        auto offer = sdpOf(request);
        if (!offer || !offer->voice()) {
            endpoint_->respond(source, request, request.makeResponse(488, "Not Acceptable Here"));
            return;
        }
        s.remote = *offer;
        s.remoteResources = resourcesMet(*offer);
        Sdp answer = nextSdp(s);
        // Our direction: what we want (held or not) and what the offer allows
        answer.direction =
            makeDirection(!s.localHold && hasRecv(offer->direction), hasSend(offer->direction));
        if (s.preconditions && offer->qos.present) {
            answer.qos.present = true;
            setPreconditions(answer, voiceBearer_ || s.confirmed, offer->qos.localCurrent);
        } else {
            answer.qos.present = false;
        }
        s.local = answer;
        s.info.remoteHeld = !hasRecv(offer->direction);
        body = true;
    } else if (invite) {
        // An offerless re-INVITE: our offer goes in the 2xx, the answer in the ACK
        Sdp offer = nextSdp(s);
        offer.direction = s.localHold ? Direction::SendOnly : Direction::SendRecv;
        s.local = offer;
        s.answerInAck = true;
        body = true;
    }
    addCommon(r, s, body);
    r.add("Supported", "100rel, precondition, timer");
    if (auto se = request.header("Session-Expires")) {
        Params p = Params::parse(*se);
        const std::string refresher = p.get("refresher").value_or("uac");
        r.add("Session-Expires", se->substr(0, se->find(';')) + ";refresher=" + refresher);
        r.add("Require", "timer");
        SipMessage view = SipMessage::response(200, "OK");
        view.add("Session-Expires", se->substr(0, se->find(';')) + ";refresher=" + refresher);
        sessionTimer(s, view, false);
    }
    if (invite) {
        s.ok = r;
        s.okRequest = request;
        s.okSource = source;
        startOkRetransmit(s);
    }
    endpoint_->respond(source, request, r);

    if (s.info.state == ImsCallState::Setup) {
        checkIncomingReady(s);
        return;
    }
    if (s.confirmed || !s.info.incoming)
        syncMedia(s);
    notify(s);
}

void ImsCalls::handleStrayResponse(const SipMessage &response)
{
    Session *s = byCallId(response.callId().value_or(""));
    if (!s || !s->lastAck || !endpoint_)
        return;
    auto cseq = response.cseq(), acked = s->lastAck->cseq();
    if (cseq && acked && cseq->first == acked->first)
        endpoint_->sendStateless(*s->lastAck, registration_->route());
}

// Front-end requests

bool ImsCalls::hangup(int id)
{
    Session *s = find(id);
    if (!s || s->info.state == ImsCallState::Ended || !endpoint_)
        return false;
    if (s->info.incoming && !s->confirmed) {
        s->reliable.reset();
        respondInvite(*s, 486, "Busy Here", false, false);
        end(*s, 0, "Rejected");
        return true;
    }
    if (!s->info.incoming && !s->confirmed) {
        cancel(*s);
        end(*s, 0, "Hung up");
        return true;
    }
    sendBye(*s);
    end(*s, 0, "Hung up");
    return true;
}

bool ImsCalls::hold(int id)
{
    Session *s = find(id);
    if (!s || !endpoint_ || s->info.state != ImsCallState::Active || !s->confirmed || s->reinvitePending || s->ok)
        return false;
    reinvite(*s, true);
    return true;
}

bool ImsCalls::resume(int id)
{
    Session *s = find(id);
    if (!s || !endpoint_ || s->info.state != ImsCallState::Held || s->reinvitePending || s->ok)
        return false;
    reinvite(*s, false);
    return true;
}

bool ImsCalls::dtmf(int id, char digit)
{
    Session *s = find(id);
    if (!s || !s->channel || (s->info.state != ImsCallState::Active && s->info.state != ImsCallState::Alerting))
        return false;
    media_.dtmf(*s->channel, digit, {});
    return true;
}

void ImsCalls::reinvite(Session &s, bool holdCall)
{
    Sdp offer = nextSdp(s);
    offer.direction = holdCall ? Direction::SendOnly : Direction::SendRecv;
    if (offer.qos.present)
        setPreconditions(offer, true, true);
    SipMessage m = inDialog(s, "INVITE");
    s.inviteCseq = s.localCseq;
    s.reinvitePending = true;
    const Sdp previous = *s.local;
    s.local = offer;
    addCommon(m, s, true);
    m.add("Supported", "100rel, precondition, timer");
    if (s.sessionInterval)
        m.add("Session-Expires", std::to_string(s.sessionInterval) + ";refresher=uac");
    const int id = s.info.id;
    const uint32_t cseq = s.inviteCseq;
    auto alive = alive_;
    log("call " + std::to_string(id) + (holdCall ? ": hold" : ": resume"));
    sendInDialog(s, std::move(m), [this, alive, id, cseq, holdCall, previous](const SipMessage *r) {
        if (!*alive || (r && r->status() < 200))
            return;
        Session *s = find(id);
        if (!s)
            return;
        s->reinvitePending = false;
        if (s->info.state == ImsCallState::Ended) {
            if (r && r->status() < 300)
                ack(*s, cseq);
            return;
        }
        if (!r || r->status() >= 300) {
            log("call " + std::to_string(id) + ": re-INVITE failed (" + (r ? std::to_string(r->status()) : "timeout") +
                ")");
            s->local = previous;
            notify(*s);
            if (r && (r->status() == 481 || r->status() == 408)) {
                sendBye(*s);
                end(*s, r->status(), r->reason());
            }
            return;
        }
        ack(*s, cseq);
        if (auto sdp = sdpOf(*r))
            s->remote = *sdp;
        s->localHold = holdCall;
        if (s->remote)
            s->info.remoteHeld = !hasRecv(s->remote->direction);
        if (r->has("Session-Expires"))
            sessionTimer(*s, *r, true);
        setState(*s, holdCall ? ImsCallState::Held : ImsCallState::Active);
        syncMedia(*s);
    });
}

// Media and the bearer

void ImsCalls::bearer(int bearerId, int qci, bool added)
{
    (void)bearerId;
    if (qci != 1)
        return;  // QCI 1 carries conversational voice (TS 23.203)
    voiceBearer_ = added;
    if (!added || !endpoint_)
        return;
    std::vector<int> ids;
    for (auto &[id, s] : sessions_)
        ids.push_back(id);
    for (int id : ids)
        if (Session *s = find(id)) {
            sendQosUpdate(*s);
            checkIncomingReady(*s);
        }
}

void ImsCalls::remoteAnswer(Session &s, const Sdp &answer)
{
    s.remote = answer;
    s.remoteResources = resourcesMet(answer);
    s.info.remoteHeld = !hasRecv(answer.direction);
    if (!s.info.incoming || s.confirmed)
        syncMedia(s);
    sendQosUpdate(s);
}

MediaSetup ImsCalls::mediaSetup(const Session &s) const
{
    const Sdp &local = *s.local, &remote = *s.remote;
    MediaSetup m;
    m.localAddress = endpoint_ ? endpoint_->localAddress() : local.address;
    m.localPort = s.port;
    m.remoteAddress = remote.address;
    m.remotePort = remote.port;
    m.remoteRtcpPort = remote.rtcpPort.value_or(0);
    // The codec chosen is the answer's first voice codec
    const Sdp &answer = s.info.incoming ? local : remote;
    const Codec *chosen = answer.voice();
    if (chosen) {
        const Codec *theirs = matching(remote, *chosen);
        const Codec *ours = matching(local, *chosen);
        m.codec = theirs ? *theirs : *chosen;
        m.codecRxPayloadType = ours ? ours->payloadType : m.codec.payloadType;
        if (const Codec *te = telephoneEventAt(remote, m.codec.rate)) {
            m.dtmf = *te;
            const Codec *mine = telephoneEventAt(local, te->rate);
            m.dtmfRxPayloadType = mine ? mine->payloadType : te->payloadType;
        }
    }
    m.ptime = remote.ptime ? remote.ptime : profile_.media.ptime;
    m.maxptime = remote.maxptime ? remote.maxptime : profile_.media.maxptime;
    m.active = local.direction == Direction::SendRecv && remote.direction == Direction::SendRecv;
    return m;
}

void ImsCalls::syncMedia(Session &s)
{
    if (s.info.state == ImsCallState::Ended || !s.local || !s.remote || s.remote->port == 0 || !s.remote->voice())
        return;
    if (s.mediaBusy) {
        s.mediaDirty = true;
        return;
    }
    const MediaSetup setup = mediaSetup(s);
    const int id = s.info.id;
    auto alive = alive_;
    s.mediaBusy = true;
    auto finished = [this, alive, id]() {
        if (!*alive)
            return;
        Session *s = find(id);
        if (!s)
            return;
        s->mediaBusy = false;
        if (s->info.state == ImsCallState::Ended) {
            if (s->channel) {
                media_.close(*s->channel, {});
                s->channel.reset();
            }
            if (s->port) {
                media_.releasePort(s->port);
                s->port = 0;
            }
            updateAudio();
            return;
        }
        if (s->mediaDirty) {
            s->mediaDirty = false;
            syncMedia(*s);
        }
        updateAudio();
    };
    if (!s.channel) {
        media_.open(setup, [this, alive, id, finished](std::optional<int> channel) {
            if (!*alive)
                return;
            if (Session *s = find(id)) {
                s->channel = channel;
                if (!channel)
                    log("call " + std::to_string(id) + ": the voice engine refused the call's media");
            } else if (channel) {
                media_.close(*channel, {});
            }
            finished();
        });
    } else {
        media_.update(*s.channel, setup, [finished](bool) { finished(); });
    }
    updateAudio();
}

void ImsCalls::sessionTimer(Session &s, const SipMessage &message, bool weAreUac)
{
    loop_.cancel(s.sessionTimer);
    s.sessionTimer = 0;
    auto se = message.header("Session-Expires");
    if (!se)
        return;
    unsigned interval = 0;
    try {
        interval = static_cast<unsigned>(std::stoul(*se));
    } catch (...) {
        return;
    }
    if (interval < 90)
        return;
    const std::string refresher = Params::parse(*se).get("refresher").value_or("uas");
    s.sessionInterval = interval;
    s.weRefresh = (refresher == "uac") == weAreUac;
    const int id = s.info.id;
    auto alive = alive_;
    if (s.weRefresh) {
        s.sessionTimer = loop_.after(std::chrono::seconds(interval / 2), [this, alive, id]() {
            if (*alive)
                refreshSession(id);
        });
    } else {
        // RFC 4028 10: the session ends unless refreshed in time
        const unsigned margin = std::min(32u, interval / 3);
        s.sessionTimer = loop_.after(std::chrono::seconds(interval - margin), [this, alive, id]() {
            if (!*alive)
                return;
            Session *s = find(id);
            if (!s || s->info.state == ImsCallState::Ended)
                return;
            s->sessionTimer = 0;
            log("call " + std::to_string(id) + ": session not refreshed");
            sendBye(*s);
            end(*s, 0, "Session expired");
        });
    }
}

void ImsCalls::refreshSession(int id)
{
    Session *s = find(id);
    if (!s || !endpoint_ || s->info.state == ImsCallState::Ended)
        return;
    s->sessionTimer = 0;
    // UPDATE without SDP (RFC 4028 7.4)
    SipMessage u = inDialog(*s, "UPDATE");
    u.add("Contact", contact());
    u.add("Supported", "timer");
    u.add("Session-Expires", std::to_string(s->sessionInterval) + ";refresher=uac");
    auto alive = alive_;
    sendInDialog(*s, std::move(u), [this, alive, id](const SipMessage *r) {
        if (!*alive || (r && r->status() < 200))
            return;
        Session *s = find(id);
        if (!s || s->info.state == ImsCallState::Ended)
            return;
        if (!r || r->status() == 481 || r->status() == 408) {
            sendBye(*s);
            end(*s, r ? r->status() : 408, "Session refresh failed");
            return;
        }
        if (r->status() >= 300) {
            // Keep the call; try again at the next half interval
            SipMessage view = SipMessage::response(200, "OK");
            view.add("Session-Expires", std::to_string(s->sessionInterval) + ";refresher=uac");
            sessionTimer(*s, view, true);
            return;
        }
        if (r->has("Session-Expires")) {
            sessionTimer(*s, *r, true);
        } else {
            SipMessage view = SipMessage::response(200, "OK");
            view.add("Session-Expires", std::to_string(s->sessionInterval) + ";refresher=uac");
            sessionTimer(*s, view, true);
        }
    });
}

}  // namespace shannond::ims
