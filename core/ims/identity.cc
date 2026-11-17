// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/identity.h"

#include <cctype>
#include <cstdio>

namespace shannond::ims {

namespace {

bool digits(const std::string &s)
{
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return !s.empty();
}

}  // namespace

std::optional<ImsIdentity> identityFromImsi(const std::string &imsi, unsigned mncLength)
{
    if (!digits(imsi) || imsi.size() < 6 || imsi.size() > 15 || (mncLength != 2 && mncLength != 3))
        return std::nullopt;
    std::string mcc = imsi.substr(0, 3);
    std::string mnc = imsi.substr(3, mncLength);
    if (mnc.size() == 2)
        mnc = "0" + mnc;
    ImsIdentity id;
    id.domain = "ims.mnc" + mnc + ".mcc" + mcc + ".3gppnetwork.org";
    id.impi = imsi + "@" + id.domain;
    id.impus.push_back("sip:" + id.impi);
    return id;
}

std::optional<std::string> isimTlv(const Bytes &data)
{
    if (data.size() < 2 || data[0] != 0x80)
        return std::nullopt;
    size_t length = data[1];
    if (length == 0 || length + 2 > data.size())
        return std::nullopt;
    std::string s(data.begin() + 2, data.begin() + 2 + length);
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x20)
            return std::nullopt;
    return s;
}

unsigned mncLengthFromAd(const Bytes &ad)
{
    if (ad.size() < 4)
        return 0;
    unsigned n = ad[3] & 0x0f;
    return n == 2 || n == 3 ? n : 0;
}

std::string instanceId(const std::string &imei)
{
    if (imei.size() < 14 || !digits(imei.substr(0, 14)))
        return {};
    return "<urn:gsma:imei:" + imei.substr(0, 8) + "-" + imei.substr(8, 6) + "-0>";
}

std::string accessNetworkInfo(const std::string &plmn, uint16_t tac, uint32_t eci, bool tdd)
{
    char cell[16];
    std::snprintf(cell, sizeof(cell), "%04x%07x", tac, eci & 0x0fffffff);
    return std::string(tdd ? "3GPP-E-UTRAN-TDD" : "3GPP-E-UTRAN-FDD") + ";utran-cell-id-3gpp=" + plmn + cell;
}

}  // namespace shannond::ims
