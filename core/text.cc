// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/text.h"

#include <array>
#include <vector>

namespace shannond {

namespace {

// 3GPP TS 23.038 6.2.1: the default alphabet by GSM code
constexpr std::array<char32_t, 128> kGsm7 = {
    U'@', U'£', U'$', U'¥', U'è', U'é', U'ù', U'ì', U'ò', U'Ç', U'\n', U'Ø', U'ø', U'\r', U'Å', U'å',
    U'Δ', U'_', U'Φ', U'Γ', U'Λ', U'Ω', U'Π', U'Ψ', U'Σ', U'Θ', U'Ξ', 0x1b, U'Æ', U'æ', U'ß', U'É',
    U' ', U'!', U'"', U'#', U'¤', U'%', U'&', U'\'', U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    U'¡', U'A', U'B', U'C', U'D', U'E', U'F', U'G', U'H', U'I', U'J', U'K', U'L', U'M', U'N', U'O',
    U'P', U'Q', U'R', U'S', U'T', U'U', U'V', U'W', U'X', U'Y', U'Z', U'Ä', U'Ö', U'Ñ', U'Ü', U'§',
    U'¿', U'a', U'b', U'c', U'd', U'e', U'f', U'g', U'h', U'i', U'j', U'k', U'l', U'm', U'n', U'o',
    U'p', U'q', U'r', U's', U't', U'u', U'v', U'w', U'x', U'y', U'z', U'ä', U'ö', U'ñ', U'ü', U'à',
};

constexpr uint8_t kEscape = 0x1b;

// 6.2.1.1: the extension table, reached through the escape code
struct Extension {
    uint8_t code;
    char32_t c;
};
constexpr Extension kGsm7Extension[] = {
    {0x0a, U'\f'}, {0x14, U'^'}, {0x28, U'{'}, {0x29, U'}'}, {0x2f, U'\\'},
    {0x3c, U'['}, {0x3d, U'~'}, {0x3e, U']'}, {0x40, U'|'}, {0x65, U'€'},
};

std::vector<char32_t> utf8Decode(const std::string &s)
{
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = s[i];
        int extra = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) {  // bad or cut short
            out.push_back(U'?');
            i++;
            continue;
        }
        char32_t c = extra == 0 ? b : b & (0x3f >> extra);
        for (int k = 1; k <= extra; k++)
            c = c << 6 | (static_cast<uint8_t>(s[i + k]) & 0x3f);
        out.push_back(c);
        i += 1 + extra;
    }
    return out;
}

void utf8Append(std::string &out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

std::string ucs2ToUtf8(const uint8_t *data, size_t size)
{
    std::string out;
    for (size_t i = 0; i + 1 < size; i += 2) {
        char32_t c = data[i] << 8 | data[i + 1];
        if (c == 0)
            break;  // as the stock RIL stops
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < size) {
            const char32_t low = data[i + 2] << 8 | data[i + 3];
            if (low >= 0xdc00 && low < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }
        }
        utf8Append(out, c);
    }
    return out;
}

}  // namespace

std::optional<Bytes> packGsm7(const std::string &text)
{
    std::vector<uint8_t> septets;
    for (char32_t c : utf8Decode(text)) {
        bool found = false;
        for (size_t code = 0; code < kGsm7.size(); code++) {
            if (code != kEscape && kGsm7[code] == c) {
                septets.push_back(static_cast<uint8_t>(code));
                found = true;
                break;
            }
        }
        for (const Extension &e : kGsm7Extension) {
            if (!found && e.c == c) {
                septets.push_back(kEscape);
                septets.push_back(e.code);
                found = true;
            }
        }
        if (!found)
            return std::nullopt;
    }

    // 6.1.2.3.1: seven spare bits in the last octet get a CR, so that they
    // do not read as an '@'
    if (septets.size() % 8 == 7)
        septets.push_back(0x0d);

    Bytes packed((septets.size() * 7 + 7) / 8, 0);
    for (size_t i = 0; i < septets.size(); i++) {
        const size_t bit = i * 7;
        packed[bit / 8] |= static_cast<uint8_t>(septets[i] << (bit % 8));
        if (bit % 8 > 1)
            packed[bit / 8 + 1] |= static_cast<uint8_t>(septets[i] >> (8 - bit % 8));
    }
    return packed;
}

std::string unpackGsm7(const Bytes &packed)
{
    const size_t count = packed.size() * 8 / 7;
    std::vector<uint8_t> septets;
    for (size_t i = 0; i < count; i++) {
        const size_t bit = i * 7;
        uint16_t v = packed[bit / 8] >> (bit % 8);
        if (bit / 8 + 1 < packed.size())
            v |= packed[bit / 8 + 1] << (8 - bit % 8);
        septets.push_back(v & 0x7f);
    }
    if (packed.size() % 7 == 0 && !septets.empty() && septets.back() == 0x0d)
        septets.pop_back();

    std::string out;
    for (size_t i = 0; i < septets.size(); i++) {
        if (septets[i] == kEscape && i + 1 < septets.size()) {
            const uint8_t code = septets[++i];
            char32_t c = U' ';
            for (const Extension &e : kGsm7Extension)
                if (e.code == code)
                    c = e.c;
            utf8Append(out, c);
            continue;
        }
        utf8Append(out, septets[i] == kEscape ? U' ' : kGsm7[septets[i]]);
    }
    return out;
}

TextCoding ussdCoding(uint8_t dcs)
{
    switch (dcs >> 4) {
    case 0x0:
    case 0x2:
    case 0x3:
        return TextCoding::Gsm7;
    case 0x1:
        // 0x10: GSM 7-bit with a language in front; 0x11: UCS2 with one
        return (dcs & 0x0f) == 1 ? TextCoding::Ucs2WithLanguage : TextCoding::Gsm7;
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0x9:
        // General data coding (and with a message class: 9): bits 3-2
        switch ((dcs >> 2) & 3) {
        case 1: return TextCoding::EightBit;
        case 2: return TextCoding::Ucs2;
        default: return TextCoding::Gsm7;
        }
    case 0xf:
        return dcs & 0x04 ? TextCoding::EightBit : TextCoding::Gsm7;
    default:
        return TextCoding::Unknown;
    }
}

std::string decodeUssd(uint8_t dcs, const Bytes &data)
{
    switch (ussdCoding(dcs)) {
    case TextCoding::Gsm7:
        return unpackGsm7(data);
    case TextCoding::Ucs2:
        return ucs2ToUtf8(data.data(), data.size());
    case TextCoding::Ucs2WithLanguage:
        // Two octets of language first
        return data.size() > 2 ? ucs2ToUtf8(data.data() + 2, data.size() - 2) : std::string();
    case TextCoding::EightBit: {
        // Copied as it is by the stock RIL; as Latin-1 here, so the text
        // stays valid UTF-8
        std::string out;
        for (uint8_t b : data)
            utf8Append(out, b);
        return out;
    }
    case TextCoding::Unknown:
        break;
    }
    return {};
}

Bytes utf8ToUcs2(const std::string &text)
{
    Bytes out;
    for (char32_t c : utf8Decode(text)) {
        if (c >= 0x10000) {
            c -= 0x10000;
            const char32_t high = 0xd800 + (c >> 10), low = 0xdc00 + (c & 0x3ff);
            out.push_back(high >> 8);
            out.push_back(high & 0xff);
            out.push_back(low >> 8);
            out.push_back(low & 0xff);
        } else {
            out.push_back(c >> 8);
            out.push_back(c & 0xff);
        }
    }
    return out;
}

}  // namespace shannond
