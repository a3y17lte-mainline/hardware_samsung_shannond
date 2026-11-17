// SPDX-License-Identifier: GPL-2.0-or-later
//
// The security agreement of IMS registration (RFC 3329, 3GPP TS 33.203 7):
// the UE offers IPsec parameters in Security-Client, the P-CSCF answers in
// Security-Server with its own, and both sides then set up four ESP
// transport mode security associations keyed from the AKA run's IK and CK.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/frame.h"

namespace shannond::ims {

struct SecurityMechanism {
    std::string mechanism = "ipsec-3gpp";
    std::string q;            // preference, kept as written
    std::string alg;          // hmac-md5-96, hmac-sha-1-96
    std::string ealg = "null";  // null, aes-cbc, des-ede3-cbc
    std::string prot = "esp";
    std::string mod = "trans";
    uint32_t spiC = 0;
    uint32_t spiS = 0;
    uint16_t portC = 0;
    uint16_t portS = 0;

    std::string str() const;
    static std::optional<SecurityMechanism> parse(std::string_view value);
};

// The algorithms we can run, in our order of preference
struct SecurityAlgorithms {
    std::vector<std::string> integrity{"hmac-sha-1-96", "hmac-md5-96"};
    std::vector<std::string> encryption{"null"};
};

// Security-Client values: one per algorithm pair, all with our SPIs and ports
std::vector<SecurityMechanism> clientOffers(const SecurityAlgorithms &algorithms, uint32_t spiC, uint32_t spiS,
                                            uint16_t portC, uint16_t portS);

// The P-CSCF's parameters: the entry with the highest preference whose
// algorithms we offered (RFC 3329 2.3.1)
std::optional<SecurityMechanism> chooseServer(const std::vector<std::string> &securityServer,
                                              const SecurityAlgorithms &algorithms);

// Keys for the SAs (TS 33.203 6.1 and 7.1): the integrity key from IK, the
// encryption key from CK; empty for the null cipher
Bytes integrityKey(const std::string &alg, const Bytes &ik);
Bytes encryptionKey(const std::string &ealg, const Bytes &ck);
bool knownIntegrity(std::string_view alg);
bool knownEncryption(std::string_view ealg);

}  // namespace shannond::ims
