// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include "frontends/aidl/Codec.h"

#include <cstdio>

namespace shannond::hal {

namespace {

int32_t callState(CallState state)
{
    // Same order as the framework's states
    switch (state) {
    case CallState::Active: return voice::Call::STATE_ACTIVE;
    case CallState::Holding: return voice::Call::STATE_HOLDING;
    case CallState::Dialing: return voice::Call::STATE_DIALING;
    case CallState::Alerting: return voice::Call::STATE_ALERTING;
    case CallState::Incoming: return voice::Call::STATE_INCOMING;
    case CallState::Waiting: return voice::Call::STATE_WAITING;
    }
    return voice::Call::STATE_ACTIVE;
}


bool hasIncoming(const std::vector<Call> &calls)
{
    for (const Call &c : calls)
        if (c.state == CallState::Incoming || c.state == CallState::Waiting)
            return true;
    return false;
}

}  // namespace

RadioVoice::RadioVoice(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        auto ringing = std::make_shared<bool>(false);
        core.calls.onChanged([this, &core, ringing] {
            auto i = indication();
            if (!i)
                return;
            i->callStateChanged(kUnsolicited);
            const bool incoming = hasIncoming(core.calls.list());
            if (incoming && !*ringing)
                i->callRing(kUnsolicited, true, {});
            *ringing = incoming;
        });
    });
}

::ndk::ScopedAStatus RadioVoice::getCurrentCalls(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core) {
            r->getCurrentCallsResponse(notAvailable(serial), {});
            return;
        }
        std::vector<voice::Call> calls;
        for (const Call &c : core->calls.list()) {
            voice::Call call;
            call.state = callState(c.state);
            call.index = c.id;
            call.toa = !c.number.empty() && c.number[0] == '+' ? 145 : 129;
            call.isMpty = c.multiparty;
            call.isMT = c.incoming;
            call.als = 0;
            call.isVoice = true;
            call.isVoicePrivacy = false;
            call.number = c.number;
            call.numberPresentation = c.number.empty() ? voice::Call::PRESENTATION_UNKNOWN
                                                       : voice::Call::PRESENTATION_ALLOWED;
            call.namePresentation = voice::Call::PRESENTATION_UNKNOWN;
            calls.push_back(std::move(call));
        }
        r->getCurrentCallsResponse(ok(serial), calls);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::dial(int32_t serial, const voice::Dial &dialInfo)
{
    // Only on the user's request: the framework dials from the dialer
    stack_->post([this, serial, number = dialInfo.address](Core *core) {
        if (!core) {
            if (auto r = response())
                r->dialResponse(notAvailable(serial));
            return;
        }
        core->calls.dial(number, [this, serial](bool success) {
            if (auto r = response())
                r->dialResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::emergencyDial(int32_t serial, const voice::Dial &dialInfo,
                                               int32_t categories,
                                               const std::vector<std::string> &urns,
                                               voice::EmergencyCallRouting routing,
                                               bool hasKnownUserIntentEmergency, bool isTesting)
{
    (void)categories;
    (void)urns;
    (void)routing;
    (void)hasKnownUserIntentEmergency;
    if (isTesting) {
        if (auto r = response())
            r->emergencyDialResponse(ok(serial));
        return ::ndk::ScopedAStatus::ok();
    }
    // A normal voice call: the modem recognises the emergency numbers itself.
    // The stock RIL's emergency call type (IpcTxCallOriginate @7) is not
    // decoded yet
    stack_->post([this, serial, number = dialInfo.address](Core *core) {
        if (!core) {
            if (auto r = response())
                r->emergencyDialResponse(notAvailable(serial));
            return;
        }
        core->calls.dial(number, [this, serial](bool success) {
            if (auto r = response())
                r->emergencyDialResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::acceptCall(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        if (!core) {
            if (auto r = response())
                r->acceptCallResponse(notAvailable(serial));
            return;
        }
        core->calls.answer([this, serial](bool success) {
            if (auto r = response())
                r->acceptCallResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

// The core has one release (IPC_CALL_RELEASE): it ends the current call or
// rejects the ringing one. Holding and multiparty calls are not built.
void RadioVoice::release(int32_t serial, const char *what)
{
    const std::string kind = what;
    stack_->post([this, serial, kind](Core *core) {
        auto respond = [this, serial, kind](RadioResponseInfo info) {
            auto r = response();
            if (!r)
                return;
            if (kind == "hangup")
                r->hangupConnectionResponse(info);
            else if (kind == "reject")
                r->rejectCallResponse(info);
            else if (kind == "background")
                r->hangupWaitingOrBackgroundResponse(info);
            else
                r->hangupForegroundResumeBackgroundResponse(info);
        };
        if (!core) {
            respond(notAvailable(serial));
            return;
        }
        if (core->calls.list().empty()) {
            respond(responseInfo(serial, RadioError::INVALID_STATE));
            return;
        }
        core->calls.hangup([serial, respond](bool success) {
            respond(success ? ok(serial) : failed(serial));
        });
    });
}

::ndk::ScopedAStatus RadioVoice::hangup(int32_t serial, int32_t gsmIndex)
{
    (void)gsmIndex;  // one call at a time
    release(serial, "hangup");
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::rejectCall(int32_t serial)
{
    release(serial, "reject");
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::hangupWaitingOrBackground(int32_t serial)
{
    release(serial, "background");
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::hangupForegroundResumeBackground(int32_t serial)
{
    release(serial, "foreground");
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::getLastCallFailCause(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        voice::LastCallFailCauseInfo info;
        if (!core) {
            r->getLastCallFailCauseResponse(notAvailable(serial), info);
            return;
        }
        const uint8_t type = core->calls.lastEndCauseType();
        const uint8_t cause = core->calls.lastEndCause();
        char vendor[16];
        snprintf(vendor, sizeof(vendor), "0x%02x/0x%02x", type, cause);
        info.causeCode = static_cast<voice::LastCallFailCause>(lastCallFailCause(type, cause));
        info.vendorCause = vendor;
        r->getLastCallFailCauseResponse(ok(serial), info);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::sendDtmf(int32_t serial, const std::string &s)
{
    // In call DTMF as start and stop per tone; the framework sends one tone
    // per request
    stack_->post([this, serial, s](Core *core) {
        if (!core || s.empty()) {
            if (auto r = response())
                r->sendDtmfResponse(core ? responseInfo(serial, RadioError::INVALID_ARGUMENTS)
                                         : notAvailable(serial));
            return;
        }
        const char tone = s[0];
        core->calls.dtmf(tone, true, [this, serial, core, tone](bool started) {
            if (!started) {
                if (auto r = response())
                    r->sendDtmfResponse(failed(serial));
                return;
            }
            core->calls.dtmf(tone, false, [this, serial](bool stopped) {
                if (auto r = response())
                    r->sendDtmfResponse(stopped ? ok(serial) : failed(serial));
            });
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::startDtmf(int32_t serial, const std::string &s)
{
    if (s.empty()) {
        if (auto r = response())
            r->startDtmfResponse(responseInfo(serial, RadioError::INVALID_ARGUMENTS));
        return ::ndk::ScopedAStatus::ok();
    }
    {
        std::lock_guard<std::mutex> lock(dtmfLock_);
        dtmfTone_ = s[0];
    }
    stack_->post([this, serial, tone = s[0]](Core *core) {
        if (!core) {
            if (auto r = response())
                r->startDtmfResponse(notAvailable(serial));
            return;
        }
        core->calls.dtmf(tone, true, [this, serial](bool success) {
            if (auto r = response())
                r->startDtmfResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::stopDtmf(int32_t serial)
{
    char tone;
    {
        std::lock_guard<std::mutex> lock(dtmfLock_);
        tone = dtmfTone_;
        dtmfTone_ = 0;
    }
    if (!tone) {
        if (auto r = response())
            r->stopDtmfResponse(ok(serial));
        return ::ndk::ScopedAStatus::ok();
    }
    stack_->post([this, serial, tone](Core *core) {
        if (!core) {
            if (auto r = response())
                r->stopDtmfResponse(notAvailable(serial));
            return;
        }
        core->calls.dtmf(tone, false, [this, serial](bool success) {
            if (auto r = response())
                r->stopDtmfResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::getMute(int32_t serial)
{
    if (auto r = response())
        r->getMuteResponse(ok(serial), mute_.load());
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::setMute(int32_t serial, bool enable)
{
    // Remembered only: the microphone path belongs to the audio HAL
    mute_ = enable;
    if (auto r = response())
        r->setMuteResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::getClir(int32_t serial)
{
    // n = 0: the network's default; m = 2: unknown (no supplementary
    // service query yet)
    if (auto r = response())
        r->getClirResponse(ok(serial), 0, 2);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::setClir(int32_t serial, int32_t status)
{
    if (auto r = response())
        r->setClirResponse(status == voice::Dial::CLIR_DEFAULT
                                   ? ok(serial)
                                   : notSupported(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::getPreferredVoicePrivacy(int32_t serial)
{
    if (auto r = response())
        r->getPreferredVoicePrivacyResponse(ok(serial), false);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::getTtyMode(int32_t serial)
{
    if (auto r = response())
        r->getTtyModeResponse(ok(serial), voice::TtyMode::OFF);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::setTtyMode(int32_t serial, voice::TtyMode mode)
{
    if (auto r = response())
        r->setTtyModeResponse(mode == voice::TtyMode::OFF ? ok(serial) : notSupported(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoice::isVoNrEnabled(int32_t serial)
{
    if (auto r = response())
        r->isVoNrEnabledResponse(ok(serial), false);
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
