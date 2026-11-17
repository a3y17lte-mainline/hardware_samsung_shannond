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

namespace shannond {

struct SmsSendResult {
    enum class Error {
        None,
        Retry,       // a temporary network failure: worth sending again
        FdnBlocked,  // the SIM's fixed dialling numbers do not allow it
        Failed,
    };
    Error error = Error::Failed;
    uint8_t reference = 0;
    int rpCause = -1;  // the network's RP cause, when it refused

    bool ok() const { return error == Error::None; }
};

// A broadcast page as the network sent it (3GPP TS 23.041 9.4.1.2: 88
// bytes for GSM, or the UMTS/LTE form)
struct CellBroadcast {
    Bytes page;
};

// A range of message identifiers and data coding schemes (TS 23.041)
struct BroadcastRange {
    uint16_t fromId = 0;
    uint16_t toId = 0;
    uint8_t fromScheme = 0;
    uint8_t toScheme = 0xff;
    bool selected = true;
};

class Sms {
public:
    // pdu: the TPDU with the SMSC address in front, as the modem delivers
    // it. Every message must be acknowledged with acknowledge(), in order
    using Incoming = std::function<void(const Bytes &pdu, bool statusReport)>;
    using Sent = std::function<void(const SmsSendResult &)>;
    using Done = std::function<void(bool ok)>;

    explicit Sms(Modem &modem);

    // For a start without the modem's "SMS ready" message
    void adopt()
    {
        if (!ready_)
            deviceReady();
    }
    bool ready() const { return ready_; }
    const Bytes &smsc() const { return smsc_; }

    // Without a listener the core acknowledges each message itself
    void onIncoming(Incoming callback) { incoming_.push_back(std::move(callback)); }
    // The SIM's message store is full (or has room again)
    void onSimFull(std::function<void(bool full)> callback) { simFull_ = std::move(callback); }

    // The oldest unacknowledged message: stored (success) or not; a full
    // memory asks the network to keep it until setMemoryAvailable(true)
    void acknowledge(bool success, bool memoryFull);
    // Whether the AP has room for messages (the network holds them back
    // while it has not)
    void setMemoryAvailable(bool available, Done done);

    // tpdu without SMSC; the SMSC from the SIM is put in front unless smsc
    // (an address as in the PDU: type of number, BCD digits) is given
    void send(const Bytes &tpdu, Sent done, const Bytes *smsc = nullptr);
    // address: type of number, BCD digits
    void setSmsc(const Bytes &address, Done done);

    // The SIM's store. pdu has the SMSC in front (length byte first);
    // status is an IPC_SMS_STATUS; indexes count from 1, as Android's
    void writeToSim(uint8_t status, const Bytes &pdu, std::function<void(std::optional<int> index)> done);
    void deleteOnSim(int index, Done done);

    // Cell broadcast
    void onBroadcast(std::function<void(const CellBroadcast &)> callback)
    {
        broadcast_ = std::move(callback);
    }
    void setBroadcastConfig(const std::vector<BroadcastRange> &ranges, Done done);
    const std::vector<BroadcastRange> &broadcastConfig() const { return broadcastRanges_; }
    void setBroadcastActivation(bool active, Done done);

private:
    struct Pending {
        uint8_t network;
        uint8_t id;
        std::chrono::steady_clock::time_point at;
    };

    void deviceReady();
    void received(const Message &message);
    void sendReport(const Pending &pending, bool success, bool memoryFull);
    void sendResult(const Message &message);
    void broadcastReceived(const Message &message);
    void sendBroadcastConfig(Done done);

    Modem &modem_;
    bool ready_ = false;
    Bytes smsc_;
    std::vector<Incoming> incoming_;
    std::deque<Pending> unacknowledged_;
    std::function<void(bool)> simFull_;
    std::deque<Sent> sending_;
    std::function<void(const CellBroadcast &)> broadcast_;
    std::vector<BroadcastRange> broadcastRanges_;
    bool broadcastActive_ = false;
};

// The modem's send result code (ipc_sms_send_msg_response_data.ack) as the
// stock RIL classifies it
SmsSendResult smsSendResult(uint16_t code, uint8_t reference);

}  // namespace shannond
