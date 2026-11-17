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

// 102 223: proactive command tag, simple TLV tags (comprehension bit off)
constexpr uint8_t kProactiveCommandTag = 0xd0;
constexpr uint8_t kCommandDetailsTag = 0x01;
constexpr uint8_t kFileListTag = 0x12;
constexpr uint8_t kAidTag = 0x2f;
constexpr uint8_t kRefreshCommand = 0x01;
constexpr uint8_t kRefreshFileChange = 0x01;
constexpr uint8_t kRefreshUiccReset = 0x04;

// The simple TLVs of a BER-TLV object, by tag without the comprehension bit
std::vector<std::pair<uint8_t, Bytes>> tlvs(const Bytes &data, size_t at, size_t end)
{
    std::vector<std::pair<uint8_t, Bytes>> out;
    while (at + 2 <= end) {
        const uint8_t tag = data[at] & 0x7f;
        size_t len = data[at + 1];
        at += 2;
        if (len == 0x81 && at < end)
            len = data[at++];
        if (at + len > end)
            break;
        out.push_back({tag, Bytes(data.begin() + at, data.begin() + at + len)});
        at += len;
    }
    return out;
}

}  // namespace

Sat::Sat(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SAT_PROACTIVE_CMD, [this](const Message &m) {
        // Answers to terminal responses have this command too
        if (m.type == IPC_TYPE_INDI)
            proactive(m);
    });
    modem_.onMessage(IPC_SAT_REFRESH, [this](const Message &m) { refreshed(m); });
    // The result of call control by the card: Android has no request for
    // it any more (the stock RIL passes it to Samsung's framework)
    modem_.onMessage(IPC_SAT_CALL_CONTROL_RESULT, [this](const Message &m) {
        modem_.log("SIM call control result, %zu bytes", m.data.size());
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
    remember(command);

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

void Sat::remember(const Bytes &command)
{
    // The stock RIL keeps what a REFRESH named until the modem reports
    // that it has refreshed (IpcStkCache)
    if (command.size() < 2 || command[0] != kProactiveCommandTag)
        return;
    size_t at = 1;
    size_t len = command[at++];
    if (len == 0x81 && at < command.size())
        len = command[at++];
    const auto items = tlvs(command, at, std::min(command.size(), at + len));
    auto details = std::find_if(items.begin(), items.end(),
                                [](const auto &t) { return t.first == kCommandDetailsTag; });
    if (details == items.end() || details->second.size() < 3 || details->second[1] != kRefreshCommand)
        return;

    refreshQualifier_ = details->second[2];
    refreshFiles_.clear();
    refreshAid_.clear();
    for (const auto &[tag, value] : items) {
        if (tag == kAidTag) {
            refreshAid_ = value;
        } else if (tag == kFileListTag && !value.empty()) {
            // Paths from the MF: each ends with the file that changed
            std::vector<uint16_t> path;
            for (size_t i = 1; i + 1 < value.size(); i += 2) {
                const uint16_t fid = value[i] << 8 | value[i + 1];
                if (fid == 0x3f00 && !path.empty()) {
                    refreshFiles_.push_back(path.back());
                    path.clear();
                }
                path.push_back(fid);
            }
            if (!path.empty())
                refreshFiles_.push_back(path.back());
        }
    }
}

void Sat::refreshed(const Message &message)
{
    // IpcRxSatRefresh: a notification with one byte; 1 ends a refresh, 2
    // a card reset
    if (message.type != IPC_TYPE_NOTI || message.data.empty() || !refreshQualifier_)
        return;
    const uint8_t phase = message.data[0];
    const bool reset = *refreshQualifier_ == kRefreshUiccReset;
    if (!((phase == 1 && !reset) || (phase == 2 && reset)))
        return;

    SimRefresh r;
    r.type = reset ? 2 : *refreshQualifier_ == kRefreshFileChange ? 0 : 1;
    r.files = refreshFiles_;
    r.aid = refreshAid_;
    refreshQualifier_.reset();
    modem_.log("SIM refresh, type %d, %zu files", r.type, r.files.size());
    if (refresh_)
        refresh_(r);
}

}  // namespace shannond
