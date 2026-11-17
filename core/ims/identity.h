// SPDX-License-Identifier: GPL-2.0-or-later
//
// Who the UE is in IMS (3GPP TS 23.003 13): the private identity (IMPI),
// public identities (IMPUs) and home domain, read from the ISIM, or derived
// from the IMSI when the card has no ISIM.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"

namespace shannond::ims {

struct ImsIdentity {
    std::string impi;
    std::vector<std::string> impus;  // the first one registers
    std::string domain;
    bool fromIsim = false;

    const std::string &impu() const { return impus.front(); }
    bool valid() const { return !impi.empty() && !impus.empty() && !domain.empty(); }
};

// 23.003 13.2-13.4: <IMSI>@ims.mnc<MNC>.mcc<MCC>.3gppnetwork.org
std::optional<ImsIdentity> identityFromImsi(const std::string &imsi, unsigned mncLength);
// The value of an ISIM identity file or record (TS 31.103 4.2.2-4.2.4:
// tag 0x80, length, UTF-8); empty if the record is unused
std::optional<std::string> isimTlv(const Bytes &data);
// The MNC length from the USIM's EF_AD (TS 31.102 4.2.18), 0 if not there
unsigned mncLengthFromAd(const Bytes &ad);

// +sip.instance: the IMEI as a URN (23.003 13.8, RFC 7254)
std::string instanceId(const std::string &imei);

// P-Access-Network-Info for an LTE cell (24.229 7.2A.4)
std::string accessNetworkInfo(const std::string &plmn, uint16_t tac, uint32_t eci, bool tdd = false);

}  // namespace shannond::ims
