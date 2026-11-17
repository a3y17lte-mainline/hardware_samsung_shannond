// SPDX-License-Identifier: GPL-2.0-or-later
//
// Packet data: contexts, their IP configuration (including the P-CSCF
// servers IMS needs) and the network interfaces that carry them.
//
// Context id N uses the modem's packet data channel N - 1, which is WWAN
// link N - 1, named rmnet<N-1> like the vendor kernel's interfaces.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/modem.h"
#include "core/network.h"

namespace shannond {

// Packet data channels 10-24 of the kernel driver
constexpr unsigned int kDataChannels = 15;

enum class IpProtocol : uint8_t { Ipv4 = 1, Ipv6 = 2, Ipv4v6 = 3 };

struct DataProfile {
    std::string apn;
    IpProtocol protocol = IpProtocol::Ipv4v6;
    std::string username;
    std::string password;
    uint8_t auth = 0;     // 0 none, 1 PAP, 2 CHAP
    bool ims = false;     // the IMS PDN, for VoLTE
};

struct DataCall {
    unsigned int cid = 0;
    std::string iface;
    AccessTech access = AccessTech::Lte;  // Wi-Fi (IWLAN) is not built yet
    std::optional<std::array<uint8_t, 4>> ipv4;
    std::optional<std::array<uint8_t, 8>> ipv6InterfaceId;
    std::vector<std::array<uint8_t, 4>> dns4;
    std::vector<std::array<uint8_t, 16>> dns6;
    std::vector<std::array<uint8_t, 4>> pcscf4;
    std::vector<std::array<uint8_t, 16>> pcscf6;
    unsigned int mtu = 0;
    bool active = false;
    uint8_t failCause = 0;
};

// How a context's interface is provided: by default a WWAN link
struct DataLinks {
    // Creates the interface, names it in call.iface and gives it the
    // call's addresses: nothing else on the system does that
    std::function<bool(DataCall &call)> up;
    std::function<void(unsigned int cid, const std::string &iface)> down;
    // Removes the interfaces an earlier client left
    std::function<void()> reset;

    static DataLinks wwan(const std::string &wwanDevice);
    static DataLinks none();  // name only, for tests
};

class Data {
public:
    using Done = std::function<void(std::optional<DataCall>)>;

    Data(Modem &modem, DataLinks links);

    // Forgets every call and removes the interfaces an earlier client left
    void reset();

    // The default bearer's APN, sent before the first connection: the
    // modem re-registers when it gets it
    void setAttachProfile(const DataProfile &profile, std::function<void(bool)> done = {});
    void connect(unsigned int cid, const DataProfile &profile, Done done);
    void disconnect(unsigned int cid, std::function<void(bool)> done = {});

    const std::map<unsigned int, DataCall> &calls() const { return calls_; }
    // The network's cause (3GPP TS 24.301 ESM cause) when the context last
    // failed or went down, 0 if none
    uint8_t lastFailCause(unsigned int cid) const
    {
        auto it = failCauses_.find(cid);
        return it == failCauses_.end() ? 0 : it->second;
    }
    void onChanged(std::function<void(const DataCall &)> cb) { changed_.push_back(std::move(cb)); }

private:
    void ipConfiguration(const Message &message);
    void callStatus(const Message &message);
    void activate(unsigned int cid, const DataProfile &profile, int attempt);
    void complete(unsigned int cid, bool ok);
    void notify(const DataCall &call);

    Modem &modem_;
    DataLinks links_;
    std::map<unsigned int, DataCall> calls_;
    std::map<unsigned int, Done> pending_;
    std::map<unsigned int, uint8_t> failCauses_;
    std::vector<std::function<void(const DataCall &)>> changed_;
};

}  // namespace shannond
