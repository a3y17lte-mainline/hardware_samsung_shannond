// SPDX-License-Identifier: GPL-2.0-or-later
//
// VoLTE calls against the fake P-CSCF, which plays the far end and the
// network in between (a scripted peer per test), with a fake media engine.
#include "core/ims/call.h"
#include "core/ims/identity.h"
#include "core/ims/registration.h"
#include "sim/fake_modem.h"
#include "sim/fake_pcscf.h"
#include "tests/test.h"

#include <cstdlib>
#include <map>

using namespace shannond;
using namespace shannond::ims;

namespace {

constexpr uint16_t kPcscfPort = 46060;
constexpr const char *kFarContact = "<sip:far@127.0.0.2:47000>";
constexpr const char *kRecordRoute = "<sip:127.0.0.2:46062;lr>";

class FakeMedia : public MediaEngine {
public:
    explicit FakeMedia(EventLoop &loop) : loop_(loop) {}

    void prepare(const std::string &, const std::string &pdn, Done done) override
    {
        prepared = pdn;
        loop_.post([done]() { done(true); });
    }
    uint16_t allocatePort() override
    {
        next_ += 2;
        ports.insert(next_);
        return next_;
    }
    void releasePort(uint16_t port) override { ports.erase(port); }
    void open(const MediaSetup &setup, std::function<void(std::optional<int>)> done) override
    {
        const int ch = channel_++;
        channels[ch] = setup;
        opened++;
        loop_.post([done, ch]() { done(ch); });
    }
    void update(int channel, const MediaSetup &setup, Done done) override
    {
        channels[channel] = setup;
        updates++;
        loop_.post([done]() { done(true); });
    }
    void close(int channel, Done done) override
    {
        channels.erase(channel);
        loop_.post([done]() {
            if (done)
                done(true);
        });
    }
    void dtmf(int, char digit, Done) override { digits += digit; }
    void bearer(bool added) { events_.bearer(5, 1, added); }

    std::string prepared;
    std::set<uint16_t> ports;
    std::map<int, MediaSetup> channels;
    unsigned opened = 0, updates = 0;
    std::string digits;

private:
    EventLoop &loop_;
    uint16_t next_ = 1232;
    int channel_ = 0;
};

using Reply = std::function<void(const SipMessage &)>;

struct Bench {
    EventLoop loop;
    sim::FakeImsNetwork net{"127.0.0.1"};
    sim::FakePcscf pcscf{loop, "127.0.0.2", kPcscfPort};
    SipEndpoint endpoint{loop, net, timers()};
    sim::Milenage card = FakeModem::testMilenage();
    sim::Sqn sqnMs{};
    ImsRegistration reg{loop, endpoint, net, [this](const Bytes &rand, const Bytes &autn, auto done) {
                            sim::Block r, a;
                            std::copy_n(rand.begin(), 16, r.begin());
                            std::copy_n(autn.begin(), 16, a.begin());
                            auto result = sim::cardAuthenticate(card, sqnMs, r, a);
                            AkaAnswer answer;
                            answer.kind = AkaAnswer::Kind::Ok;
                            answer.res = result.values[0];
                            answer.ck = result.values[1];
                            answer.ik = result.values[2];
                            loop.post([done, answer]() { done(answer); });
                        }};
    FakeMedia media{loop};
    ImsCalls calls{loop, media};
    std::vector<ImsCallInfo> changes;
    std::vector<bool> audio;

    // What reached the far end, and how to answer it
    std::vector<SipMessage> got;
    std::function<void(const SipMessage &, const Reply &)> script;

    Bench()
    {
        endpoint.onRequest([this](const SipMessage &request, const Source &source) {
            if (reg.handleRequest(request, source) || calls.handleRequest(request, source))
                return;
            endpoint.respond(source, request, request.makeResponse(501, "Not Implemented"));
        });
        pcscf.farEnd = [this](const SipMessage &m, const Reply &reply) {
            auto cseq = m.cseq();
            if (!m.isRequest() && cseq && cseq->second == "NOTIFY")
                return;
            got.push_back(m);
            if (script)
                script(m, reply);
        };
        calls.onChanged([this](const ImsCallInfo &c) { changes.push_back(c); });
        calls.onAudio([this](bool on) { audio.push_back(on); });
        if (std::getenv("IMS_TEST_LOG")) {
            auto print = [](const std::string &l) { std::printf("    %s\n", l.c_str()); };
            calls.setLog(print);
            endpoint.setLog(print);
        }
    }

    static SipTimers timers()
    {
        SipTimers t;
        t.t1 = std::chrono::milliseconds(10);
        t.t2 = std::chrono::milliseconds(40);
        return t;
    }

    RegistrationParams params()
    {
        RegistrationParams p;
        p.profile = defaultProfile();
        p.profile.pcscfPort = kPcscfPort;
        p.profile.localPort = 0;
        p.profile.portBase = 46000;
        p.profile.timers = timers();
        p.profile.sessionExpires = 3600;
        p.profile.csfbCodes = {403};
        p.identity = *identityFromImsi("230020000000001", 2);
        p.pcscfs = {"127.0.0.2"};
        p.instance = instanceId("350000000000001");
        p.userAgent = userAgentFor(p.profile, DeviceInfo{});
        return p;
    }

    bool start(std::function<void(RegistrationParams &)> tweak = {})
    {
        auto p = params();
        if (tweak)
            tweak(p);
        reg.start(p);
        if (!loop.runUntil([&] { return reg.state() == RegistrationState::Registered; }, std::chrono::seconds(5)))
            return false;
        calls.attach(endpoint, reg, "ims");
        return loop.runUntil([&] { return media.prepared == "ims"; });
    }

    bool waitFor(const std::function<bool()> &pred) { return loop.runUntil(pred, std::chrono::seconds(5)); }

    const SipMessage *last(const std::string &method) const
    {
        for (auto it = got.rbegin(); it != got.rend(); ++it)
            if (it->isRequest() ? it->method() == method : it->cseq() && it->cseq()->second == method)
                return &*it;
        return nullptr;
    }
    size_t count(const std::string &method, int status = 0) const
    {
        size_t n = 0;
        for (const auto &m : got)
            if (status ? (!m.isRequest() && m.status() == status && m.cseq()->second == method)
                       : (m.isRequest() && m.method() == method))
                n++;
        return n;
    }
    std::optional<ImsCallInfo> call(int id) const
    {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it)
            if (it->id == id)
                return *it;
        return std::nullopt;
    }
};

// The far end's answer to an offer: its first voice codec, resources not
// reserved yet
std::string farAnswer(const std::string &offerText, bool reserved, Direction direction = Direction::SendRecv)
{
    auto offer = Sdp::parse(offerText);
    MediaCapabilities caps;
    auto answer = makeAnswer(caps, *offer, "127.0.0.2", 40000, 5);
    answer->qos.present = offer->qos.present;
    answer->qos.localCurrent = reserved;
    answer->qos.remoteCurrent = offer->qos.localCurrent;
    answer->qos.localDesired = "mandatory";
    answer->qos.remoteDesired = "mandatory";
    answer->direction = direction;
    return answer->str();
}

SipMessage response(const SipMessage &request, int status, const std::string &reason, const std::string &toTag = "far1")
{
    SipMessage r = request.makeResponse(status, reason);
    if (status > 100)
        r.set("To", request.header("To").value_or("") + (request.header("To")->find("tag=") == std::string::npos
                                                               ? ";tag=" + toTag
                                                               : ""));
    return r;
}

void withSdp(SipMessage &m, const std::string &sdp)
{
    m.add("Content-Type", "application/sdp");
    m.body = sdp;
}

// A request from the far end inside a dialog the UE made
SipMessage farRequest(sim::FakePcscf &pcscf, const std::string &method, const SipMessage &dialogRequest, uint32_t cseq,
                      bool ueIsCaller)
{
    // The UE's Contact is the target
    auto contact = NameAddr::parse(dialogRequest.header("Contact").value_or(""));
    SipMessage m = SipMessage::request(method, contact ? contact->uri : "sip:ue");
    m.add("Via", pcscf.via());
    m.add("Max-Forwards", "70");
    if (ueIsCaller) {
        m.add("From", dialogRequest.header("To").value_or("") + ";tag=far1");
        m.add("To", dialogRequest.header("From").value_or(""));
    } else {
        m.add("From", dialogRequest.header("From").value_or(""));
        m.add("To", dialogRequest.header("To").value_or(""));
    }
    m.add("Call-ID", dialogRequest.callId().value_or(""));
    m.add("CSeq", std::to_string(cseq) + " " + method);
    m.add("Contact", kFarContact);
    return m;
}

const char kOffer[] =
    "v=0\r\n"
    "o=- 3815 3815 IN IP4 127.0.0.2\r\n"
    "s=-\r\n"
    "c=IN IP4 127.0.0.2\r\n"
    "t=0 0\r\n"
    "m=audio 41000 RTP/AVP 104 102 105\r\n"
    "a=rtpmap:104 AMR-WB/16000/1\r\n"
    "a=fmtp:104 mode-change-capability=2;max-red=0\r\n"
    "a=rtpmap:102 AMR/8000/1\r\n"
    "a=rtpmap:105 telephone-event/16000\r\n"
    "a=fmtp:105 0-15\r\n"
    "a=ptime:20\r\n"
    "a=maxptime:240\r\n"
    "a=curr:qos local none\r\n"
    "a=curr:qos remote none\r\n"
    "a=des:qos mandatory local sendrecv\r\n"
    "a=des:qos optional remote sendrecv\r\n"
    "a=sendrecv\r\n";

}  // namespace

TEST(call_target_uri)
{
    const std::string d = "ims.mnc002.mcc230.3gppnetwork.org";
    CHECK(ImsCalls::targetUri("+420 603-123 456", d) == "sip:+420603123456@" + d + ";user=phone");
    CHECK(ImsCalls::targetUri("603123456", d) == "sip:603123456;phone-context=" + d + "@" + d + ";user=phone");
    CHECK(ImsCalls::targetUri("tel:+420603123456", d) == "tel:+420603123456");
    CHECK(ImsCalls::targetUri("alice@example.org", d) == "sip:alice@example.org");
}

TEST(call_outgoing_with_preconditions)
{
    Bench b;
    CHECK(b.start());
    std::optional<SipMessage> invite;
    Reply inviteReply;
    b.script = [&](const SipMessage &m, const Reply &reply) {
        if (m.method() == "INVITE" && !invite) {
            invite = m;
            inviteReply = reply;
            reply(response(m, 100, "Trying"));
            SipMessage r = response(m, 183, "Session Progress");
            r.add("Record-Route", kRecordRoute);
            r.add("Contact", kFarContact);
            r.add("Require", "100rel, precondition");
            r.add("RSeq", "1");
            withSdp(r, farAnswer(m.body, false));
            reply(r);
        } else if (m.method() == "PRACK" || m.method() == "UPDATE" || m.method() == "BYE") {
            SipMessage r = response(m, 200, "OK");
            if (m.method() == "UPDATE")
                withSdp(r, farAnswer(m.body, true));
            reply(r);
            if (m.method() == "UPDATE") {
                SipMessage ringing = response(*invite, 180, "Ringing");
                ringing.add("Contact", kFarContact);
                ringing.add("Record-Route", kRecordRoute);
                inviteReply(ringing);
            }
        } else if (m.method() == "INVITE") {
            // Hold: answer recvonly
            SipMessage r = response(m, 200, "OK");
            r.add("Contact", kFarContact);
            withSdp(r, farAnswer(m.body, true, Direction::RecvOnly));
            reply(r);
        }
    };

    auto id = b.calls.dial("+420603123456");
    CHECK(id);
    CHECK(b.waitFor([&] { return b.count("PRACK") == 1; }));
    // The INVITE as stock sends it
    CHECK(invite->requestUri() == "sip:+420603123456@ims.mnc002.mcc230.3gppnetwork.org;user=phone");
    CHECK(invite->hasOption("Supported", "precondition") && invite->hasOption("Supported", "100rel"));
    CHECK(invite->header("P-Preferred-Service") == "urn:urn-7:3gpp-service.ims.icsi.mmtel");
    CHECK(invite->header("Accept-Contact")->find("mmtel") != std::string::npos);
    CHECK(invite->header("Session-Expires") == "3600");
    CHECK(invite->header("From")->find("+420600000001") != std::string::npos);  // the default IMPU
    auto offer = Sdp::parse(invite->body);
    CHECK(offer && offer->qos.present && !offer->qos.localCurrent && offer->codecs.size() == 6);
    // PRACK inside the early dialog: the far end's Contact, its Record-Route
    const SipMessage *prack = b.last("PRACK");
    CHECK(prack->requestUri() == "sip:far@127.0.0.2:47000" && prack->header("RAck") == "1 1 INVITE");
    CHECK(prack->header("Route") == kRecordRoute && prack->header("To")->find("tag=far1") != std::string::npos);
    // Early media: the answer opens the voice channel
    CHECK(b.waitFor([&] { return b.media.opened == 1; }));
    CHECK(b.media.channels[0].remotePort == 40000 && b.media.channels[0].codec.name == "AMR-WB" &&
          b.media.channels[0].codec.payloadType == 116 && b.media.channels[0].dtmf->payloadType == 111);
    CHECK(b.count("UPDATE") == 0);

    // The voice bearer: UPDATE with our resources, then the far end rings
    b.media.bearer(true);
    CHECK(b.waitFor([&] { return b.call(*id) && b.call(*id)->state == ImsCallState::Alerting; }));
    auto update = Sdp::parse(b.last("UPDATE")->body);
    CHECK(update && update->qos.localCurrent && update->version == offer->version + 1);

    // Answer: ACK, active
    SipMessage ok = response(*invite, 200, "OK");
    ok.add("Contact", kFarContact);
    ok.add("Record-Route", kRecordRoute);
    ok.add("Session-Expires", "3600;refresher=uac");
    inviteReply(ok);
    CHECK(b.waitFor([&] { return b.count("ACK") == 1; }));
    CHECK(b.call(*id)->state == ImsCallState::Active);
    CHECK(b.last("ACK")->header("CSeq") == "1 ACK" && b.last("ACK")->header("Route") == kRecordRoute);
    // A lost ACK: the 2xx comes again, so does the ACK
    inviteReply(ok);
    CHECK(b.waitFor([&] { return b.count("ACK") == 2; }));
    CHECK(!b.audio.empty() && b.audio.back());

    CHECK(b.calls.dtmf(*id, '5') && b.media.digits == "5");

    // Hold and resume
    CHECK(b.calls.hold(*id));
    CHECK(b.waitFor([&] { return b.call(*id)->state == ImsCallState::Held; }));
    CHECK(Sdp::parse(b.last("INVITE")->body)->direction == Direction::SendOnly);
    CHECK(b.waitFor([&] { return !b.media.channels[0].active; }));
    CHECK(b.waitFor([&] { return b.count("ACK") == 3; }));

    CHECK(b.calls.hangup(*id));
    CHECK(b.waitFor([&] { return b.count("BYE") == 1; }));
    CHECK(b.call(*id)->state == ImsCallState::Ended);
    CHECK(b.waitFor([&] { return b.media.channels.empty() && b.media.ports.empty(); }));
    CHECK(!b.audio.back());
}

TEST(call_outgoing_rejected_goes_to_cs)
{
    Bench b;
    CHECK(b.start());
    b.script = [&](const SipMessage &m, const Reply &reply) {
        if (m.method() == "INVITE")
            reply(response(m, 403, "Forbidden"));
    };
    auto id = b.calls.dial("603123456");
    CHECK(id);
    CHECK(b.waitFor([&] { return b.call(*id) && b.call(*id)->state == ImsCallState::Ended; }));
    CHECK(b.call(*id)->retryOnCs && b.call(*id)->endStatus == 403);
    CHECK(b.last("INVITE")->requestUri().find(";phone-context=ims.mnc002.mcc230.3gppnetwork.org@") !=
          std::string::npos);
    CHECK(b.waitFor([&] { return b.media.ports.empty(); }));
}

TEST(call_outgoing_cancelled)
{
    Bench b;
    CHECK(b.start());
    std::optional<SipMessage> invite;
    Reply inviteReply;
    b.script = [&](const SipMessage &m, const Reply &reply) {
        if (m.method() == "INVITE") {
            invite = m;
            inviteReply = reply;
            SipMessage r = response(m, 180, "Ringing");
            r.add("Contact", kFarContact);
            reply(r);
        } else if (m.method() == "CANCEL") {
            reply(response(m, 200, "OK"));
            inviteReply(response(*invite, 487, "Request Terminated"));
        }
    };
    auto id = b.calls.dial("+420603123456");
    CHECK(b.waitFor([&] { return b.call(*id) && b.call(*id)->state == ImsCallState::Alerting; }));
    CHECK(b.calls.hangup(*id));
    CHECK(b.waitFor([&] { return b.count("CANCEL") == 1; }));
    const SipMessage *c = b.last("CANCEL");
    CHECK(c->header("CSeq") == "1 CANCEL" && c->branch() == invite->branch());
    // The INVITE's 487 is ACKed by the transaction layer
    CHECK(b.waitFor([&] { return b.count("ACK") == 1; }));
    CHECK(b.call(*id)->state == ImsCallState::Ended && !b.call(*id)->retryOnCs);
}

TEST(call_incoming_with_preconditions)
{
    Bench b;
    CHECK(b.start());
    std::optional<SipMessage> invite;
    b.script = [&](const SipMessage &m, const Reply &reply) {
        (void)reply;
        if (m.isRequest())
            return;
        auto cseq = m.cseq();
        if (cseq->second == "INVITE" && m.status() == 183) {
            SipMessage prack = farRequest(b.pcscf, "PRACK", *invite, 2, false);
            prack.set("To", m.header("To").value_or(""));
            prack.add("RAck", m.header("RSeq").value_or("") + " 1 INVITE");
            b.pcscf.sendToUe(prack);
        } else if (cseq->second == "PRACK" && m.status() == 200) {
            // The network's resources are there
            SipMessage update = farRequest(b.pcscf, "UPDATE", *invite, 3, false);
            update.set("To", b.last("INVITE")->header("To").value_or(""));
            for (auto it = b.got.rbegin(); it != b.got.rend(); ++it)
                if (!it->isRequest() && it->status() == 183)
                    update.set("To", it->header("To").value_or(""));
            Sdp sdp = *Sdp::parse(kOffer);
            sdp.version++;
            sdp.qos.localCurrent = true;
            withSdp(update, sdp.str());
            b.pcscf.sendToUe(update);
        } else if (cseq->second == "INVITE" && m.status() == 200) {
            SipMessage ack = farRequest(b.pcscf, "ACK", *invite, 1, false);
            ack.set("To", m.header("To").value_or(""));
            b.pcscf.sendToUe(ack);
        }
    };

    invite = SipMessage::request("INVITE", "sip:ue@127.0.0.1");
    invite->add("Via", b.pcscf.via());
    invite->add("Max-Forwards", "68");
    invite->add("Record-Route", kRecordRoute);
    invite->add("From", "<sip:+420777123456@ims.example>;tag=caller1");
    invite->add("To", "<sip:+420600000001@ims.mnc002.mcc230.3gppnetwork.org>");
    invite->add("Call-ID", "mt-call-1");
    invite->add("CSeq", "1 INVITE");
    invite->add("Contact", kFarContact);
    invite->add("P-Asserted-Identity", "<sip:+420777123456@ims.example;user=phone>, <tel:+420777123456>");
    invite->add("Supported", "100rel, precondition, timer");
    invite->add("Session-Expires", "1800");
    withSdp(*invite, kOffer);
    b.pcscf.sendToUe(*invite);

    // 100, then the reliable 183 with our answer asking for confirmation
    CHECK(b.waitFor([&] { return b.count("INVITE", 183) >= 1; }));
    std::optional<SipMessage> progress;
    for (const auto &m : b.got)
        if (!m.isRequest() && m.status() == 183)
            progress = m;
    CHECK(progress->hasOption("Require", "100rel") && progress->header("RSeq") == "1");
    CHECK(progress->header("Record-Route") == kRecordRoute);
    auto answer = Sdp::parse(progress->body);
    CHECK(answer && answer->codecs[0].payloadType == 104 && answer->qos.present && !answer->qos.localCurrent &&
          answer->qos.confirmRemote);
    // Not shown yet, no audio before the answer
    CHECK(b.calls.calls().empty() && b.media.opened == 0);

    // PRACK and the network's UPDATE done; the phone rings once our bearer is up
    CHECK(b.waitFor([&] { return b.count("UPDATE", 200) == 1; }));
    CHECK(b.count("INVITE", 180) == 0);
    b.media.bearer(true);
    // Our resources: an UPDATE of our own, then 180
    CHECK(b.waitFor([&] { return b.count("INVITE", 180) == 1; }));
    CHECK(b.calls.calls().size() == 1);
    ImsCallInfo info = b.calls.calls()[0];
    CHECK(info.incoming && info.state == ImsCallState::Incoming && info.number == "+420777123456");

    CHECK(b.calls.answer(info.id));
    CHECK(b.waitFor([&] { return b.count("INVITE", 200) >= 1 && b.media.opened == 1; }));
    std::optional<SipMessage> ok;
    for (const auto &m : b.got)
        if (!m.isRequest() && m.status() == 200 && m.cseq()->second == "INVITE")
            ok = m;
    // The answer went in the 183: none in the 200
    CHECK(ok->body.empty() && ok->header("Session-Expires") == "1800;refresher=uac");
    CHECK(b.media.channels[0].codec.payloadType == 104 && b.media.channels[0].remotePort == 41000 &&
          b.media.channels[0].active);
    // The ACK stops the 2xx retransmissions
    size_t oks = b.count("INVITE", 200);
    b.loop.runUntil([] { return false; }, std::chrono::milliseconds(150));
    CHECK(b.count("INVITE", 200) == oks);

    // The far end hangs up
    SipMessage bye = farRequest(b.pcscf, "BYE", *invite, 4, false);
    bye.set("To", ok->header("To").value_or(""));
    b.pcscf.sendToUe(bye);
    CHECK(b.waitFor([&] { return b.count("BYE", 200) == 1; }));
    CHECK(b.call(info.id)->state == ImsCallState::Ended);
    CHECK(b.waitFor([&] { return b.media.channels.empty(); }));
}

TEST(call_incoming_cancelled_and_reinvited)
{
    Bench b;
    CHECK(b.start());
    // Without preconditions: 180 at once, then the caller gives up
    auto invite = SipMessage::request("INVITE", "sip:ue@127.0.0.1");
    invite.add("Via", b.pcscf.via());
    invite.add("From", "<tel:+420777123456>;tag=caller2");
    invite.add("To", "<sip:+420600000001@ims.mnc002.mcc230.3gppnetwork.org>");
    invite.add("Call-ID", "mt-call-2");
    invite.add("CSeq", "1 INVITE");
    invite.add("Contact", kFarContact);
    Sdp plain = *Sdp::parse(kOffer);
    plain.qos.present = false;
    withSdp(invite, plain.str());
    b.pcscf.sendToUe(invite);
    CHECK(b.waitFor([&] { return b.count("INVITE", 180) == 1; }));
    CHECK(b.calls.calls().size() == 1 && b.calls.calls()[0].number == "+420777123456");
    SipMessage cancel = SipMessage::request("CANCEL", "sip:ue@127.0.0.1");
    cancel.add("Via", invite.header("Via").value_or(""));
    for (const char *h : {"From", "To", "Call-ID"})
        cancel.add(h, invite.header(h).value_or(""));
    cancel.add("CSeq", "1 CANCEL");
    b.pcscf.sendToUe(cancel);
    CHECK(b.waitFor([&] { return b.count("INVITE", 487) == 1 && b.count("CANCEL", 200) == 1; }));
    CHECK(b.calls.calls().empty());

    // A second call, answered, then held by the far end
    std::string toTag;
    b.script = [&](const SipMessage &m, const Reply &) {
        if (!m.isRequest() && m.status() == 200 && m.cseq()->second == "INVITE")
            toTag = m.header("To").value_or("");
    };
    invite.set("Call-ID", "mt-call-3");
    invite.set("Via", b.pcscf.via());
    b.pcscf.sendToUe(invite);
    CHECK(b.waitFor([&] { return b.calls.calls().size() == 1; }));
    int id = b.calls.calls()[0].id;
    CHECK(b.calls.answer(id));
    CHECK(b.waitFor([&] { return !toTag.empty() && b.media.opened == 1; }));
    SipMessage ack = SipMessage::request("ACK", "sip:ue@127.0.0.1");
    ack.add("Via", b.pcscf.via());
    ack.add("From", invite.header("From").value_or(""));
    ack.add("To", toTag);
    ack.add("Call-ID", "mt-call-3");
    ack.add("CSeq", "1 ACK");
    b.pcscf.sendToUe(ack);

    SipMessage hold = SipMessage::request("INVITE", "sip:ue@127.0.0.1");
    hold.add("Via", b.pcscf.via());
    hold.add("From", invite.header("From").value_or(""));
    hold.add("To", toTag);
    hold.add("Call-ID", "mt-call-3");
    hold.add("CSeq", "2 INVITE");
    hold.add("Contact", kFarContact);
    plain.version++;
    plain.direction = Direction::SendOnly;
    withSdp(hold, plain.str());
    b.pcscf.sendToUe(hold);
    CHECK(b.waitFor([&] { return b.call(id) && b.call(id)->remoteHeld && b.count("INVITE", 200) >= 2; }));
    std::optional<SipMessage> answer;
    for (const auto &m : b.got)
        if (!m.isRequest() && m.status() == 200 && m.cseq()->first == 2)
            answer = m;
    CHECK(answer && Sdp::parse(answer->body)->direction == Direction::RecvOnly);
    CHECK(b.waitFor([&] { return b.media.updates >= 1 && !b.media.channels.begin()->second.active; }));

    // Registration lost: the call ends here
    b.calls.detach();
    CHECK(b.call(id)->state == ImsCallState::Ended);
}
