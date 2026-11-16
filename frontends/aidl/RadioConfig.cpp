// SPDX-License-Identifier: GPL-2.0-or-later
//
// One modem with one SIM slot (the A3's second slot is the microSD tray's
// alternative and is not used).
#include "frontends/aidl/Services.h"

namespace modemd::hal {

::ndk::ScopedAStatus RadioConfig::getHalDeviceCapabilities(int32_t serial)
{
    if (auto r = response())
        r->getHalDeviceCapabilitiesResponse(ok(serial), false);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfig::getNumOfLiveModems(int32_t serial)
{
    if (auto r = response())
        r->getNumOfLiveModemsResponse(ok(serial), 1);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfig::getPhoneCapability(int32_t serial)
{
    config::PhoneCapability capability;
    capability.maxActiveData = 1;
    capability.maxActiveInternetData = 1;
    capability.isInternetLingeringSupported = false;
    capability.logicalModemIds = {0};
    capability.maxActiveVoice = 1;
    if (auto r = response())
        r->getPhoneCapabilityResponse(ok(serial), capability);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfig::getSimSlotsStatus(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        config::SimSlotStatus slot;
        config::SimPortInfo port;
        port.logicalSlotId = 0;
        port.portActive = true;
        if (core) {
            const SimState state = core->sim.state();
            slot.cardState = state == SimState::Absent ? sim::CardStatus::STATE_ABSENT
                             : state == SimState::Error ? sim::CardStatus::STATE_ERROR
                                                        : sim::CardStatus::STATE_PRESENT;
            port.iccId = core->sim.iccid();
        } else {
            slot.cardState = sim::CardStatus::STATE_ABSENT;
        }
        slot.portInfo = {port};
        if (auto r = response())
            r->getSimSlotsStatusResponse(ok(serial), {slot});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfig::setPreferredDataModem(int32_t serial, int8_t modemId)
{
    if (auto r = response())
        r->setPreferredDataModemResponse(modemId == 0 ? ok(serial)
                                                      : responseInfo(serial, RadioError::INVALID_ARGUMENTS));
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace modemd::hal
