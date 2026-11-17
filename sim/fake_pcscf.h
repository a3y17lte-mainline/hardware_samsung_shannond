// SPDX-License-Identifier: GPL-2.0-or-later
//
// A fake IMS network for the tests: a P-CSCF (with the S-CSCF behind it)
// that challenges REGISTER with IMS AKA (Milenage, test keys), runs the
// security agreement, checks the digest and answers the reg event
// subscription. No IPsec: its "protected" ports are plain sockets, and
// FakeImsNetwork only records what the UE asks of the kernel.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/ims/network.h"
#include "core/ims/sip.h"
#include "core/loop.h"
#include "sim/milenage.h"

namespace shannond::sim {

class FakeImsNetwork : public ims::ImsNetwork {
public:
    explicit FakeImsNetwork(std::string address = "127.0.0.1") : address_(std::move(address)) {}

    std::string localAddress() override { return address_; }
    int udpSocket(uint16_t port) override;
    int tcpSocket(uint16_t port) override;
    bool reserveSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) override;
    void releaseSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) override;
    bool installSas(const ims::SaSet &sas) override;
    bool protectSocket(int fd, Role role, const ims::SaSet &sas) override;
    void removeSas(const ims::SaSet &sas) override;

    std::vector<ims::SaSet> installed;  // currently installed
    std::vector<ims::SaSet> removed;
    std::vector<uint32_t> reserved;     // SPIs reserved and not yet used or released
    size_t protectedSockets = 0;

private:
    int bound(int type, uint16_t port);
    std::string address_;
};

class FakePcscf {
public:
    FakePcscf(EventLoop &loop, std::string address, uint16_t port);
    ~FakePcscf();

    // The home network's AKA: the card's keys and the network's SQN
    Milenage milenage;
    Sqn sqn{0, 0, 0, 0, 0, 0x40};

    // Behaviour
    bool silent = false;          // answer nothing
    int failStatus = 0;           // answer every REGISTER with this
    unsigned minExpires = 0;      // 423 below this
    unsigned grantExpires = 3600;
    bool challengeRefresh = false;  // challenge protected REGISTERs again
    std::string alg = "hmac-md5-96";
    uint32_t spiC = 0x5000, spiS = 0x6000;
    uint16_t portC, portS;  // protected ports, set from the plain one

    // What happened
    std::vector<ims::SipMessage> requests;
    unsigned registered = 0;      // 200 OK to a REGISTER with expires
    unsigned deregistered = 0;
    unsigned challenges = 0;
    unsigned resyncs = 0;
    std::string lastSecurityVerify;
    std::string lastContact;
    ims::Endpoint ueServer;       // the UE's protected server port, from Via

    // Real IPsec (in a network namespace): the protected sockets get the
    // mirror of the UE's socket policies once it offered its SPIs
    ims::ImsNetwork *ipsec = nullptr;
    bool protectedWithIpsec = false;

    // Sends a reg event NOTIFY saying the registration ended
    void terminateRegistration();

    // The other end of calls: every message but REGISTER, SUBSCRIBE and the
    // answers to our NOTIFYs, with a way to answer on the same path
    std::function<void(const ims::SipMessage &m, const std::function<void(const ims::SipMessage &)> &reply)> farEnd;
    // A request to the UE, from our client port to its server port
    void sendToUe(const ims::SipMessage &request);
    // A top Via for such a request
    std::string via() const;
    const std::string &address() const { return address_; }

private:
    struct Challenge {
        AuthVector vector;
        std::string nonce;
    };
    int open(int type, uint16_t port);
    void read(int fd);
    void handle(const ims::SipMessage &m, int fd, const ims::Endpoint &from);
    void sendTo(int fd, const ims::Endpoint &to, const ims::SipMessage &m);
    void challenge(const ims::SipMessage &m, int fd, const ims::Endpoint &to, bool protectedPorts);
    bool openProtected();
    void protectWith(const ims::SaSet &ue);

    EventLoop &loop_;
    std::string address_;
    uint16_t port_;
    int plain_ = -1;
    int protectedC_ = -1, protectedS_ = -1, protectedTcp_ = -1;
    std::vector<int> fds_;  // every socket, closed at the end
    std::map<std::string, Challenge> nonces_;
    std::string subCallId_, subFromTag_, subToTag_, subContact_;
    unsigned notifyCseq_ = 0;
    std::string impu_;
    uint32_t lastOfferSpiC = 0;  // spi-c of the challenged request's offer
};

}  // namespace shannond::sim
