// SPDX-License-Identifier: GPL-2.0-or-later
//
// HTTP digest authentication with AKA (RFC 2617, RFC 3310), as IMS uses it
// for REGISTER (3GPP TS 24.229 5.1.1.5, TS 33.203 6.1).
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/frame.h"

namespace shannond::ims {

std::string base64Encode(const Bytes &data);
std::optional<Bytes> base64Decode(std::string_view text);
std::string hex(const Bytes &data);
std::string md5Hex(std::string_view data);

// A WWW-Authenticate (or Proxy-Authenticate) Digest challenge
struct DigestChallenge {
    std::string realm;
    std::string nonce;
    std::string algorithm;  // "AKAv1-MD5", "MD5", ...
    std::string opaque;
    bool qopAuth = false;   // "auth" offered
    bool stale = false;

    bool isAka() const;
    static std::optional<DigestChallenge> parse(std::string_view value);
};

// RAND and AUTN from an AKA nonce (base64 of RAND || AUTN || server data)
struct AkaVector {
    Bytes rand;  // 16 bytes
    Bytes autn;  // 16 bytes
    static std::optional<AkaVector> fromNonce(std::string_view nonce);
};

struct DigestCredentials {
    std::string username;  // the IMPI
    std::string uri;       // the request URI
    std::string method;
    // AKAv1: RES as the password, raw bytes; empty on a resynchronisation
    // or a challenge the SIM refused
    Bytes password;
    std::optional<Bytes> auts;  // resynchronisation (RFC 3310 3.4)
};

// The Authorization header value for a challenge. cnonce and nc are the
// caller's (nc counts uses of the same nonce).
std::string digestAuthorization(const DigestChallenge &challenge, const DigestCredentials &credentials,
                                const std::string &cnonce, unsigned nc);

// The Authorization header of an initial REGISTER (24.229 5.1.1.2.2): the
// IMPI, an empty nonce and response
std::string initialAuthorization(const std::string &impi, const std::string &realm, const std::string &uri);

}  // namespace shannond::ims
