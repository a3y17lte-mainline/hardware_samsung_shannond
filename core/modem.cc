// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/modem.h"

#include <cerrno>
#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {
// What one read takes. A FMT message has a 16 bit length; RFS messages carry
// NV data and can be longer, and both may arrive over several reads
constexpr size_t kMaxFrame = 0x10000;
constexpr size_t kMaxRfsFrame = 0x80000;
}

const char *commandName(uint16_t command)
{
    return ipc_command_string(command);
}

Modem::Modem(EventLoop &loop, int fmtFd, int rfsFd)
    : loop_(loop), fmtFd_(fmtFd), rfsFd_(rfsFd)
{
    loop_.watch(fmtFd_, EPOLLIN, [this](uint32_t events) {
        if (events & EPOLLIN)
            readFmt();
        else if (events & (EPOLLERR | EPOLLHUP))
            closed();
    });
    if (rfsFd_ >= 0) {
        loop_.watch(rfsFd_, EPOLLIN, [this](uint32_t events) {
            if (events & EPOLLIN)
                readRfs();
            else if (events & (EPOLLERR | EPOLLHUP))
                closed();
        });
    }
}

Modem::~Modem()
{
    loop_.unwatch(fmtFd_);
    if (rfsFd_ >= 0)
        loop_.unwatch(rfsFd_);
    for (auto &[seq, pending] : pending_)
        loop_.cancel(pending.timer);
}

void Modem::log(const char *format, ...)
{
    if (!log_)
        return;

    char buffer[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    log_(buffer);
}

uint8_t Modem::nextSeq()
{
    // 0 is not used as a sequence number; skip numbers still waiting
    for (int i = 0; i < 255; i++) {
        seq_ = seq_ == 255 ? 1 : seq_ + 1;
        if (!pending_.count(seq_))
            return seq_;
    }
    return seq_;
}

void Modem::request(uint16_t command, uint8_t type, Bytes data,
                    Callback callback, std::chrono::milliseconds timeout)
{
    Queued request{command, type, std::move(data), std::move(callback), timeout};

    if (isClosed_) {
        log("send %s failed", commandName(command));
        if (request.callback)
            request.callback(Result{});
        return;
    }
    if (busy_.count(command >> 8)) {
        queued_[command >> 8].push_back(std::move(request));
        return;
    }
    send(std::move(request));
}

void Modem::send(Queued request)
{
    Message message;
    message.command = request.command;
    message.type = request.type;
    message.mseq = nextSeq();
    message.data = std::move(request.data);

    const Bytes frame = encodeFmt(message);
    const uint8_t mseq = message.mseq;
    const bool sent = write(fmtFd_, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size());
    if (!sent)
        log("send %s failed", commandName(request.command));

    // A failed send ends like any other request, so the next one goes out
    auto timer = loop_.after(sent ? request.timeout : std::chrono::milliseconds(0), [this, mseq, sent] {
        Result result;
        result.timeout = sent;
        finish(mseq, result);
    });
    busy_[request.command >> 8] = mseq;
    pending_[mseq] = Pending{request.command, request.type, std::move(request.callback), timer};
}

void Modem::event(uint16_t command, Bytes data)
{
    Message message;
    message.command = command;
    message.type = IPC_TYPE_EVENT;
    message.mseq = nextSeq();
    message.data = std::move(data);
    const Bytes frame = encodeFmt(message);
    if (isClosed_ || write(fmtFd_, frame.data(), frame.size()) != static_cast<ssize_t>(frame.size()))
        log("send %s failed", commandName(command));
}

void Modem::onMessage(uint16_t command, Listener listener)
{
    listeners_.emplace(command, std::move(listener));
}

void Modem::onRfs(uint8_t command, RfsHandler handler)
{
    rfsHandlers_[command] = std::move(handler);
}

void Modem::sendRfs(const RfsMessage &message)
{
    const Bytes frame = encodeRfs(message);
    if (rfsFd_ < 0 || write(rfsFd_, frame.data(), frame.size()) != static_cast<ssize_t>(frame.size()))
        log("RFS send failed");
}

void Modem::finish(uint8_t mseq, const Result &result)
{
    auto it = pending_.find(mseq);
    if (it == pending_.end())
        return;

    Pending pending = std::move(it->second);
    pending_.erase(it);
    loop_.cancel(pending.timer);

    if (result.timeout)
        log("%s: no answer", commandName(pending.command));

    // The group's next request goes out before the callback can add more
    const uint8_t group = pending.command >> 8;
    busy_.erase(group);
    auto waiting = queued_.find(group);
    if (waiting != queued_.end() && !isClosed_) {
        Queued next = std::move(waiting->second.front());
        waiting->second.pop_front();
        if (waiting->second.empty())
            queued_.erase(waiting);
        send(std::move(next));
    }
    if (pending.callback)
        pending.callback(result);
}

void Modem::handle(const Message &message)
{
    // Answers to our requests. Some come without the request's sequence
    // number: those belong to the one request their group has out
    const bool answer = message.type == IPC_TYPE_RESP || message.command == IPC_GEN_PHONE_RES;
    if (message.aseq || answer) {
        auto it = pending_.find(message.aseq);
        if (it == pending_.end() && answer) {
            auto res = message.as<ipc_gen_phone_res_data>();
            const uint8_t group = message.command == IPC_GEN_PHONE_RES
                                          ? (res ? res->group : 0)
                                          : message.command >> 8;
            auto out = busy_.find(group);
            if (out != busy_.end())
                it = pending_.find(out->second);
        }
        const uint8_t mseq = it != pending_.end() ? it->first : 0;
        if (it != pending_.end()) {
            const Pending &pending = it->second;

            if (message.command == IPC_GEN_PHONE_RES) {
                auto res = message.as<ipc_gen_phone_res_data>();
                if (res && ((res->group << 8) | res->index) == pending.command) {
                    Result result;
                    result.code = res->code;
                    result.ok = res->code == IPC_GEN_PHONE_RES_CODE_SUCCESS;
                    finish(mseq, result);
                    return;
                }
            } else if (message.command == pending.command &&
                       (message.type == IPC_TYPE_RESP ||
                        // Some commands are answered by a notification that
                        // carries the request's sequence number
                        (message.type == IPC_TYPE_NOTI && message.aseq &&
                         pending.type == IPC_TYPE_EXEC))) {
                Result result;
                result.ok = true;
                result.response = &message;
                // Listeners see answers to GETs as well, for state caching
                auto range = listeners_.equal_range(message.command);
                for (auto l = range.first; l != range.second; ++l)
                    l->second(message);
                finish(mseq, result);
                return;
            }
        }
    }

    auto range = listeners_.equal_range(message.command);
    if (range.first == range.second && message.command != IPC_GEN_PHONE_RES)
        log("unhandled %s type %u (%zu bytes)", commandName(message.command),
            message.type, message.data.size());
    for (auto it = range.first; it != range.second; ++it)
        it->second(message);
}

void Modem::readFmt()
{
    uint8_t buffer[kMaxFrame];
    const ssize_t n = read(fmtFd_, buffer, sizeof(buffer));

    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return;
        closed();
        return;
    }
    if (n == 0) {
        closed();
        return;
    }

    bool bad = false;
    for (const Bytes &frame : fmtAssembler_.add({buffer, static_cast<size_t>(n)}, &bad)) {
        auto message = decodeFmt(frame);
        if (message)
            handle(*message);
    }
    if (bad)
        log("bad IPC frame (%zd bytes read)", n);
}

void Modem::readRfs()
{
    if (rfsBuffer_.size() < kMaxRfsFrame)
        rfsBuffer_.resize(kMaxRfsFrame);
    const ssize_t n = read(rfsFd_, rfsBuffer_.data(), rfsBuffer_.size());

    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        closed();
        return;
    }

    bool bad = false;
    auto frames = rfsAssembler_.add({rfsBuffer_.data(), static_cast<size_t>(n)}, &bad);
    if (bad)
        log("bad RFS frame (%zd bytes read)", n);
    for (const Bytes &frame : frames) {
        auto message = decodeRfs(frame);
        if (message)
            handleRfs(*message);
    }
}

void Modem::handleRfs(const RfsMessage &message)
{
    auto it = rfsHandlers_.find(message.command);
    if (it == rfsHandlers_.end()) {
        log("unhandled RFS command 0x%02x", message.command);
        return;
    }
    it->second(message);
}

void Modem::closed()
{
    if (isClosed_)
        return;
    isClosed_ = true;

    log("modem link closed");
    loop_.unwatch(fmtFd_);
    if (rfsFd_ >= 0)
        loop_.unwatch(rfsFd_);

    // Fail everything still waiting
    while (!pending_.empty())
        finish(pending_.begin()->first, Result{});
    while (!queued_.empty()) {
        auto queue = std::move(queued_.begin()->second);
        queued_.erase(queued_.begin());
        for (Queued &request : queue)
            if (request.callback)
                request.callback(Result{});
    }

    if (onClosed_)
        onClosed_();
}

}  // namespace shannond
