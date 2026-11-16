// SPDX-License-Identifier: GPL-2.0-or-later
//
// SMS over the circuit switched side (3GPP TS 23.040 PDUs). Sending is only
// ever done on a front end's request.
#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "core/modem.h"

namespace modemd {

class Sms {
public:
    // pdu: the TPDU with the SMSC address in front, as the modem delivers it
    using Incoming = std::function<void(const Bytes &pdu)>;
    using Sent = std::function<void(bool ok, uint8_t reference)>;

    explicit Sms(Modem &modem);

    bool ready() const { return ready_; }
    const Bytes &smsc() const { return smsc_; }

    void onIncoming(Incoming callback) { incoming_.push_back(std::move(callback)); }

    // tpdu without SMSC; the SMSC from the SIM is put in front unless smsc
    // (an address as in the PDU: type of number, BCD digits) is given
    void send(const Bytes &tpdu, Sent done, const Bytes *smsc = nullptr);

private:
    void deviceReady();
    void received(const Message &message);
    void sendResult(const Message &message);

    Modem &modem_;
    bool ready_ = false;
    Bytes smsc_;
    std::vector<Incoming> incoming_;
    std::deque<Sent> sending_;
};

}  // namespace modemd
