// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/linux_network.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/xfrm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace shannond::ims {

namespace {

struct Address {
    int family = 0;
    xfrm_address_t xa{};
    sockaddr_storage ss{};
    socklen_t length = 0;
};

bool parseAddress(const std::string &ip, uint16_t port, Address &out)
{
    out = {};
    auto *s6 = reinterpret_cast<sockaddr_in6 *>(&out.ss);
    auto *s4 = reinterpret_cast<sockaddr_in *>(&out.ss);
    if (inet_pton(AF_INET6, ip.c_str(), &s6->sin6_addr) == 1) {
        out.family = AF_INET6;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        out.length = sizeof(*s6);
        std::memcpy(&out.xa.a6, &s6->sin6_addr, 16);
        return true;
    }
    if (inet_pton(AF_INET, ip.c_str(), &s4->sin_addr) == 1) {
        out.family = AF_INET;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        out.length = sizeof(*s4);
        std::memcpy(&out.xa.a4, &s4->sin_addr, 4);
        return true;
    }
    return false;
}

xfrm_lifetime_cfg infinite()
{
    xfrm_lifetime_cfg l{};
    l.soft_byte_limit = l.hard_byte_limit = XFRM_INF;
    l.soft_packet_limit = l.hard_packet_limit = XFRM_INF;
    return l;
}

class Message {
public:
    Message(uint16_t type, uint16_t flags, const void *body, size_t length)
    {
        std::memset(buffer_, 0, sizeof(buffer_));
        header()->nlmsg_type = type;
        header()->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
        header()->nlmsg_len = NLMSG_LENGTH(length);
        std::memcpy(NLMSG_DATA(header()), body, length);
    }

    void attr(uint16_t type, const void *data, size_t length)
    {
        auto *a = reinterpret_cast<nlattr *>(buffer_ + NLMSG_ALIGN(header()->nlmsg_len));
        a->nla_type = type;
        a->nla_len = NLA_HDRLEN + length;
        std::memcpy(reinterpret_cast<char *>(a) + NLA_HDRLEN, data, length);
        header()->nlmsg_len = NLMSG_ALIGN(header()->nlmsg_len) + NLA_ALIGN(a->nla_len);
    }

    nlmsghdr *header() { return reinterpret_cast<nlmsghdr *>(buffer_); }

private:
    alignas(nlmsghdr) char buffer_[1024];
};

struct Algorithm {
    const char *ours;
    const char *kernel;
};
constexpr Algorithm kIntegrity[] = {{"hmac-md5-96", "hmac(md5)"}, {"hmac-sha-1-96", "hmac(sha1)"}};
constexpr Algorithm kEncryption[] = {
    {"null", "ecb(cipher_null)"}, {"aes-cbc", "cbc(aes)"}, {"des-ede3-cbc", "cbc(des3_ede)"}};

const char *kernelName(const Algorithm *table, size_t n, const std::string &name)
{
    for (size_t i = 0; i < n; i++)
        if (name == table[i].ours)
            return table[i].kernel;
    return nullptr;
}

}  // namespace

LinuxImsNetwork::LinuxImsNetwork(std::string iface, std::string address)
    : iface_(std::move(iface)), address_(std::move(address))
{
}

LinuxImsNetwork::~LinuxImsNetwork()
{
    if (nl_ >= 0)
        close(nl_);
}

std::string LinuxImsNetwork::localAddress()
{
    if (!address_.empty())
        return address_;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) < 0)
        return {};
    std::string v4, v6;
    for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || iface_ != a->ifa_name)
            continue;
        char buf[INET6_ADDRSTRLEN];
        if (a->ifa_addr->sa_family == AF_INET6) {
            auto *s6 = reinterpret_cast<sockaddr_in6 *>(a->ifa_addr);
            if (IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr) || !v6.empty())
                continue;
            v6 = inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
        } else if (a->ifa_addr->sa_family == AF_INET && v4.empty()) {
            v4 = inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, buf, sizeof(buf));
        }
    }
    freeifaddrs(list);
    return v6.empty() ? v4 : v6;
}

int LinuxImsNetwork::socketFor(int type, uint16_t port)
{
    Address local;
    if (!parseAddress(localAddress(), port, local)) {
        error_ = "no address on the IMS PDN";
        return -1;
    }
    int fd = socket(local.family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error_ = std::string("socket: ") + strerror(errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (!iface_.empty() &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface_.c_str(), iface_.size()) < 0) {
        error_ = std::string("bind to ") + iface_ + ": " + strerror(errno);
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&local.ss), local.length) < 0) {
        error_ = std::string("bind: ") + strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

int LinuxImsNetwork::udpSocket(uint16_t port)
{
    return socketFor(SOCK_DGRAM, port);
}

int LinuxImsNetwork::tcpSocket(uint16_t port)
{
    return socketFor(SOCK_STREAM, port);
}

bool LinuxImsNetwork::xfrm(void *message, size_t length, void *reply, size_t replyLength)
{
    if (nl_ < 0) {
        nl_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_XFRM);
        if (nl_ < 0) {
            error_ = std::string("netlink: ") + strerror(errno);
            return false;
        }
    }
    auto *h = static_cast<nlmsghdr *>(message);
    h->nlmsg_seq = ++seq_;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (sendto(nl_, message, length, 0, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) < 0) {
        error_ = std::string("netlink send: ") + strerror(errno);
        return false;
    }
    alignas(nlmsghdr) char buf[4096];
    for (;;) {
        ssize_t n = recv(nl_, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            error_ = std::string("netlink recv: ") + strerror(errno);
            return false;
        }
        for (auto *r = reinterpret_cast<nlmsghdr *>(buf); NLMSG_OK(r, n); r = NLMSG_NEXT(r, n)) {
            if (r->nlmsg_seq != seq_)
                continue;
            if (r->nlmsg_type == NLMSG_ERROR) {
                auto *e = static_cast<nlmsgerr *>(NLMSG_DATA(r));
                if (e->error) {
                    error_ = std::string("xfrm: ") + strerror(-e->error);
                    return false;
                }
                return true;
            }
            if (reply && r->nlmsg_len >= NLMSG_LENGTH(replyLength))
                std::memcpy(reply, NLMSG_DATA(r), replyLength);
        }
    }
}

bool LinuxImsNetwork::reserveSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs)
{
    Address ue, peer;
    if (!parseAddress(localAddress(), 0, ue) || !parseAddress(pcscf, 0, peer) || ue.family != peer.family) {
        error_ = "bad addresses";
        return false;
    }
    for (uint32_t spi : {spiUc, spiUs}) {
        // ALLOCSPI over a range of one: exactly this SPI or nothing
        xfrm_userspi_info info{};
        info.info.id.daddr = ue.xa;
        info.info.id.proto = IPPROTO_ESP;
        info.info.saddr = peer.xa;
        info.info.family = ue.family;
        info.info.mode = XFRM_MODE_TRANSPORT;
        info.info.lft = infinite();
        info.min = info.max = spi;
        Message m(XFRM_MSG_ALLOCSPI, 0, &info, sizeof(info));
        xfrm_usersa_info sa{};
        if (!xfrm(m.header(), m.header()->nlmsg_len, &sa, sizeof(sa)) || ntohl(sa.id.spi) != spi) {
            if (spi == spiUs)
                releaseSpis(pcscf, spiUc, 0);
            return false;
        }
    }
    return true;
}

void LinuxImsNetwork::releaseSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs)
{
    (void)pcscf;
    Address ue;
    if (!parseAddress(localAddress(), 0, ue))
        return;
    for (uint32_t spi : {spiUc, spiUs}) {
        if (!spi)
            continue;
        xfrm_usersa_id id{};
        id.daddr = ue.xa;
        id.spi = htonl(spi);
        id.family = ue.family;
        id.proto = IPPROTO_ESP;
        Message m(XFRM_MSG_DELSA, 0, &id, sizeof(id));
        xfrm(m.header(), m.header()->nlmsg_len);
    }
}

bool LinuxImsNetwork::installSas(const SaSet &s)
{
    Address ue, peer;
    if (!parseAddress(s.ue, 0, ue) || !parseAddress(s.pcscf, 0, peer) || ue.family != peer.family) {
        error_ = "bad addresses";
        return false;
    }
    const char *auth = kernelName(kIntegrity, std::size(kIntegrity), s.alg);
    const char *crypt = kernelName(kEncryption, std::size(kEncryption), s.ealg);
    if (!auth || !crypt) {
        error_ = "unknown algorithm";
        return false;
    }

    struct One {
        bool inbound;
        uint32_t spi;
        uint16_t sport, dport;
    };
    const One sas[] = {
        {true, s.spiUc, s.portPs, s.portUc},   // replies to our requests
        {true, s.spiUs, s.portPc, s.portUs},   // the P-CSCF's requests
        {false, s.spiPs, s.portUc, s.portPs},  // our requests
        {false, s.spiPc, s.portUs, s.portPc},  // our replies
    };
    for (const One &one : sas) {
        xfrm_usersa_info info{};
        const Address &src = one.inbound ? peer : ue;
        const Address &dst = one.inbound ? ue : peer;
        info.sel.family = ue.family;
        info.sel.saddr = src.xa;
        info.sel.daddr = dst.xa;
        info.sel.prefixlen_s = info.sel.prefixlen_d = ue.family == AF_INET6 ? 128 : 32;
        info.sel.sport = htons(one.sport);
        info.sel.sport_mask = 0xffff;
        info.sel.dport = htons(one.dport);
        info.sel.dport_mask = 0xffff;
        info.id.daddr = dst.xa;
        info.id.spi = htonl(one.spi);
        info.id.proto = IPPROTO_ESP;
        info.saddr = src.xa;
        info.lft = infinite();
        info.family = ue.family;
        info.mode = XFRM_MODE_TRANSPORT;
        info.replay_window = 32;
        // Ours were reserved as larval states: updated in place
        Message m(one.inbound ? XFRM_MSG_UPDSA : XFRM_MSG_NEWSA, 0, &info, sizeof(info));

        if (s.integrityKey.size() > 64 || s.encryptionKey.size() > 64) {
            error_ = "key too long";
            removeSas(s);
            return false;
        }
        alignas(xfrm_algo_auth) char authBuf[sizeof(xfrm_algo_auth) + 64] = {};
        auto *a = reinterpret_cast<xfrm_algo_auth *>(authBuf);
        std::strncpy(a->alg_name, auth, sizeof(a->alg_name) - 1);
        a->alg_key_len = s.integrityKey.size() * 8;
        a->alg_trunc_len = 96;
        if (!s.integrityKey.empty())
            std::memcpy(authBuf + sizeof(xfrm_algo_auth), s.integrityKey.data(), s.integrityKey.size());
        m.attr(XFRMA_ALG_AUTH_TRUNC, authBuf, sizeof(xfrm_algo_auth) + s.integrityKey.size());

        alignas(xfrm_algo) char cryptBuf[sizeof(xfrm_algo) + 64] = {};
        auto *c = reinterpret_cast<xfrm_algo *>(cryptBuf);
        std::strncpy(c->alg_name, crypt, sizeof(c->alg_name) - 1);
        c->alg_key_len = s.encryptionKey.size() * 8;
        if (!s.encryptionKey.empty())
            std::memcpy(cryptBuf + sizeof(xfrm_algo), s.encryptionKey.data(), s.encryptionKey.size());
        m.attr(XFRMA_ALG_CRYPT, cryptBuf, sizeof(xfrm_algo) + s.encryptionKey.size());

        bool ok = xfrm(m.header(), m.header()->nlmsg_len);
        if (!ok && one.inbound) {
            // The reservation ran out (the kernel keeps one for 30 s): the
            // association as a new one
            m.header()->nlmsg_type = XFRM_MSG_NEWSA;
            ok = xfrm(m.header(), m.header()->nlmsg_len);
        }
        if (!ok) {
            removeSas(s);
            return false;
        }
    }
    return true;
}

bool LinuxImsNetwork::protectSocket(int fd, Role role, const SaSet &s)
{
    Address ue, peer;
    if (!parseAddress(s.ue, 0, ue) || !parseAddress(s.pcscf, 0, peer)) {
        error_ = "bad addresses";
        return false;
    }
    const uint32_t in = role == Role::Client ? s.spiUc : s.spiUs;
    const uint32_t out = role == Role::Client ? s.spiPs : s.spiPc;
    for (int dir : {XFRM_POLICY_IN, XFRM_POLICY_OUT}) {
        struct {
            xfrm_userpolicy_info p;
            xfrm_user_tmpl t;
        } policy{};
        policy.p.sel.family = ue.family;
        policy.p.lft = infinite();
        policy.p.dir = dir;
        policy.p.action = XFRM_POLICY_ALLOW;
        policy.p.share = XFRM_SHARE_ANY;
        const bool inbound = dir == XFRM_POLICY_IN;
        policy.t.id.daddr = inbound ? ue.xa : peer.xa;
        policy.t.id.spi = htonl(inbound ? in : out);
        policy.t.id.proto = IPPROTO_ESP;
        policy.t.family = ue.family;
        policy.t.saddr = inbound ? peer.xa : ue.xa;
        policy.t.mode = XFRM_MODE_TRANSPORT;
        policy.t.aalgos = policy.t.ealgos = policy.t.calgos = ~0u;
        const int level = ue.family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
        const int option = ue.family == AF_INET6 ? IPV6_XFRM_POLICY : IP_XFRM_POLICY;
        if (setsockopt(fd, level, option, &policy, sizeof(policy)) < 0) {
            error_ = std::string("socket policy: ") + strerror(errno);
            return false;
        }
    }
    return true;
}

void LinuxImsNetwork::removeSas(const SaSet &s)
{
    Address ue, peer;
    if (!parseAddress(s.ue, 0, ue) || !parseAddress(s.pcscf, 0, peer))
        return;
    const std::pair<const Address *, uint32_t> sas[] = {
        {&ue, s.spiUc}, {&ue, s.spiUs}, {&peer, s.spiPs}, {&peer, s.spiPc}};
    for (auto [dst, spi] : sas) {
        if (!spi)
            continue;
        xfrm_usersa_id id{};
        id.daddr = dst->xa;
        id.spi = htonl(spi);
        id.family = ue.family;
        id.proto = IPPROTO_ESP;
        Message m(XFRM_MSG_DELSA, 0, &id, sizeof(id));
        xfrm(m.header(), m.header()->nlmsg_len);
    }
}

}  // namespace shannond::ims
