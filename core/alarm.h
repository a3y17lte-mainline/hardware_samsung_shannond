// SPDX-License-Identifier: GPL-2.0-or-later
//
// A one-shot timer on the boot clock: it counts time in suspend and, where
// the process may (CAP_WAKE_ALARM), wakes the system when it fires. For
// deadlines set by the network (IMS registration expiry), which the loop's
// monotonic timers would miss after a long suspend.
#pragma once

#include <chrono>
#include <functional>

#include "core/loop.h"

namespace shannond {

class AlarmTimer {
public:
    explicit AlarmTimer(EventLoop &loop);
    ~AlarmTimer();
    AlarmTimer(const AlarmTimer &) = delete;
    AlarmTimer &operator=(const AlarmTimer &) = delete;

    void start(std::chrono::milliseconds delay, std::function<void()> callback);
    void cancel();
    bool active() const { return armed_; }
    bool wakes() const { return wakes_; }

private:
    EventLoop &loop_;
    int fd_ = -1;
    bool wakes_ = false;
    bool armed_ = false;
    std::function<void()> callback_;
};

}  // namespace shannond
