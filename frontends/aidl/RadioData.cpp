// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include "frontends/aidl/Codec.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cstdint>
#include <sys/socket.h>

#include <aidl/android/hardware/radio/data/ApnTypes.h>
#include <aidl/android/hardware/radio/data/DataCallFailCause.h>

namespace shannond::hal {

using ::aidl::android::hardware::radio::AccessNetwork;

namespace {

std::string ipv4String(const std::array<uint8_t, 4> &a)
{
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, a.data(), text, sizeof(text));
    return text;
}

std::string ipv6String(const std::array<uint8_t, 16> &a)
{
    char text[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, a.data(), text, sizeof(text));
    return text;
}

IpProtocol ipProtocol(data::PdpProtocolType type)
{
    switch (type) {
    case data::PdpProtocolType::IP: return IpProtocol::Ipv4;
    case data::PdpProtocolType::IPV6: return IpProtocol::Ipv6;
    default: return IpProtocol::Ipv4v6;
    }
}

uint8_t authType(data::ApnAuthType type)
{
    switch (type) {
    case data::ApnAuthType::PAP_NO_CHAP: return 1;
    case data::ApnAuthType::NO_PAP_CHAP:
    case data::ApnAuthType::PAP_CHAP: return 2;
    default: return 0;
    }
}

DataProfile dataProfile(const data::DataProfileInfo &info, bool roaming)
{
    DataProfile profile;
    profile.apn = info.apn;
    profile.protocol = ipProtocol(roaming ? info.roamingProtocol : info.protocol);
    profile.username = info.user;
    profile.password = info.password;
    profile.auth = authType(info.authType);
    // An APN that is also the default one is Android's own data context,
    // not the IMS PDN (which the HAL brings up itself)
    profile.ims = (info.supportedApnTypesBitmap & static_cast<int32_t>(data::ApnTypes::IMS)) &&
                  !(info.supportedApnTypesBitmap & static_cast<int32_t>(data::ApnTypes::DEFAULT));
    return profile;
}

data::SetupDataCallResult result(const DataCall &call)
{
    data::SetupDataCallResult r;
    r.cause = data::DataCallFailCause::NONE;
    r.suggestedRetryTime = -1;
    r.cid = static_cast<int32_t>(call.cid);
    r.active = call.active ? data::SetupDataCallResult::DATA_CONNECTION_STATUS_ACTIVE
                           : data::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
    r.type = call.ipv4 && call.ipv6InterfaceId ? data::PdpProtocolType::IPV4V6
             : call.ipv6InterfaceId           ? data::PdpProtocolType::IPV6
                                              : data::PdpProtocolType::IP;
    r.ifname = call.iface;

    // Raw IP point to point links: routes without a gateway
    if (call.ipv4) {
        data::LinkAddress a;
        a.address = ipv4String(*call.ipv4) + "/32";
        a.addressProperties = data::LinkAddress::ADDRESS_PROPERTY_NONE;
        a.deprecationTime = INT64_MAX;
        a.expirationTime = INT64_MAX;
        r.addresses.push_back(a);
        r.gateways.push_back("0.0.0.0");
    }
    if (call.ipv6InterfaceId) {
        // The link local address; the global prefix comes by router
        // advertisement on the interface
        std::array<uint8_t, 16> ll{0xfe, 0x80};
        std::copy(call.ipv6InterfaceId->begin(), call.ipv6InterfaceId->end(), ll.begin() + 8);
        data::LinkAddress a;
        a.address = ipv6String(ll) + "/64";
        a.addressProperties = data::LinkAddress::ADDRESS_PROPERTY_NONE;
        a.deprecationTime = INT64_MAX;
        a.expirationTime = INT64_MAX;
        r.addresses.push_back(a);
        r.gateways.push_back("::");
    }
    for (const auto &d : call.dns4)
        r.dnses.push_back(ipv4String(d));
    for (const auto &d : call.dns6)
        r.dnses.push_back(ipv6String(d));
    for (const auto &p : call.pcscf4)
        r.pcscf.push_back(ipv4String(p));
    for (const auto &p : call.pcscf6)
        r.pcscf.push_back(ipv6String(p));
    r.mtuV4 = call.ipv4 ? static_cast<int32_t>(call.mtu) : 0;
    r.mtuV6 = call.ipv6InterfaceId ? static_cast<int32_t>(call.mtu) : 0;
    r.handoverFailureMode = data::SetupDataCallResult::HANDOVER_FAILURE_MODE_LEGACY;
    r.pduSessionId = 0;
    return r;
}

// A failed call with the modem's cause
data::SetupDataCallResult failure(uint8_t cause)
{
    data::SetupDataCallResult r;
    // The modem's codes are its own: converted as the stock RIL does
    r.cause = cause ? static_cast<data::DataCallFailCause>(dataCallFailCause(cause))
                    : data::DataCallFailCause::ERROR_UNSPECIFIED;
    r.suggestedRetryTime = -1;
    r.active = data::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
    return r;
}

}  // namespace

RadioData::RadioData(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        core.data.onChanged([this, &core](const DataCall &call) {
            // The HAL's own IMS PDN is not Android's business
            if (!cids_.count(call.cid))
                return;
            if (auto i = indication())
                i->dataCallListChanged(kUnsolicited, list(core));
            if (!call.active)
                cids_.erase(call.cid);
        });
    });
}

std::vector<data::SetupDataCallResult> RadioData::list(Core &core)
{
    std::vector<data::SetupDataCallResult> calls;
    for (const auto &[cid, call] : core.data.calls())
        if (call.active && cids_.count(cid))
            calls.push_back(result(call));
    return calls;
}

::ndk::ScopedAStatus RadioData::setupDataCall(int32_t serial, AccessNetwork accessNetwork,
                                              const data::DataProfileInfo &dataProfileInfo,
                                              bool roamingAllowed, data::DataRequestReason reason,
                                              const std::vector<data::LinkAddress> &addresses,
                                              const std::vector<std::string> &dnses,
                                              int32_t pduSessionId,
                                              const std::optional<data::SliceInfo> &sliceInfo,
                                              bool matchAllRuleAllowed)
{
    (void)roamingAllowed;
    (void)reason;  // handover (IWLAN) is not built
    (void)addresses;
    (void)dnses;
    (void)pduSessionId;
    (void)sliceInfo;
    (void)matchAllRuleAllowed;

    // Cellular only. Wi-Fi calling would add IWLAN here: an ePDG tunnel
    // instead of a modem context, behind the same DataCall
    if (accessNetwork != AccessNetwork::EUTRAN && accessNetwork != AccessNetwork::UTRAN &&
        accessNetwork != AccessNetwork::GERAN) {
        if (auto r = response())
            r->setupDataCallResponse(notSupported(serial), {});
        return ::ndk::ScopedAStatus::ok();
    }

    stack_->post([this, serial, dataProfileInfo](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setupDataCallResponse(notAvailable(serial), {});
            return;
        }

        // Context ids as the stock RIL gives them (GetCidFromApnType): 1 is
        // the IMS PDN's (the modem's voice engine wants it there), 2 the
        // internet's, the others from 3
        const auto free = [core](unsigned int c) { return !core->data.calls().count(c); };
        const int32_t types = dataProfileInfo.supportedApnTypesBitmap;
        unsigned int cid = 0;
        if ((types & static_cast<int32_t>(data::ApnTypes::IMS)) &&
            !(types & static_cast<int32_t>(data::ApnTypes::DEFAULT))) {
            // The IMS PDN is the HAL's own (cid 1, brought up by its IMS
            // stack): not Android's to set up, and not worth a retry
            if (auto r = response()) {
                data::SetupDataCallResult own = failure(0);
                own.cause = data::DataCallFailCause::INSUFFICIENT_RESOURCES;
                own.suggestedRetryTime = INT64_MAX;
                r->setupDataCallResponse(ok(serial), own);
            }
            return;
        } else if ((types & static_cast<int32_t>(data::ApnTypes::DEFAULT)) && free(2))
            cid = 2;
        else
            for (unsigned int c = 3; c <= kDataChannels && !cid; c++)
                if (free(c))
                    cid = c;
        if (!cid) {
            if (auto r = response()) {
                data::SetupDataCallResult full = failure(0);
                full.cause = data::DataCallFailCause::INSUFFICIENT_RESOURCES;
                r->setupDataCallResponse(ok(serial), full);
            }
            return;
        }

        const bool roaming = core->network.data().state == RegState::Roaming;
        cids_.insert(cid);
        core->data.connect(cid, dataProfile(dataProfileInfo, roaming),
                           [this, serial, cid, core](std::optional<DataCall> call) {
            if (!call || !call->active)
                cids_.erase(cid);
            auto r = response();
            if (!r)
                return;
            // Failures go in the result's cause, with the response itself fine
            if (call && call->active)
                r->setupDataCallResponse(ok(serial), result(*call));
            else
                r->setupDataCallResponse(ok(serial), failure(core->data.lastFailCause(cid)));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioData::deactivateDataCall(int32_t serial, int32_t cid,
                                                   data::DataRequestReason reason)
{
    (void)reason;
    stack_->post([this, serial, cid](Core *core) {
        if (!core) {
            if (auto r = response())
                r->deactivateDataCallResponse(notAvailable(serial));
            return;
        }
        if (cid <= 0 || !cids_.count(static_cast<unsigned int>(cid)) ||
            !core->data.calls().count(static_cast<unsigned int>(cid))) {
            if (auto r = response())
                r->deactivateDataCallResponse(responseInfo(serial, RadioError::INVALID_CALL_ID));
            return;
        }
        core->data.disconnect(static_cast<unsigned int>(cid), [this, serial, cid](bool success) {
            if (success)
                cids_.erase(static_cast<unsigned int>(cid));
            if (auto r = response())
                r->deactivateDataCallResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioData::getDataCallList(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getDataCallListResponse(core ? ok(serial) : notAvailable(serial),
                                       core ? list(*core)
                                            : std::vector<data::SetupDataCallResult>{});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioData::setInitialAttachApn(
        int32_t serial, const std::optional<data::DataProfileInfo> &dataProfileInfo)
{
    if (!dataProfileInfo) {
        if (auto r = response())
            r->setInitialAttachApnResponse(ok(serial));
        return ::ndk::ScopedAStatus::ok();
    }
    stack_->post([this, serial, info = *dataProfileInfo](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setInitialAttachApnResponse(notAvailable(serial));
            return;
        }
        // The modem re-registers with the new default bearer APN
        core->data.setAttachProfile(dataProfile(info, false), [this, serial](bool success) {
            if (auto r = response())
                r->setInitialAttachApnResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioData::setDataAllowed(int32_t serial, bool allow)
{
    (void)allow;  // one SIM: data stays allowed, the framework tears calls down itself
    if (auto r = response())
        r->setDataAllowedResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioData::setDataProfile(int32_t serial,
                                               const std::vector<data::DataProfileInfo> &profiles)
{
    (void)profiles;  // profiles come with each setupDataCall
    if (auto r = response())
        r->setDataProfileResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
