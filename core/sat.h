// SPDX-License-Identifier: GPL-2.0-or-later
//
// SIM toolkit: the card's proactive commands and the answers to them. The
// commands themselves (BER-TLV) are the client's business.
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "core/modem.h"

namespace shannond {

// A REFRESH the card asked for (3GPP TS 102 223 6.4.7), once the modem has
// done it. type: Android's SimRefreshResult type (0 files changed, 1 the
// application started again, 2 the card reset)
struct SimRefresh {
    int type = 0;
    std::vector<uint16_t> files;  // the elementary files that changed
    Bytes aid;
};

struct EnvelopeResult {
    uint8_t sw1 = 0;
    uint8_t sw2 = 0;
    Bytes data;
};

class Sat {
public:
    explicit Sat(Modem &modem);

    // Commands wait until the client says it can take them
    void onProactiveCommand(std::function<void(const Bytes &)> callback)
    {
        command_ = std::move(callback);
    }
    void onSessionEnd(std::function<void()> callback) { sessionEnd_ = std::move(callback); }
    void onRefresh(std::function<void(const SimRefresh &)> callback) { refresh_ = std::move(callback); }
    void clientRunning();

    void terminalResponse(const Bytes &response, std::function<void(bool ok)> done);
    void envelope(const Bytes &envelope,
                  std::function<void(std::optional<EnvelopeResult>)> done);

private:
    void proactive(const Message &message);
    void remember(const Bytes &command);
    void refreshed(const Message &message);
    void envelopeAnswer(std::optional<EnvelopeResult> result);

    Modem &modem_;
    bool running_ = false;
    std::vector<Bytes> waiting_;
    std::function<void(const Bytes &)> command_;
    std::function<void()> sessionEnd_;
    std::function<void(std::optional<EnvelopeResult>)> envelopeDone_;
    bool menuSelection_ = false;
    std::function<void(const SimRefresh &)> refresh_;
    // The last REFRESH command: its qualifier, files and application
    std::optional<uint8_t> refreshQualifier_;
    std::vector<uint16_t> refreshFiles_;
    Bytes refreshAid_;
};

}  // namespace shannond
