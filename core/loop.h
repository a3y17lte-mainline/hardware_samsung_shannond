// SPDX-License-Identifier: GPL-2.0-or-later
//
// A single-threaded event loop: file descriptors (epoll) and timers.
// Everything in the daemon runs on it, so the modules need no locking.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace shannond {

class EventLoop {
public:
    using Clock = std::chrono::steady_clock;
    using FdHandler = std::function<void(uint32_t events)>;
    using TimerId = uint64_t;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // events: EPOLLIN etc. The handler may unwatch its own fd.
    bool watch(int fd, uint32_t events, FdHandler handler);
    void unwatch(int fd);

    TimerId after(std::chrono::milliseconds delay, std::function<void()> callback);
    // From any thread: run callback on the loop's thread
    void post(std::function<void()> callback);
    void cancel(TimerId id);

    // Wait at most timeout for one round of events and due timers
    void runOnce(std::chrono::milliseconds timeout);
    void run();
    void stop() { running_ = false; }

    // For tests: run until pred() or until limit passed; returns pred()
    bool runUntil(const std::function<bool()> &pred,
                  std::chrono::milliseconds limit = std::chrono::seconds(5));

private:
    struct Timer {
        TimerId id;
        std::function<void()> callback;
    };

    void runTimers();
    void runPosted();

    int epfd_;
    int wakeFd_;
    std::mutex postedLock_;
    std::vector<std::function<void()>> posted_;
    bool running_ = false;
    TimerId nextTimer_ = 1;
    std::unordered_map<int, std::shared_ptr<FdHandler>> handlers_;
    std::multimap<Clock::time_point, Timer> timers_;
};

}  // namespace shannond
