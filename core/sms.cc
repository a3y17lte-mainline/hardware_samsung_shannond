// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/sms.h"

#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

// Cell broadcast as the stock RIL configures it (CBFilter): normal channels
// 0-999 and the warning channels (ETWS, CMAS, EU-Alert) 0x1100-0x18ff; the
// modem takes a list of at most 50 identifiers
constexpr uint16_t kNormalChannels = 1000;
constexpr uint16_t kWarningFirst = 0x1100;
constexpr uint16_t kWarningLast = 0x18ff;
constexpr size_t kMaxListedChannels = IPC_SMS_CBS_LIST_MAX;

using Channels = std::bitset<0x10000>;

Channels selectedChannels(const std::vector<BroadcastRange> &ranges)
{
    Channels channels;
    for (const BroadcastRange &r : ranges) {
        const uint16_t from = std::min(r.fromId, r.toId);
        const uint16_t to = std::max(r.fromId, r.toId);
        const bool normal = to < kNormalChannels;
        const bool warning = from >= kWarningFirst && to <= kWarningLast;
        if (!normal && !warning)
            continue;  // the modem has no filter for these
        for (uint32_t id = from; id <= to; id++)
            channels[id] = r.selected;
    }
    return channels;
}

bool any(const Channels &c, uint32_t from, uint32_t to)
{
    for (uint32_t id = from; id <= to; id++)
        if (c[id])
            return true;
    return false;
}

bool all(const Channels &c, uint32_t from, uint32_t to)
{
    for (uint32_t id = from; id <= to; id++)
        if (!c[id])
            return false;
    return true;
}

// The message identifier of a page: GSM pages start with the serial
// number, UMTS ones with the message type (TS 23.041 9.4)
std::optional<uint16_t> broadcastId(const Bytes &page, bool umts)
{
    const size_t at = umts ? 1 : 2;
    if (page.size() < at + 2)
        return std::nullopt;
    return static_cast<uint16_t>(page[at] << 8 | page[at + 1]);
}

}  // namespace

SmsSendResult smsSendResult(uint16_t code, uint8_t reference)
{
    // IpcProtocol41::GetSmsResult and SmsManager::OnSendSmsDone
    SmsSendResult r;
    r.reference = reference;
    if (code == IPC_SMS_ACK_NO_ERROR) {
        r.error = SmsSendResult::Error::None;
    } else if (code == IPC_SMS_SEND_RESULT_FDN_BLOCKED || code == IPC_SMS_SEND_RESULT_FDN_BLOCKED_SMSC) {
        r.error = SmsSendResult::Error::FdnBlocked;
    } else if (code > IPC_SMS_SEND_RESULT_RP_CAUSE) {
        r.rpCause = code - IPC_SMS_SEND_RESULT_RP_CAUSE;
        // Destination out of order, temporary failure, congestion,
        // resources unavailable, and two of the modem's own: try again
        switch (r.rpCause) {
        case 27: case 41: case 42: case 47: case 98: case 111:
            r.error = SmsSendResult::Error::Retry;
            break;
        default:
            r.error = SmsSendResult::Error::Failed;
            break;
        }
    } else {
        r.error = SmsSendResult::Error::Failed;
    }
    return r;
}

Sms::Sms(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SMS_DEVICE_READY, [this](const Message &) { deviceReady(); });
    modem_.onMessage(IPC_SMS_SVC_CENTER_ADDR, [this](const Message &m) {
        if (m.type == IPC_TYPE_RESP || m.type == IPC_TYPE_NOTI) {
            if (m.data.empty() || m.data[0] >= m.data.size())
                return;
            smsc_.assign(m.data.begin() + 1, m.data.begin() + 1 + m.data[0]);
        }
    });
    modem_.onMessage(IPC_SMS_INCOMING_MSG, [this](const Message &m) { received(m); });
    modem_.onMessage(IPC_SMS_SEND_MSG, [this](const Message &m) { sendResult(m); });
    modem_.onMessage(IPC_SMS_MEM_STATUS, [this](const Message &m) {
        auto status = m.as<ipc_sms_mem_status_data>();
        if (m.type != IPC_TYPE_NOTI || !status || status->network != IPC_SMS_NETWORK_GSM)
            return;
        modem_.log("SIM message store %s", status->status == IPC_SMS_MEM_STATUS_FULL ? "full" : "has room");
        if (simFull_)
            simFull_(status->status == IPC_SMS_MEM_STATUS_FULL);
    });
    modem_.onMessage(IPC_SMS_CBS_MSG, [this](const Message &m) { broadcastReceived(m); });
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

    // The stock RIL acknowledges once the framework has the message, with
    // the network byte the message came with
    const Pending pending{header->msg_type, header->id};
    const bool statusReport = header->type == IPC_SMS_TYPE_STATUS_REPORT;
    const Bytes pdu(message.data.begin() + sizeof(*header),
                    message.data.begin() + sizeof(*header) + header->length);

    if (incoming_.empty()) {
        sendReport(pending, true, false);
        return;
    }
    unacknowledged_.push_back(pending);
    for (auto &callback : incoming_)
        callback(pdu, statusReport);
}

void Sms::acknowledge(bool success, bool memoryFull)
{
    if (unacknowledged_.empty()) {
        modem_.log("SMS acknowledgement without a message");
        return;
    }
    const Pending pending = unacknowledged_.front();
    unacknowledged_.pop_front();
    sendReport(pending, success, memoryFull);
}

void Sms::sendReport(const Pending &pending, bool success, bool memoryFull)
{
    // IpcProtocol41::IpcTxDeliveryReport: for GSM a full memory is "AP
    // memory full", anything else unspecified
    ipc_sms_deliver_report_request_data report;
    std::memset(&report, 0, sizeof(report));
    report.type = pending.network;
    report.ack = success ? IPC_SMS_ACK_NO_ERROR
                         : memoryFull ? IPC_SMS_ACK_PDA_FULL_ERROR : IPC_SMS_ACK_UNSPEC_ERROR;
    report.id = pending.id;
    modem_.request(IPC_SMS_DELIVER_REPORT, IPC_TYPE_EXEC, report);
}

void Sms::setMemoryAvailable(bool available, Done done)
{
    ipc_sms_mem_status_request_data data{};
    data.status = available ? IPC_SMS_MEM_STATUS_AVAILABLE : IPC_SMS_MEM_STATUS_FULL;
    modem_.request(IPC_SMS_MEM_STATUS, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

void Sms::send(const Bytes &tpdu, Sent done, const Bytes *smscOverride)
{
    const Bytes &smsc_ = smscOverride && !smscOverride->empty() ? *smscOverride : this->smsc_;
    if (tpdu.empty() || 1 + smsc_.size() + tpdu.size() > 0xff) {
        done(SmsSendResult{});
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
                           done(SmsSendResult{});
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
    const SmsSendResult r = smsSendResult(result->ack, result->id);
    if (!r.ok())
        modem_.log("SMS not sent: result 0x%04x", result->ack);
    done(r);
}

void Sms::setSmsc(const Bytes &address, Done done)
{
    // IpcTxSetSmscAddress: the address with its length first, zero padded
    if (address.empty() || 1 + address.size() > IPC_SMS_SVC_CENTER_ADDR_MAX) {
        done(false);
        return;
    }
    ipc_sms_svc_center_addr_set_data data{};
    data.address[0] = static_cast<uint8_t>(address.size());
    std::memcpy(&data.address[1], address.data(), address.size());
    modem_.request(IPC_SMS_SVC_CENTER_ADDR, IPC_TYPE_SET, data,
                   [this, address, done](const Result &r) {
                       if (r.ok)
                           smsc_ = address;
                       done(r.ok);
                   });
}

void Sms::writeToSim(uint8_t status, const Bytes &pdu,
                     std::function<void(std::optional<int>)> done)
{
    // IpcTxWriteSmsToSim: GSM store, the first free record
    if (pdu.empty() || pdu.size() > 0xff) {
        done(std::nullopt);
        return;
    }
    ipc_sms_save_msg_request_header header{};
    header.magic = IPC_SMS_NETWORK_GSM;
    header.index = IPC_SMS_SIM_INDEX_ANY;
    header.status = status;
    header.length = static_cast<uint8_t>(pdu.size());
    Bytes payload = toBytes(header);
    payload.insert(payload.end(), pdu.begin(), pdu.end());

    modem_.request(IPC_SMS_SAVE_MSG, IPC_TYPE_EXEC, std::move(payload),
                   [this, done](const Result &r) {
                       auto res = r.response ? r.response->as<ipc_sms_save_msg_response_data>()
                                             : std::nullopt;
                       if (!r.ok || !res || res->error != IPC_SMS_ACK_NO_ERROR) {
                           if (res)
                               modem_.log("SMS not stored on the SIM: 0x%04x", res->error);
                           done(std::nullopt);
                           return;
                       }
                       done(res->index + 1);
                   });
}

void Sms::deleteOnSim(int index, Done done)
{
    if (index < 1 || index > 0xffff) {
        done(false);
        return;
    }
    ipc_sms_del_msg_request_data data{};
    data.magic = IPC_SMS_NETWORK_GSM;
    data.index = static_cast<uint16_t>(index - 1);
    modem_.request(IPC_SMS_DEL_MSG, IPC_TYPE_EXEC, data, [done](const Result &r) {
        auto res = r.response ? r.response->as<ipc_sms_del_msg_response_data>() : std::nullopt;
        done(r.ok && (!res || res->error == IPC_SMS_ACK_NO_ERROR));
    });
}

void Sms::broadcastReceived(const Message &message)
{
    // IpcRxIncomingCBMsg: the page's format (2: UMTS), its length (one
    // byte, or two in the stock RIL's Japanese and Korean variants), the page
    auto info = message.as<ipc_sms_cbs_msg_header>();
    if (!info) {
        modem_.log("short cell broadcast");
        return;
    }
    const Bytes &d = message.data;
    Bytes page;
    if (sizeof(*info) + info->length == d.size()) {
        page.assign(d.begin() + sizeof(*info), d.end());
    } else if (d.size() >= 3 && 3u + (d[1] | d[2] << 8) == d.size()) {
        page.assign(d.begin() + 3, d.end());
    } else {
        modem_.log("cell broadcast of %zu bytes with length %u", d.size(), info->length);
        return;
    }

    // CBFilter::FilterBroadcastSms: only the channels the framework asked for
    const auto id = broadcastId(page, info->type == IPC_SMS_CBS_TYPE_UMTS);
    if (!id || !selectedChannels(broadcastRanges_)[*id]) {
        modem_.log("cell broadcast on channel %d dropped", id ? *id : -1);
        return;
    }
    if (broadcast_)
        broadcast_(CellBroadcast{std::move(page)});
}

void Sms::setBroadcastConfig(const std::vector<BroadcastRange> &ranges, Done done)
{
    broadcastRanges_ = ranges;
    sendBroadcastConfig(std::move(done));
}

void Sms::sendBroadcastConfig(Done done)
{
    // SmsManager::DoGsmSetBroadcastSmsConfig with CBFilter::GetSelectedId
    // and GetBroadcastSmsConfig, for a phone without a regional broadcast type
    const Channels c = selectedChannels(broadcastRanges_);
    const bool enabled = c.any();
    const bool warnings = any(c, kWarningFirst, kWarningLast);
    const bool allNormal = all(c, 0, kNormalChannels - 1);

    uint8_t selection;
    if (warnings) {
        selection = IPC_SMS_CBS_SELECT_WARNINGS;
        if (allNormal)
            selection |= IPC_SMS_CBS_SELECT_ALL_NORMAL;
        if (any(c, 0x1112, 0x112f))
            selection |= IPC_SMS_CBS_SELECT_ALL_WARNINGS;
    } else {
        selection = allNormal ? IPC_SMS_CBS_SELECT_ALL : IPC_SMS_CBS_SELECT_LIST;
    }

    std::vector<uint16_t> ids;
    const bool listNormal = selection == IPC_SMS_CBS_SELECT_LIST ||
                            (selection & (IPC_SMS_CBS_SELECT_WARNINGS | IPC_SMS_CBS_SELECT_ALL_NORMAL)) ==
                                    IPC_SMS_CBS_SELECT_WARNINGS;
    if (listNormal)
        for (uint32_t id = 0; id < kNormalChannels && ids.size() < kMaxListedChannels; id++)
            if (c[id])
                ids.push_back(id);
    if ((selection & (IPC_SMS_CBS_SELECT_WARNINGS | IPC_SMS_CBS_SELECT_ALL_WARNINGS)) ==
        IPC_SMS_CBS_SELECT_WARNINGS)
        for (uint32_t id = kWarningFirst; id <= kWarningLast + 1 && ids.size() < kMaxListedChannels; id++)
            if (c[id])
                ids.push_back(id);
    // IpcTxSetCBConfig: without the warning bit the list is padded to its
    // full length with 0xffff
    if (!(selection & IPC_SMS_CBS_SELECT_WARNINGS))
        ids.resize(kMaxListedChannels, 0xffff);

    ipc_sms_cbs_cfg_header header{};
    header.enable = enabled ? IPC_SMS_CBS_ENABLE : IPC_SMS_CBS_DISABLE;
    header.selection = selection;
    header.count = static_cast<uint8_t>(ids.size());
    Bytes payload = toBytes(header);
    for (uint16_t id : ids) {
        payload.push_back(id >> 8);  // big endian, as the stock RIL swaps them
        payload.push_back(id & 0xff);
    }
    modem_.request(IPC_SMS_CBS_CFG, IPC_TYPE_SET, std::move(payload),
                   [done](const Result &r) { done(r.ok); });
}

void Sms::setBroadcastActivation(bool active, Done done)
{
    // SmsManager::DoGsmSetCBActivation: on when any channel is chosen,
    // whatever the framework asks (IpcTxSetCBActivation)
    (void)active;
    broadcastActive_ = selectedChannels(broadcastRanges_).any();
    ipc_sms_cbs_activation_data data;
    std::memset(&data, 0, sizeof(data));
    data.enable = broadcastActive_ ? IPC_SMS_CBS_ENABLE : IPC_SMS_CBS_DISABLE;
    data.unknown1 = 1;
    data.unknown2 = 10;
    std::memset(data.unknown3, 0xff, sizeof(data.unknown3));
    modem_.request(IPC_SMS_CBS_CFG, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

}  // namespace shannond
