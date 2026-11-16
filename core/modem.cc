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
    Message message;
    message.command = command;
    message.type = type;
    message.mseq = nextSeq();
    message.data = std::move(data);

    const Bytes frame = encodeFmt(message);
    if (isClosed_ || write(fmtFd_, frame.data(), frame.size()) != static_cast<ssize_t>(frame.size())) {
        log("send %s failed", commandName(command));
        if (callback)
            callback(Result{});
        return;
    }

    const uint8_t mseq = message.mseq;
    auto timer = loop_.after(timeout, [this, mseq] {
        Result result;
        result.timeout = true;
        finish(mseq, result);
    });
    pending_[mseq] = Pending{command, type, std::move(callback), timer};
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
    if (pending.callback)
        pending.callback(result);
}

void Modem::handle(const Message &message)
{
    // Answers to our requests
    if (message.aseq) {
        auto it = pending_.find(message.aseq);
        if (it != pending_.end()) {
            const Pending &pending = it->second;

            if (message.command == IPC_GEN_PHONE_RES) {
                auto res = message.as<ipc_gen_phone_res_data>();
                if (res && ((res->group << 8) | res->index) == pending.command) {
                    Result result;
                    result.code = res->code;
                    result.ok = res->code == IPC_GEN_PHONE_RES_CODE_SUCCESS;
                    finish(message.aseq, result);
                    return;
                }
            } else if (message.command == pending.command &&
                       message.type == IPC_TYPE_RESP) {
                Result result;
                result.ok = true;
                result.response = &message;
                // Listeners see answers to GETs as well, for state caching
                auto range = listeners_.equal_range(message.command);
                for (auto l = range.first; l != range.second; ++l)
                    l->second(message);
                finish(message.aseq, result);
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

    if (onClosed_)
        onClosed_();
}

}  // namespace shannond
