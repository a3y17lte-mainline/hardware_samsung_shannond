// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the IMS stack needs from the network it runs over: sockets on the IMS
// PDN and IPsec for them. One implementation per platform: Linux sets up the
// security associations itself (XFRM), Android's comes from the ImsService
// app (IpSecManager). Both protect sockets one by one, never the whole host.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "core/frame.h"

namespace shannond::ims {

struct Endpoint {
    std::string ip;  // numeric, IPv4 or IPv6 without brackets
    uint16_t port = 0;

    bool operator==(const Endpoint &) const = default;
    std::string str() const;  // "1.2.3.4:5", "[::1]:5"
};

// The four security associations of a registration (TS 33.203 7.1). Ours
// are inbound: client SPI for the replies to our requests, server SPI for
// the P-CSCF's requests. Theirs are outbound.
struct SaSet {
    std::string ue;
    std::string pcscf;
    uint32_t spiUc = 0, spiUs = 0;  // ours
    uint16_t portUc = 0, portUs = 0;
    uint32_t spiPc = 0, spiPs = 0;  // the P-CSCF's
    uint16_t portPc = 0, portPs = 0;
    std::string alg;   // hmac-md5-96, hmac-sha-1-96
    std::string ealg;  // null, aes-cbc, des-ede3-cbc
    Bytes integrityKey;
    Bytes encryptionKey;
};

class ImsNetwork {
public:
    enum class Role { Client, Server };

    virtual ~ImsNetwork() = default;

    // The UE's address on the IMS PDN, empty while it has none
    virtual std::string localAddress() = 0;
    // Non-blocking sockets bound to the local address (port 0: any), -1 on
    // failure. TCP sockets are only bound: the caller protects them before
    // it connects or listens.
    virtual int udpSocket(uint16_t port) = 0;
    virtual int tcpSocket(uint16_t port) = 0;

    // Reserves these two SPIs for our inbound SAs (client, server): false
    // if either is taken. Held until the SAs are installed with them or
    // released.
    virtual bool reserveSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) = 0;
    virtual void releaseSpis(const std::string &pcscf, uint32_t spiUc, uint32_t spiUs) = 0;
    virtual bool installSas(const SaSet &sas) = 0;
    // A socket on our client port (to the P-CSCF's server port) or our
    // server port: only ESP with these SAs in and out
    virtual bool protectSocket(int fd, Role role, const SaSet &sas) = 0;
    virtual void removeSas(const SaSet &sas) = 0;
};

}  // namespace shannond::ims
