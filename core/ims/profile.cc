// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/profile.h"

#include <algorithm>

namespace shannond::ims {

namespace {

ImsProfile telefonicaCz()
{
    // The stock image's "Telefonica for Czech VoLTE" profile and its
    // operator settings (imsprofile.json, globalsettings.json, customer.xml)
    ImsProfile p = defaultProfile();
    p.name = "O2 CZ";
    p.plmns = {"23002"};
    p.apn = "ims";
    p.ipv6 = true;
    p.pcscfPort = 5060;
    p.ipsec = true;
    p.algorithms.encryption = {"null"};  // integrity only
    // European operators: the last three characters of the firmware
    // version; no template of its own, so the default one
    p.buildVersionDigits = 3;
    p.timers.t1 = std::chrono::milliseconds(500);
    p.timers.t2 = std::chrono::milliseconds(4000);
    p.timers.t4 = std::chrono::milliseconds(5000);
    p.preconditions = true;
    p.sessionExpires = 3600;
    p.ringingTimer = 90;
    p.ringbackTimer = 90;
    // Media: the stock defaults O2 inherits, RTCP during the call its own
    p.rtcpOnCall = true;
    p.csfbCodes = {403};
    p.conferenceUri = "sip:mmtel@conf-factory.ims.mnc002.mcc230.3gppnetwork.org";
    p.access = {{"lte", {"mmtel"}}};
    return p;
}

}  // namespace

bool ImsProfile::offers(const std::string &accessType, const std::string &service) const
{
    for (const auto &a : access)
        if (a.type == accessType)
            return std::find(a.services.begin(), a.services.end(), service) != a.services.end();
    return false;
}

std::string userAgentFor(const ImsProfile &profile, const DeviceInfo &device)
{
    auto replace = [](std::string s, const std::string &from, const std::string &to) {
        for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
            s.replace(pos, from.size(), to);
        return s;
    };
    std::string build = device.firmware;
    if (profile.buildVersionDigits && build.size() > profile.buildVersionDigits)
        build = build.substr(build.size() - profile.buildVersionDigits);
    std::string ua = profile.userAgent;
    ua = replace(ua, "[SUPPORT]", "VoLTE");
    ua = replace(ua, "[OS_VERSION]", device.osVersion);
    ua = replace(ua, "[BUILD_VERSION]", build);
    ua = replace(ua, "[PRODUCT_MODEL]", device.model);
    return ua;
}

ImsProfile defaultProfile()
{
    ImsProfile p;
    p.name = "IR.92 default";
    return p;
}

const ImsProfile *profileFor(const std::string &plmn)
{
    static const ImsProfile profiles[] = {telefonicaCz()};
    for (const auto &p : profiles)
        if (std::find(p.plmns.begin(), p.plmns.end(), plmn) != p.plmns.end())
            return &p;
    return nullptr;
}

}  // namespace shannond::ims
