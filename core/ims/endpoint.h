// SPDX-License-Identifier: GPL-2.0-or-later
//
// The SIP transport and transaction layers (RFC 3261 17, 18) for one UE:
// its unprotected sockets, the protected ones of the current security
// associations (TS 33.203 7), and the client and server transactions that
// run over them. Everything runs on the daemon's event loop.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "core/ims/network.h"
#include "core/ims/sip.h"
#include "core/loop.h"

namespace shannond::ims {

// Which of our sockets a message goes over
enum class Flow {
    Plain,   // unprotected (initial REGISTER)
    Client,  // protected, from our client port to the P-CSCF's server port
    Server,  // protected, our server port (requests from the P-CSCF, our answers)
};

struct Route {
    Flow flow = Flow::Plain;
    Endpoint to;
    bool tcp = false;  // TCP even if small
};

// Where a request came from: its server transaction answers the same way
struct Source {
    Flow flow = Flow::Plain;
    Endpoint from;
    int connection = -1;  // the TCP connection, -1 for UDP
};

struct SipTimers {
    std::chrono::milliseconds t1{500};
    std::chrono::milliseconds t2{4000};
    std::chrono::milliseconds t4{5000};
};

class SipEndpoint {
public:
    // Called for every response of a transaction; nullptr when it timed out
    // or the transport failed. A final response or nullptr ends it.
    using ResponseHandler = std::function<void(const SipMessage *response)>;
    using RequestHandler = std::function<void(const SipMessage &request, const Source &source)>;
    using LogFn = std::function<void(const std::string &)>;

    SipEndpoint(EventLoop &loop, ImsNetwork &network, SipTimers timers = {});
    ~SipEndpoint();
    SipEndpoint(const SipEndpoint &) = delete;
    SipEndpoint &operator=(const SipEndpoint &) = delete;

    void setLog(LogFn log) { log_ = std::move(log); }
    // Called for every message sent or received (to keep the system awake
    // while SIP is going on)
    void onActivity(std::function<void()> callback) { activity_ = std::move(callback); }
    void onRequest(RequestHandler handler) { requestHandler_ = std::move(handler); }
    // A 2xx of an INVITE whose transaction already ended (a retransmission:
    // its ACK was lost)
    void onStrayResponse(std::function<void(const SipMessage &)> handler) { strayHandler_ = std::move(handler); }

    // The unprotected sockets: UDP and a TCP listener on port
    bool openPlain(uint16_t port);
    // The protected sockets of an SA set: UDP on both ports, TCP listening
    // on the server port. One set at a time.
    bool openProtected(const SaSet &sas);
    void closeProtected();
    void closeAll();
    bool isProtected() const { return current_ != nullptr; }

    const std::string &localAddress() const { return local_; }
    // Our port for Via and Contact on a flow
    uint16_t port(Flow flow) const;
    bool canUseTcp() const { return true; }

    // Starts a client transaction: adds the top Via (branch, rport) and
    // sends; retransmits over UDP. Returns the branch.
    std::string send(SipMessage request, const Route &route, ResponseHandler handler);
    // Drops a client transaction without calling its handler
    void forget(const std::string &branch);
    // CANCELs the INVITE of a client transaction (RFC 3261 9.1); false if
    // it already ended
    bool cancel(const std::string &branch, ResponseHandler handler);
    // Sends outside any transaction (ACK for a 2xx)
    bool sendStateless(const SipMessage &message, const Route &route);
    // Answers a received request through its server transaction
    void respond(const Source &source, const SipMessage &request, const SipMessage &response);

    // Over UDP, messages longer than this go over TCP (RFC 3261 18.1.1)
    size_t udpLimit = 1300;

private:
    struct Socket;
    struct Connection;
    struct SaSockets;
    struct ClientTransaction;
    struct ServerTransaction;

    void log(const std::string &line);
    int udpFor(Flow flow) const;
    Route udpResponseRoute(const Source &source, const SipMessage &request) const;
    bool transmit(const std::string &data, const Route &route, int connection = -1);
    Connection *connect(const Route &route);
    void watchUdp(int fd, Flow flow);
    void watchListener(int fd, Flow flow);
    void readUdp(int fd, Flow flow);
    void readTcp(int fd);
    void writeTcp(int fd);
    void drop(int fd);
    void received(const std::string &text, const Source &source);
    void handleResponse(SipMessage response);
    void handleRequest(SipMessage request, const Source &source);
    void retransmit(const std::string &branch);
    void timeout(const std::string &branch);
    void finish(const std::string &branch);
    void sendAck(const ClientTransaction &t, const SipMessage &response);

    EventLoop &loop_;
    ImsNetwork &network_;
    SipTimers timers_;
    LogFn log_;
    RequestHandler requestHandler_;
    std::function<void()> activity_;
    std::function<void(const SipMessage &)> strayHandler_;
    std::string local_;
    int plainUdp_ = -1;
    int plainTcp_ = -1;
    uint16_t plainPort_ = 0;
    std::unique_ptr<SaSockets> current_;
    std::map<int, std::unique_ptr<Connection>> connections_;
    std::map<std::string, std::unique_ptr<ClientTransaction>> clients_;
    std::map<std::string, std::unique_ptr<ServerTransaction>> servers_;
};

}  // namespace shannond::ims
