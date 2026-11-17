// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/sat.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

// How the stock RIL reads the status words: the card has nothing more
bool sessionOver(uint8_t sw1, uint8_t sw2)
{
    return (sw1 == 0x90 || sw1 == 0x6f) && sw2 == 0;
}

constexpr uint8_t kMenuSelectionTag = 0xd3;

}  // namespace

Sat::Sat(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SAT_PROACTIVE_CMD, [this](const Message &m) {
        // Answers to terminal responses have this command too
        if (m.type == IPC_TYPE_INDI)
            proactive(m);
    });
    modem_.onMessage(IPC_SAT_ENVELOPE_CMD, [this](const Message &m) {
        auto header = m.as<ipc_sat_envelope_response_header>();
        if (m.type != IPC_TYPE_NOTI || !header)
            return;
        EnvelopeResult result;
        result.sw1 = header->sw1;
        result.sw2 = header->sw2;
        const size_t length = std::min<size_t>(header->length, m.data.size() - sizeof(*header));
        result.data.assign(m.data.begin() + sizeof(*header),
                           m.data.begin() + sizeof(*header) + length);
        envelopeAnswer(std::move(result));
    });
}

void Sat::proactive(const Message &message)
{
    auto header = message.as<ipc_sat_proactive_cmd_header>();
    if (!header || header->length > message.data.size() - sizeof(*header)) {
        modem_.log("short proactive command");
        return;
    }
    Bytes command(message.data.begin() + sizeof(*header),
                  message.data.begin() + sizeof(*header) + header->length);

    if (running_ && command_)
        command_(command);
    else
        waiting_.push_back(std::move(command));
}

void Sat::clientRunning()
{
    running_ = true;
    std::vector<Bytes> waiting;
    waiting.swap(waiting_);
    for (const Bytes &command : waiting)
        if (command_)
            command_(command);
}

void Sat::terminalResponse(const Bytes &response, std::function<void(bool)> done)
{
    if (response.size() > 0xff) {
        done(false);
        return;
    }
    ipc_sat_terminal_response_data data{};
    data.length = static_cast<uint8_t>(response.size());
    std::memcpy(data.data, response.data(), response.size());

    modem_.request(IPC_SAT_PROACTIVE_CMD, IPC_TYPE_GET, toBytes(data),
                   [this, done](const Result &r) {
        auto result = r.ok && r.response ? r.response->as<ipc_sat_result_data>() : std::nullopt;
        done(r.ok);
        if (result && sessionOver(result->sw1, result->sw2) && sessionEnd_)
            sessionEnd_();
    });
}

void Sat::envelope(const Bytes &envelope, std::function<void(std::optional<EnvelopeResult>)> done)
{
    if (envelope.empty() || envelope.size() > IPC_SAT_ENVELOPE_MAX - 1 || envelopeDone_) {
        done(std::nullopt);
        return;
    }
    ipc_sat_envelope_request_data data{};
    data.length = static_cast<uint8_t>(envelope.size());
    std::memcpy(data.data, envelope.data(), envelope.size());

    envelopeDone_ = std::move(done);
    menuSelection_ = envelope[0] == kMenuSelectionTag;
    // The answer is a notification, which the listener above takes; this
    // callback is for a request that fails or gets none
    modem_.request(IPC_SAT_ENVELOPE_CMD, IPC_TYPE_EXEC, toBytes(data), [this](const Result &r) {
        if (!r.ok)
            envelopeAnswer(std::nullopt);
    });
}

void Sat::envelopeAnswer(std::optional<EnvelopeResult> result)
{
    if (!envelopeDone_)
        return;
    auto done = std::move(envelopeDone_);
    envelopeDone_ = nullptr;

    // A menu selection that the card just accepts starts nothing
    const bool over = result && menuSelection_ && result->sw1 == 0x90 && result->sw2 == 0;
    menuSelection_ = false;
    done(std::move(result));
    if (over && sessionEnd_)
        sessionEnd_();
}

}  // namespace shannond
