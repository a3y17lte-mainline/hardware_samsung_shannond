// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hex, base64 and BCD helpers for the HAL's string-encoded fields.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"

namespace shannond::hal {

std::string toHex(const Bytes &bytes);  // upper case, as the framework writes it
std::optional<Bytes> fromHex(const std::string &hex);
std::string toBase64(const Bytes &bytes);
std::optional<Bytes> fromBase64(const std::string &text);

// An address as in an SMS PDU (type of number, then BCD with swapped
// nibbles) as "+420123..."
std::string bcdAddressToString(const Bytes &address);

// A USIM GET RESPONSE (an FCP template, tag 0x62) as the 15 bytes of a SIM
// GET RESPONSE, which is what the framework parses
std::optional<Bytes> fcpToSimResponse(const Bytes &fcp, uint16_t fileId);

// "3F007F20" -> {0x3f00, 0x7f20}
std::vector<uint16_t> parsePath(const std::string &path);

// The class byte for a logical channel (ETSI TS 102 221 10.1.1): channels
// 1-3 in the low bits, 4-19 with the extended coding
uint8_t channelCla(int32_t cla, int32_t channel);

// EAP-AKA through requestIccSimAuthentication: the challenge is
// length, RAND, length, AUTN (3GPP TS 31.102 7.1.2); the answer is
// 0xDB RES CK IK, or 0xDC AUTS on a synchronisation failure, each value with
// a length byte in front
struct AkaChallenge {
    Bytes rand;
    Bytes autn;
};
std::optional<AkaChallenge> parseAkaChallenge(const Bytes &data);

struct AkaAnswer {
    Bytes data;
    uint8_t sw1 = 0;
    uint8_t sw2 = 0;
};
// status and values as the modem reports them (0: RES, CK, IK; 4/5: AUTS)
AkaAnswer akaAnswer(uint8_t status, const std::vector<Bytes> &values);

// The modem's call end (IPC_CALL_STATUS: cause type and cause) as Android's
// LastCallFailCause value, as the stock RIL converts it (GetReleaseCause,
// without its operator specific branches)
int32_t lastCallFailCause(uint8_t causeType, uint8_t cause);

// The modem's data call fail cause as Android's DataCallFailCause value, as
// the stock RIL converts it (ConvertDataFailCauseFromIpc); the modem's codes
// are its own, not the 3GPP ones
int32_t dataCallFailCause(uint8_t cause);

}  // namespace shannond::hal
