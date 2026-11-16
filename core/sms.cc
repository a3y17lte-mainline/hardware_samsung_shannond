// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/sms.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include <samsung-ipc.h>
}

namespace modemd {

Sms::Sms(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SMS_DEVICE_READY, [this](const Message &) { deviceReady(); });
    modem_.onMessage(IPC_SMS_SVC_CENTER_ADDR, [this](const Message &m) {
        if (m.data.empty() || m.data[0] >= m.data.size())
            return;
        smsc_.assign(m.data.begin() + 1, m.data.begin() + 1 + m.data[0]);
    });
    modem_.onMessage(IPC_SMS_INCOMING_MSG, [this](const Message &m) { received(m); });
    modem_.onMessage(IPC_SMS_SEND_MSG, [this](const Message &m) { sendResult(m); });
}

void Sms::deviceReady()
{
    ready_ = true;
    modem_.request(IPC_SMS_SVC_CENTER_ADDR, IPC_TYPE_GET);
}

void Sms::received(const Message &message)
{
    auto header = message.as<ipc_sms_incoming_msg_header>();
    if (!header || sizeof(*header) + header->length > message.data.size()) {
        modem_.log("short incoming SMS");
        return;
    }

    // Acknowledge with the type the message came with: acknowledging a
    // delivered SMS as a status report made the network deliver it again,
    // and the modem crashed after the duplicate
    ipc_sms_deliver_report_request_data report;
    std::memset(&report, 0, sizeof(report));
    report.type = header->type;
    report.ack = IPC_SMS_ACK_NO_ERROR;
    report.id = header->id;
    modem_.request(IPC_SMS_DELIVER_REPORT, IPC_TYPE_EXEC, report);

    const Bytes pdu(message.data.begin() + sizeof(*header),
                    message.data.begin() + sizeof(*header) + header->length);
    for (auto &callback : incoming_)
        callback(pdu);
}

void Sms::send(const Bytes &tpdu, Sent done)
{
    if (tpdu.empty() || 1 + smsc_.size() + tpdu.size() > 0xff) {
        done(false, 0);
        return;
    }

    ipc_sms_send_msg_request_header header{};
    header.type = IPC_SMS_TYPE_OUTGOING;
    header.msg_type = IPC_SMS_MSG_TYPE_SINGLE;
    header.length = static_cast<unsigned char>(1 + smsc_.size() + tpdu.size());

    Bytes payload = toBytes(header);
    payload.push_back(static_cast<uint8_t>(smsc_.size()));
    payload.insert(payload.end(), smsc_.begin(), smsc_.end());
    payload.insert(payload.end(), tpdu.begin(), tpdu.end());

    sending_.push_back(std::move(done));
    modem_.request(IPC_SMS_SEND_MSG, IPC_TYPE_EXEC, std::move(payload),
                   [this](const Result &r) {
                       // Refused outright: there will be no send result
                       if (!r.ok && !sending_.empty()) {
                           auto done = std::move(sending_.front());
                           sending_.pop_front();
                           done(false, 0);
                       }
                   },
                   std::chrono::seconds(60));
}

void Sms::sendResult(const Message &message)
{
    auto result = message.as<ipc_sms_send_msg_response_data>();
    if (!result || sending_.empty())
        return;

    auto done = std::move(sending_.front());
    sending_.pop_front();
    done(result->ack == IPC_SMS_ACK_NO_ERROR, result->id);
}

}  // namespace modemd
