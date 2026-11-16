// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/loop.h"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace shannond {

EventLoop::EventLoop()
    : epfd_(epoll_create1(EPOLL_CLOEXEC)), wakeFd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
{
    watch(wakeFd_, EPOLLIN, [this](uint32_t) { runPosted(); });
}

EventLoop::~EventLoop()
{
    if (wakeFd_ >= 0)
        close(wakeFd_);
    if (epfd_ >= 0)
        close(epfd_);
}

void EventLoop::post(std::function<void()> callback)
{
    {
        std::lock_guard<std::mutex> lock(postedLock_);
        posted_.push_back(std::move(callback));
    }
    const uint64_t one = 1;
    (void)!write(wakeFd_, &one, sizeof(one));
}

void EventLoop::runPosted()
{
    uint64_t count;
    (void)!read(wakeFd_, &count, sizeof(count));

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(postedLock_);
        callbacks.swap(posted_);
    }
    for (auto &callback : callbacks)
        callback();
}

bool EventLoop::watch(int fd, uint32_t events, FdHandler handler)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    const int op = handlers_.count(fd) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(epfd_, op, fd, &ev) < 0)
        return false;

    handlers_[fd] = std::make_shared<FdHandler>(std::move(handler));
    return true;
}

void EventLoop::unwatch(int fd)
{
    if (handlers_.erase(fd))
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
}

EventLoop::TimerId EventLoop::after(std::chrono::milliseconds delay,
                                    std::function<void()> callback)
{
    const TimerId id = nextTimer_++;
    timers_.emplace(Clock::now() + delay, Timer{id, std::move(callback)});
    return id;
}

void EventLoop::cancel(TimerId id)
{
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.id == id) {
            timers_.erase(it);
            return;
        }
    }
}

void EventLoop::runTimers()
{
    const auto now = Clock::now();

    while (!timers_.empty() && timers_.begin()->first <= now) {
        auto callback = std::move(timers_.begin()->second.callback);
        timers_.erase(timers_.begin());
        callback();
    }
}

void EventLoop::runOnce(std::chrono::milliseconds timeout)
{
    if (!timers_.empty()) {
        const auto untilTimer = std::chrono::duration_cast<std::chrono::milliseconds>(
            timers_.begin()->first - Clock::now());
        if (untilTimer < timeout)
            timeout = untilTimer < std::chrono::milliseconds(0)
                          ? std::chrono::milliseconds(0)
                          : untilTimer + std::chrono::milliseconds(1);
    }

    epoll_event events[16];
    const int n = epoll_wait(epfd_, events, 16, static_cast<int>(timeout.count()));

    for (int i = 0; i < n; i++) {
        auto it = handlers_.find(events[i].data.fd);
        if (it == handlers_.end())
            continue;
        // Keep the handler alive even if it unwatches its fd
        auto handler = it->second;
        (*handler)(events[i].events);
    }

    runTimers();
}

void EventLoop::run()
{
    running_ = true;
    while (running_)
        runOnce(std::chrono::seconds(60));
}

bool EventLoop::runUntil(const std::function<bool()> &pred,
                         std::chrono::milliseconds limit)
{
    const auto end = Clock::now() + limit;

    while (!pred()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - Clock::now());
        if (left <= std::chrono::milliseconds(0))
            return false;
        runOnce(std::min(left, std::chrono::milliseconds(50)));
    }

    return true;
}

}  // namespace shannond
