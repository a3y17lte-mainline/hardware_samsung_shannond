// SPDX-License-Identifier: GPL-2.0-or-later
#include "sim/fake_pcscf.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/ims/digest.h"
#include "core/ims/secagree.h"
#include "sim/fake_modem.h"

namespace shannond::sim {

using namespace ims;

namespace {

constexpr const char *kDefaultImpu = "sip:+420600000001@ims.mnc002.mcc230.3gppnetwork.org";

bool sockaddrFor(const std::string &ip, uint16_t port, sockaddr_storage &ss, socklen_t &length)
{
    ss = {};
    auto *s6 = reinterpret_cast<sockaddr_in6 *>(&ss);
    auto *s4 = reinterpret_cast<sockaddr_in *>(&ss);
    if (inet_pton(AF_INET6, ip.c_str(), &s6->sin6_addr) == 1) {
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        length = sizeof(*s6);
        return true;
    }
    s4->sin_family = AF_INET;
    s4->sin_port = htons(port);
    length = sizeof(*s4);
    return inet_pton(AF_INET, ip.c_str(), &s4->sin_addr) == 1;
}

Endpoint endpointOf(const sockaddr_storage &ss)
{
    char ip[INET6_ADDRSTRLEN];
    if (ss.ss_family == AF_INET6) {
        auto *s6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
        return Endpoint{inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof(ip)), ntohs(s6->sin6_port)};
    }
    auto *s4 = reinterpret_cast<const sockaddr_in *>(&ss);
    return Endpoint{inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip)), ntohs(s4->sin_port)};
}

int bindSocket(const std::string &ip, int type, uint16_t port)
{
    sockaddr_storage ss;
    socklen_t length;
    if (!sockaddrFor(ip, port, ss, length))
        return -1;
    int fd = socket(ss.ss_family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, reinterpret_cast<sockaddr *>(&ss), length) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// name="value" pairs of a Digest Authorization header
std::map<std::string, std::string> digestFields(const std::string &value)
{
    std::map<std::string, std::string> out;
    if (value.size() < 7)
        return out;
    for (const auto &item : splitList(std::string_view(value).substr(7))) {
        size_t eq = item.find('=');
        if (eq != std::string::npos)
            out[std::string(trim(std::string_view(item).substr(0, eq)))] =
                unquote(std::string_view(item).substr(eq + 1));
    }
    return out;
}

void increment(Sqn &sqn)
{
    for (int i = 5; i >= 0 && ++sqn[i] == 0; i--) {
    }
}

}  // namespace

int FakeImsNetwork::bound(int type, uint16_t port)
{
    return bindSocket(address_, type, port);
}

int FakeImsNetwork::udpSocket(uint16_t port)
{
    return bound(SOCK_DGRAM, port);
}

int FakeImsNetwork::tcpSocket(uint16_t port)
{
    return bound(SOCK_STREAM, port);
}

bool FakeImsNetwork::reserveSpis(const std::string &, uint32_t spiUc, uint32_t spiUs)
{
    for (uint32_t spi : {spiUc, spiUs}) {
        if (std::find(reserved.begin(), reserved.end(), spi) != reserved.end())
            return false;
        for (const auto &s : installed)
            if (s.spiUc == spi || s.spiUs == spi)
                return false;
    }
    reserved.push_back(spiUc);
    reserved.push_back(spiUs);
    return true;
}

void FakeImsNetwork::releaseSpis(const std::string &, uint32_t spiUc, uint32_t spiUs)
{
    std::erase(reserved, spiUc);
    std::erase(reserved, spiUs);
}

bool FakeImsNetwork::installSas(const SaSet &sas)
{
    std::erase(reserved, sas.spiUc);
    std::erase(reserved, sas.spiUs);
    installed.push_back(sas);
    return true;
}

bool FakeImsNetwork::protectSocket(int, Role, const SaSet &)
{
    protectedSockets++;
    return true;
}

void FakeImsNetwork::removeSas(const SaSet &sas)
{
    std::erase_if(installed, [&](const SaSet &s) { return s.spiUc == sas.spiUc; });
    removed.push_back(sas);
}

FakePcscf::FakePcscf(EventLoop &loop, std::string address, uint16_t port)
    : milenage(FakeModem::testMilenage()), portC(port + 1), portS(port + 2), loop_(loop),
      address_(std::move(address)), port_(port)
{
    plain_ = open(SOCK_DGRAM, port_);
    int tcp = open(SOCK_STREAM, port_);
    (void)tcp;
}

FakePcscf::~FakePcscf()
{
    for (int fd : fds_) {
        loop_.unwatch(fd);
        close(fd);
    }
}

int FakePcscf::open(int type, uint16_t port)
{
    int fd = bindSocket(address_, type, port);
    if (fd < 0)
        return -1;
    fds_.push_back(fd);
    if (type == SOCK_STREAM) {
        listen(fd, 4);
        loop_.watch(fd, EPOLLIN, [this, fd](uint32_t) {
            int c = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (c < 0)
                return;
            fds_.push_back(c);
            auto buffer = std::make_shared<std::string>();
            loop_.watch(c, EPOLLIN, [this, c, buffer](uint32_t) {
                char buf[16384];
                ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) {
                    loop_.unwatch(c);
                    close(c);
                    std::erase(fds_, c);
                    return;
                }
                buffer->append(buf, n);
                for (;;) {
                    auto length = sipFrameLength(*buffer);
                    if (!length || !*length)
                        return;
                    auto m = SipMessage::parse(buffer->substr(0, *length));
                    buffer->erase(0, *length);
                    if (m && !silent) {
                        requests.push_back(*m);
                        handle(*m, c, Endpoint{});
                    }
                }
            });
        });
    } else {
        loop_.watch(fd, EPOLLIN, [this, fd](uint32_t) { read(fd); });
    }
    return fd;
}

bool FakePcscf::openProtected()
{
    if (protectedC_ < 0)
        protectedC_ = open(SOCK_DGRAM, portC);
    if (protectedS_ < 0) {
        protectedS_ = open(SOCK_DGRAM, portS);
        protectedTcp_ = open(SOCK_STREAM, portS);
    }
    return protectedC_ >= 0 && protectedS_ >= 0;
}

void FakePcscf::protectWith(const SaSet &ue)
{
    if (!ipsec)
        return;
    // The UE's four SAs seen from this side: our server port talks to its
    // client port, our client port to its server port
    SaSet m;
    m.ue = ue.pcscf;
    m.pcscf = ue.ue;
    m.spiUc = ue.spiPs;  // in on our server port
    m.spiPs = ue.spiUc;  // out to the UE's client port
    m.spiUs = ue.spiPc;  // in on our client port
    m.spiPc = ue.spiUs;  // out to the UE's server port
    bool ok = ipsec->protectSocket(protectedS_, ImsNetwork::Role::Client, m) &&
              ipsec->protectSocket(protectedTcp_, ImsNetwork::Role::Client, m) &&
              ipsec->protectSocket(protectedC_, ImsNetwork::Role::Server, m);
    protectedWithIpsec = ok;
}

void FakePcscf::read(int fd)
{
    for (;;) {
        char buf[65536];
        sockaddr_storage sa{};
        socklen_t length = sizeof(sa);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&sa), &length);
        if (n < 0)
            return;
        auto m = SipMessage::parse(std::string(buf, n));
        if (!m || silent)
            continue;
        Endpoint from = endpointOf(sa);
        requests.push_back(*m);
        handle(*m, fd, from);
    }
}

void FakePcscf::sendTo(int fd, const Endpoint &to, const SipMessage &m)
{
    std::string data = m.serialize();
    if (to.ip.empty()) {  // a TCP connection
        ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        return;
    }
    sockaddr_storage sa;
    socklen_t length;
    if (sockaddrFor(to.ip, to.port, sa, length))
        sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&sa), length);
}

void FakePcscf::challenge(const SipMessage &m, int fd, const Endpoint &to, bool protectedPorts)
{
    (void)protectedPorts;
    Block rand;
    for (auto &b : rand)
        b = static_cast<uint8_t>(std::rand());
    increment(sqn);
    Challenge c;
    c.vector = makeVector(milenage, rand, sqn, {0x80, 0x00});
    Bytes nonce(c.vector.rand.begin(), c.vector.rand.end());
    nonce.insert(nonce.end(), c.vector.autn.begin(), c.vector.autn.end());
    c.nonce = base64Encode(nonce);
    nonces_[c.nonce] = c;
    challenges++;

    if (!openProtected())
        return;
    // The UE's offer (any entry: they share SPIs and ports)
    if (auto client = m.header("Security-Client")) {
        if (auto offer = SecurityMechanism::parse(*client)) {
            lastOfferSpiC = offer->spiC;
            SaSet ue;
            ue.ue = to.ip.empty() ? ueServer.ip : to.ip;
            ue.pcscf = address_;
            ue.spiUc = offer->spiC;
            ue.spiUs = offer->spiS;
            ue.spiPc = spiC;
            ue.spiPs = spiS;
            protectWith(ue);
        }
    }
    SipMessage r = m.makeResponse(401, "Unauthorized");
    r.add("WWW-Authenticate", "Digest realm=\"ims.mnc002.mcc230.3gppnetwork.org\",nonce=\"" + c.nonce +
                                  "\",algorithm=AKAv1-MD5,qop=\"auth\"");
    r.add("Security-Server", "ipsec-3gpp;q=0.1;alg=" + alg + ";ealg=null;spi-c=" + std::to_string(spiC) +
                                 ";spi-s=" + std::to_string(spiS) + ";port-c=" + std::to_string(portC) +
                                 ";port-s=" + std::to_string(portS));
    sendTo(fd, to, r);
}

void FakePcscf::handle(const SipMessage &m, int fd, const Endpoint &from)
{
    // Answers to our own requests at the client port are lost, as on the
    // real network (they belong at the Via's port-s)
    if (!m.isRequest() && fd == protectedC_) {
        misdirected++;
        return;
    }
    auto via = Via::parse(m.header("Via").value_or(""));
    // UDP answers go to the Via's sent-by (for protected requests, the
    // UE's protected server port); TCP ones back over the connection
    Endpoint to = from;
    if (!from.ip.empty() && via)
        to = Endpoint{via->host, via->port ? via->port : uint16_t(5060)};
    const bool onProtected = fd == protectedC_ || fd == protectedS_ || from.ip.empty();
    if (onProtected && via && m.isRequest())
        ueServer = Endpoint{via->host, via->port};
    // A P-CSCF answers protected UDP requests from its client port to the
    // UE's server port (the Via), the SA pair of its own requests
    if (fd == protectedS_ && protectedC_ >= 0)
        fd = protectedC_;

    if (m.method() == "REGISTER") {
        if (failStatus) {
            sendTo(fd, to, m.makeResponse(failStatus, "Failed"));
            return;
        }
        unsigned expires = 0;
        if (auto e = m.header("Expires"))
            expires = std::stoul(*e);
        if (expires && expires < minExpires) {
            auto r = m.makeResponse(423, "Interval Too Brief");
            r.add("Min-Expires", std::to_string(minExpires));
            sendTo(fd, to, r);
            return;
        }
        if (auto from = m.header("From"))
            if (auto n = NameAddr::parse(*from))
                impu_ = n->uri;
        auto fields = digestFields(m.header("Authorization").value_or(""));
        auto known = nonces_.find(fields["nonce"]);
        if (fields.count("auts") && known != nonces_.end()) {
            auto auts = base64Decode(fields["auts"]);
            auto ms = auts ? sqnFromAuts(milenage, known->second.vector.rand, *auts) : std::nullopt;
            if (!ms) {
                sendTo(fd, to, m.makeResponse(403, "Forbidden"));
                return;
            }
            resyncs++;
            sqn = *ms;
            challenge(m, fd, to, onProtected);
            return;
        }
        const bool refresh = onProtected && known != nonces_.end() && registered;
        if (known == nonces_.end() || fields["response"].empty() || (refresh && challengeRefresh && expires)) {
            if (refresh && challengeRefresh)
                challengeRefresh = false;
            challenge(m, fd, to, onProtected);
            return;
        }
        // The digest with XRES as the password
        const auto &c = known->second;
        std::string password(c.vector.xres.begin(), c.vector.xres.end());
        std::string ha1 = md5Hex(fields["username"] + ":" + fields["realm"] + ":" + password);
        std::string ha2 = md5Hex("REGISTER:" + fields["uri"]);
        std::string expected = md5Hex(ha1 + ":" + c.nonce + ":" + fields["nc"] + ":" + fields["cnonce"] +
                                      ":auth:" + ha2);
        if (expected != fields["response"] || !onProtected) {
            sendTo(fd, to, m.makeResponse(403, "Forbidden"));
            return;
        }
        // 33.203: the protected REGISTER repeats the Security-Client the
        // SAs were made from
        auto client = SecurityMechanism::parse(m.header("Security-Client").value_or(""));
        if (!client || (lastOfferSpiC && client->spiC != lastOfferSpiC && !refresh)) {
            SipMessage bad = m.makeResponse(400, "Bad Request");
            bad.add("Warning", "399 pcscf \"Security-Client missing or changed\"");
            sendTo(fd, to, bad);
            return;
        }
        lastSecurityVerify = m.header("Security-Verify").value_or("");
        // Over TCP the stack reads the list joined
        if (lastSecurityVerify.empty() == false && m.headers("Security-Verify").size() > 1) {
            lastSecurityVerify.clear();
            for (const auto &v : m.headers("Security-Verify"))
                lastSecurityVerify += (lastSecurityVerify.empty() ? "" : ", ") + v;
        }
        lastContact = m.header("Contact").value_or("");
        SipMessage r = m.makeResponse(200, "OK");
        r.set("To", m.header("To").value_or("") + ";tag=reg1");
        if (expires == 0) {
            deregistered++;
        } else {
            registered++;
            auto contact = NameAddr::parse(lastContact);
            if (contact)
                r.add("Contact", "<" + contact->uri + ">;expires=" + std::to_string(grantExpires));
            r.add("Service-Route", "<sip:orig@scscf.ims.example;lr>");
            r.add("P-Associated-URI", std::string("<") + kDefaultImpu + ">, <" + impu_ + ">, <tel:+420600000001>");
        }
        sendTo(fd, to, r);
        return;
    }
    if (m.method() == "SUBSCRIBE") {
        // The IMSI-derived identity is barred outside REGISTER
        if (m.header("From").value_or("").find("<" + impu_ + ">") == 0) {
            sendTo(fd, to, m.makeResponse(403, "Forbidden - barred identity"));
            return;
        }
        subCallId_ = m.callId().value_or("");
        if (auto f = NameAddr::parse(m.header("From").value_or("")))
            subFromTag_ = f->params.get("tag").value_or("");
        subToTag_ = "sub1";
        if (auto c = NameAddr::parse(m.header("Contact").value_or("")))
            subContact_ = c->uri;
        SipMessage r = m.makeResponse(200, "OK");
        r.set("To", m.header("To").value_or("") + ";tag=" + subToTag_);
        r.add("Expires", "3600");
        sendTo(fd, to, r);
        return;
    }
    // Calls and everything else: the test's far end
    if (farEnd && (!m.isRequest() || m.method() != "NOTIFY")) {
        farEnd(m, [this, fd, to](const SipMessage &reply) { sendTo(fd, to, reply); });
        return;
    }
    // Answers to our NOTIFYs need nothing
}

void FakePcscf::sendToUe(const SipMessage &request)
{
    if (protectedC_ >= 0)
        sendTo(protectedC_, ueServer, request);
}

std::string FakePcscf::via() const
{
    Via v;
    v.transport = "UDP";
    // As O2's P-CSCF: it sends from its client port but names its
    // protected server port, without rport; answers must come there
    v.host = address_;
    v.port = portS;
    v.params.set("branch", newBranch());
    return v.str();
}

void FakePcscf::terminateRegistration()
{
    if (protectedC_ < 0 || subCallId_.empty())
        return;
    SipMessage n = SipMessage::request("NOTIFY", subContact_);
    Via via;
    via.transport = "UDP";
    via.host = address_;
    via.port = portC;
    via.params.set("branch", newBranch());
    n.add("Via", via.str());
    n.add("From", "<" + std::string(kDefaultImpu) + ">;tag=" + subToTag_);
    n.add("To", "<" + std::string(kDefaultImpu) + ">;tag=" + subFromTag_);
    n.add("Call-ID", subCallId_);
    n.add("CSeq", std::to_string(++notifyCseq_) + " NOTIFY");
    n.add("Event", "reg");
    n.add("Subscription-State", "terminated;reason=deactivated");
    n.add("Content-Type", "application/reginfo+xml");
    n.body = "<?xml version=\"1.0\"?><reginfo xmlns=\"urn:ietf:params:xml:ns:reginfo\" version=\"1\" "
             "state=\"partial\"><registration aor=\"" +
             std::string(kDefaultImpu) + "\" id=\"r1\" state=\"terminated\"><contact id=\"c1\" state=\"terminated\" "
                     "event=\"deactivated\"><uri>" +
             subContact_ + "</uri></contact></registration></reginfo>";
    sendTo(protectedC_, ueServer, n);
}

}  // namespace shannond::sim
