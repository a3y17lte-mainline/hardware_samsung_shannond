// SPDX-License-Identifier: GPL-2.0-or-later
#include "sim/fake_modem.h"

#include "core/sim.h"
#include "core/text.h"

#include <algorithm>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

// Test values only: not this phone's identities
constexpr char kImei[] = "350000000000001";
constexpr char kImsi[] = "230020000000001";
constexpr char kPlmn[] = "23002#";
constexpr uint8_t kRawTechLte = 0x21;

}  // namespace

FakeModem::FakeModem(EventLoop &loop) : loop_(loop)
{
    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fmt_);
    socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, rfs_);
    loop_.watch(fmt_[1], EPOLLIN, [this](uint32_t) { readFmt(); });
    loop_.watch(rfs_[1], EPOLLIN, [this](uint32_t) { readRfs(); });
}

FakeModem::~FakeModem()
{
    for (int fd : {fmt_[1], rfs_[1]}) {
        if (fd >= 0) {
            loop_.unwatch(fd);
            close(fd);
        }
    }
    close(fmt_[0]);
    close(rfs_[0]);
}

size_t FakeModem::count(uint16_t command) const
{
    size_t n = 0;
    for (auto &m : received)
        n += m.command == command;
    return n;
}

void FakeModem::send(uint16_t command, uint8_t type, const Bytes &data, uint8_t aseq)
{
    Message m;
    m.command = command;
    m.type = type;
    m.mseq = ++seq_;
    m.aseq = aseq;
    m.data = data;
    const Bytes frame = encodeFmt(m);
    (void)!write(fmt_[1], frame.data(), frame.size());
}

void FakeModem::ack(const Message &request, bool ok)
{
    ipc_gen_phone_res_data res{};
    res.group = request.command >> 8;
    res.index = request.command & 0xff;
    res.type = request.type;
    res.code = ok ? IPC_GEN_PHONE_RES_CODE_SUCCESS : 0x8001;
    send(IPC_GEN_PHONE_RES, IPC_TYPE_RESP, toBytes(res), request.mseq);
}

void FakeModem::respond(const Message &request, const Bytes &data)
{
    send(request.command, IPC_TYPE_RESP, data, request.mseq);
}

void FakeModem::boot()
{
    send(IPC_PWR_PHONE_PWR_UP, IPC_TYPE_NOTI, {});

    ipc_sec_pin_status_response_data pin{};
    pin.status = IPC_SEC_PIN_STATUS_INIT_COMPLETE;
    send(IPC_SEC_PIN_STATUS, IPC_TYPE_NOTI, toBytes(pin));

    Bytes iccid = {10, 0x98, 0x32, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf1};
    send(IPC_SEC_SIM_ICCID, IPC_TYPE_NOTI, iccid);

    send(IPC_SMS_DEVICE_READY, IPC_TYPE_NOTI, {});
    sendRegistration(1, 0);  // LTE: one record for both domains
}

sim::Milenage FakeModem::testMilenage()
{
    sim::Milenage m;
    m.k = {0x46, 0x5b, 0x5c, 0xe8, 0xb1, 0x99, 0xb4, 0x9f, 0xaa, 0x5f, 0x0a, 0x2e, 0xe2, 0x38, 0xa6, 0xbc};
    m.opc = {0xcd, 0x63, 0xcb, 0x71, 0x95, 0x4a, 0x9f, 0x4e, 0x48, 0xa5, 0x99, 0x4e, 0x37, 0xa0, 0x2b, 0xaf};
    return m;
}

void FakeModem::sendRegistration(uint8_t domain, uint8_t aseq)
{
    ipc_net_regist_response_ext_data reg{};
    reg.act = kRawTechLte;
    reg.domain = domain;
    reg.status = IPC_NET_REGISTRATION_STATUS_HOME;
    reg.lac = 0x1234;
    reg.cid = 0x0abcdef;
    reg.tac = 0x5678;
    reg.volte = volteSupported ? IPC_NET_LTE_SUPPORTED : IPC_NET_LTE_NOT_SUPPORTED;
    reg.ims_emergency = IPC_NET_LTE_NOT_SUPPORTED;
    reg.pci = 301;
    send(IPC_NET_REGIST, aseq ? IPC_TYPE_RESP : IPC_TYPE_NOTI, toBytes(reg), aseq);
}

void FakeModem::networkChanged()
{
    sendRegistration(IPC_NET_SERVICE_DOMAIN_GSM, 0);
    sendRegistration(IPC_NET_SERVICE_DOMAIN_GPRS, 0);
}

void FakeModem::limitedService()
{
    for (uint8_t domain : {IPC_NET_SERVICE_DOMAIN_GSM, IPC_NET_SERVICE_DOMAIN_GPRS}) {
        ipc_net_regist_response_data reg{};
        reg.act = 0x01;  // GSM
        reg.domain = domain;
        reg.status = IPC_NET_REGISTRATION_STATUS_EMERGENCY;
        send(IPC_NET_REGIST, IPC_TYPE_NOTI, toBytes(reg));
    }
}

void FakeModem::pinStatus(uint8_t status, uint8_t facilityLock)
{
    ipc_sec_pin_status_response_data pin{};
    pin.status = status;
    pin.facility_lock = facilityLock;
    send(IPC_SEC_PIN_STATUS, IPC_TYPE_NOTI, toBytes(pin));
}

void FakeModem::readFmt()
{
    uint8_t buffer[0x10000];
    ssize_t n;
    while ((n = read(fmt_[1], buffer, sizeof(buffer))) > 0) {
        auto m = decodeFmt({buffer, static_cast<size_t>(n)});
        if (m) {
            received.push_back(*m);
            handle(*m);
        }
    }
}

void FakeModem::readRfs()
{
    uint8_t buffer[0x10000];
    ssize_t n;
    while ((n = read(rfs_[1], buffer, sizeof(buffer))) > 0) {
        if (auto m = decodeRfs({buffer, static_cast<size_t>(n)}))
            rfsReceived.push_back(*m);
    }
}

Bytes FakeModem::callList() const
{
    Bytes data{static_cast<uint8_t>(calls_.size())};
    for (auto &c : calls_) {
        ipc_call_list_entry e{};
        e.type = IPC_CALL_TYPE_VOICE;
        e.id = c.id;
        e.term = c.incoming ? IPC_CALL_TERM_MT : IPC_CALL_TERM_MO;
        e.status = c.status;
        e.mpty = c.mpty;
        e.number_length = c.number.size();
        e.unknown2 = 0x81;
        Bytes entry = toBytes(e);
        data.insert(data.end(), entry.begin(), entry.end());
        data.insert(data.end(), c.number.begin(), c.number.end());
    }
    return data;
}

void FakeModem::setCallState(uint8_t status)
{
    ipc_call_status_data s{};
    s.type = IPC_CALL_TYPE_VOICE;
    s.id = calls_.empty() ? 1 : calls_[0].id;
    s.status = status;
    send(IPC_CALL_STATUS, IPC_TYPE_NOTI, toBytes(s));
}

void FakeModem::sendUssd(uint8_t state, const std::string &text)
{
    const Bytes packed = packGsm7(text).value_or(Bytes{});
    Bytes data{state, 0x0f, static_cast<uint8_t>(packed.size())};
    data.insert(data.end(), packed.begin(), packed.end());
    send(IPC_SS_USSD, IPC_TYPE_NOTI, data);
}

void FakeModem::incomingSms(const Bytes &pdu, uint8_t type)
{
    ipc_sms_incoming_msg_header header{};
    header.msg_type = IPC_SMS_NETWORK_GSM;
    header.type = type;
    header.id = smsId_++;
    header.length = pdu.size();
    Bytes data = toBytes(header);
    data.insert(data.end(), pdu.begin(), pdu.end());
    send(IPC_SMS_INCOMING_MSG, IPC_TYPE_NOTI, data);
}

void FakeModem::incomingCall(const std::string &number)
{
    calls_ = {{1, IPC_CALL_LIST_ENTRY_STATUS_INCOMING, true, number}};
    ipc_call_incoming_data in{};
    in.type = IPC_CALL_TYPE_VOICE;
    in.id = 1;
    send(IPC_CALL_INCOMING, IPC_TYPE_NOTI, toBytes(in));
}

void FakeModem::waitingCall(const std::string &number)
{
    const uint8_t id = calls_.empty() ? 1 : calls_.back().id + 1;
    calls_.push_back({id, IPC_CALL_LIST_ENTRY_STATUS_WAITING, true, number});
    ipc_call_incoming_data in{};
    in.type = IPC_CALL_TYPE_VOICE;
    in.id = id;
    send(IPC_CALL_INCOMING, IPC_TYPE_NOTI, toBytes(in));
}

void FakeModem::manageCalls(const Message &r)
{
    // 22.030 call handling on the fake calls
    auto m = r.as<ipc_ss_manage_call_data>();
    if (!m) {
        ack(r, false);
        return;
    }
    auto has = [&](uint8_t status) {
        return std::any_of(calls_.begin(), calls_.end(), [&](const FakeCall &c) { return c.status == status; });
    };
    auto drop = [&](auto pred) { calls_.erase(std::remove_if(calls_.begin(), calls_.end(), pred), calls_.end()); };
    auto set = [&](uint8_t from, uint8_t to) {
        for (auto &c : calls_)
            if (c.status == from)
                c.status = to;
    };
    constexpr uint8_t kParked = 0xee;
    switch (m->operation) {
    case IPC_SS_MANAGE_CALL_RELEASE_HELD_OR_WAITING:
        if (has(IPC_CALL_LIST_ENTRY_STATUS_WAITING))
            drop([](const FakeCall &c) { return c.status == IPC_CALL_LIST_ENTRY_STATUS_WAITING; });
        else
            drop([](const FakeCall &c) { return c.status == IPC_CALL_LIST_ENTRY_STATUS_HOLDING; });
        break;
    case IPC_SS_MANAGE_CALL_RELEASE_ACTIVE_ACCEPT:
        drop([](const FakeCall &c) { return c.status == IPC_CALL_LIST_ENTRY_STATUS_ACTIVE; });
        if (has(IPC_CALL_LIST_ENTRY_STATUS_WAITING))
            set(IPC_CALL_LIST_ENTRY_STATUS_WAITING, IPC_CALL_LIST_ENTRY_STATUS_ACTIVE);
        else
            set(IPC_CALL_LIST_ENTRY_STATUS_HOLDING, IPC_CALL_LIST_ENTRY_STATUS_ACTIVE);
        break;
    case IPC_SS_MANAGE_CALL_RELEASE_ONE:
        drop([&](const FakeCall &c) { return c.id == m->id; });
        break;
    case IPC_SS_MANAGE_CALL_SWAP:
        set(IPC_CALL_LIST_ENTRY_STATUS_ACTIVE, kParked);
        if (has(IPC_CALL_LIST_ENTRY_STATUS_WAITING))
            set(IPC_CALL_LIST_ENTRY_STATUS_WAITING, IPC_CALL_LIST_ENTRY_STATUS_ACTIVE);
        else
            set(IPC_CALL_LIST_ENTRY_STATUS_HOLDING, IPC_CALL_LIST_ENTRY_STATUS_ACTIVE);
        set(kParked, IPC_CALL_LIST_ENTRY_STATUS_HOLDING);
        break;
    case IPC_SS_MANAGE_CALL_CONFERENCE:
        for (auto &c : calls_) {
            if (c.status == IPC_CALL_LIST_ENTRY_STATUS_ACTIVE || c.status == IPC_CALL_LIST_ENTRY_STATUS_HOLDING) {
                c.status = IPC_CALL_LIST_ENTRY_STATUS_ACTIVE;
                c.mpty = true;
            }
        }
        break;
    case IPC_SS_MANAGE_CALL_SPLIT:
        for (auto &c : calls_) {
            c.status = c.id == m->id ? IPC_CALL_LIST_ENTRY_STATUS_ACTIVE : IPC_CALL_LIST_ENTRY_STATUS_HOLDING;
            c.mpty = c.id != m->id && calls_.size() > 2;
        }
        break;
    case IPC_SS_MANAGE_CALL_TRANSFER:
        calls_.clear();
        break;
    default:
        ack(r, false);
        return;
    }
    ack(r);
    ipc_call_status_data st{};
    st.type = IPC_CALL_TYPE_VOICE;
    st.id = m->id;
    st.status = calls_.empty() ? IPC_CALL_STATUS_RELEASED : IPC_CALL_STATUS_CONNECTED;
    send(IPC_CALL_STATUS, IPC_TYPE_NOTI, toBytes(st));
}

void FakeModem::remoteHangup()
{
    setCallState(IPC_CALL_STATUS_RELEASED);
    calls_.clear();
}

void FakeModem::nvRead(uint32_t offset, uint32_t length)
{
    ipc_rfs_nv_read_item_request_data req{offset, length};
    RfsMessage m{IPC_RFS_NV_READ_ITEM & 0xff, ++rfsSeq_, toBytes(req)};
    const Bytes frame = encodeRfs(m);
    (void)!write(rfs_[1], frame.data(), frame.size());
}

void FakeModem::nvWrite(uint32_t offset, const Bytes &data)
{
    ipc_rfs_nv_write_item_request_header req{offset, static_cast<unsigned int>(data.size())};
    RfsMessage m{IPC_RFS_NV_WRITE_ITEM & 0xff, ++rfsSeq_, toBytes(req)};
    m.data.insert(m.data.end(), data.begin(), data.end());
    const Bytes frame = encodeRfs(m);
    const size_t chunk = rfsChunk ? rfsChunk : frame.size();
    for (size_t at = 0; at < frame.size(); at += chunk)
        (void)!write(rfs_[1], frame.data() + at, std::min(chunk, frame.size() - at));
}

void FakeModem::crash()
{
    for (int *fd : {&fmt_[1], &rfs_[1]}) {
        loop_.unwatch(*fd);
        close(*fd);
        *fd = -1;
    }
}

void FakeModem::handle(const Message &r)
{
    if (r.type == IPC_TYPE_EVENT)
        return;  // events have no answer
    switch (r.command) {
    case IPC_MISC_ME_SN: {
        ipc_misc_me_sn_response_data sn{};
        sn.type = IPC_MISC_ME_SN_SERIAL_NUM;
        sn.length = sizeof(kImei) - 1;
        std::memcpy(sn.data, kImei, sn.length);
        respond(r, toBytes(sn));
        return;
    }
    case IPC_SAT_PROACTIVE_CMD:
        // A terminal response: the card has nothing more
        respond(r, Bytes{0x90, 0x00});
        return;
    case IPC_SAT_ENVELOPE_CMD:
        // Answered by a notification, with two bytes from the card
        send(IPC_SAT_ENVELOPE_CMD, IPC_TYPE_NOTI, Bytes{0x90, 0x00, 2, 0xaa, 0xbb},
             envelopeAnswersWithSequence ? r.mseq : 0);
        return;
    case IPC_MISC_ME_VERSION:
        respond(r, Bytes(sizeof(ipc_misc_me_version_response_data), 'v'));
        return;
    case IPC_SEC_PIN_STATUS: {
        if (r.type != IPC_TYPE_GET) {
            ack(r);
            return;
        }
        ipc_sec_pin_status_response_data pin{};
        pin.status = IPC_SEC_PIN_STATUS_INIT_COMPLETE;
        respond(r, toBytes(pin));
        return;
    }
    case IPC_MISC_ME_IMSI: {
        Bytes data{static_cast<uint8_t>(sizeof(kImsi) - 1)};
        data.insert(data.end(), kImsi, kImsi + sizeof(kImsi) - 1);
        respond(r, data);
        return;
    }
    case IPC_NET_REGIST: {
        auto req = r.as<ipc_net_regist_request_data>();
        sendRegistration(req ? req->domain : 1, r.mseq);
        return;
    }
    case IPC_NET_SERVING_NETWORK: {
        ipc_net_serving_network_data s{};
        std::memcpy(s.plmn, kPlmn, sizeof(s.plmn));
        respond(r, toBytes(s));

        // The network's names follow, as the phone's modem sends them
        Bytes identity(103, 0);
        identity[1] = 0xff;
        for (size_t at : {size_t{2}, size_t{37}}) {
            identity[at] = 1;
            identity[at + 1] = 5;
            identity[at + 2] = 1;
            std::memcpy(&identity[at + 3], "O2.CZ", 5);
        }
        send(IPC_NET_IDENTITY, IPC_TYPE_NOTI, identity);
        return;
    }
    case IPC_NET_MODE_SEL: {
        auto mode = r.as<ipc_net_mode_sel_data>();
        bool ok = mode != std::nullopt;
        for (int bit = 0; ok && bit < 8; bit++)
            if ((mode->mode_sel & (1 << bit)) && !acceptedModes.count(1 << bit))
                ok = false;
        ack(r, ok);
        return;
    }
    case IPC_SEC_SIM_APPS_INFO: {
        Bytes data(1 + 2 * sizeof(ipc_sec_sim_app), 0);
        data[0] = 2;
        auto *apps = reinterpret_cast<ipc_sec_sim_app *>(data.data() + 1);
        static const uint8_t usim[] = {0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02};
        static const uint8_t isim[] = {0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x04};
        apps[0].type = kSimAppUsim;
        apps[0].aid_length = sizeof(usim);
        std::memcpy(apps[0].aid, usim, sizeof(usim));
        apps[1].type = kSimAppIsim;
        apps[1].aid_length = sizeof(isim);
        std::memcpy(apps[1].aid, isim, sizeof(isim));
        respond(r, data);
        return;
    }
    case IPC_SEC_RSIM_ACCESS: {
        auto h = r.as<ipc_sec_rsim_aid_request_header>();
        auto answer = [&](uint8_t sw1, uint8_t sw2, const Bytes &body) {
            Bytes data{sw1, sw2, static_cast<uint8_t>(body.size())};
            data.insert(data.end(), body.begin(), body.end());
            respond(r, data);
        };
        const uint16_t fileId = h ? uint16_t(h->file_id) : 0;  // packed: copied before use
        const bool binary = h && simBinary.count(fileId);
        const bool records = h && simRecords.count(fileId);
        if (!binary && !records) {
            answer(0x90, 0x00, Bytes{'a', 'b', 'c'});
            return;
        }
        if (h->command == IPC_SEC_RSIM_COMMAND_GET_RESPONSE) {
            // FCP: descriptor, file id, size or record length and count
            Bytes fcp{0x62, 0};
            if (binary) {
                size_t n = simBinary[fileId].size();
                fcp.insert(fcp.end(), {0x82, 0x02, 0x41, 0x21, 0x80, 0x02, uint8_t(n >> 8), uint8_t(n)});
            } else {
                const auto &recs = simRecords[fileId];
                uint8_t length = recs.empty() ? 0 : static_cast<uint8_t>(recs[0].size());
                fcp.insert(fcp.end(), {0x82, 0x05, 0x42, 0x21, 0x00, length, uint8_t(recs.size())});
            }
            fcp[1] = static_cast<uint8_t>(fcp.size() - 2);
            answer(0x90, 0x00, fcp);
            return;
        }
        if (h->command == IPC_SEC_RSIM_COMMAND_READ_BINARY && binary) {
            const Bytes &f = simBinary[fileId];
            size_t offset = h->p1 << 8 | h->p2;
            size_t n = std::min<size_t>(h->p3, offset < f.size() ? f.size() - offset : 0);
            answer(0x90, 0x00, Bytes(f.begin() + std::min(offset, f.size()), f.begin() + std::min(offset, f.size()) + n));
            return;
        }
        if (h->command == IPC_SEC_RSIM_COMMAND_READ_RECORD && records) {
            const auto &recs = simRecords[fileId];
            if (h->p1 == 0 || h->p1 > recs.size()) {
                answer(0x6a, 0x83, {});  // record not found
                return;
            }
            answer(0x90, 0x00, recs[h->p1 - 1]);
            return;
        }
        answer(0x6a, 0x82, {});  // file not found
        return;
    }
    case IPC_SEC_GSIM_ACCESS: {
        // MANAGE CHANNEL open: channel 1, 90 00
        respond(r, Bytes{0x00, 0x03, 0x01, 0x90, 0x00});
        return;
    }
    case IPC_SEC_IMS_AUTH: {
        // type, RAND and AUTN with a length byte each
        const Bytes &q = r.data;
        if (q.size() < 2 + 16 + 1 + 16 || q[1] != 16 || q[18] != 16) {
            respond(r, Bytes{q.empty() ? uint8_t(0) : q[0], 2});
            return;
        }
        sim::Block rand, autn;
        std::copy_n(q.begin() + 2, 16, rand.begin());
        std::copy_n(q.begin() + 19, 16, autn.begin());
        auto result = sim::cardAuthenticate(milenage, sqnMs, rand, autn);
        Bytes data{q[0], result.status};
        for (const Bytes &v : result.values) {
            data.push_back(static_cast<uint8_t>(v.size()));
            data.insert(data.end(), v.begin(), v.end());
        }
        if (result.status == 4)  // as the stock RIL reads it: two empty values after AUTS
            data.insert(data.end(), {0, 0});
        respond(r, data);
        return;
    }
    case IPC_SMS_SVC_CENTER_ADDR:
        if (r.type != IPC_TYPE_GET) {
            ack(r);
            return;
        }
        respond(r, Bytes{3, 0x91, 0x24, 0x13});
        return;
    case IPC_SMS_SAVE_MSG:
    case IPC_SMS_DEL_MSG: {
        // Saved to record 3; deleted from the record asked for
        ipc_sms_save_msg_response_data res{};
        res.magic = IPC_SMS_NETWORK_GSM;
        res.error = IPC_SMS_ACK_NO_ERROR;
        auto del = r.as<ipc_sms_del_msg_request_data>();
        res.index = r.command == IPC_SMS_SAVE_MSG ? 3 : (del ? del->index : 0);
        respond(r, toBytes(res));
        return;
    }
    case IPC_SMS_SEND_MSG: {
        ack(r);
        ipc_sms_send_msg_response_data res{};
        res.type = IPC_SMS_TYPE_OUTGOING;
        res.ack = sendResult;
        res.id = 7;
        send(IPC_SMS_SEND_MSG, IPC_TYPE_RESP, toBytes(res), r.mseq);
        return;
    }
    case IPC_SS_USSD: {
        // A menu, then the balance for the reply "1"
        auto req = r.as<ipc_ss_ussd_request_data>();
        ack(r);
        if (!req || req->action != IPC_SS_USSD_SEND)
            return;
        if (req->type == IPC_SS_USSD_TYPE_REQUEST)
            sendUssd(IPC_SS_USSD_ACTION_REQUIRE, "1 Credit 2 Data");
        else
            sendUssd(IPC_SS_USSD_NO_ACTION_REQUIRE, "Credit: 100 CZK");
        return;
    }
    case IPC_SS_WAITING:
        if (r.type != IPC_TYPE_GET) {
            ack(r);
            return;
        }
        respond(r, Bytes{1, IPC_SS_CLASS_VOICE, IPC_SS_STATUS_ACTIVE});
        return;
    case IPC_SS_CLI:
        if (r.type != IPC_TYPE_GET || r.data.empty()) {
            ack(r);
            return;
        }
        // CLIP provisioned, CLIR temporarily allowed
        respond(r, Bytes{r.data[0], static_cast<uint8_t>(r.data[0] == IPC_SS_CLI_TYPE_CLIP ? 1 : 6)});
        return;
    case IPC_SS_FORWARDING: {
        if (r.type != IPC_TYPE_GET) {
            ack(r);
            return;
        }
        // Busy: voice and data to one number (two entries), fax elsewhere
        Bytes data{r.data.empty() ? uint8_t{2} : r.data[0], 3};
        auto entry = [&](uint8_t cls, const char *number) {
            ipc_ss_forwarding_entry e{};
            e.class_type = cls;
            e.status = IPC_SS_STATUS_ACTIVE;
            e.no_reply_time = 20;
            e.number_length = std::strlen(number);
            e.number_type = IPC_SS_FORWARDING_ENTRY_NUMBER_INTL;
            std::memcpy(e.number, number, e.number_length);
            Bytes b = toBytes(e);
            data.insert(data.end(), b.begin(), b.end());
        };
        entry(IPC_SS_CLASS_VOICE, "420123456");
        entry(IPC_SS_CLASS_DATA, "420123456");
        entry(IPC_SS_CLASS_FAX, "420999");
        respond(r, data);
        return;
    }
    case IPC_SS_BARRING: {
        if (r.type == IPC_TYPE_GET) {
            respond(r, Bytes{0, 1, IPC_SS_CLASS_VOICE, IPC_SS_STATUS_ACTIVE});
            return;
        }
        auto set = r.as<ipc_ss_barring_set_data>();
        const bool right = set && std::string(set->password, 4) == barringPassword;
        if (right) {
            ack(r);
            return;
        }
        ipc_gen_phone_res_data res{};
        res.group = r.command >> 8;
        res.index = r.command & 0xff;
        res.type = r.type;
        res.code = 0x8126;  // the network's "wrong password"
        send(IPC_GEN_PHONE_RES, IPC_TYPE_RESP, toBytes(res), r.mseq);
        return;
    }
    case IPC_GPRS_PDP_CONTEXT: {
        auto set = r.as<ipc_gprs_pdp_context_request_set_data>();
        if (r.type != IPC_TYPE_SET || !set) {
            ack(r, false);
            return;
        }
        ack(r);

        ipc_gprs_call_status_ext_data status{};
        status.cid = set->cid;
        status.status = set->enable ? IPC_GPRS_STATUS_ENABLED : IPC_GPRS_STATUS_NOT_ENABLED;
        if (!set->enable) {
            send(IPC_GPRS_CALL_STATUS, IPC_TYPE_NOTI, toBytes(status));
            return;
        }

        // The long form of the IP configuration, with two P-CSCF servers
        Bytes ip(0x108 - 7, 0);
        ip[0] = set->cid;
        ip[1] = IPC_GPRS_IP_CONFIGURATION_FLAG_IPV4 | IPC_GPRS_IP_CONFIGURATION_FLAG_IPV6;
        const uint8_t addr[] = {10, 64, 0, static_cast<uint8_t>(set->cid)};
        std::memcpy(&ip[0x03], addr, 4);
        const uint8_t dns[] = {10, 0, 0, 53};
        std::memcpy(&ip[0x07], dns, 4);
        std::memcpy(&ip[0x17], pcscf4.data(), 4);
        ip[0x23] = 0xfe;
        ip[0x24] = 0x80;
        ip[0x2b] = 0x42;  // interface id
        ip[0x33] = 0x20;  // an IPv6 P-CSCF
        ip[0x34] = 0x01;
        ip[0x42] = 0x05;
        send(IPC_GPRS_IP_CONFIGURATION, IPC_TYPE_NOTI, ip);
        send(IPC_GPRS_CALL_STATUS, IPC_TYPE_NOTI, toBytes(status));
        return;
    }
    case IPC_CALL_OUTGOING: {
        auto out = r.as<ipc_call_outgoing_data>();
        ack(r);
        calls_ = {{1, IPC_CALL_LIST_ENTRY_STATUS_DIALING, false,
                   out ? std::string(reinterpret_cast<const char *>(out->number),
                                     out->number_length)
                       : std::string()}};
        setCallState(IPC_CALL_STATUS_DIALING);
        loop_.after(std::chrono::milliseconds(20), [this] {
            if (calls_.empty())
                return;
            calls_[0].status = IPC_CALL_LIST_ENTRY_STATUS_ACTIVE;
            setCallState(IPC_CALL_STATUS_CONNECTED);
        });
        return;
    }
    case IPC_CALL_ANSWER:
        ack(r);
        if (!calls_.empty()) {
            calls_[0].status = IPC_CALL_LIST_ENTRY_STATUS_ACTIVE;
            setCallState(IPC_CALL_STATUS_CONNECTED);
        }
        return;
    case IPC_CALL_RELEASE:
        ack(r);
        setCallState(IPC_CALL_STATUS_RELEASED);
        calls_.clear();
        return;
    case IPC_CALL_LIST:
        respond(r, callList());
        return;
    case IPC_SS_MANAGE_CALL:
        manageCalls(r);
        return;
    default:
        // Settings are accepted; questions without a model get an error
        ack(r, r.type != IPC_TYPE_GET);
        return;
    }
}

}  // namespace shannond
