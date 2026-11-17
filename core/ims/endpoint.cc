// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/endpoint.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace shannond::ims {

namespace {

constexpr size_t kTcpBufferMax = 256 * 1024;

std::optional<Endpoint> peerOf(const sockaddr_storage &ss)
{
    char buf[INET6_ADDRSTRLEN];
    Endpoint e;
    if (ss.ss_family == AF_INET6) {
        auto *s6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            e.ip = inet_ntop(AF_INET, s6->sin6_addr.s6_addr + 12, buf, sizeof(buf));
        } else {
            e.ip = inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
        }
        e.port = ntohs(s6->sin6_port);
        return e;
    }
    if (ss.ss_family == AF_INET) {
        auto *s4 = reinterpret_cast<const sockaddr_in *>(&ss);
        e.ip = inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf));
        e.port = ntohs(s4->sin_port);
        return e;
    }
    return std::nullopt;
}

bool toSockaddr(const Endpoint &e, sockaddr_storage &ss, socklen_t &length)
{
    ss = {};
    auto *s6 = reinterpret_cast<sockaddr_in6 *>(&ss);
    auto *s4 = reinterpret_cast<sockaddr_in *>(&ss);
    if (inet_pton(AF_INET6, e.ip.c_str(), &s6->sin6_addr) == 1) {
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(e.port);
        length = sizeof(*s6);
        return true;
    }
    if (inet_pton(AF_INET, e.ip.c_str(), &s4->sin_addr) == 1) {
        s4->sin_family = AF_INET;
        s4->sin_port = htons(e.port);
        length = sizeof(*s4);
        return true;
    }
    return false;
}

uint16_t boundPort(int fd)
{
    sockaddr_storage ss{};
    socklen_t length = sizeof(ss);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &length) < 0)
        return 0;
    auto e = peerOf(ss);
    return e ? e->port : 0;
}

std::string keyOf(const std::string &branch, const std::string &method)
{
    return branch + " " + method;
}

// A short description for the log: no identities
std::string describe(const SipMessage &m)
{
    auto cseq = m.cseq();
    std::string c = cseq ? std::to_string(cseq->first) + " " + cseq->second : "?";
    if (m.isRequest())
        return m.method() + " (CSeq " + c + ")";
    return std::to_string(m.status()) + " " + m.reason() + " (CSeq " + c + ")";
}

}  // namespace

std::string Endpoint::str() const
{
    return ip.find(':') != std::string::npos ? "[" + ip + "]:" + std::to_string(port)
                                             : ip + ":" + std::to_string(port);
}

struct SipEndpoint::Connection {
    int fd = -1;
    Flow flow = Flow::Plain;
    Endpoint peer;
    bool connected = false;
    std::string in;
    std::string out;
};

struct SipEndpoint::SaSockets {
    SaSet sas;
    int clientUdp = -1;
    int serverUdp = -1;
    int serverTcp = -1;
};

struct SipEndpoint::ClientTransaction {
    SipMessage request;
    std::string key;
    std::string data;
    Route route;
    bool udp = true;
    bool invite = false;
    bool provisional = false;
    int connection = -1;
    std::chrono::milliseconds interval{};
    EventLoop::TimerId retransmitTimer = 0;
    EventLoop::TimerId timeoutTimer = 0;
    ResponseHandler handler;
};

struct SipEndpoint::ServerTransaction {
    Source source;
    std::string lastResponse;
    EventLoop::TimerId expire = 0;
};

SipEndpoint::SipEndpoint(EventLoop &loop, ImsNetwork &network, SipTimers timers)
    : loop_(loop), network_(network), timers_(timers)
{
}

SipEndpoint::~SipEndpoint()
{
    closeAll();
}

void SipEndpoint::log(const std::string &line)
{
    if (log_)
        log_(line);
}

bool SipEndpoint::openPlain(uint16_t port)
{
    if (plainUdp_ >= 0)
        return true;
    local_ = network_.localAddress();
    plainUdp_ = network_.udpSocket(port);
    if (plainUdp_ < 0)
        return false;
    plainPort_ = boundPort(plainUdp_);
    watchUdp(plainUdp_, Flow::Plain);
    plainTcp_ = network_.tcpSocket(plainPort_);
    if (plainTcp_ >= 0 && listen(plainTcp_, 8) == 0) {
        watchListener(plainTcp_, Flow::Plain);
    } else if (plainTcp_ >= 0) {
        close(plainTcp_);
        plainTcp_ = -1;
    }
    return true;
}

bool SipEndpoint::openProtected(const SaSet &sas)
{
    auto s = std::make_unique<SaSockets>();
    s->sas = sas;
    auto fail = [&]() {
        for (int fd : {s->clientUdp, s->serverUdp, s->serverTcp})
            if (fd >= 0) {
                loop_.unwatch(fd);
                close(fd);
            }
        return false;
    };
    s->clientUdp = network_.udpSocket(sas.portUc);
    if (s->clientUdp < 0 || !network_.protectSocket(s->clientUdp, ImsNetwork::Role::Client, sas))
        return fail();
    s->serverUdp = network_.udpSocket(sas.portUs);
    if (s->serverUdp < 0 || !network_.protectSocket(s->serverUdp, ImsNetwork::Role::Server, sas))
        return fail();
    s->serverTcp = network_.tcpSocket(sas.portUs);
    if (s->serverTcp < 0 || !network_.protectSocket(s->serverTcp, ImsNetwork::Role::Server, sas) ||
        listen(s->serverTcp, 8) < 0)
        return fail();
    closeProtected();
    local_ = sas.ue;
    watchUdp(s->clientUdp, Flow::Client);
    watchUdp(s->serverUdp, Flow::Server);
    watchListener(s->serverTcp, Flow::Server);
    current_ = std::move(s);
    return true;
}

void SipEndpoint::closeProtected()
{
    if (!current_)
        return;
    std::vector<int> drops;
    for (auto &[fd, c] : connections_)
        if (c->flow != Flow::Plain)
            drops.push_back(fd);
    for (int fd : drops)
        drop(fd);
    for (int fd : {current_->clientUdp, current_->serverUdp, current_->serverTcp}) {
        loop_.unwatch(fd);
        close(fd);
    }
    current_.reset();
}

void SipEndpoint::closeAll()
{
    closeProtected();
    std::vector<int> drops;
    for (auto &[fd, c] : connections_)
        drops.push_back(fd);
    for (int fd : drops)
        drop(fd);
    for (int *fd : {&plainUdp_, &plainTcp_})
        if (*fd >= 0) {
            loop_.unwatch(*fd);
            close(*fd);
            *fd = -1;
        }
    for (auto &[key, t] : clients_) {
        loop_.cancel(t->retransmitTimer);
        loop_.cancel(t->timeoutTimer);
    }
    clients_.clear();
    for (auto &[key, t] : servers_)
        loop_.cancel(t->expire);
    servers_.clear();
}

uint16_t SipEndpoint::port(Flow flow) const
{
    if (flow == Flow::Plain || !current_)
        return plainPort_;
    // Via and Contact carry the protected server port for both flows: the
    // P-CSCF sends UDP replies and its own requests there
    return current_->sas.portUs;
}

int SipEndpoint::udpFor(Flow flow) const
{
    switch (flow) {
    case Flow::Plain: return plainUdp_;
    case Flow::Client: return current_ ? current_->clientUdp : -1;
    case Flow::Server: return current_ ? current_->serverUdp : -1;
    }
    return -1;
}

void SipEndpoint::watchUdp(int fd, Flow flow)
{
    loop_.watch(fd, EPOLLIN, [this, fd, flow](uint32_t) { readUdp(fd, flow); });
}

void SipEndpoint::watchListener(int fd, Flow flow)
{
    loop_.watch(fd, EPOLLIN, [this, fd, flow](uint32_t) {
        sockaddr_storage ss{};
        socklen_t length = sizeof(ss);
        int c = accept4(fd, reinterpret_cast<sockaddr *>(&ss), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0)
            return;
        auto peer = peerOf(ss);
        if (!peer) {
            close(c);
            return;
        }
        auto conn = std::make_unique<Connection>();
        conn->fd = c;
        conn->flow = flow;
        conn->peer = *peer;
        conn->connected = true;
        connections_[c] = std::move(conn);
        loop_.watch(c, EPOLLIN, [this, c](uint32_t events) {
            if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
                readTcp(c);
            else if (events & EPOLLOUT)
                writeTcp(c);
        });
    });
}

void SipEndpoint::readUdp(int fd, Flow flow)
{
    for (;;) {
        char buf[65536];
        sockaddr_storage ss{};
        socklen_t length = sizeof(ss);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&ss), &length);
        if (n < 0)
            return;
        auto peer = peerOf(ss);
        if (!peer)
            continue;
        Source source;
        source.flow = flow;
        source.from = *peer;
        std::string text(buf, n);
        if (text == "\r\n\r\n" || text == "\r\n")
            continue;  // keep-alive
        received(text, source);
    }
}

void SipEndpoint::readTcp(int fd)
{
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;
    Connection *c = it->second.get();
    if (!c->connected) {
        writeTcp(fd);
        return;
    }
    for (;;) {
        char buf[16384];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            drop(fd);
            return;
        }
        if (n < 0)
            break;
        c->in.append(buf, n);
        if (c->in.size() > kTcpBufferMax) {
            log("SIP over TCP: message too long, connection dropped");
            drop(fd);
            return;
        }
    }
    for (;;) {
        auto length = sipFrameLength(c->in);
        if (!length) {
            log("SIP over TCP: broken stream, connection dropped");
            drop(fd);
            return;
        }
        if (*length == 0)
            return;
        std::string text = c->in.substr(0, *length);
        c->in.erase(0, *length);
        if (text.find_first_not_of("\r\n") == std::string::npos)
            continue;
        Source source;
        source.flow = c->flow;
        source.from = c->peer;
        source.connection = fd;
        received(text, source);
        // The handler may have dropped this connection
        if (!connections_.count(fd))
            return;
    }
}

void SipEndpoint::writeTcp(int fd)
{
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;
    Connection *c = it->second.get();
    if (!c->connected) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) {
            log("SIP over TCP: connect to " + c->peer.str() + " failed: " + strerror(error));
            drop(fd);
            return;
        }
        c->connected = true;
    }
    while (!c->out.empty()) {
        ssize_t n = ::send(fd, c->out.data(), c->out.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            drop(fd);
            return;
        }
        c->out.erase(0, n);
    }
    loop_.watch(fd, EPOLLIN | (c->out.empty() ? 0 : EPOLLOUT), [this, fd](uint32_t events) {
        if (events & EPOLLOUT)
            writeTcp(fd);
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            readTcp(fd);
    });
}

void SipEndpoint::drop(int fd)
{
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;
    loop_.unwatch(fd);
    close(fd);
    connections_.erase(it);
    // Client transactions waiting on this connection fail
    std::vector<std::string> failed;
    for (auto &[key, t] : clients_)
        if (t->connection == fd)
            failed.push_back(key);
    for (const auto &key : failed)
        timeout(key);
}

SipEndpoint::Connection *SipEndpoint::connect(const Route &route)
{
    for (auto &[fd, c] : connections_)
        if (c->flow == route.flow && c->peer == route.to)
            return c.get();
    if (route.flow == Flow::Server)
        return nullptr;  // only the P-CSCF connects to our server port
    if (route.flow == Flow::Client && !current_)
        return nullptr;
    int fd = network_.tcpSocket(route.flow == Flow::Client ? current_->sas.portUc : 0);
    if (fd < 0)
        return nullptr;
    if (route.flow == Flow::Client && !network_.protectSocket(fd, ImsNetwork::Role::Client, current_->sas)) {
        close(fd);
        return nullptr;
    }
    sockaddr_storage ss;
    socklen_t length;
    if (!toSockaddr(route.to, ss, length) ||
        (::connect(fd, reinterpret_cast<sockaddr *>(&ss), length) < 0 && errno != EINPROGRESS)) {
        close(fd);
        return nullptr;
    }
    auto conn = std::make_unique<Connection>();
    conn->fd = fd;
    conn->flow = route.flow;
    conn->peer = route.to;
    Connection *c = conn.get();
    connections_[fd] = std::move(conn);
    loop_.watch(fd, EPOLLOUT | EPOLLIN, [this, fd](uint32_t events) {
        if (events & EPOLLOUT)
            writeTcp(fd);
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            readTcp(fd);
    });
    return c;
}

bool SipEndpoint::transmit(const std::string &data, const Route &route, int connection)
{
    const bool tcp = connection >= 0 || route.tcp;
    if (tcp) {
        Connection *c = nullptr;
        if (connection >= 0) {
            auto it = connections_.find(connection);
            if (it != connections_.end())
                c = it->second.get();
        }
        if (!c)
            c = connect(route);
        if (!c)
            return false;
        c->out += data;
        if (c->connected)
            writeTcp(c->fd);
        return true;
    }
    int fd = udpFor(route.flow);
    sockaddr_storage ss;
    socklen_t length;
    if (fd < 0 || !toSockaddr(route.to, ss, length))
        return false;
    if (sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&ss), length) ==
        static_cast<ssize_t>(data.size()))
        return true;
    log(std::string("SIP: sendto: ") + strerror(errno));
    return false;
}

std::string SipEndpoint::send(SipMessage request, const Route &route, ResponseHandler handler)
{
    auto t = std::make_unique<ClientTransaction>();
    t->route = route;
    t->invite = request.method() == "INVITE";
    std::string branch;
    if (auto existing = request.branch()) {
        branch = *existing;  // CANCEL: the INVITE's branch
    } else {
        branch = newBranch();
        // Size with the Via decides the transport
        Via via;
        via.transport = "UDP";
        via.host = local_;
        via.port = port(route.flow);
        via.params.set("branch", branch);
        via.params.set("rport");
        SipMessage probe = request;
        probe.add("Via", via.str());
        t->route.tcp = route.tcp || probe.serialize().size() > udpLimit;
        if (t->route.tcp)
            via.transport = "TCP";
        auto headers = request.allHeaders();
        SipMessage withVia = request.isRequest() ? SipMessage::request(request.method(), request.requestUri())
                                                 : request;
        withVia.add("Via", via.str());
        for (const auto &[k, v] : headers)
            withVia.add(k, v);
        withVia.body = request.body;
        request = std::move(withVia);
    }
    t->udp = !t->route.tcp;
    t->key = keyOf(branch, request.method());
    t->request = request;
    t->data = request.serialize();
    t->handler = std::move(handler);
    t->interval = timers_.t1;
    log("SIP out: " + describe(request) + " to " + route.to.str() + (t->udp ? " UDP" : " TCP"));

    const std::string key = t->key;
    if (!transmit(t->data, t->route)) {
        log("SIP: could not send to " + route.to.str());
        auto h = std::move(t->handler);
        loop_.post([h]() {
            if (h)
                h(nullptr);
        });
        return branch;
    }
    if (!t->udp) {
        for (auto &[fd, c] : connections_)
            if (c->flow == t->route.flow && c->peer == t->route.to)
                t->connection = fd;
    } else {
        t->retransmitTimer = loop_.after(t->interval, [this, key]() { retransmit(key); });
    }
    t->timeoutTimer = loop_.after(timers_.t1 * 64, [this, key]() { timeout(key); });
    clients_[key] = std::move(t);
    return branch;
}

void SipEndpoint::retransmit(const std::string &key)
{
    auto it = clients_.find(key);
    if (it == clients_.end())
        return;
    ClientTransaction &t = *it->second;
    transmit(t.data, t.route);
    // INVITE: doubling without a cap (timer A); others: capped at T2 (timer E)
    t.interval = t.invite ? t.interval * 2 : std::min(t.interval * 2, timers_.t2);
    t.retransmitTimer = loop_.after(t.interval, [this, key]() { retransmit(key); });
}

void SipEndpoint::timeout(const std::string &key)
{
    auto it = clients_.find(key);
    if (it == clients_.end())
        return;
    auto handler = std::move(it->second->handler);
    log("SIP: " + describe(it->second->request) + " got no final answer");
    finish(key);
    if (handler)
        handler(nullptr);
}

void SipEndpoint::finish(const std::string &key)
{
    auto it = clients_.find(key);
    if (it == clients_.end())
        return;
    loop_.cancel(it->second->retransmitTimer);
    loop_.cancel(it->second->timeoutTimer);
    clients_.erase(it);
}

void SipEndpoint::forget(const std::string &branch)
{
    std::vector<std::string> keys;
    for (auto &[key, t] : clients_)
        if (key.starts_with(branch + " "))
            keys.push_back(key);
    for (const auto &key : keys)
        finish(key);
}

bool SipEndpoint::cancel(const std::string &branch, ResponseHandler handler)
{
    auto it = clients_.find(keyOf(branch, "INVITE"));
    if (it == clients_.end())
        return false;
    const ClientTransaction &t = *it->second;
    // Same Request-URI, Call-ID, To, From, top Via and Route; the CSeq
    // number with method CANCEL
    SipMessage c = SipMessage::request("CANCEL", t.request.requestUri());
    if (auto via = t.request.header("Via"))
        c.add("Via", *via);
    for (const char *h : {"Route", "From", "To", "Call-ID"})
        for (const auto &v : t.request.headers(h))
            c.add(h, v);
    if (auto cseq = t.request.cseq())
        c.add("CSeq", std::to_string(cseq->first) + " CANCEL");
    c.add("Max-Forwards", "70");
    send(std::move(c), t.route, std::move(handler));
    return true;
}

void SipEndpoint::sendAck(const ClientTransaction &t, const SipMessage &response)
{
    // RFC 3261 17.1.1.3: same Request-URI, Call-ID, From, top Via, CSeq
    // number; To from the response
    SipMessage ack = SipMessage::request("ACK", t.request.requestUri());
    if (auto via = t.request.header("Via"))
        ack.add("Via", *via);
    for (const char *h : {"Route", "From", "Call-ID"})
        for (const auto &v : t.request.headers(h))
            ack.add(h, v);
    if (auto to = response.header("To"))
        ack.add("To", *to);
    if (auto cseq = t.request.cseq())
        ack.add("CSeq", std::to_string(cseq->first) + " ACK");
    ack.add("Max-Forwards", "70");
    transmit(ack.serialize(), t.route, t.connection);
}

bool SipEndpoint::sendStateless(const SipMessage &message, const Route &route)
{
    log("SIP out: " + describe(message) + " to " + route.to.str());
    return transmit(message.serialize(), route);
}

void SipEndpoint::received(const std::string &text, const Source &source)
{
    auto message = SipMessage::parse(text);
    if (!message || !message->has("Via") || !message->cseq() || !message->callId()) {
        log("SIP in: unreadable message from " + source.from.str() + ", dropped");
        return;
    }
    log("SIP in: " + describe(*message) + " from " + source.from.str());
    if (message->isRequest())
        handleRequest(std::move(*message), source);
    else
        handleResponse(std::move(*message));
}

void SipEndpoint::handleResponse(SipMessage response)
{
    auto branch = response.branch();
    auto cseq = response.cseq();
    if (!branch || !cseq)
        return;
    const std::string key = keyOf(*branch, cseq->second);
    auto it = clients_.find(key);
    if (it == clients_.end()) {
        // A retransmitted 2xx of an INVITE: the call layer ACKs it again
        if (cseq->second == "INVITE" && response.status() >= 200 && response.status() < 300 &&
            strayHandler_)
            strayHandler_(response);
        return;  // a retransmission after the end, or not ours
    }
    ClientTransaction &t = *it->second;
    const int status = response.status();
    if (status < 200) {
        if (!t.provisional) {
            t.provisional = true;
            // INVITE: timer B only runs until a provisional answer; the
            // call layer times the ringing itself
            if (t.invite) {
                loop_.cancel(t.timeoutTimer);
                t.timeoutTimer = 0;
            }
            if (t.udp) {
                loop_.cancel(t.retransmitTimer);
                t.retransmitTimer = 0;
                if (!t.invite) {  // timer E continues at T2
                    t.interval = timers_.t2;
                    t.retransmitTimer = loop_.after(t.interval, [this, key]() { retransmit(key); });
                }
            }
        }
        if (t.handler)
            t.handler(&response);
        return;
    }
    if (t.invite && status >= 300)
        sendAck(t, response);
    auto handler = std::move(t.handler);
    finish(key);
    if (handler)
        handler(&response);
}

void SipEndpoint::handleRequest(SipMessage request, const Source &source)
{
    auto branch = request.branch();
    if (!branch)
        return;
    const std::string &method = request.method();
    if (method == "ACK") {
        // The ACK of a non-2xx final answer ends that INVITE's server
        // transaction; an ACK for a 2xx has its own branch and goes up
        auto it = servers_.find(keyOf(*branch, "INVITE"));
        if (it != servers_.end())
            return;
        if (requestHandler_)
            requestHandler_(request, source);
        return;
    }
    const std::string key = keyOf(*branch, method);
    auto it = servers_.find(key);
    if (it != servers_.end()) {
        // A retransmission: the last answer again
        if (!it->second->lastResponse.empty()) {
            Route route;
            route.flow = source.flow;
            route.to = source.from;
            transmit(it->second->lastResponse, route, source.connection);
        }
        return;
    }
    auto t = std::make_unique<ServerTransaction>();
    t->source = source;
    // An INVITE's lives until its final answer (then 64*T1 more, see
    // respond()); the limit only guards against a call layer that never answers
    t->expire = loop_.after(method == "INVITE" ? std::chrono::milliseconds(std::chrono::minutes(5)) : timers_.t1 * 64,
                            [this, key]() {
        auto it = servers_.find(key);
        if (it != servers_.end())
            servers_.erase(it);
    });
    servers_[key] = std::move(t);
    if (requestHandler_)
        requestHandler_(request, source);
    else
        respond(source, request, request.makeResponse(501, "Not Implemented"));
}

// A UDP answer goes where the top Via says (RFC 3261 18.2.2, RFC 3581): the
// received address, else the sent-by host; the rport value, else the sent-by
// port. The P-CSCF names its protected server port there (O2's sends from its
// client port, Via port-s, no rport, and drops answers to its client port),
// so the answer leaves from our protected client port, over the SA to port-s.
Route SipEndpoint::udpResponseRoute(const Source &source, const SipMessage &request) const
{
    Route route;
    route.flow = source.flow;
    route.to = source.from;
    auto vias = request.headers("Via");
    auto via = vias.empty() ? std::nullopt : Via::parse(vias.front());
    if (!via)
        return route;
    auto numeric = [](const std::string &host) {
        unsigned char buf[16];
        return inet_pton(AF_INET, host.c_str(), buf) == 1 || inet_pton(AF_INET6, host.c_str(), buf) == 1;
    };
    if (auto received = via->params.get("received"); received && numeric(*received))
        route.to.ip = *received;
    else if (numeric(via->host))
        route.to.ip = via->host;
    uint16_t port = via->port ? via->port : uint16_t(5060);
    if (auto rport = via->params.get("rport"); rport && !rport->empty()) {
        char *end = nullptr;
        unsigned long p = std::strtoul(rport->c_str(), &end, 10);
        if (end && !*end && p > 0 && p <= 0xffff)
            port = static_cast<uint16_t>(p);
    }
    route.to.port = port;
    if (source.flow != Flow::Plain && current_) {
        if (port == current_->sas.portPs)
            route.flow = Flow::Client;
        else if (port == current_->sas.portPc)
            route.flow = Flow::Server;
    }
    return route;
}

void SipEndpoint::respond(const Source &source, const SipMessage &request, const SipMessage &response)
{
    auto branch = request.branch();
    std::string data = response.serialize();
    if (branch) {
        const std::string key = keyOf(*branch, request.method());
        auto it = servers_.find(key);
        if (it != servers_.end()) {
            it->second->lastResponse = data;
            if (request.method() == "INVITE" && response.status() >= 200) {
                loop_.cancel(it->second->expire);
                it->second->expire = loop_.after(timers_.t1 * 64, [this, key]() {
                    auto it = servers_.find(key);
                    if (it != servers_.end())
                        servers_.erase(it);
                });
            }
        }
    }
    Route route;
    route.flow = source.flow;
    route.to = source.from;
    if (source.connection < 0)
        route = udpResponseRoute(source, request);
    log("SIP out: " + describe(response) + " to " + route.to.str());
    if (!transmit(data, route, source.connection) && source.connection >= 0)
        transmit(data, route);  // the connection is gone: over UDP
}

}  // namespace shannond::ims
