// SPDX-License-Identifier: GPL-2.0-or-later
//
// ImsNetwork on plain Linux: sockets bound to the IMS PDN's interface, ESP
// security associations through XFRM netlink and per-socket policies (the
// same model as Android's IpSecManager). Needs CAP_NET_ADMIN.
#pragma once

#include <string>

#include "core/ims/network.h"

namespace shannond::ims {

class LinuxImsNetwork : public ImsNetwork {
public:
    // iface: the PDN's interface (sockets are bound to it); address: the
    // UE address to use instead of the interface's own (tests)
    explicit LinuxImsNetwork(std::string iface, std::string address = {});
    ~LinuxImsNetwork() override;

    std::string localAddress() override;
    int udpSocket(uint16_t port) override;
    int tcpSocket(uint16_t port) override;
    bool reserveSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) override;
    void releaseSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) override;
    bool installSas(const SaSet &sas) override;
    bool protectSocket(int fd, Role role, const SaSet &sas) override;
    void removeSas(const SaSet &sas) override;

    const std::string &lastError() const { return error_; }

private:
    int socketFor(int type, uint16_t port);
    bool xfrm(void *message, size_t length, void *reply = nullptr, size_t replyLength = 0);

    std::string iface_;
    std::string address_;
    std::string error_;
    int nl_ = -1;
    uint32_t seq_ = 1;
};

}  // namespace shannond::ims
