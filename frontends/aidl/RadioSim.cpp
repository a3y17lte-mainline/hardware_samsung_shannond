// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include <algorithm>
#include <cctype>

#include "frontends/aidl/Codec.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond::hal {

namespace {

constexpr int kAuthContextEapSim = 128;
constexpr int kAuthContextEapAka = 129;
constexpr uint8_t kGetResponse = 0xc0;

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

const SimApp *findApp(Core &core, const std::string &aid)
{
    if (aid.empty())
        return nullptr;
    for (const SimApp &app : core.sim.apps())
        if (toHex(app.aid) == upper(aid))
            return &app;
    return nullptr;
}

sim::IccIoResult apduResult(const Bytes &response)
{
    sim::IccIoResult result;
    if (response.size() >= 2) {
        result.sw1 = response[response.size() - 2];
        result.sw2 = response[response.size() - 1];
        result.simResponse = toHex(Bytes(response.begin(), response.end() - 2));
    }
    return result;
}

// The modem gives one PIN status per application (0 fine, 1 PIN1 wanted,
// 2 PUK1, 3 PIN2, 4 PUK2, 5 blocked for good), read as the stock RIL does
sim::PinState pin1State(uint8_t raw, bool ready)
{
    if (ready)
        return sim::PinState::ENABLED_VERIFIED;
    switch (raw) {
    case 0: return sim::PinState::ENABLED_VERIFIED;
    case 1: return sim::PinState::ENABLED_NOT_VERIFIED;
    case 2: return sim::PinState::ENABLED_BLOCKED;
    case 5: return sim::PinState::ENABLED_PERM_BLOCKED;
    default: return sim::PinState::UNKNOWN;
    }
}

sim::PinState pin2State(uint8_t raw)
{
    switch (raw) {
    case 0: return sim::PinState::ENABLED_VERIFIED;
    case 3: return sim::PinState::ENABLED_NOT_VERIFIED;
    case 4: return sim::PinState::ENABLED_BLOCKED;
    case 5: return sim::PinState::ENABLED_PERM_BLOCKED;
    default: return sim::PinState::UNKNOWN;
    }
}

// The personalisation lock, as the stock RIL reports it (a facility lock of
// 2 asks for the lock's PUK)
sim::PersoSubstate persoSubstate(uint8_t lock, uint8_t facility)
{
    const bool puk = facility == 2;
    switch (lock) {
    case IPC_SEC_PIN_STATUS_SIM_LOCK_REQUIRED: return sim::PersoSubstate::SIM_SIM;
    case IPC_SEC_PIN_STATUS_LOCK_PN:
        return puk ? sim::PersoSubstate::SIM_NETWORK_PUK : sim::PersoSubstate::SIM_NETWORK;
    case IPC_SEC_PIN_STATUS_LOCK_PU:
        return puk ? sim::PersoSubstate::SIM_NETWORK_SUBSET_PUK
                   : sim::PersoSubstate::SIM_NETWORK_SUBSET;
    case IPC_SEC_PIN_STATUS_LOCK_PP:
        return puk ? sim::PersoSubstate::SIM_SERVICE_PROVIDER_PUK
                   : sim::PersoSubstate::SIM_SERVICE_PROVIDER;
    case IPC_SEC_PIN_STATUS_LOCK_PC:
        return puk ? sim::PersoSubstate::SIM_CORPORATE_PUK : sim::PersoSubstate::SIM_CORPORATE;
    default: return sim::PersoSubstate::UNKNOWN;
    }
}

}  // namespace

RadioSim::RadioSim(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        core.sim.onChanged([this] {
            if (auto i = indication())
                i->simStatusChanged(kUnsolicited);
        });
        if (auto i = indication())
            i->simStatusChanged(kUnsolicited);

        // SIM toolkit: the commands are the framework's to read and answer
        core.sat.onProactiveCommand([this](const Bytes &command) {
            if (auto i = indication())
                i->stkProactiveCommand(kUnsolicited, toHex(command));
        });
        core.sat.onSessionEnd([this] {
            if (auto i = indication())
                i->stkSessionEnd(kUnsolicited);
        });
        if (stkRunning_)
            core.sat.clientRunning();
    });
}

::ndk::ScopedAStatus RadioSim::getIccCardStatus(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        sim::CardStatus status;
        status.cardState = sim::CardStatus::STATE_ABSENT;
        status.universalPinState = sim::PinState::UNKNOWN;
        status.gsmUmtsSubscriptionAppIndex = -1;
        status.cdmaSubscriptionAppIndex = -1;
        status.imsSubscriptionAppIndex = -1;
        status.slotMap.physicalSlotId = 0;
        status.slotMap.portId = 0;

        if (core) {
            const SimState state = core->sim.state();
            status.cardState = state == SimState::Absent ? sim::CardStatus::STATE_ABSENT
                               : state == SimState::Error ? sim::CardStatus::STATE_ERROR
                                                          : sim::CardStatus::STATE_PRESENT;
            status.iccid = core->sim.iccid();

            int appState = sim::AppStatus::APP_STATE_DETECTED;
            switch (state) {
            case SimState::Ready: appState = sim::AppStatus::APP_STATE_READY; break;
            case SimState::PinRequired: appState = sim::AppStatus::APP_STATE_PIN; break;
            case SimState::PukRequired:
            case SimState::PermanentlyBlocked:
                appState = sim::AppStatus::APP_STATE_PUK;
                break;
            case SimState::NetworkLocked: appState = sim::AppStatus::APP_STATE_SUBSCRIPTION_PERSO; break;
            default: break;
            }

            const auto &apps = core->sim.apps();
            for (size_t i = 0; i < apps.size(); i++) {
                sim::AppStatus app;
                app.appType = androidAppType(apps[i].type);
                app.appState = appState;
                app.persoSubstate = state == SimState::NetworkLocked
                                            ? persoSubstate(core->sim.lockStatus(),
                                                            core->sim.facilityLock())
                                            : sim::PersoSubstate::READY;
                app.aidPtr = toHex(apps[i].aid);
                app.pin1 = state == SimState::PermanentlyBlocked
                                   ? sim::PinState::ENABLED_PERM_BLOCKED
                                   : pin1State(apps[i].pin1State, state == SimState::Ready);
                app.pin2 = pin2State(apps[i].pin2State);
                status.applications.push_back(app);

                if (app.appType == sim::AppStatus::APP_TYPE_USIM ||
                    (app.appType == sim::AppStatus::APP_TYPE_SIM &&
                     status.gsmUmtsSubscriptionAppIndex < 0))
                    status.gsmUmtsSubscriptionAppIndex = static_cast<int32_t>(i);
                if (app.appType == sim::AppStatus::APP_TYPE_ISIM)
                    status.imsSubscriptionAppIndex = static_cast<int32_t>(i);
            }
            if (!status.applications.empty())
                status.universalPinState = status.applications[0].pin1;
        }

        if (auto r = response())
            r->getIccCardStatusResponse(ok(serial), status);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::getImsiForApp(int32_t serial, const std::string &aid)
{
    (void)aid;  // the IMSI is the USIM's; the ISIM has none
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core || core->sim.imsi().empty())
            r->getImsiForAppResponse(notAvailable(serial), "");
        else
            r->getImsiForAppResponse(ok(serial), core->sim.imsi());
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::iccIoForApp(int32_t serial, const sim::IccIo &iccIo)
{
    stack_->post([this, serial, iccIo](Core *core) {
        if (!core) {
            if (auto r = response())
                r->iccIoForAppResponse(notAvailable(serial), {});
            return;
        }
        const Bytes data = fromHex(iccIo.data).value_or(Bytes{});
        const SimApp *app = findApp(*core, iccIo.aid);
        core->sim.io(static_cast<uint8_t>(iccIo.command), static_cast<uint16_t>(iccIo.fileId),
                     parsePath(iccIo.path), static_cast<uint8_t>(iccIo.p1),
                     static_cast<uint8_t>(iccIo.p2), static_cast<uint8_t>(iccIo.p3), data, app,
                     [this, serial, iccIo](std::optional<SimIoResult> result) {
            auto r = response();
            if (!r)
                return;
            if (!result) {
                r->iccIoForAppResponse(failed(serial), {});
                return;
            }
            sim::IccIoResult out;
            out.sw1 = result->sw1;
            out.sw2 = result->sw2;
            Bytes payload = result->data;
            if (iccIo.command == kGetResponse)
                if (auto converted = fcpToSimResponse(payload, static_cast<uint16_t>(iccIo.fileId)))
                    payload = *converted;
            out.simResponse = toHex(payload);
            r->iccIoForAppResponse(ok(serial), out);
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::iccOpenLogicalChannel(int32_t serial, const std::string &aid,
                                                     int32_t p2)
{
    stack_->post([this, serial, aid, p2](Core *core) {
        if (!core) {
            if (auto r = response())
                r->iccOpenLogicalChannelResponse(notAvailable(serial), 0, {});
            return;
        }
        // MANAGE CHANNEL (open), then SELECT the application on the new channel
        core->sim.transmitApdu(Bytes{0x00, 0x70, 0x00, 0x00, 0x01},
                               [this, serial, aid, p2, core](std::optional<Bytes> opened) {
            if (!opened || opened->size() != 3 || (*opened)[1] != 0x90) {
                if (auto r = response())
                    r->iccOpenLogicalChannelResponse(responseInfo(serial, RadioError::MISSING_RESOURCE),
                                                     0, {});
                return;
            }
            const int32_t channel = (*opened)[0];
            const Bytes aidBytes = fromHex(aid).value_or(Bytes{});
            if (aidBytes.empty()) {
                if (auto r = response())
                    r->iccOpenLogicalChannelResponse(ok(serial), channel, {0x90, 0x00});
                return;
            }
            Bytes select{channelCla(0, channel), 0xa4, 0x04, static_cast<uint8_t>(p2),
                         static_cast<uint8_t>(aidBytes.size())};
            select.insert(select.end(), aidBytes.begin(), aidBytes.end());
            core->sim.transmitApdu(select, [this, serial, channel, core](std::optional<Bytes> selected) {
                const bool good = selected && selected->size() >= 2 &&
                                  ((*selected)[selected->size() - 2] == 0x90 ||
                                   (*selected)[selected->size() - 2] == 0x61);
                if (!good) {
                    core->sim.transmitApdu(Bytes{0x00, 0x70, 0x80, static_cast<uint8_t>(channel), 0x00},
                                           [](std::optional<Bytes>) {});
                    if (auto r = response())
                        r->iccOpenLogicalChannelResponse(
                                responseInfo(serial, RadioError::NO_SUCH_ELEMENT), 0, {});
                    return;
                }
                if (auto r = response())
                    r->iccOpenLogicalChannelResponse(ok(serial), channel, *selected);
            });
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::iccCloseLogicalChannelWithSessionInfo(
        int32_t serial, const sim::SessionInfo &sessionInfo)
{
    const int32_t channel = sessionInfo.sessionId;
    stack_->post([this, serial, channel](Core *core) {
        if (!core) {
            if (auto r = response())
                r->iccCloseLogicalChannelWithSessionInfoResponse(notAvailable(serial));
            return;
        }
        core->sim.transmitApdu(Bytes{0x00, 0x70, 0x80, static_cast<uint8_t>(channel), 0x00},
                               [this, serial](std::optional<Bytes> closed) {
            const bool good = closed && closed->size() >= 2 && (*closed)[closed->size() - 2] == 0x90;
            if (auto r = response())
                r->iccCloseLogicalChannelWithSessionInfoResponse(good ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

void RadioSim::transmit(int32_t serial, const sim::SimApdu &message, bool logical)
{
    stack_->post([this, serial, message, logical](Core *core) {
        auto respond = [this, serial, logical](RadioResponseInfo info, const sim::IccIoResult &result) {
            if (auto r = response()) {
                if (logical)
                    r->iccTransmitApduLogicalChannelResponse(info, result);
                else
                    r->iccTransmitApduBasicChannelResponse(info, result);
            }
        };
        if (!core) {
            respond(notAvailable(serial), {});
            return;
        }
        Bytes apdu{channelCla(message.cla, logical ? message.sessionId : 0),
                   static_cast<uint8_t>(message.instruction), static_cast<uint8_t>(message.p1),
                   static_cast<uint8_t>(message.p2)};
        const Bytes data = fromHex(message.data).value_or(Bytes{});
        if (!data.empty()) {
            apdu.push_back(static_cast<uint8_t>(data.size()));
            apdu.insert(apdu.end(), data.begin(), data.end());
        } else if (message.p3 >= 0) {
            apdu.push_back(static_cast<uint8_t>(message.p3));
        }
        core->sim.transmitApdu(apdu, [serial, respond](std::optional<Bytes> answer) {
            if (!answer)
                respond(failed(serial), {});
            else
                respond(ok(serial), apduResult(*answer));
        });
    });
}

::ndk::ScopedAStatus RadioSim::iccTransmitApduBasicChannel(int32_t serial, const sim::SimApdu &message)
{
    transmit(serial, message, false);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::iccTransmitApduLogicalChannel(int32_t serial,
                                                             const sim::SimApdu &message)
{
    transmit(serial, message, true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::requestIccSimAuthentication(int32_t serial, int32_t authContext,
                                                           const std::string &authData,
                                                           const std::string &aid)
{
    (void)aid;  // the modem picks the application itself (type 0: AKA)
    if (authContext != kAuthContextEapAka) {
        (void)kAuthContextEapSim;
        if (auto r = response())
            r->requestIccSimAuthenticationResponse(notSupported(serial), {});
        return ::ndk::ScopedAStatus::ok();
    }

    stack_->post([this, serial, authData](Core *core) {
        auto respond = [this, serial](RadioResponseInfo info, const sim::IccIoResult &result) {
            if (auto r = response())
                r->requestIccSimAuthenticationResponse(info, result);
        };
        if (!core) {
            respond(notAvailable(serial), {});
            return;
        }

        // Base64 of: length, RAND, length, AUTN
        auto data = fromBase64(authData);
        auto challenge = data ? parseAkaChallenge(*data) : std::nullopt;
        if (!challenge) {
            respond(responseInfo(serial, RadioError::INVALID_ARGUMENTS), {});
            return;
        }
        core->sim.authenticate(0, challenge->rand, challenge->autn,
                               [serial, respond](std::optional<SimAuthResult> result) {
            if (!result) {
                respond(failed(serial), {});
                return;
            }
            const AkaAnswer answer = akaAnswer(result->status, result->values);
            sim::IccIoResult out;
            out.sw1 = answer.sw1;
            out.sw2 = answer.sw2;
            out.simResponse = toBase64(answer.data);
            respond(ok(serial), out);
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::supplyIccPinForApp(int32_t serial, const std::string &pin,
                                                  const std::string &aid)
{
    (void)aid;
    // Only ever on the user's request: the framework asks for it from the PIN screen
    stack_->post([this, serial, pin](Core *core) {
        if (!core) {
            if (auto r = response())
                r->supplyIccPinForAppResponse(notAvailable(serial), -1);
            return;
        }
        core->sim.enterPin(pin, [this, serial](bool success) {
            if (auto r = response())
                r->supplyIccPinForAppResponse(
                        success ? ok(serial) : responseInfo(serial, RadioError::PASSWORD_INCORRECT), -1);
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::getFacilityLockForApp(int32_t serial, const std::string &facility,
                                                     const std::string &password,
                                                     int32_t serviceClass, const std::string &appId)
{
    (void)password;
    (void)serviceClass;
    (void)appId;
    stack_->post([this, serial, facility](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core) {
            r->getFacilityLockForAppResponse(notAvailable(serial), 0);
            return;
        }
        int32_t locked = 0;
        if (facility == "SC" && !core->sim.apps().empty())
            locked = core->sim.apps()[0].pin1State == 0 ? 0 : 1;
        r->getFacilityLockForAppResponse(ok(serial), locked);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::areUiccApplicationsEnabled(int32_t serial)
{
    if (auto r = response())
        r->areUiccApplicationsEnabledResponse(ok(serial), true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::enableUiccApplications(int32_t serial, bool enable)
{
    (void)enable;
    if (auto r = response())
        r->enableUiccApplicationsResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::reportStkServiceIsRunning(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (auto r = response())
            r->reportStkServiceIsRunningResponse(ok(serial));
        // Commands the card sent before now were kept for this moment
        stkRunning_ = true;
        if (core)
            core->sat.clientRunning();
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::sendTerminalResponseToSim(int32_t serial,
                                                         const std::string &contents)
{
    stack_->post([this, serial, contents](Core *core) {
        auto bytes = fromHex(contents);
        if (!core || !bytes) {
            if (auto r = response())
                r->sendTerminalResponseToSimResponse(
                        core ? responseInfo(serial, RadioError::INVALID_ARGUMENTS)
                             : notAvailable(serial));
            return;
        }
        core->sat.terminalResponse(*bytes, [this, serial](bool success) {
            if (auto r = response())
                r->sendTerminalResponseToSimResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::sendEnvelope(int32_t serial, const std::string &contents)
{
    stack_->post([this, serial, contents](Core *core) {
        auto bytes = fromHex(contents);
        if (!core || !bytes) {
            if (auto r = response())
                r->sendEnvelopeResponse(core ? responseInfo(serial, RadioError::INVALID_ARGUMENTS)
                                             : notAvailable(serial),
                                        {});
            return;
        }
        core->sat.envelope(*bytes, [this, serial](std::optional<EnvelopeResult> result) {
            if (auto r = response())
                r->sendEnvelopeResponse(result ? ok(serial) : failed(serial),
                                        result ? toHex(result->data) : std::string());
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSim::sendEnvelopeWithStatus(int32_t serial, const std::string &contents)
{
    stack_->post([this, serial, contents](Core *core) {
        auto bytes = fromHex(contents);
        if (!core || !bytes) {
            if (auto r = response())
                r->sendEnvelopeWithStatusResponse(
                        core ? responseInfo(serial, RadioError::INVALID_ARGUMENTS)
                             : notAvailable(serial),
                        {});
            return;
        }
        core->sat.envelope(*bytes, [this, serial](std::optional<EnvelopeResult> result) {
            sim::IccIoResult io;
            if (result) {
                io.sw1 = result->sw1;
                io.sw2 = result->sw2;
                io.simResponse = toHex(result->data);
            }
            if (auto r = response())
                r->sendEnvelopeWithStatusResponse(result ? ok(serial) : failed(serial), io);
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
