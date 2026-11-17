// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/ShannonIms.h"

#include <future>

namespace shannond::hal {

namespace {

using ::aidl::vendor::shannond::ims::Call;
using ::aidl::vendor::shannond::ims::CallState;
using ::aidl::vendor::shannond::ims::IShannonIms;
using ::aidl::vendor::shannond::ims::Registration;
using ::aidl::vendor::shannond::ims::RegistrationState;

Registration registration(Core &core)
{
    Registration r;
    switch (core.ims.state()) {
    case ims::RegistrationState::Idle: r.state = RegistrationState::IDLE; break;
    case ims::RegistrationState::Registering: r.state = RegistrationState::REGISTERING; break;
    case ims::RegistrationState::Registered: r.state = RegistrationState::REGISTERED; break;
    case ims::RegistrationState::Deregistering: r.state = RegistrationState::DEREGISTERING; break;
    }
    r.enabled = core.ims.enabled();
    r.reason = core.ims.status();
    if (auto *reg = core.ims.registration(); reg && r.state == RegistrationState::REGISTERED)
        r.associatedUris = reg->info().associatedUris;
    return r;
}

Call call(const ims::ImsCallInfo &info)
{
    Call c;
    c.id = info.id;
    switch (info.state) {
    case ims::ImsCallState::Dialing: c.state = CallState::DIALING; break;
    case ims::ImsCallState::Alerting: c.state = CallState::ALERTING; break;
    case ims::ImsCallState::Incoming: c.state = CallState::INCOMING; break;
    case ims::ImsCallState::Active: c.state = CallState::ACTIVE; break;
    case ims::ImsCallState::Held: c.state = CallState::HELD; break;
    case ims::ImsCallState::Setup:  // not shown to anyone
    case ims::ImsCallState::Ended: c.state = CallState::ENDED; break;
    }
    c.incoming = info.incoming;
    c.number = info.number;
    c.remoteHeld = info.remoteHeld;
    c.endStatus = info.endStatus;
    c.endReason = info.endReason;
    c.retryOnCs = info.retryOnCs;
    c.handedOver = info.handedOver;
    return c;
}

}  // namespace

ShannonIms::ShannonIms(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        core.ims.onChanged([this, &core] {
            if (auto cb = callback())
                cb->onRegistrationChanged(registration(core));
        });
        core.ims.calls().onChanged([this](const ims::ImsCallInfo &info) {
            if (auto cb = callback())
                cb->onCallChanged(call(info));
        });
    });
}

std::shared_ptr<ShannonIms::Callback> ShannonIms::callback()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return callback_;
}

::ndk::ScopedAStatus ShannonIms::onCore(std::function<int32_t(Core &)> fn)
{
    std::promise<int32_t> done;
    stack_->post([&done, &fn](Core *core) { done.set_value(core ? fn(*core) : IShannonIms::ERROR_NOT_READY); });
    const int32_t error = done.get_future().get();
    return error ? ::ndk::ScopedAStatus::fromServiceSpecificError(error) : ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus ShannonIms::setCallback(const std::shared_ptr<Callback> &callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus ShannonIms::setEnabled(bool enabled)
{
    return onCore([enabled](Core &core) {
        core.ims.setEnabled(enabled);
        return 0;
    });
}

::ndk::ScopedAStatus ShannonIms::getRegistration(Registration *_aidl_return)
{
    return onCore([_aidl_return](Core &core) {
        *_aidl_return = registration(core);
        return 0;
    });
}

::ndk::ScopedAStatus ShannonIms::getCalls(std::vector<Call> *_aidl_return)
{
    return onCore([_aidl_return](Core &core) {
        _aidl_return->clear();
        for (const auto &info : core.ims.calls().calls())
            _aidl_return->push_back(call(info));
        return 0;
    });
}

::ndk::ScopedAStatus ShannonIms::dial(const std::string &number, int32_t *_aidl_return)
{
    return onCore([&number, _aidl_return](Core &core) -> int32_t {
        if (!core.ims.calls().attached())
            return IShannonIms::ERROR_NOT_READY;
        auto id = core.ims.calls().dial(number);
        if (!id)
            return IShannonIms::ERROR_REFUSED;
        *_aidl_return = *id;
        return 0;
    });
}

::ndk::ScopedAStatus ShannonIms::callRequest(int32_t id, std::function<bool(ims::ImsCalls &, int)> request)
{
    return onCore([id, &request](Core &core) -> int32_t {
        bool known = false;
        for (const auto &info : core.ims.calls().calls())
            known = known || info.id == id;
        if (!known)
            return IShannonIms::ERROR_NO_SUCH_CALL;
        return request(core.ims.calls(), id) ? 0 : IShannonIms::ERROR_REFUSED;
    });
}

::ndk::ScopedAStatus ShannonIms::answer(int32_t id)
{
    return callRequest(id, [](ims::ImsCalls &calls, int call) { return calls.answer(call); });
}

::ndk::ScopedAStatus ShannonIms::hangup(int32_t id)
{
    return callRequest(id, [](ims::ImsCalls &calls, int call) { return calls.hangup(call); });
}

::ndk::ScopedAStatus ShannonIms::hold(int32_t id)
{
    return callRequest(id, [](ims::ImsCalls &calls, int call) { return calls.hold(call); });
}

::ndk::ScopedAStatus ShannonIms::resume(int32_t id)
{
    return callRequest(id, [](ims::ImsCalls &calls, int call) { return calls.resume(call); });
}

::ndk::ScopedAStatus ShannonIms::sendDtmf(int32_t id, char16_t digit)
{
    if (digit > 0x7f)
        return ::ndk::ScopedAStatus::fromServiceSpecificError(IShannonIms::ERROR_REFUSED);
    const char c = static_cast<char>(digit);
    return callRequest(id, [c](ims::ImsCalls &calls, int call) { return calls.dtmf(call, c); });
}

}  // namespace shannond::hal
