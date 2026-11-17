// SPDX-License-Identifier: GPL-2.0-or-later
//
// Per-operator IMS settings. Our own table: the values for a network come
// from what that operator's stock firmware uses (for O2 CZ the Galaxy A3's
// own image), everything else is the GSMA IR.92 default. Services are kept
// per access network so that Wi-Fi calling is a table entry, not code.
#pragma once

#include <string>
#include <vector>

#include "core/ims/endpoint.h"
#include "core/ims/sdp.h"
#include "core/ims/secagree.h"

namespace shannond::ims {

struct ImsProfile {
    std::string name;
    std::vector<std::string> plmns;  // MCC MNC, e.g. "23002"
    std::string apn = "ims";
    bool ipv6 = true;
    uint16_t pcscfPort = 5060;
    uint16_t localPort = 5060;  // our unprotected port (0: any)
    bool ipsec = true;
    // The protected ports (33.203 7.1) as the stock stack picks them: a
    // counter from here that steps by 100 for every new registration;
    // server port = counter, client port = counter + 1
    uint16_t portBase = 6000;
    SecurityAlgorithms algorithms;
    unsigned regExpires = 600000;  // seconds asked for
    // User-Agent template of the stock profile ([PRODUCT_MODEL],
    // [BUILD_VERSION], [OS_VERSION], [SUPPORT]); [BUILD_VERSION] is the last
    // buildVersionDigits characters of the firmware version (0: all of it)
    std::string userAgent = "[PRODUCT_MODEL]-[BUILD_VERSION] Samsung IMS 6.0";
    unsigned buildVersionDigits = 0;
    bool subscribeRegEvent = true;
    unsigned retryBase = 30;   // seconds, doubling per failure
    unsigned retryMax = 1800;
    SipTimers timers;

    // Calls
    bool preconditions = true;
    unsigned sessionExpires = 1800;  // seconds (RFC 4028), 0: no session timer
    unsigned minSe = 90;
    unsigned ringingTimer = 30;   // an incoming call rings this long (seconds)
    unsigned ringbackTimer = 32;  // an outgoing call waits this long for an answer
    // An outgoing call may take this long to start ringing (our own value)
    unsigned setupTimer = 60;
    // Media: the SDP and the modem's voice engine
    MediaCapabilities media;
    uint16_t audioPort = 1234;   // the first local RTP port
    unsigned rtpTimeout = 20;    // seconds
    unsigned rtcpTimeout = 20;
    bool rtcpOnCall = false;
    std::vector<int> csfbCodes;  // final answers that send the call to CS
    std::string conferenceUri;

    struct Access {
        std::string type;  // "lte"; later "wifi"
        std::vector<std::string> services;  // "mmtel", "smsip"
    };
    std::vector<Access> access{{"lte", {"mmtel"}}};
    // VoLTE on other operators' networks (needs their roaming agreement)
    bool roaming = false;

    bool offers(const std::string &accessType, const std::string &service) const;
};

// What the stock firmware says about the phone, for the User-Agent
struct DeviceInfo {
    std::string model = "SM-A320FL";          // ro.product.model
    std::string firmware = "A320FLXXS9CTK2";  // ro.build.PDA
    std::string osVersion = "8.0.0";          // ro.build.version.release
};
// The User-Agent as the stock app builds it (buildUserAgentString)
std::string userAgentFor(const ImsProfile &profile, const DeviceInfo &device);

// The profile of a network, or nullptr: VoLTE only where it is known to work
const ImsProfile *profileFor(const std::string &plmn);
// The profile of the SIM's operator (its IMSI starts with the PLMN)
const ImsProfile *profileForImsi(const std::string &imsi);
// IR.92 defaults, for tests and for trying an unknown network by hand
ImsProfile defaultProfile();

}  // namespace shannond::ims
