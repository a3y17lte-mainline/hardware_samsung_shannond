// SPDX-License-Identifier: GPL-2.0-or-later
//
// Text as the network sends it: the GSM 7-bit default alphabet and its
// extension table (3GPP TS 23.038 6.2.1), packed into septets, and the
// coding schemes of USSD and broadcast data (23.038 5). Strings on our side
// are UTF-8.
#pragma once

#include <optional>
#include <string>

#include "core/frame.h"

namespace shannond {

// UTF-8 as GSM 7-bit, packed. Empty if a character has no GSM code
std::optional<Bytes> packGsm7(const std::string &text);
// Packed GSM 7-bit as UTF-8 (a CR that only fills the last octet dropped)
std::string unpackGsm7(const Bytes &packed);

enum class TextCoding { Gsm7, EightBit, Ucs2, Ucs2WithLanguage, Unknown };

// The character set of a USSD/CBS data coding scheme, as the stock RIL
// reads it (GetDataCodingScheme)
TextCoding ussdCoding(uint8_t dcs);
// The text of a USSD string; empty for a coding nobody can read
std::string decodeUssd(uint8_t dcs, const Bytes &data);

// UTF-16 big endian, as UCS2 text is sent
Bytes utf8ToUcs2(const std::string &text);

}  // namespace shannond
