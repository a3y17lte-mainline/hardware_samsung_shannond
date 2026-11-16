// SPDX-License-Identifier: GPL-2.0-or-later
//
// The link to the modem: the IPC and RFS channels, requests with their
// answers, and notifications.
//
// A request carries a sequence number (mseq); the modem answers a GET with
// the same command and type RESP, and a SET or EXEC with IPC_GEN_PHONE_RES,
// both with aseq = the request's mseq. Everything else is a notification.
#pragma once

#include <chrono>
#include <cstdarg>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/loop.h"

namespace shannond {

struct Result {
    bool ok = false;
    bool timeout = false;
    uint16_t code = 0;            // IPC_GEN_PHONE_RES code, if that was the answer
    const Message *response = nullptr;  // RESP of a GET
};

class Modem {
public:
    using Callback = std::function<void(const Result &)>;
    using Listener = std::function<void(const Message &)>;
    using RfsHandler = std::function<void(const RfsMessage &)>;
    using LogFn = std::function<void(const std::string &)>;

    static constexpr std::chrono::milliseconds kDefaultTimeout{10000};

    // fmtFd and rfsFd are message-oriented (one frame per read and write):
    // the kernel's WWAN ports, or SOCK_SEQPACKET sockets in tests. The
    // modem does not take ownership.
    Modem(EventLoop &loop, int fmtFd, int rfsFd);
    ~Modem();

    void request(uint16_t command, uint8_t type, Bytes data = {},
                 Callback callback = {},
                 std::chrono::milliseconds timeout = kDefaultTimeout);

    template <class T>
    void request(uint16_t command, uint8_t type, const T &payload,
                 Callback callback = {},
                 std::chrono::milliseconds timeout = kDefaultTimeout)
    {
        request(command, type, toBytes(payload), std::move(callback), timeout);
    }

    // Notifications and unsolicited answers of a command
    void onMessage(uint16_t command, Listener listener);
    // Requests of the modem on the RFS channel, by RFS command index
    void onRfs(uint8_t command, RfsHandler handler);
    void sendRfs(const RfsMessage &message);
    // A port reported an error or hang-up: the modem crashed or went away
    void onClosed(std::function<void()> callback) { onClosed_ = std::move(callback); }

    void setLog(LogFn log) { log_ = std::move(log); }
    void log(const char *format, ...) __attribute__((format(printf, 2, 3)));

    EventLoop &loop() { return loop_; }
    size_t pendingCount() const { return pending_.size(); }

private:
    struct Pending {
        uint16_t command;
        uint8_t type;
        Callback callback;
        EventLoop::TimerId timer;
    };

    void readFmt();
    void readRfs();
    void handleRfs(const RfsMessage &message);
    void handle(const Message &message);
    struct Queued {
        uint16_t command;
        uint8_t type;
        Bytes data;
        Callback callback;
        std::chrono::milliseconds timeout;
    };

    void send(Queued request);
    void finish(uint8_t mseq, const Result &result);
    uint8_t nextSeq();
    void closed();

    EventLoop &loop_;
    int fmtFd_;
    int rfsFd_;
    std::vector<uint8_t> rfsBuffer_;
    FrameAssembler fmtAssembler_{kFmtHeaderSize, 2, 0xffff};
    FrameAssembler rfsAssembler_{kRfsHeaderSize, 4, 0x80000};
    uint8_t seq_ = 0;
    std::map<uint8_t, Pending> pending_;
    // One request per message group is with the modem at a time, as the
    // stock RIL works: the modem drops some of a burst, and not every answer
    // carries its request's sequence number. Group -> the sequence number
    // out, and the requests waiting behind it
    std::map<uint8_t, uint8_t> busy_;
    std::map<uint8_t, std::deque<Queued>> queued_;
    std::multimap<uint16_t, Listener> listeners_;
    std::map<uint8_t, RfsHandler> rfsHandlers_;
    std::function<void()> onClosed_;
    LogFn log_;
    bool isClosed_ = false;
};

const char *commandName(uint16_t command);

}  // namespace shannond
