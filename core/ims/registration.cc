// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/registration.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <random>

namespace shannond::ims {

namespace {

constexpr const char *kMmtel = "\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel\"";
constexpr const char *kAllow = "INVITE, ACK, CANCEL, BYE, UPDATE, PRACK, NOTIFY, OPTIONS, INFO, REFER";

std::optional<unsigned> number(std::string_view s)
{
    s = trim(s);
    unsigned v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::string join(const std::vector<std::string> &items)
{
    std::string out;
    for (const auto &i : items)
        out += (out.empty() ? "" : ", ") + i;
    return out;
}

// The value of attribute name inside the XML element that starts at pos
std::string xmlAttribute(const std::string &body, size_t pos, const std::string &name)
{
    size_t end = body.find('>', pos);
    if (end == std::string::npos)
        return {};
    std::string element = body.substr(pos, end - pos);
    size_t a = element.find(" " + name + "=\"");
    if (a == std::string::npos)
        return {};
    a += name.size() + 3;
    size_t b = element.find('"', a);
    return b == std::string::npos ? std::string() : element.substr(a, b - a);
}

// The stock stack's protected ports (UserAgentManager::AllocateUserIdentity):
// a counter that steps by 100 for every user agent it creates, wrapping at
// 10000 back to its start + 100; server port = counter, client port + 1
std::pair<uint16_t, uint16_t> allocatePorts(uint16_t base)
{
    static std::map<uint16_t, unsigned> counters;
    auto [it, fresh] = counters.try_emplace(base, base);
    unsigned next = it->second + 100;
    if (next == 10000 || next + 1 > 65535)
        next = base + 100u;
    it->second = next;
    return {static_cast<uint16_t>(next + 1), static_cast<uint16_t>(next)};
}

}  // namespace

const char *registrationStateName(RegistrationState state)
{
    switch (state) {
    case RegistrationState::Idle: return "idle";
    case RegistrationState::Registering: return "registering";
    case RegistrationState::Registered: return "registered";
    case RegistrationState::Deregistering: return "deregistering";
    }
    return "?";
}

ImsRegistration::ImsRegistration(EventLoop &loop, SipEndpoint &endpoint, ImsNetwork &network, AkaFn aka)
    : loop_(loop), endpoint_(endpoint), network_(network), aka_(std::move(aka)), refreshTimer_(loop)
{
}

ImsRegistration::~ImsRegistration()
{
    *alive_ = false;
    loop_.cancel(retryTimer_);
    if (!pendingBranch_.empty())
        endpoint_.forget(pendingBranch_);
    clearSecurity();
}

void ImsRegistration::log(const std::string &line)
{
    if (log_)
        log_("IMS registration: " + line);
}

void ImsRegistration::setState(RegistrationState state, const std::string &why)
{
    if (state == state_ && state != RegistrationState::Registering)
        return;
    state_ = state;
    log(std::string(registrationStateName(state)) + (why.empty() ? "" : " (" + why + ")"));
    if (changed_)
        changed_(state, why);
}

std::chrono::seconds ImsRegistration::backoff(unsigned failures) const
{
    // 24.229 5.1.1.9: W = min(max, base * 2^failures), a random wait in [W/2, W]
    double w = params_.profile.retryBase;
    for (unsigned i = 1; i < failures && w < params_.profile.retryMax; i++)
        w *= 2;
    w = std::min<double>(w, params_.profile.retryMax);
    static std::mt19937 engine(std::random_device{}());
    std::uniform_real_distribution<double> pick(w / 2, w);
    return std::chrono::seconds(std::max<long>(1, static_cast<long>(pick(engine))));
}

void ImsRegistration::start(RegistrationParams params)
{
    abort();
    params_ = std::move(params);
    failures_ = 0;
    pcscfIndex_ = 0;
    // A new user agent in the stock stack: new ports, kept for its life
    std::tie(portUc_, portUs_) = allocatePorts(params_.profile.portBase);
    setState(RegistrationState::Registering, "start");
    begin();
}

void ImsRegistration::begin()
{
    retryTimer_ = 0;
    if (params_.pcscfs.empty() || !params_.identity.valid()) {
        failed(params_.pcscfs.empty() ? "no P-CSCF" : "no identity", false);
        return;
    }
    info_ = {};
    info_.pcscf = params_.pcscfs[pcscfIndex_ % params_.pcscfs.size()];
    callId_ = randomHex(24);
    fromTag_ = randomHex(10);
    cseq_ = 0;
    challenge_.reset();
    res_.clear();
    auts_.reset();
    akaFailed_ = false;
    securityServer_.clear();
    chosen_.reset();
    if (!endpoint_.openPlain(params_.profile.localPort)) {
        failed("no socket on the IMS PDN", false);
        return;
    }
    if (params_.profile.ipsec && !reserveSpis()) {
        failed("no SPIs", false);
        return;
    }
    expires_ = params_.profile.regExpires;
    sendRegister(false, expires_);
}

bool ImsRegistration::reserveSpis()
{
    releaseReserved();
    // New SPIs for every Security-Client, as the stock stack picks them:
    // spi-c = rand() % 100000 + 100, spi-s = spi-c + 1
    static std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<uint32_t> pick(100, 100099);
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t spiC = pick(engine);
        if (network_.reserveSpis(info_.pcscf, spiC, spiC + 1)) {
            reservedUc_ = spiC;
            reservedUs_ = spiC + 1;
            return true;
        }
    }
    return false;
}

void ImsRegistration::releaseReserved()
{
    if (reservedUc_ || reservedUs_)
        network_.releaseSpis(info_.pcscf, reservedUc_, reservedUs_);
    reservedUc_ = reservedUs_ = 0;
}

std::string ImsRegistration::contactUri() const
{
    const std::string &impi = params_.identity.impi;
    std::string user = impi.substr(0, impi.find('@'));
    const std::string &host = endpoint_.localAddress();
    uint16_t port = params_.profile.ipsec ? portUs_ : endpoint_.port(Flow::Plain);
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    return "sip:" + user + "@" + h + ":" + std::to_string(port);
}

std::string ImsRegistration::contactHeader(unsigned expires) const
{
    std::string c = "<" + contactUri() + ">";
    if (expires == 0)
        c += ";expires=0";
    if (!params_.instance.empty())
        c += ";+sip.instance=\"" + params_.instance + "\"";
    c += std::string(";+g.3gpp.icsi-ref=") + kMmtel + ";audio";
    if (params_.profile.offers("lte", "smsip"))
        c += ";+g.3gpp.smsip";
    return c;
}

std::string ImsRegistration::authorization(const std::string &method, const std::string &uri)
{
    const auto &id = params_.identity;
    if (!challenge_)
        return initialAuthorization(id.impi, id.domain, uri);
    DigestCredentials c;
    c.username = id.impi;
    c.uri = uri;
    c.method = method;
    if (!auts_)
        c.password = res_;
    c.auts = auts_;
    return digestAuthorization(*challenge_, c, randomHex(16), ++nc_);
}

void ImsRegistration::sendRegister(bool protectedFlow, unsigned expires)
{
    const auto &id = params_.identity;
    const auto &profile = params_.profile;
    const std::string uri = "sip:" + id.domain;
    SipMessage r = SipMessage::request("REGISTER", uri);
    r.add("Max-Forwards", "70");
    r.add("From", "<" + id.impu() + ">;tag=" + fromTag_);
    r.add("To", "<" + id.impu() + ">");
    r.add("Call-ID", callId_);
    r.add("CSeq", std::to_string(++cseq_) + " REGISTER");
    r.add("Contact", contactHeader(expires));
    r.add("Expires", std::to_string(expires));
    r.add("Supported", profile.ipsec ? "path, sec-agree" : "path");
    if (profile.ipsec) {
        r.add("Require", "sec-agree");
        r.add("Proxy-Require", "sec-agree");
    }
    r.add("Allow", kAllow);
    r.add("Authorization", authorization("REGISTER", uri));
    // Security-Client in every REGISTER (33.203 7.2, 24.229 5.1.1.2): a new
    // offer while SPIs are reserved (initial, refresh), else the one the
    // SAs in use came from, repeated unchanged
    const uint32_t offerUc = reservedUc_ ? reservedUc_ : (sas_ ? sas_->spiUc : 0);
    const uint32_t offerUs = reservedUc_ ? reservedUs_ : (sas_ ? sas_->spiUs : 0);
    if (profile.ipsec && offerUc)
        r.add("Security-Client", [&] {
            std::vector<std::string> offers;
            for (const auto &m : clientOffers(profile.algorithms, offerUc, offerUs, portUc_, portUs_))
                offers.push_back(m.str());
            return join(offers);
        }());
    if (protectedFlow && !securityServer_.empty())
        r.add("Security-Verify", join(securityServer_));
    if (protectedFlow && params_.accessNetworkInfo) {
        std::string pani = params_.accessNetworkInfo();
        if (!pani.empty())
            r.add("P-Access-Network-Info", pani);
    }
    if (!params_.userAgent.empty())
        r.add("User-Agent", params_.userAgent);

    Route route;
    route.flow = protectedFlow ? Flow::Client : Flow::Plain;
    route.to.ip = info_.pcscf;
    route.to.port = protectedFlow && sas_ ? sas_->portPs : profile.pcscfPort;
    auto alive = alive_;
    pendingBranch_ = endpoint_.send(std::move(r), route, [this, alive, protectedFlow, expires](const SipMessage *m) {
        if (!*alive)
            return;
        registerAnswered(m, protectedFlow, expires);
    });
}

void ImsRegistration::registerAnswered(const SipMessage *response, bool protectedFlow, unsigned expires)
{
    if (response && response->status() < 200)
        return;
    pendingBranch_.clear();
    if (expires == 0) {  // deregistration: whatever the answer, it is over
        endpoint_.closeProtected();
        clearSecurity();
        setState(RegistrationState::Idle, response ? "deregistered" : "deregistered without an answer");
        if (auto done = std::move(stopped_))
            done();
        return;
    }
    if (state_ == RegistrationState::Idle)
        return;
    if (!response) {
        failed("no answer from the P-CSCF", true);
        return;
    }
    const int status = response->status();
    if (status < 300) {
        registered(*response);
        return;
    }
    if (status == 401) {
        challenged(*response, protectedFlow);
        return;
    }
    if (status == 423) {  // Interval Too Brief
        auto min = response->header("Min-Expires");
        auto v = min ? number(*min) : std::nullopt;
        if (v && *v > expires_) {
            expires_ = *v;
            sendRegister(protectedFlow, expires_);
            return;
        }
    }
    std::optional<std::chrono::seconds> retryAfter;
    if (auto ra = response->header("Retry-After"))
        if (auto v = number(ra->substr(0, ra->find(';'))))
            retryAfter = std::chrono::seconds(*v);
    std::string why = std::to_string(status) + " " + response->reason();
    if (auto warning = response->header("Warning"))
        why += " (" + *warning + ")";
    failed(why, true, retryAfter);
}

void ImsRegistration::challenged(const SipMessage &response, bool protectedFlow)
{
    std::optional<DigestChallenge> challenge;
    for (const auto &value : response.headers("WWW-Authenticate"))
        if (auto c = DigestChallenge::parse(value); c && c->isAka())
            challenge = c;
    if (!challenge) {
        failed("401 without an AKA challenge", true);
        return;
    }
    if (auts_ && challenge_ && challenge->nonce == challenge_->nonce) {
        failed("the network did not take the resynchronisation", true);
        return;
    }
    challenge_ = challenge;
    nc_ = 0;
    if (params_.profile.ipsec) {
        securityServer_ = response.headers("Security-Server");
        chosen_ = chooseServer(securityServer_, params_.profile.algorithms);
        if (!chosen_) {
            failed("no usable Security-Server", true);
            return;
        }
        if (!reservedUc_ && !reserveSpis()) {  // a challenge to a request without an offer
            failed("no SPIs", false);
            return;
        }
    }
    auto vector = AkaVector::fromNonce(challenge->nonce);
    if (!vector) {
        failed("unreadable AKA nonce", true);
        return;
    }
    (void)protectedFlow;
    auto alive = alive_;
    const unsigned expires = expires_;
    aka_(vector->rand, vector->autn, [this, alive, expires](AkaAnswer answer) {
        if (!*alive)
            return;
        akaAnswered(std::move(answer), expires);
    });
}

void ImsRegistration::akaAnswered(AkaAnswer answer, unsigned expires)
{
    if (state_ == RegistrationState::Idle)
        return;
    switch (answer.kind) {
    case AkaAnswer::Kind::Failed:
        // The network failed the SIM's check (or the SIM did not answer):
        // nothing to send it
        failed("the SIM refused the network's challenge", true);
        return;
    case AkaAnswer::Kind::Resync:
        log("SIM asks to resynchronise");
        auts_ = answer.auts;
        res_.clear();
        sendRegister(endpoint_.isProtected(), expires);
        return;
    case AkaAnswer::Kind::Ok:
        break;
    }
    auts_.reset();
    res_ = answer.res;
    if (!params_.profile.ipsec) {
        sendRegister(false, expires);
        return;
    }
    if (sas_) {
        // A challenged refresh: the stock stack finds an SA set on the same
        // client port ("Vaild SA already exsits") and goes on with it; the
        // SPIs it offered are not used
        log("challenged again: keeping the security associations");
        releaseReserved();
        sendRegister(true, expires);
        return;
    }
    SaSet s;
    s.ue = endpoint_.localAddress();
    s.pcscf = info_.pcscf;
    s.spiUc = reservedUc_;
    s.spiUs = reservedUs_;
    s.portUc = portUc_;
    s.portUs = portUs_;
    s.spiPc = chosen_->spiC;
    s.spiPs = chosen_->spiS;
    s.portPc = chosen_->portC;
    s.portPs = chosen_->portS;
    s.alg = chosen_->alg;
    s.ealg = chosen_->ealg;
    s.integrityKey = integrityKey(s.alg, answer.ik);
    s.encryptionKey = encryptionKey(s.ealg, answer.ck);
    if (s.integrityKey.empty() || (s.ealg != "null" && s.encryptionKey.empty())) {
        failed("bad keys from the SIM (IK " + std::to_string(answer.ik.size()) + ", CK " +
                   std::to_string(answer.ck.size()) + " bytes; " + s.alg + "/" + s.ealg + ")",
               true);
        return;
    }
    if (!network_.installSas(s)) {
        failed("could not set up the security associations", false);
        return;
    }
    reservedUc_ = reservedUs_ = 0;  // owned by the SAs now
    if (!endpoint_.openProtected(s)) {
        network_.removeSas(s);
        failed("no protected sockets", false);
        return;
    }
    sas_ = s;
    sendRegister(true, expires);
}

void ImsRegistration::registered(const SipMessage &response)
{
    info_.serviceRoute = response.headers("Service-Route");
    info_.associatedUris = response.headers("P-Associated-URI");
    info_.defaultImpu = params_.identity.impu();
    if (!info_.associatedUris.empty())
        if (auto first = NameAddr::parse(info_.associatedUris.front()))
            info_.defaultImpu = first->uri;
    {
        std::string schemes;  // no identities in the log
        for (const auto &a : info_.associatedUris)
            if (auto n = NameAddr::parse(a))
                schemes += (schemes.empty() ? "" : ", ") + n->uri.substr(0, n->uri.find(':'));
        log("associated identities: " + (schemes.empty() ? std::string("none") : schemes));
    }
    unsigned expires = expires_;
    if (auto e = response.header("Expires"))
        if (auto v = number(*e))
            expires = *v;
    const std::string mine = contactUri();
    for (const auto &c : response.headers("Contact")) {
        auto n = NameAddr::parse(c);
        if (!n)
            continue;
        auto ours = Uri::parse(mine), theirs = Uri::parse(n->uri);
        if (ours && theirs && ours->host == theirs->host && ours->port == theirs->port)
            if (auto e = n->params.get("expires"))
                if (auto v = number(*e))
                    expires = *v;
    }
    if (expires == 0) {
        failed("the network registered us for no time", true);
        return;
    }
    info_.expires = expires;
    info_.contact = contactHeader(expires);
    failures_ = 0;
    releaseReserved();

    // 24.229 5.1.1.4.1: 600 s before expiry, or half way for short ones
    const unsigned refreshIn = expires > 1200 ? expires - 600 : expires / 2;
    auto alive = alive_;
    refreshTimer_.start(std::chrono::seconds(refreshIn), [this, alive]() {
        if (*alive)
            refresh();
    });
    const bool first = state_ != RegistrationState::Registered;
    setState(RegistrationState::Registered, "for " + std::to_string(expires) + " s");
    if (first && params_.profile.subscribeRegEvent)
        subscribe();
}

void ImsRegistration::refresh()
{
    if (state_ != RegistrationState::Registered)
        return;
    log("refresh");
    if (params_.profile.ipsec && !reserveSpis())
        log("no SPIs for a new SA set; refreshing without an offer");
    sendRegister(true, expires_);
}

void ImsRegistration::failed(const std::string &why, bool nextPcscf, std::optional<std::chrono::seconds> retryAfter)
{
    pendingBranch_.clear();
    refreshTimer_.cancel();
    endpoint_.closeProtected();
    clearSecurity();
    if (nextPcscf)
        pcscfIndex_++;
    failures_++;
    auto delay = retryAfter.value_or(backoff(failures_));
    setState(RegistrationState::Registering, why + "; again in " + std::to_string(delay.count()) + " s");
    loop_.cancel(retryTimer_);
    auto alive = alive_;
    retryTimer_ = loop_.after(delay, [this, alive]() {
        if (*alive)
            begin();
    });
}

void ImsRegistration::clearSecurity()
{
    releaseReserved();
    if (sas_)
        network_.removeSas(*sas_);
    sas_.reset();
    challenge_.reset();
    res_.clear();
    securityServer_.clear();
    chosen_.reset();
    auts_.reset();
}

void ImsRegistration::stop(std::function<void()> done)
{
    loop_.cancel(retryTimer_);
    retryTimer_ = 0;
    refreshTimer_.cancel();
    if (state_ == RegistrationState::Registered && sas_) {
        stopped_ = std::move(done);
        releaseReserved();
        setState(RegistrationState::Deregistering, "asked to");
        if (!pendingBranch_.empty())
            endpoint_.forget(pendingBranch_);
        sendRegister(true, 0);
        return;
    }
    abort();
    if (done)
        done();
}

void ImsRegistration::abort()
{
    loop_.cancel(retryTimer_);
    retryTimer_ = 0;
    refreshTimer_.cancel();
    if (!pendingBranch_.empty())
        endpoint_.forget(pendingBranch_);
    pendingBranch_.clear();
    endpoint_.closeProtected();
    clearSecurity();
    subCallId_.clear();
    if (state_ != RegistrationState::Idle)
        setState(RegistrationState::Idle, "stopped");
}

Route ImsRegistration::route() const
{
    Route r;
    r.flow = sas_ ? Flow::Client : Flow::Plain;
    r.to.ip = info_.pcscf;
    r.to.port = sas_ ? sas_->portPs : params_.profile.pcscfPort;
    return r;
}

void ImsRegistration::decorate(SipMessage &request, bool withRoute) const
{
    // 24.229 5.1.2A.1.1: the preloaded route is the P-CSCF (its protected
    // server port), then the Service-Route the registration gave
    if (sas_ && withRoute) {
        Uri pcscf;
        pcscf.scheme = "sip";
        pcscf.host = info_.pcscf;
        pcscf.port = sas_->portPs;
        pcscf.params.set("lr");
        request.add("Route", "<" + pcscf.str() + ">");
    }
    if (withRoute)
        for (const auto &r : info_.serviceRoute)
            request.add("Route", r);
    if (params_.accessNetworkInfo) {
        std::string pani = params_.accessNetworkInfo();
        if (!pani.empty())
            request.add("P-Access-Network-Info", pani);
    }
    if (!params_.userAgent.empty())
        request.add("User-Agent", params_.userAgent);
}

void ImsRegistration::subscribe()
{
    const std::string &impu = info_.defaultImpu;
    subCallId_ = randomHex(24);
    subFromTag_ = randomHex(10);
    subToTag_.clear();
    subCseq_ = 0;
    SipMessage s = SipMessage::request("SUBSCRIBE", impu);
    s.add("Max-Forwards", "70");
    decorate(s);
    s.add("From", "<" + impu + ">;tag=" + subFromTag_);
    s.add("To", "<" + impu + ">");
    s.add("Call-ID", subCallId_);
    s.add("CSeq", std::to_string(++subCseq_) + " SUBSCRIBE");
    s.add("Contact", "<" + contactUri() + ">");
    s.add("Event", "reg");
    s.add("Accept", "application/reginfo+xml");
    s.add("Expires", std::to_string(expires_));
    s.add("P-Preferred-Identity", "<" + impu + ">");
    auto alive = alive_;
    endpoint_.send(std::move(s), route(), [this, alive](const SipMessage *m) {
        if (!*alive || (m && m->status() < 200))
            return;
        if (!m || m->status() >= 300) {
            std::string why = m ? ": " + std::to_string(m->status()) + " " + m->reason() : "";
            if (m)
                if (auto warning = m->header("Warning"))
                    why += " (" + *warning + ")";
            log("reg event subscription failed" + why);
            return;
        }
        if (auto to = m->header("To"))
            if (auto n = NameAddr::parse(*to))
                subToTag_ = n->params.get("tag").value_or("");
    });
}

bool ImsRegistration::handleRequest(const SipMessage &request, const Source &source)
{
    if (request.method() != "NOTIFY" || subCallId_.empty() || request.callId() != subCallId_)
        return false;
    endpoint_.respond(source, request, request.makeResponse(200, "OK"));
    // A registration of our IMPU that the network ended (24.229 5.1.1.6):
    // register again from the start
    const std::string &body = request.body;
    for (size_t pos = body.find("<registration"); pos != std::string::npos;
         pos = body.find("<registration", pos + 1)) {
        const std::string aor = xmlAttribute(body, pos, "aor");
        if ((aor == params_.identity.impu() || aor == info_.defaultImpu) &&
            xmlAttribute(body, pos, "state") == "terminated" &&
            state_ == RegistrationState::Registered) {
            log("the network ended the registration");
            subCallId_.clear();
            endpoint_.closeProtected();
            clearSecurity();
            failures_ = 0;
            setState(RegistrationState::Registering, "ended by the network");
            auto alive = alive_;
            retryTimer_ = loop_.after(std::chrono::seconds(1), [this, alive]() {
                if (*alive)
                    begin();
            });
            break;
        }
    }
    return true;
}

}  // namespace shannond::ims
