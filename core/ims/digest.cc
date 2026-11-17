// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/digest.h"

#include <cstdio>

#include <openssl/evp.h>

#include "core/ims/sip.h"

namespace shannond::ims {

namespace {

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c)
{
    for (int i = 0; i < 64; i++)
        if (kBase64[i] == c)
            return i;
    return -1;
}

}  // namespace

std::string base64Encode(const Bytes &data)
{
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < data.size())
            v |= data[i + 1] << 8;
        if (i + 2 < data.size())
            v |= data[i + 2];
        out += kBase64[v >> 18 & 63];
        out += kBase64[v >> 12 & 63];
        out += i + 1 < data.size() ? kBase64[v >> 6 & 63] : '=';
        out += i + 2 < data.size() ? kBase64[v & 63] : '=';
    }
    return out;
}

std::optional<Bytes> base64Decode(std::string_view text)
{
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    size_t padding = 0;
    for (char c : text) {
        if (c == '=') {
            padding++;
            continue;
        }
        if (padding)  // data after padding
            return std::nullopt;
        int v = base64Value(c);
        if (v < 0)
            return std::nullopt;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    if (padding > 2)
        return std::nullopt;
    return out;
}

std::string hex(const Bytes &data)
{
    std::string out;
    char buf[3];
    for (uint8_t b : data) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

std::string md5Hex(std::string_view data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), digest, &length, EVP_md5(), nullptr);
    return hex(Bytes(digest, digest + length));
}

bool DigestChallenge::isAka() const
{
    return iequals(algorithm, "AKAv1-MD5");
}

std::optional<DigestChallenge> DigestChallenge::parse(std::string_view value)
{
    value = trim(value);
    if (value.size() < 7 || !iequals(value.substr(0, 6), "Digest") || (value[6] != ' ' && value[6] != '\t'))
        return std::nullopt;
    DigestChallenge c;
    c.algorithm = "MD5";
    for (const std::string &item : splitList(value.substr(7))) {
        size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        std::string_view name = trim(std::string_view(item).substr(0, eq));
        std::string v = unquote(std::string_view(item).substr(eq + 1));
        if (iequals(name, "realm"))
            c.realm = v;
        else if (iequals(name, "nonce"))
            c.nonce = v;
        else if (iequals(name, "algorithm"))
            c.algorithm = v;
        else if (iequals(name, "opaque"))
            c.opaque = v;
        else if (iequals(name, "stale"))
            c.stale = iequals(v, "true");
        else if (iequals(name, "qop"))
            for (const std::string &q : splitList(v))
                if (iequals(q, "auth"))
                    c.qopAuth = true;
    }
    if (c.nonce.empty())
        return std::nullopt;
    return c;
}

std::optional<AkaVector> AkaVector::fromNonce(std::string_view nonce)
{
    auto raw = base64Decode(nonce);
    if (!raw || raw->size() < 32)
        return std::nullopt;
    AkaVector v;
    v.rand.assign(raw->begin(), raw->begin() + 16);
    v.autn.assign(raw->begin() + 16, raw->begin() + 32);
    return v;
}

std::string digestAuthorization(const DigestChallenge &challenge, const DigestCredentials &credentials,
                                const std::string &cnonce, unsigned nc)
{
    std::string password(credentials.password.begin(), credentials.password.end());
    std::string ha1 = md5Hex(credentials.username + ":" + challenge.realm + ":" + password);
    std::string ha2 = md5Hex(credentials.method + ":" + credentials.uri);
    char ncs[9];
    std::snprintf(ncs, sizeof(ncs), "%08x", nc);
    std::string response = challenge.qopAuth
                                ? md5Hex(ha1 + ":" + challenge.nonce + ":" + ncs + ":" + cnonce + ":auth:" + ha2)
                                : md5Hex(ha1 + ":" + challenge.nonce + ":" + ha2);
    if (credentials.emptyResponse)
        response.clear();

    std::string out = "Digest username=" + quote(credentials.username) + ",realm=" + quote(challenge.realm) +
                      ",nonce=" + quote(challenge.nonce) + ",uri=" + quote(credentials.uri) +
                      ",response=" + quote(response) + ",algorithm=" + challenge.algorithm;
    if (challenge.qopAuth)
        out += std::string(",cnonce=") + quote(cnonce) + ",qop=auth,nc=" + ncs;
    if (!challenge.opaque.empty())
        out += ",opaque=" + quote(challenge.opaque);
    if (credentials.auts)
        out += ",auts=" + quote(base64Encode(*credentials.auts));
    return out;
}

std::string initialAuthorization(const std::string &impi, const std::string &realm, const std::string &uri)
{
    return "Digest username=" + quote(impi) + ",realm=" + quote(realm) + ",uri=" + quote(uri) +
           ",nonce=\"\",response=\"\"";
}

}  // namespace shannond::ims
