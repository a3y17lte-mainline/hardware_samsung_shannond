// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reading the IMS identities from the card: the ISIM's EF_IMPI, EF_DOMAIN
// and EF_IMPU (TS 31.103 4.2), or, without an ISIM, the IMSI with the MNC
// length from the USIM's EF_AD (TS 23.003 13.2-13.4).
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/ims/identity.h"
#include "core/sim.h"

namespace shannond::ims {

// The size of a transparent file, or the record length and count of a
// linear fixed one, from a GET RESPONSE answer (FCP, TS 102 221 11.1.1.3)
struct FileInfo {
    size_t size = 0;
    size_t recordLength = 0;
    size_t records = 0;
};
std::optional<FileInfo> parseFcp(const Bytes &fcp);

// plmn: the network's MCC MNC, used for the MNC length when EF_AD is not
// readable and the IMSI starts with it
void readImsIdentity(Sim &sim, const std::string &plmn, std::function<void(std::optional<ImsIdentity>)> done);

}  // namespace shannond::ims
