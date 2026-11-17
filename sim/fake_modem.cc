// SPDX-License-Identifier: GPL-2.0-or-later
#include "sim/fake_modem.h"

#include "core/sim.h"

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

void FakeModem::sendRegistration(uint8_t domain, uint8_t aseq)
{
    ipc_net_regist_response_data reg{};
    reg.act = kRawTechLte;
    reg.domain = domain;
    reg.status = IPC_NET_REGISTRATION_STATUS_HOME;
    reg.lac = 0x1234;
    reg.cid = 0x0abcdef;
    send(IPC_NET_REGIST, aseq ? IPC_TYPE_RESP : IPC_TYPE_NOTI, toBytes(reg), aseq);
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

void FakeModem::incomingSms(const Bytes &pdu)
{
    ipc_sms_incoming_msg_header header{};
    header.msg_type = IPC_SMS_MSG_TYPE_SINGLE;
    header.type = IPC_SMS_TYPE_POINT_TO_POINT;
    header.id = 5;
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
    switch (r.command) {
    case IPC_MISC_ME_SN: {
        ipc_misc_me_sn_response_data sn{};
        sn.type = IPC_MISC_ME_SN_SERIAL_NUM;
        sn.length = sizeof(kImei) - 1;
        std::memcpy(sn.data, kImei, sn.length);
        respond(r, toBytes(sn));
        return;
    }
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
        Bytes data{0x90, 0x00, 3, 'a', 'b', 'c'};
        respond(r, data);
        return;
    }
    case IPC_SEC_GSIM_ACCESS: {
        // MANAGE CHANNEL open: channel 1, 90 00
        respond(r, Bytes{0x00, 0x03, 0x01, 0x90, 0x00});
        return;
    }
    case IPC_SEC_IMS_AUTH: {
        Bytes data{0, 0};
        for (uint8_t len : {8, 16, 16}) {
            data.push_back(len);
            data.insert(data.end(), len, len);
        }
        respond(r, data);
        return;
    }
    case IPC_SMS_SVC_CENTER_ADDR:
        respond(r, Bytes{3, 0x91, 0x24, 0x13});
        return;
    case IPC_SMS_SEND_MSG: {
        ack(r);
        ipc_sms_send_msg_response_data res{};
        res.type = IPC_SMS_TYPE_OUTGOING;
        res.ack = IPC_SMS_ACK_NO_ERROR;
        res.id = 7;
        send(IPC_SMS_SEND_MSG, IPC_TYPE_RESP, toBytes(res), r.mseq);
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
        const uint8_t pcscf[] = {10, 0, 0, 80};
        std::memcpy(&ip[0x17], pcscf, 4);
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
    default:
        // Settings are accepted; questions without a model get an error
        ack(r, r.type != IPC_TYPE_GET);
        return;
    }
}

}  // namespace shannond
