// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio HAL services. Each derives from the generated "not supported"
// base and overrides what the core supports.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "frontends/aidl/Stack.h"
#include "frontends/aidl/generated/NotSupportedConfig.h"
#include "frontends/aidl/generated/NotSupportedData.h"
#include "frontends/aidl/generated/NotSupportedMessaging.h"
#include "frontends/aidl/generated/NotSupportedModem.h"
#include "frontends/aidl/generated/NotSupportedNetwork.h"
#include "frontends/aidl/generated/NotSupportedSim.h"
#include "frontends/aidl/generated/NotSupportedVoice.h"

namespace shannond::hal {

namespace config = ::aidl::android::hardware::radio::config;
namespace data = ::aidl::android::hardware::radio::data;
namespace messaging = ::aidl::android::hardware::radio::messaging;
namespace modem = ::aidl::android::hardware::radio::modem;
namespace network = ::aidl::android::hardware::radio::network;
namespace sim = ::aidl::android::hardware::radio::sim;
namespace voice = ::aidl::android::hardware::radio::voice;

class RadioConfig : public NotSupportedConfig {
  public:
    explicit RadioConfig(std::shared_ptr<Stack> stack) : stack_(std::move(stack)) {}

    ::ndk::ScopedAStatus getHalDeviceCapabilities(int32_t serial) override;
    ::ndk::ScopedAStatus getNumOfLiveModems(int32_t serial) override;
    ::ndk::ScopedAStatus getPhoneCapability(int32_t serial) override;
    ::ndk::ScopedAStatus getSimSlotsStatus(int32_t serial) override;
    ::ndk::ScopedAStatus setPreferredDataModem(int32_t serial, int8_t modemId) override;

  private:
    std::shared_ptr<Stack> stack_;
};

class RadioModem : public NotSupportedModem {
  public:
    explicit RadioModem(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus enableModem(int32_t serial, bool on) override;
    ::ndk::ScopedAStatus getBasebandVersion(int32_t serial) override;
    ::ndk::ScopedAStatus getImei(int32_t serial) override;
    ::ndk::ScopedAStatus getModemStackStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getRadioCapability(int32_t serial) override;
    ::ndk::ScopedAStatus requestShutdown(int32_t serial) override;
    ::ndk::ScopedAStatus sendDeviceState(int32_t serial, modem::DeviceStateType stateType,
                                         bool state) override;
    ::ndk::ScopedAStatus setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                       bool preferredForEmergencyCall) override;

  protected:
    void onResponseFunctionsSet() override;

  private:
    void announce(Core &core);

    std::shared_ptr<Stack> stack_;
    bool lastRadioOn_ = false;  // core thread only
};

class RadioSim : public NotSupportedSim {
  public:
    explicit RadioSim(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus areUiccApplicationsEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus enableUiccApplications(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus getFacilityLockForApp(int32_t serial, const std::string &facility,
                                               const std::string &password, int32_t serviceClass,
                                               const std::string &appId) override;
    ::ndk::ScopedAStatus setFacilityLockForApp(int32_t serial, const std::string &facility,
                                               bool lockState, const std::string &password,
                                               int32_t serviceClass, const std::string &appId) override;
    ::ndk::ScopedAStatus getIccCardStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getImsiForApp(int32_t serial, const std::string &aid) override;
    ::ndk::ScopedAStatus iccCloseLogicalChannelWithSessionInfo(
            int32_t serial, const sim::SessionInfo &sessionInfo) override;
    ::ndk::ScopedAStatus iccIoForApp(int32_t serial, const sim::IccIo &iccIo) override;
    ::ndk::ScopedAStatus iccOpenLogicalChannel(int32_t serial, const std::string &aid,
                                               int32_t p2) override;
    ::ndk::ScopedAStatus iccTransmitApduBasicChannel(int32_t serial,
                                                     const sim::SimApdu &message) override;
    ::ndk::ScopedAStatus iccTransmitApduLogicalChannel(int32_t serial,
                                                       const sim::SimApdu &message) override;
    ::ndk::ScopedAStatus reportStkServiceIsRunning(int32_t serial) override;
    ::ndk::ScopedAStatus sendEnvelope(int32_t serial, const std::string &contents) override;
    ::ndk::ScopedAStatus sendEnvelopeWithStatus(int32_t serial,
                                                const std::string &contents) override;
    ::ndk::ScopedAStatus sendTerminalResponseToSim(int32_t serial,
                                                   const std::string &contents) override;
    ::ndk::ScopedAStatus requestIccSimAuthentication(int32_t serial, int32_t authContext,
                                                     const std::string &authData,
                                                     const std::string &aid) override;
    ::ndk::ScopedAStatus supplyIccPinForApp(int32_t serial, const std::string &pin,
                                            const std::string &aid) override;

  private:
    void transmit(int32_t serial, const sim::SimApdu &message, bool logical);

    std::shared_ptr<Stack> stack_;
    // The framework's SIM toolkit service has said it runs (binder threads and the core's)
    std::atomic<bool> stkRunning_{false};
};

class RadioNetwork : public NotSupportedNetwork {
  public:
    explicit RadioNetwork(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus getAllowedNetworkTypesBitmap(int32_t serial) override;
    ::ndk::ScopedAStatus getCellInfoList(int32_t serial) override;
    ::ndk::ScopedAStatus getDataRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t serial) override;
    ::ndk::ScopedAStatus getOperator(int32_t serial) override;
    ::ndk::ScopedAStatus getSignalStrength(int32_t serial) override;
    ::ndk::ScopedAStatus getUsageSetting(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus setAllowedNetworkTypesBitmap(int32_t serial,
                                                      int32_t networkTypeBitmap) override;
    ::ndk::ScopedAStatus setBarringPassword(int32_t serial, const std::string &facility,
                                            const std::string &oldPassword,
                                            const std::string &newPassword) override;
    ::ndk::ScopedAStatus setIndicationFilter(int32_t serial, int32_t indicationFilter) override;
    ::ndk::ScopedAStatus setLinkCapacityReportingCriteria(
            int32_t serial, int32_t hysteresisMs, int32_t hysteresisDlKbps,
            int32_t hysteresisUlKbps, const std::vector<int32_t> &thresholdsDownlinkKbps,
            const std::vector<int32_t> &thresholdsUplinkKbps,
            ::aidl::android::hardware::radio::AccessNetwork accessNetwork) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeAutomatic(int32_t serial) override;
    ::ndk::ScopedAStatus setSignalStrengthReportingCriteria(
            int32_t serial,
            const std::vector<network::SignalThresholdInfo> &signalThresholdInfos) override;
    ::ndk::ScopedAStatus setSuppServiceNotifications(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus setUsageSetting(int32_t serial,
                                         network::UsageSetting usageSetting) override;

  private:
    std::shared_ptr<Stack> stack_;
    std::atomic<int32_t> indicationFilter_{-1};  // IndicationFilter::ALL
    std::atomic<bool> suppServiceNotifications_{false};
};

class RadioData : public NotSupportedData {
  public:
    explicit RadioData(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus deactivateDataCall(int32_t serial, int32_t cid,
                                            data::DataRequestReason reason) override;
    ::ndk::ScopedAStatus getDataCallList(int32_t serial) override;
    ::ndk::ScopedAStatus setDataAllowed(int32_t serial, bool allow) override;
    ::ndk::ScopedAStatus setDataProfile(int32_t serial,
                                        const std::vector<data::DataProfileInfo> &profiles) override;
    ::ndk::ScopedAStatus setInitialAttachApn(
            int32_t serial, const std::optional<data::DataProfileInfo> &dataProfileInfo) override;
    ::ndk::ScopedAStatus setupDataCall(int32_t serial,
                                       ::aidl::android::hardware::radio::AccessNetwork accessNetwork,
                                       const data::DataProfileInfo &dataProfileInfo,
                                       bool roamingAllowed, data::DataRequestReason reason,
                                       const std::vector<data::LinkAddress> &addresses,
                                       const std::vector<std::string> &dnses, int32_t pduSessionId,
                                       const std::optional<data::SliceInfo> &sliceInfo,
                                       bool matchAllRuleAllowed) override;

  private:
    std::vector<data::SetupDataCallResult> list(Core &core);

    std::shared_ptr<Stack> stack_;
};

class RadioMessaging : public NotSupportedMessaging {
  public:
    explicit RadioMessaging(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus acknowledgeLastIncomingGsmSms(
            int32_t serial, bool success, messaging::SmsAcknowledgeFailCause cause) override;
    ::ndk::ScopedAStatus deleteSmsOnSim(int32_t serial, int32_t index) override;
    ::ndk::ScopedAStatus getGsmBroadcastConfig(int32_t serial) override;
    ::ndk::ScopedAStatus getSmscAddress(int32_t serial) override;
    ::ndk::ScopedAStatus reportSmsMemoryStatus(int32_t serial, bool available) override;
    ::ndk::ScopedAStatus sendSms(int32_t serial, const messaging::GsmSmsMessage &message) override;
    ::ndk::ScopedAStatus sendSmsExpectMore(int32_t serial,
                                           const messaging::GsmSmsMessage &message) override;
    ::ndk::ScopedAStatus setGsmBroadcastActivation(int32_t serial, bool activate) override;
    ::ndk::ScopedAStatus setGsmBroadcastConfig(
            int32_t serial,
            const std::vector<messaging::GsmBroadcastSmsConfigInfo> &configInfo) override;
    ::ndk::ScopedAStatus setSmscAddress(int32_t serial, const std::string &smsc) override;
    ::ndk::ScopedAStatus writeSmsToSim(int32_t serial,
                                       const messaging::SmsWriteArgs &args) override;

  private:
    void send(int32_t serial, const messaging::GsmSmsMessage &message, bool more);

    std::shared_ptr<Stack> stack_;
};

class RadioVoice : public NotSupportedVoice {
  public:
    explicit RadioVoice(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus acceptCall(int32_t serial) override;
    ::ndk::ScopedAStatus cancelPendingUssd(int32_t serial) override;
    ::ndk::ScopedAStatus conference(int32_t serial) override;
    ::ndk::ScopedAStatus dial(int32_t serial, const voice::Dial &dialInfo) override;
    ::ndk::ScopedAStatus emergencyDial(int32_t serial, const voice::Dial &dialInfo,
                                       int32_t categories, const std::vector<std::string> &urns,
                                       voice::EmergencyCallRouting routing,
                                       bool hasKnownUserIntentEmergency, bool isTesting) override;
    ::ndk::ScopedAStatus explicitCallTransfer(int32_t serial) override;
    ::ndk::ScopedAStatus getCallForwardStatus(int32_t serial,
                                              const voice::CallForwardInfo &callInfo) override;
    ::ndk::ScopedAStatus getCallWaiting(int32_t serial, int32_t serviceClass) override;
    ::ndk::ScopedAStatus getClip(int32_t serial) override;
    ::ndk::ScopedAStatus getClir(int32_t serial) override;
    ::ndk::ScopedAStatus getCurrentCalls(int32_t serial) override;
    ::ndk::ScopedAStatus getLastCallFailCause(int32_t serial) override;
    ::ndk::ScopedAStatus getMute(int32_t serial) override;
    ::ndk::ScopedAStatus getPreferredVoicePrivacy(int32_t serial) override;
    ::ndk::ScopedAStatus getTtyMode(int32_t serial) override;
    ::ndk::ScopedAStatus hangup(int32_t serial, int32_t gsmIndex) override;
    ::ndk::ScopedAStatus hangupForegroundResumeBackground(int32_t serial) override;
    ::ndk::ScopedAStatus hangupWaitingOrBackground(int32_t serial) override;
    ::ndk::ScopedAStatus isVoNrEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus rejectCall(int32_t serial) override;
    ::ndk::ScopedAStatus sendDtmf(int32_t serial, const std::string &s) override;
    ::ndk::ScopedAStatus sendUssd(int32_t serial, const std::string &ussd) override;
    ::ndk::ScopedAStatus separateConnection(int32_t serial, int32_t gsmIndex) override;
    ::ndk::ScopedAStatus setCallForward(int32_t serial, const voice::CallForwardInfo &callInfo) override;
    ::ndk::ScopedAStatus setCallWaiting(int32_t serial, bool enable, int32_t serviceClass) override;
    ::ndk::ScopedAStatus setClir(int32_t serial, int32_t status) override;
    ::ndk::ScopedAStatus setMute(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus setTtyMode(int32_t serial, voice::TtyMode mode) override;
    ::ndk::ScopedAStatus startDtmf(int32_t serial, const std::string &s) override;
    ::ndk::ScopedAStatus stopDtmf(int32_t serial) override;
    ::ndk::ScopedAStatus switchWaitingOrHoldingAndActive(int32_t serial) override;

  private:
    enum class Op {
        Hangup,
        Reject,
        HangupWaitingOrBackground,
        HangupForegroundResumeBackground,
        Switch,
        Conference,
        Separate,
        Transfer,
    };
    void handle(int32_t serial, Op op, int32_t gsmIndex = 0);

    std::shared_ptr<Stack> stack_;
    std::atomic<bool> mute_{false};
    std::mutex dtmfLock_;
    char dtmfTone_ = 0;
};

}  // namespace shannond::hal
