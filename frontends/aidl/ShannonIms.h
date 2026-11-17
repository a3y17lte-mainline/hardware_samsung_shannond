// SPDX-License-Identifier: GPL-2.0-or-later
//
// The private IMS interface (vendor.shannond.ims) over the core's IMS stack:
// the ImsService app switches VoLTE on, follows the registration and drives
// the calls. Requests run on the core's thread and the binder call waits for
// their (immediate) result; what happens later comes through the app's
// callback, which is oneway: the HAL never waits for the app.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <aidl/vendor/shannond/ims/BnShannonIms.h>
#include <aidl/vendor/shannond/ims/IShannonImsCallback.h>

#include "frontends/aidl/Stack.h"

namespace shannond::hal {

class ShannonIms : public ::aidl::vendor::shannond::ims::BnShannonIms {
  public:
    using Callback = ::aidl::vendor::shannond::ims::IShannonImsCallback;
    using Registration = ::aidl::vendor::shannond::ims::Registration;
    using Call = ::aidl::vendor::shannond::ims::Call;

    explicit ShannonIms(std::shared_ptr<Stack> stack);

    ::ndk::ScopedAStatus setCallback(const std::shared_ptr<Callback> &callback) override;
    ::ndk::ScopedAStatus setEnabled(bool enabled) override;
    ::ndk::ScopedAStatus getRegistration(Registration *_aidl_return) override;
    ::ndk::ScopedAStatus getCalls(std::vector<Call> *_aidl_return) override;
    ::ndk::ScopedAStatus dial(const std::string &number, int32_t *_aidl_return) override;
    ::ndk::ScopedAStatus answer(int32_t id) override;
    ::ndk::ScopedAStatus hangup(int32_t id) override;
    ::ndk::ScopedAStatus hold(int32_t id) override;
    ::ndk::ScopedAStatus resume(int32_t id) override;
    ::ndk::ScopedAStatus sendDtmf(int32_t id, char16_t digit) override;

  private:
    std::shared_ptr<Callback> callback();
    // Runs fn on the core's thread and waits for it: its result is the
    // interface's error code (0: fine); ERROR_NOT_READY while the modem is
    // not up
    ::ndk::ScopedAStatus onCore(std::function<int32_t(Core &)> fn);
    ::ndk::ScopedAStatus callRequest(int32_t id, std::function<bool(ims::ImsCalls &, int)> request);

    std::shared_ptr<Stack> stack_;
    std::mutex mutex_;
    std::shared_ptr<Callback> callback_;
};

}  // namespace shannond::hal
