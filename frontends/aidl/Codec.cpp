// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Codec.h"

#include <map>

namespace shannond::hal {

std::string toHex(const Bytes &bytes)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::optional<Bytes> fromHex(const std::string &hex)
{
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2)
        return std::nullopt;
    Bytes bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return bytes;
}

std::string toBase64(const Bytes &bytes)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = bytes[i] << 16 | bytes[i + 1] << 8 | bytes[i + 2];
        s += table[v >> 18];
        s += table[(v >> 12) & 63];
        s += table[(v >> 6) & 63];
        s += table[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = bytes[i] << 16;
        s += table[v >> 18];
        s += table[(v >> 12) & 63];
        s += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = bytes[i] << 16 | bytes[i + 1] << 8;
        s += table[v >> 18];
        s += table[(v >> 12) & 63];
        s += table[(v >> 6) & 63];
        s += '=';
    }
    return s;
}

std::optional<Bytes> fromBase64(const std::string &text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes bytes;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        const int v = value(c);
        if (v < 0)
            return std::nullopt;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return bytes;
}

std::string bcdAddressToString(const Bytes &address)
{
    if (address.empty())
        return {};
    std::string s = (address[0] & 0x70) == 0x10 ? "+" : "";
    for (size_t i = 1; i < address.size(); i++) {
        const uint8_t lo = address[i] & 0x0f, hi = address[i] >> 4;
        if (lo <= 9) s += char('0' + lo);
        if (hi <= 9) s += char('0' + hi);
    }
    return s;
}

std::optional<Bytes> fcpToSimResponse(const Bytes &fcp, uint16_t fileId)
{
    if (fcp.size() < 2 || fcp[0] != 0x62)
        return std::nullopt;

    // Tags of the template
    std::map<uint8_t, Bytes> tags;
    const size_t end = std::min<size_t>(fcp.size(), 2 + fcp[1]);
    for (size_t i = 2; i + 1 < end;) {
        const uint8_t tag = fcp[i], len = fcp[i + 1];
        if (i + 2 + len > end)
            break;
        tags[tag] = Bytes(fcp.begin() + i + 2, fcp.begin() + i + 2 + len);
        i += 2 + len;
    }

    Bytes sim(15, 0);
    sim[4] = fileId >> 8;
    sim[5] = fileId & 0xff;
    sim[6] = 0x04;  // EF
    sim[11] = 0x01;  // not invalidated
    sim[12] = 0x02;

    uint32_t size = 0;
    if (tags.count(0x80) && tags[0x80].size() >= 2)
        size = tags[0x80][0] << 8 | tags[0x80][1];

    if (tags.count(0x82)) {
        const Bytes &d = tags[0x82];
        switch (d[0] & 0x07) {
        case 0x02:  // linear fixed
        case 0x06:  // cyclic
            sim[13] = (d[0] & 0x07) == 0x02 ? 0x01 : 0x03;
            if (d.size() >= 5) {
                sim[14] = d[3];  // record length (the low byte is enough here)
                if (!size)
                    size = d[3] * d[4];
            }
            break;
        default:
            sim[13] = 0x00;  // transparent
            break;
        }
        if ((d[0] & 0x38) == 0x38)
            sim[6] = 0x02;  // a DF
    }

    sim[2] = (size >> 8) & 0xff;
    sim[3] = size & 0xff;
    return sim;
}

std::vector<uint16_t> parsePath(const std::string &path)
{
    std::vector<uint16_t> ids;
    auto bytes = fromHex(path);
    if (!bytes)
        return ids;
    for (size_t i = 0; i + 1 < bytes->size(); i += 2)
        ids.push_back(static_cast<uint16_t>((*bytes)[i] << 8 | (*bytes)[i + 1]));
    return ids;
}

uint8_t channelCla(int32_t cla, int32_t channel)
{
    if (channel <= 0)
        return static_cast<uint8_t>(cla);
    if (channel < 4)
        return static_cast<uint8_t>((cla & 0xbc) | channel);
    return static_cast<uint8_t>((cla & 0xb0) | 0x40 | ((channel - 4) & 0x0f));
}

std::optional<AkaChallenge> parseAkaChallenge(const Bytes &data)
{
    if (data.empty())
        return std::nullopt;
    const size_t randLength = data[0];
    const size_t autnAt = 1 + randLength;
    if (autnAt >= data.size())
        return std::nullopt;
    const size_t autnLength = data[autnAt];
    if (autnAt + 1 + autnLength > data.size())
        return std::nullopt;
    AkaChallenge c;
    c.rand.assign(data.begin() + 1, data.begin() + autnAt);
    c.autn.assign(data.begin() + autnAt + 1, data.begin() + autnAt + 1 + autnLength);
    return c;
}

AkaAnswer akaAnswer(uint8_t status, const std::vector<Bytes> &values)
{
    AkaAnswer a;
    auto put = [&a](const Bytes &v) {
        a.data.push_back(static_cast<uint8_t>(v.size()));
        a.data.insert(a.data.end(), v.begin(), v.end());
    };
    if (status == 0 && values.size() >= 3) {
        a.data.push_back(0xdb);  // successful: RES, CK, IK
        for (size_t i = 0; i < 3; i++)
            put(values[i]);
        a.sw1 = 0x90;
    } else if ((status == 4 || status == 5) && !values.empty()) {
        a.data.push_back(0xdc);  // synchronisation failure: AUTS
        put(values[0]);
        a.sw1 = 0x90;
    } else {
        a.sw1 = 0x98;  // authentication error (MAC failure)
        a.sw2 = 0x62;
    }
    return a;
}

namespace {
// LastCallFailCause values
constexpr int32_t kNormal = 16;
constexpr int32_t kBusy = 17;
constexpr int32_t kNoUserResponding = 18;
constexpr int32_t kNoAnswerFromUser = 19;
constexpr int32_t kNoCircuitAvail = 34;
constexpr int32_t kAcmLimitExceeded = 68;
constexpr int32_t kFdnBlocked = 240;
constexpr int32_t kRadioOff = 1000;
constexpr int32_t kNoValidSim = 1002;
constexpr int32_t kRadioInternalError = 1003;
constexpr int32_t kErrorUnspecified = 0xffff;
}  // namespace

int32_t lastCallFailCause(uint8_t causeType, uint8_t cause)
{
    // Cause types that decide the result by themselves
    switch (causeType) {
    case 2: case 4: case 5: case 8: return kErrorUnspecified;
    case 3: return kNoCircuitAvail;
    case 6: return kAcmLimitExceeded;
    case 10: return kFdnBlocked;
    case 11: return kRadioInternalError;
    case 12: return kNoValidSim;
    case 16: return kRadioOff;
    default: break;
    }

    switch (cause) {
    case 0x04: return kFdnBlocked;
    case 0x06: return kBusy;
    case 0x07: return kNoUserResponding;
    case 0x08: return kNoAnswerFromUser;
    case 0x09:
    case 0x10:
    case 0x13:
    case 0x2f:
        return kErrorUnspecified;
    case 0x14: return kNoCircuitAvail;
    case 0x1f: return kAcmLimitExceeded;
    case 0x35: return 0x44d;
    case 0x84: return 0xf3;
    case 0x95: return kFdnBlocked;
    default: return kNormal;
    }
}

int32_t androidAppType(uint8_t modemType)
{
    switch (modemType) {
    case 0: return 2;  // USIM
    case 1: return 5;  // ISIM
    case 2: return 4;  // CSIM
    case 3: return 1;  // SIM
    case 4: return 3;  // RUIM
    default: return 0;
    }
}

int32_t dataCallFailCause(uint8_t cause)
{
    switch (cause) {
    case 0x00: return 0x00;  // none
    case 0x48: return 0x48;
    case 0x02: return 0x24;
    case 0x03: return 0x19;
    case 0x04: return 0x1a;
    case 0x05: return 0x1b;
    case 0x06: return 0x1c;
    case 0x07: return 0x1d;
    case 0x08: return 0x1e;
    case 0x09: return 0x1f;
    case 0x0a: return 0x20;
    case 0x0b: return 0x21;
    case 0x0c: return 0x22;
    case 0x0d: return 0x23;
    case 0x0e: return 0x25;
    case 0x0f: return 0x26;
    case 0x10: return 0x27;
    case 0x11: return 0x28;
    case 0x13: return 0x2b;
    case 0x15: return 0x6f;
    case 0x19: return 0x0c;
    case 0x1a: return 0x70;
    case 0x1b: return 0x29;
    case 0x1c: return 0x2a;
    case 0x1d: case 0x1e: case 0x1f: case 0x24: case 0x25: case 0x27: case 0x28:
        return 0x6f;  // protocol errors (outside Korea)
    case 0x20: return 0x2c;
    case 0x21: return 0x2d;
    case 0x22: return 0x51;
    case 0x29: return 0x08;
    case 0x2a: return 0x2e;
    case 0x2d: return 0x32;
    case 0x2e: return 0x33;
    case 0x2f: return 0x34;
    case 0x3e: return -11;
    case 0x3f: return -8;
    case 0x40: return 0x42;
    case 0x41: return 0x37;
    case 0x42: return 0x43;
    case 0x43: return 0x44;
    case 0x44: return 0x45;
    case 0x45: return 0x46;
    case 0x46: return -9;
    case 0x47: return -10;
    default: return 0x0e;
    }
}

}  // namespace shannond::hal
