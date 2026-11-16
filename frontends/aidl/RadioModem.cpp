// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include <aidl/android/hardware/radio/RadioAccessFamily.h>

namespace modemd::hal {

using ::aidl::android::hardware::radio::RadioAccessFamily;

namespace {

int32_t supportedRaf()
{
    return static_cast<int32_t>(RadioAccessFamily::GSM) |
           static_cast<int32_t>(RadioAccessFamily::GPRS) |
           static_cast<int32_t>(RadioAccessFamily::EDGE) |
           static_cast<int32_t>(RadioAccessFamily::UMTS) |
           static_cast<int32_t>(RadioAccessFamily::HSDPA) |
           static_cast<int32_t>(RadioAccessFamily::HSUPA) |
           static_cast<int32_t>(RadioAccessFamily::HSPA) |
           static_cast<int32_t>(RadioAccessFamily::HSPAP) |
           static_cast<int32_t>(RadioAccessFamily::LTE);
}

modem::RadioState radioState(Core &core)
{
    return core.network.radioOn() ? modem::RadioState::ON : modem::RadioState::OFF;
}

}  // namespace

RadioModem::RadioModem(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        announce(core);
        core.network.onChanged([this, &core] {
            if (core.network.radioOn() != lastRadioOn_) {
                lastRadioOn_ = core.network.radioOn();
                if (auto i = indication())
                    i->radioStateChanged(kUnsolicited, radioState(core));
            }
        });
    });
}

void RadioModem::announce(Core &core)
{
    if (auto i = indication()) {
        i->rilConnected(kUnsolicited);
        i->radioStateChanged(kUnsolicited, radioState(core));
    }
}

void RadioModem::onResponseFunctionsSet()
{
    // The framework may connect after the modem came up
    stack_->post([this](Core *core) {
        if (core)
            announce(*core);
    });
}

::ndk::ScopedAStatus RadioModem::enableModem(int32_t serial, bool on)
{
    (void)on;  // one modem, always enabled
    if (auto r = response())
        r->enableModemResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::getBasebandVersion(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->getBasebandVersionResponse(core ? ok(serial) : notAvailable(serial),
                                          core ? core->network.version() : std::string());
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::getImei(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core || core->network.imei().empty()) {
            r->getImeiResponse(notAvailable(serial), std::nullopt);
            return;
        }
        modem::ImeiInfo info;
        info.type = modem::ImeiInfo::ImeiType::PRIMARY;
        info.imei = core->network.imei();
        info.svn = "00";
        r->getImeiResponse(ok(serial), info);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::getModemStackStatus(int32_t serial)
{
    if (auto r = response())
        r->getModemStackStatusResponse(ok(serial), stack_->connected());
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::getRadioCapability(int32_t serial)
{
    modem::RadioCapability rc;
    rc.session = 0;
    rc.phase = modem::RadioCapability::PHASE_CONFIGURED;
    rc.raf = supportedRaf();
    rc.logicalModemUuid = "com.samsung.modem0";
    rc.status = modem::RadioCapability::STATUS_NONE;
    if (auto r = response())
        r->getRadioCapabilityResponse(ok(serial), rc);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::requestShutdown(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (!core) {
            if (auto r = response())
                r->requestShutdownResponse(ok(serial));
            return;
        }
        core->network.setRadioPower(false, [this, serial](bool) {
            if (auto r = response())
                r->requestShutdownResponse(ok(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::sendDeviceState(int32_t serial, modem::DeviceStateType stateType,
                                                 bool state)
{
    (void)stateType;
    (void)state;  // screen and charging state: nothing to tell the modem yet
    if (auto r = response())
        r->sendDeviceStateResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModem::setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                               bool preferredForEmergencyCall)
{
    (void)forEmergencyCall;
    (void)preferredForEmergencyCall;
    stack_->post([this, serial, powerOn](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setRadioPowerResponse(notAvailable(serial));
            return;
        }
        core->network.setRadioPower(powerOn, [this, serial, core, powerOn](bool success) {
            if (auto r = response())
                r->setRadioPowerResponse(success ? ok(serial) : failed(serial));
            // The modem confirms with IPC_PWR_PHONE_STATE; until then report the request
            if (success)
                if (auto i = indication())
                    i->radioStateChanged(kUnsolicited,
                                         powerOn ? modem::RadioState::ON : modem::RadioState::OFF);
            (void)core;
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace modemd::hal
