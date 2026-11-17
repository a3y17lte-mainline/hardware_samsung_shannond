// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/alarm.h"

#include <cstdint>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace shannond {

AlarmTimer::AlarmTimer(EventLoop &loop) : loop_(loop)
{
    fd_ = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
    wakes_ = fd_ >= 0;
    if (fd_ < 0)
        fd_ = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd_ >= 0)
        loop_.watch(fd_, EPOLLIN, [this](uint32_t) {
            uint64_t expirations;
            if (read(fd_, &expirations, sizeof(expirations)) != sizeof(expirations))
                return;
            armed_ = false;
            auto callback = std::move(callback_);
            if (callback)
                callback();
        });
}

AlarmTimer::~AlarmTimer()
{
    if (fd_ >= 0) {
        loop_.unwatch(fd_);
        close(fd_);
    }
}

void AlarmTimer::start(std::chrono::milliseconds delay, std::function<void()> callback)
{
    callback_ = std::move(callback);
    if (delay.count() <= 0)
        delay = std::chrono::milliseconds(1);
    itimerspec spec{};
    spec.it_value.tv_sec = delay.count() / 1000;
    spec.it_value.tv_nsec = (delay.count() % 1000) * 1000000;
    armed_ = fd_ >= 0 && timerfd_settime(fd_, 0, &spec, nullptr) == 0;
}

void AlarmTimer::cancel()
{
    itimerspec spec{};
    if (fd_ >= 0)
        timerfd_settime(fd_, 0, &spec, nullptr);
    armed_ = false;
    callback_ = nullptr;
}

}  // namespace shannond
