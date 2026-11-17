// SPDX-License-Identifier: GPL-2.0-or-later
//
// MILENAGE (3GPP TS 35.206), for the fake SIM and the fake IMS network of the
// tests: both sides of AKA, with test keys only.
#pragma once

#include <array>
#include <optional>

#include "core/frame.h"

namespace shannond::sim {

using Block = std::array<uint8_t, 16>;
using Sqn = std::array<uint8_t, 6>;

struct Milenage {
    Block k{};
    Block opc{};

    static Block opcFrom(const Block &k, const Block &op);

    // f1, f1*: MAC-A and MAC-S
    std::array<uint8_t, 8> f1(const Block &rand, const Sqn &sqn, const std::array<uint8_t, 2> &amf,
                              bool star = false) const;
    // f2-f5: RES, CK, IK, AK; f5* for resynchronisation
    struct Output {
        std::array<uint8_t, 8> res;
        Block ck, ik;
        Sqn ak, akStar;
    };
    Output f2345(const Block &rand) const;
};

// The network side: an authentication vector (RAND || AUTN as the nonce
// carries them, and the expected RES, CK, IK)
struct AuthVector {
    Block rand;
    Block autn;
    std::array<uint8_t, 8> xres;
    Block ck, ik;
};
AuthVector makeVector(const Milenage &m, const Block &rand, const Sqn &sqn, const std::array<uint8_t, 2> &amf);
// The SQN a UE sent back in AUTS, if its MAC-S is right
std::optional<Sqn> sqnFromAuts(const Milenage &m, const Block &rand, const Bytes &auts);

// The card side, as the modem's AKA command answers: status 0 with RES, CK,
// IK; 4 with AUTS on a stale SQN; 1 on a MAC failure
struct CardResult {
    uint8_t status = 0;
    std::vector<Bytes> values;
};
CardResult cardAuthenticate(const Milenage &m, Sqn &sqnMs, const Block &rand, const Block &autn);

}  // namespace shannond::sim
