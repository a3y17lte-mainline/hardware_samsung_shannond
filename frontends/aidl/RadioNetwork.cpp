// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include <aidl/android/hardware/radio/network/IndicationFilter.h>

#include <limits>

#include <aidl/android/hardware/radio/RadioAccessFamily.h>
#include <aidl/android/hardware/radio/RadioTechnology.h>

namespace shannond::hal {

using ::aidl::android::hardware::radio::AccessNetwork;
using ::aidl::android::hardware::radio::RadioAccessFamily;
using ::aidl::android::hardware::radio::RadioTechnology;

namespace {

constexpr int32_t kUnavailable = std::numeric_limits<int32_t>::max();

constexpr int32_t raf(RadioAccessFamily f) { return static_cast<int32_t>(f); }

constexpr int32_t kGsmFamily = raf(RadioAccessFamily::GSM) | raf(RadioAccessFamily::GPRS) |
                               raf(RadioAccessFamily::EDGE);
constexpr int32_t kUmtsFamily = raf(RadioAccessFamily::UMTS) | raf(RadioAccessFamily::HSDPA) |
                                raf(RadioAccessFamily::HSUPA) | raf(RadioAccessFamily::HSPA) |
                                raf(RadioAccessFamily::HSPAP);
constexpr int32_t kLteFamily = raf(RadioAccessFamily::LTE) | raf(RadioAccessFamily::LTE_CA);

int32_t modesToBitmap(uint8_t modes)
{
    int32_t bitmap = 0;
    if (modes & kModeGsm)
        bitmap |= kGsmFamily;
    if (modes & kModeUmts)
        bitmap |= kUmtsFamily;
    if (modes & kModeLte)
        bitmap |= kLteFamily;
    return bitmap;
}

uint8_t bitmapToModes(int32_t bitmap)
{
    uint8_t modes = 0;
    if (bitmap & kGsmFamily)
        modes |= kModeGsm;
    if (bitmap & kUmtsFamily)
        modes |= kModeUmts;
    if (bitmap & kLteFamily)
        modes |= kModeLte;
    return modes;
}

RadioTechnology technology(AccessTech tech)
{
    switch (tech) {
    case AccessTech::Gsm: return RadioTechnology::GSM;
    case AccessTech::Gprs: return RadioTechnology::GPRS;
    case AccessTech::Edge: return RadioTechnology::EDGE;
    case AccessTech::Umts: return RadioTechnology::UMTS;
    case AccessTech::Lte: return RadioTechnology::LTE;
    case AccessTech::Iwlan: return RadioTechnology::IWLAN;
    default: return RadioTechnology::UNKNOWN;
    }
}

// Searching, denied and unknown are reported as their "emergency calls
// possible" variants, as the stock RIL does (NetRegState::SetEmergencyState):
// Android then shows "Emergency calls only" instead of "No service"
network::RegState regState(RegState state)
{
    switch (state) {
    case RegState::NotRegistered: return network::RegState::NOT_REG_MT_NOT_SEARCHING_OP;
    case RegState::Searching: return network::RegState::NOT_REG_MT_SEARCHING_OP_EM;
    case RegState::Home: return network::RegState::REG_HOME;
    case RegState::Roaming: return network::RegState::REG_ROAMING;
    case RegState::Denied: return network::RegState::REG_DENIED_EM;
    case RegState::EmergencyOnly: return network::RegState::NOT_REG_MT_NOT_SEARCHING_OP_EM;
    default: return network::RegState::UNKNOWN_EM;
    }
}

// Whether the modem is camped or looking for a cell: the stock RIL keeps
// the technology then, and clears it otherwise
bool emergencyPossible(RegState state)
{
    return state == RegState::Searching || state == RegState::Denied ||
           state == RegState::Unknown || state == RegState::EmergencyOnly;
}

void splitPlmn(const std::string &plmn, std::string &mcc, std::string &mnc)
{
    if (plmn.size() >= 5) {
        mcc = plmn.substr(0, 3);
        mnc = plmn.substr(3);
    }
}

network::OperatorInfo operatorInfo(const std::string &plmn)
{
    // The modem gives the PLMN only; the framework takes the name from the
    // SIM (SPN, EONS) or its own tables
    network::OperatorInfo info;
    info.alphaLong = plmn;
    info.alphaShort = plmn;
    info.operatorNumeric = plmn;
    info.status = network::OperatorInfo::STATUS_CURRENT;
    return info;
}

network::CellIdentity cellIdentity(const Registration &reg, const std::string &plmn)
{
    network::CellIdentity identity;
    if (!registered(reg.state))
        return identity;  // noinit

    std::string mcc, mnc;
    splitPlmn(plmn, mcc, mnc);

    switch (reg.tech) {
    case AccessTech::Lte: {
        network::CellIdentityLte lte;
        lte.mcc = mcc;
        lte.mnc = mnc;
        lte.ci = static_cast<int32_t>(reg.cell);
        lte.pci = kUnavailable;
        lte.tac = reg.area;
        lte.earfcn = kUnavailable;
        lte.operatorNames = operatorInfo(plmn);
        lte.bandwidth = kUnavailable;
        identity.set<network::CellIdentity::lte>(std::move(lte));
        break;
    }
    case AccessTech::Umts: {
        network::CellIdentityWcdma wcdma;
        wcdma.mcc = mcc;
        wcdma.mnc = mnc;
        wcdma.lac = reg.area;
        wcdma.cid = static_cast<int32_t>(reg.cell);
        wcdma.psc = kUnavailable;
        wcdma.uarfcn = kUnavailable;
        wcdma.operatorNames = operatorInfo(plmn);
        identity.set<network::CellIdentity::wcdma>(std::move(wcdma));
        break;
    }
    case AccessTech::Gsm:
    case AccessTech::Gprs:
    case AccessTech::Edge: {
        network::CellIdentityGsm gsm;
        gsm.mcc = mcc;
        gsm.mnc = mnc;
        gsm.lac = reg.area;
        gsm.cid = static_cast<int32_t>(reg.cell);
        gsm.arfcn = kUnavailable;
        gsm.bsic = static_cast<int8_t>(0xff);
        gsm.operatorNames = operatorInfo(plmn);
        identity.set<network::CellIdentity::gsm>(std::move(gsm));
        break;
    }
    default:
        break;
    }
    return identity;
}

network::RegStateResult regStateResult(const Registration &reg, const std::string &plmn)
{
    network::RegStateResult result;
    result.regState = regState(reg.state);
    result.rat = registered(reg.state) || emergencyPossible(reg.state) ? technology(reg.tech)
                                                                      : RadioTechnology::UNKNOWN;
    // The modem reports the 3GPP reject cause, which the enum uses as values
    result.reasonForDenial = reg.state == RegState::Denied
                                     ? static_cast<network::RegistrationFailCause>(reg.rejectCause)
                                     : network::RegistrationFailCause::NONE;
    result.cellIdentity = cellIdentity(reg, plmn);
    result.registeredPlmn = registered(reg.state) ? plmn : std::string();
    if (reg.tech == AccessTech::Lte) {
        // VoLTE support comes with IMS; until then no VoPS
        network::EutranRegistrationInfo eutran;
        eutran.lteVopsInfo.isVopsSupported = false;
        eutran.lteVopsInfo.isEmcBearerSupported = false;
        result.accessTechnologySpecificInfo.set<network::AccessTechnologySpecificInfo::eutranInfo>(
                std::move(eutran));
    }
    return result;
}

network::SignalStrength signalStrength(const Network &net)
{
    network::SignalStrength s;
    s.gsm.signalStrength = s.gsm.bitErrorRate = s.gsm.timingAdvance = kUnavailable;
    s.cdma.dbm = s.cdma.ecio = kUnavailable;
    s.evdo.dbm = s.evdo.ecio = s.evdo.signalNoiseRatio = kUnavailable;
    s.lte.signalStrength = s.lte.rsrp = s.lte.rsrq = s.lte.rssnr = kUnavailable;
    s.lte.cqi = s.lte.timingAdvance = s.lte.cqiTableIndex = kUnavailable;
    s.tdscdma.signalStrength = s.tdscdma.bitErrorRate = s.tdscdma.rscp = kUnavailable;
    s.wcdma.signalStrength = s.wcdma.bitErrorRate = s.wcdma.rscp = s.wcdma.ecno = kUnavailable;
    s.nr.ssRsrp = s.nr.ssRsrq = s.nr.ssSinr = kUnavailable;
    s.nr.csiRsrp = s.nr.csiRsrq = s.nr.csiSinr = kUnavailable;
    s.nr.csiCqiTableIndex = kUnavailable;

    // IPC_DISP_RSSI_INFO is the RSSI as -dBm; as ASU: dBm = 2 * ASU - 113
    const int rssi = net.signal();
    if (rssi <= 0)
        return s;
    int asu = (113 - rssi) / 2;
    asu = asu < 0 ? 0 : asu > 31 ? 31 : asu;

    const Registration &reg = registered(net.data().state) ? net.data() : net.voice();
    switch (reg.tech) {
    case AccessTech::Lte: {
        // Without RSRP the framework falls back to the RSSI for the level
        const LteSignal &lte = net.lteSignal();
        s.lte.signalStrength = lte.strength.value_or(asu);
        s.lte.rsrp = lte.rsrp.value_or(kUnavailable);
        s.lte.rsrq = lte.rsrq.value_or(kUnavailable);
        s.lte.rssnr = lte.snr.value_or(kUnavailable);
        s.lte.cqi = lte.cqi.value_or(kUnavailable);
        break;
    }
    case AccessTech::Umts:
        s.wcdma.signalStrength = asu;
        s.wcdma.bitErrorRate = 99;
        break;
    case AccessTech::Gsm:
    case AccessTech::Gprs:
    case AccessTech::Edge:
        s.gsm.signalStrength = asu;
        s.gsm.bitErrorRate = 99;
        break;
    default:
        break;
    }
    return s;
}

}  // namespace

RadioNetwork::RadioNetwork(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        core.ss.onNotification([this](const SsNotification &n) {
            if (!suppServiceNotifications_)
                return;
            network::SuppSvcNotification out;
            out.isMT = n.mt;
            out.code = n.code;
            out.index = n.index;
            out.type = n.toa;
            out.number = n.number;
            if (auto i = indication())
                i->suppSvcNotify(kUnsolicited, out);
        });
        core.network.onChanged([this, &core] {
            // Registration, operator and signal all arrive here; the framework
            // polls the state after networkStateChanged
            if (auto i = indication()) {
                i->networkStateChanged(kUnsolicited);
                if (indicationFilter_ & static_cast<int32_t>(network::IndicationFilter::SIGNAL_STRENGTH))
                    i->currentSignalStrength(kUnsolicited, signalStrength(core.network));
            }
        });
    });
}

::ndk::ScopedAStatus RadioNetwork::getAllowedNetworkTypesBitmap(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getAllowedNetworkTypesBitmapResponse(core ? ok(serial) : notAvailable(serial),
                                                    core ? modesToBitmap(core->network.modes()) : 0);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setAllowedNetworkTypesBitmap(int32_t serial,
                                                                int32_t networkTypeBitmap)
{
    stack_->post([this, serial, networkTypeBitmap](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setAllowedNetworkTypesBitmapResponse(notAvailable(serial));
            return;
        }
        uint8_t modes = bitmapToModes(networkTypeBitmap);
        if (core->supportedModes)
            modes &= core->supportedModes;
        if (!modes) {
            if (auto r = response())
                r->setAllowedNetworkTypesBitmapResponse(
                        responseInfo(serial, RadioError::INVALID_ARGUMENTS));
            return;
        }
        if (modes == core->network.modes()) {
            if (auto r = response())
                r->setAllowedNetworkTypesBitmapResponse(ok(serial));
            return;
        }
        core->network.setModes(modes, [this, serial](bool success) {
            if (auto r = response())
                r->setAllowedNetworkTypesBitmapResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getCellInfoList(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core) {
            r->getCellInfoListResponse(notAvailable(serial), {});
            return;
        }
        // Only the serving cell: neighbour cells need messages not decoded yet
        std::vector<network::CellInfo> cells;
        const Registration &reg = registered(core->network.data().state) ? core->network.data()
                                                                         : core->network.voice();
        const auto identity = cellIdentity(reg, core->network.operatorCode());
        const auto strength = signalStrength(core->network);
        network::CellInfo info;
        info.registered = true;
        info.connectionStatus = network::CellConnectionStatus::PRIMARY_SERVING;
        switch (identity.getTag()) {
        case network::CellIdentity::lte:
        {
            network::CellInfoLte lte;
            lte.cellIdentityLte = identity.get<network::CellIdentity::lte>();
            lte.signalStrengthLte = strength.lte;
            info.ratSpecificInfo.set<network::CellInfoRatSpecificInfo::lte>(std::move(lte));
        }
            cells.push_back(std::move(info));
            break;
        case network::CellIdentity::wcdma:
        {
            network::CellInfoWcdma wcdma;
            wcdma.cellIdentityWcdma = identity.get<network::CellIdentity::wcdma>();
            wcdma.signalStrengthWcdma = strength.wcdma;
            info.ratSpecificInfo.set<network::CellInfoRatSpecificInfo::wcdma>(std::move(wcdma));
        }
            cells.push_back(std::move(info));
            break;
        case network::CellIdentity::gsm:
        {
            network::CellInfoGsm gsm;
            gsm.cellIdentityGsm = identity.get<network::CellIdentity::gsm>();
            gsm.signalStrengthGsm = strength.gsm;
            info.ratSpecificInfo.set<network::CellInfoRatSpecificInfo::gsm>(std::move(gsm));
        }
            cells.push_back(std::move(info));
            break;
        default:
            break;
        }
        r->getCellInfoListResponse(ok(serial), cells);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getDataRegistrationState(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getDataRegistrationStateResponse(
                    core ? ok(serial) : notAvailable(serial),
                    core ? regStateResult(core->network.data(), core->network.operatorCode())
                         : network::RegStateResult{});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getVoiceRegistrationState(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getVoiceRegistrationStateResponse(
                    core ? ok(serial) : notAvailable(serial),
                    core ? regStateResult(core->network.voice(), core->network.operatorCode())
                         : network::RegStateResult{});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getVoiceRadioTechnology(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getVoiceRadioTechnologyResponse(
                    core ? ok(serial) : notAvailable(serial),
                    core ? technology(core->network.voice().tech) : RadioTechnology::UNKNOWN);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getNetworkSelectionMode(int32_t serial)
{
    // Automatic only: manual selection needs the network scan
    if (auto r = response())
        r->getNetworkSelectionModeResponse(ok(serial), false);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setNetworkSelectionModeAutomatic(int32_t serial)
{
    if (auto r = response())
        r->setNetworkSelectionModeAutomaticResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getOperator(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core) {
            r->getOperatorResponse(notAvailable(serial), "", "", "");
            return;
        }
        const bool reg = registered(core->network.voice().state) ||
                         registered(core->network.data().state);
        const std::string plmn = reg ? core->network.operatorCode() : std::string();
        // Without a name from the network the framework gets the code
        const std::string &full = core->network.operatorLongName();
        const std::string &brief = core->network.operatorShortName();
        r->getOperatorResponse(ok(serial), reg && !full.empty() ? full : plmn,
                               reg && !brief.empty() ? brief : plmn, plmn);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getSignalStrength(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getSignalStrengthResponse(core ? ok(serial) : notAvailable(serial),
                                         core ? signalStrength(core->network)
                                              : network::SignalStrength{});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::getUsageSetting(int32_t serial)
{
    if (auto r = response())
        r->getUsageSettingResponse(ok(serial), network::UsageSetting::VOICE_CENTRIC);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setUsageSetting(int32_t serial, network::UsageSetting usageSetting)
{
    if (auto r = response())
        r->setUsageSettingResponse(usageSetting == network::UsageSetting::VOICE_CENTRIC
                                           ? ok(serial)
                                           : responseInfo(serial, RadioError::INVALID_ARGUMENTS));
    return ::ndk::ScopedAStatus::ok();
}

// The framework drops signal strength reports while nobody looks at the
// screen (DeviceStateMonitor): that is the stock RIL's screen state, which
// the modem uses to send less. Registration changes still go up

::ndk::ScopedAStatus RadioNetwork::setIndicationFilter(int32_t serial, int32_t indicationFilter)
{
    indicationFilter_ = indicationFilter;
    const bool screenOn =
            indicationFilter & static_cast<int32_t>(network::IndicationFilter::SIGNAL_STRENGTH);
    stack_->post([screenOn](Core *core) {
        if (core)
            core->network.setScreenOn(screenOn);
    });
    if (auto r = response())
        r->setIndicationFilterResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setLinkCapacityReportingCriteria(
        int32_t serial, int32_t hysteresisMs, int32_t hysteresisDlKbps, int32_t hysteresisUlKbps,
        const std::vector<int32_t> &thresholdsDownlinkKbps,
        const std::vector<int32_t> &thresholdsUplinkKbps, AccessNetwork accessNetwork)
{
    (void)hysteresisMs;
    (void)hysteresisDlKbps;
    (void)hysteresisUlKbps;
    (void)thresholdsDownlinkKbps;
    (void)thresholdsUplinkKbps;
    (void)accessNetwork;
    if (auto r = response())
        r->setLinkCapacityReportingCriteriaResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setSignalStrengthReportingCriteria(
        int32_t serial, const std::vector<network::SignalThresholdInfo> &signalThresholdInfos)
{
    (void)signalThresholdInfos;
    if (auto r = response())
        r->setSignalStrengthReportingCriteriaResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setSuppServiceNotifications(int32_t serial, bool enable)
{
    // The modem always sends them; they are passed on when wanted
    suppServiceNotifications_ = enable;
    if (auto r = response())
        r->setSuppServiceNotificationsResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetwork::setBarringPassword(int32_t serial, const std::string &facility,
                                                      const std::string &oldPassword,
                                                      const std::string &newPassword)
{
    // The stock RIL sends the passwords alone, for every facility
    (void)facility;
    stack_->post([this, serial, oldPassword, newPassword](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setBarringPasswordResponse(notAvailable(serial));
            return;
        }
        core->ss.changeBarringPassword(oldPassword, newPassword, [this, serial](const SsStatus &st) {
            if (auto r = response())
                r->setBarringPasswordResponse(ssResult(serial, st.ok, st.code));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
