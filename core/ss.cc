// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ss.h"

#include <algorithm>
#include <cstring>

#include "core/text.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

// Requests that go to the network can take a while
constexpr std::chrono::seconds kSsTimeout{60};

// 3GPP TS 23.038: GSM 7-bit, no language given; and UCS2
constexpr uint8_t kDcsGsm7 = 0x0f;
constexpr uint8_t kDcsUcs2 = 0x48;

// Android's SsModeType (disable, enable, interrogate, register, erase) as
// the modem's mode, from the stock RIL's table
uint8_t ssMode(int mode)
{
    static const uint8_t table[] = {IPC_SS_MODE_DEACTIVATE, IPC_SS_MODE_ACTIVATE, IPC_SS_MODE_ACTIVATE,
                                    IPC_SS_MODE_REGISTRATION, IPC_SS_MODE_ERASURE};
    return mode >= 0 && mode < 5 ? table[mode] : IPC_SS_MODE_ACTIVATE;
}

uint8_t ssReason(int reason)
{
    return reason >= 0 && reason < 6 ? reason + 1 : 5;
}

std::optional<uint8_t> barringFacility(const std::string &facility)
{
    // Facility::ToBarringType and the stock RIL's facility table
    static const struct {
        const char *name;
        uint8_t code;
    } table[] = {{"AO", 1}, {"OI", 2}, {"OX", 3}, {"AI", 4}, {"IR", 5}, {"AB", 6}, {"AG", 7}, {"AC", 8},
                 {"ACB", IPC_SS_BARRING_ACB}, {"ICB", IPC_SS_BARRING_ICB}};
    for (auto &t : table)
        if (facility == t.name)
            return t.code;
    return std::nullopt;
}

void copyPassword(char (&out)[4], const std::string &password)
{
    std::memset(out, 0, sizeof(out));
    std::memcpy(out, password.data(), std::min(password.size(), sizeof(out)));
}

// The answers to queries list classes with their status: the classes
// that are on (or, if none is, those reported off)
struct ClassSum {
    bool active = false;
    int classes = 0;
};

ClassSum classSum(const Message *m, size_t header)
{
    ClassSum sum;
    if (!m || m->data.size() < header)
        return sum;
    const uint8_t count = m->data[header - 1];
    int off = 0, on = 0;
    for (size_t i = 0; i < count; i++) {
        const size_t at = header + i * sizeof(ipc_ss_class_status);
        if (at + sizeof(ipc_ss_class_status) > m->data.size())
            break;
        const int c = ssClassFromIpc(m->data[at]);
        if (m->data[at + 1] == IPC_SS_STATUS_ACTIVE) {
            sum.active = true;
            on += c;
        } else {
            off += c;
        }
    }
    sum.classes = sum.active ? on : off;
    return sum;
}

}  // namespace

uint8_t ssClassToIpc(int c)
{
    switch (c) {
    case 0x01: return IPC_SS_CLASS_VOICE;
    case 0x02: return IPC_SS_CLASS_DATA;
    case 0x04: return IPC_SS_CLASS_FAX;
    case 0x05: return 0x19;
    case 0x08: return IPC_SS_CLASS_SMS;
    case 0x0c: return 0x28;
    case 0x0d: return 0x10;
    case 0x10: return 0x24;
    case 0x11: return 0x26;
    case 0x20: return 0x25;
    case 0x30: return 0x20;
    case 0x40: return 0x99;
    case 0x50: return 0x22;
    case 0x80: return 0x27;
    case 0xa0: return 0x21;
    default: return IPC_SS_CLASS_ALL;
    }
}

int ssClassFromIpc(uint8_t c)
{
    switch (c) {
    case 0x10: return 0x0d;
    case IPC_SS_CLASS_VOICE: return 0x01;
    case IPC_SS_CLASS_DATA: return 0x02;
    case IPC_SS_CLASS_FAX: return 0x04;
    case IPC_SS_CLASS_SMS: return 0x08;
    case 0x19: return 0x05;
    case 0x20: return 0x30;
    case 0x21: return 0xa0;
    case 0x22: return 0x50;
    case 0x24: return 0x10;
    case 0x25: case 0x28: return 0x20;
    case 0x26: return 0x40;
    case 0x27: return 0x80;
    case IPC_SS_CLASS_ALL: return 0xff;
    default: return 0;
    }
}

bool Ss::isBarringFacility(const std::string &facility)
{
    return barringFacility(facility).has_value();
}

Ss::Ss(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SS_USSD, [this](const Message &m) { ussdReceived(m); });
    modem_.onMessage(IPC_SS_INFO, [this](const Message &m) { infoReceived(m); });
}

void Ss::request(uint16_t command, uint8_t type, Bytes data,
                 std::function<void(const SsStatus &, const Message *)> done)
{
    modem_.request(command, type, std::move(data), [this, command, done](const Result &r) {
        SsStatus s{r.ok, r.timeout, r.code};
        if (!r.ok)
            modem_.log("%s refused: 0x%04x%s", commandName(command), r.code, r.timeout ? " (no answer)" : "");
        done(s, r.ok ? r.response : nullptr);
    }, kSsTimeout);
}

void Ss::sendUssd(const std::string &text, Done done)
{
    // IpcTxSsSendUssd: GSM 7-bit with the reply flag; text outside that
    // alphabet as UCS2 (IpcTxSsSendEncodedUssd)
    ipc_ss_ussd_request_data data;
    std::memset(&data, 0, sizeof(data));
    data.action = IPC_SS_USSD_SEND;
    Bytes encoded;
    if (auto packed = packGsm7(text)) {
        encoded = *packed;
        data.type = ussdReplyPending_ ? IPC_SS_USSD_TYPE_RESPONSE : IPC_SS_USSD_TYPE_REQUEST;
        data.dcs = kDcsGsm7;
    } else {
        encoded = utf8ToUcs2(text);
        data.type = IPC_SS_USSD_TYPE_REQUEST;
        data.dcs = kDcsUcs2;
    }
    if (encoded.empty() || encoded.size() > sizeof(data.data)) {
        done(SsStatus{});
        return;
    }
    data.length = static_cast<uint8_t>(encoded.size());
    std::memcpy(data.data, encoded.data(), encoded.size());
    ussdReplyPending_ = false;
    request(IPC_SS_USSD, IPC_TYPE_EXEC, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

void Ss::cancelUssd(Done done)
{
    ipc_ss_ussd_request_data data;
    std::memset(&data, 0, sizeof(data));
    data.action = IPC_SS_USSD_CANCEL;
    ussdReplyPending_ = false;
    request(IPC_SS_USSD, IPC_TYPE_EXEC, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

void Ss::ussdReceived(const Message &m)
{
    // IpcRxSsUssd: the answer to the exec is no USSD string
    if (m.type == IPC_TYPE_RESP)
        return;
    auto h = m.as<ipc_ss_ussd_header>();
    if (!h || sizeof(*h) + h->length > m.data.size()) {
        modem_.log("short USSD message");
        return;
    }
    const UssdMode mode = h->state >= 1 && h->state <= 6 ? static_cast<UssdMode>(h->state - 1)
                                                          : UssdMode::Notify;
    const Bytes data(m.data.begin() + sizeof(*h), m.data.begin() + sizeof(*h) + h->length);
    ussdReplyPending_ = mode == UssdMode::Request;
    if (ussd_)
        ussd_(mode, decodeUssd(h->dcs, data));
}

void Ss::infoReceived(const Message &m)
{
    // IpcRxSsInfo and ConvertIpcToSsCode, the 27.007 codes only
    if (m.type == IPC_TYPE_RESP)
        return;
    auto h = m.as<ipc_ss_info_header>();
    if (!h)
        return;

    SsNotification n;
    static const std::pair<uint8_t, int> mo[] = {{2, 3}, {4, 2}, {5, 6}, {6, 5}, {7, 8},
                                                 {8, 7}, {9, 0}, {10, 1}, {11, 5}};
    static const std::pair<uint8_t, int> mt[] = {{0x33, 0}, {0x35, 9}, {0x36, 0}, {0x61, 2}, {0x62, 3},
                                                 {0x63, 4}, {0x64, 5}, {0x65, 7}, {0x66, 8}, {0x67, 6}};
    bool known = false;
    for (auto [code, android] : mo) {
        if (h->code == code) {
            n.code = android;
            known = true;
        }
    }
    for (auto [code, android] : mt) {
        if (h->code == code) {
            n.mt = true;
            n.code = android;
            known = true;
        }
    }
    if (!known) {
        modem_.log("SS info 0x%02x not passed on", h->code);
        return;
    }
    if (n.mt) {
        if (h->no_number == 0 && h->number_length >= 1 && h->number_length <= 32 &&
            sizeof(*h) + h->number_length <= m.data.size()) {
            n.toa = h->number_type;
            n.number.assign(m.data.begin() + sizeof(*h), m.data.begin() + sizeof(*h) + h->number_length);
        }
        if ((h->code == 0x61 || h->code == 0x62) && h->index)
            n.index = h->index;
    }
    if (notification_)
        notification_(n);
}

void Ss::queryCallWaiting(int serviceClass,
                          std::function<void(const SsStatus &, bool, int)> done)
{
    request(IPC_SS_WAITING, IPC_TYPE_GET, Bytes{ssClassToIpc(serviceClass)},
            [done](const SsStatus &s, const Message *m) {
                const ClassSum sum = classSum(m, sizeof(ipc_ss_waiting_response_header));
                done(s, sum.active, sum.classes);
            });
}

void Ss::setCallWaiting(bool enable, int serviceClass, Done done)
{
    ipc_ss_waiting_set_data data{};
    data.class_type = ssClassToIpc(serviceClass);
    data.mode = ssMode(enable ? 1 : 0);
    request(IPC_SS_WAITING, IPC_TYPE_SET, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

void Ss::queryClip(std::function<void(const SsStatus &, int)> done)
{
    request(IPC_SS_CLI, IPC_TYPE_GET, Bytes{IPC_SS_CLI_TYPE_CLIP}, [done](const SsStatus &s, const Message *m) {
        // IpcRxSsClip
        int status = 2;
        if (auto cli = m ? m->as<ipc_ss_cli_data>() : std::nullopt) {
            if (cli->status == 1)
                status = 0;
            else if (cli->status == 2 || cli->status == 3)
                status = 1;
        }
        done(s, status);
    });
}

void Ss::queryClir(std::function<void(const SsStatus &, int, int)> done)
{
    request(IPC_SS_CLI, IPC_TYPE_GET, Bytes{IPC_SS_CLI_TYPE_CLIR}, [this, done](const SsStatus &s, const Message *m) {
        // IpcRxSsClir: 0 not provisioned, 3 temporarily restricted, 4
        // temporarily allowed, 2 unknown
        int state = 2;
        if (auto cli = m ? m->as<ipc_ss_cli_data>() : std::nullopt) {
            switch (cli->status) {
            case 1: state = 0; break;
            case 2: case 3: case 5: state = 3; break;
            case 6: state = 4; break;
            default: break;
            }
        }
        done(s, clir_, state);
    });
}

void Ss::setClir(int n, Done done)
{
    // IpcTxSsSetClir: hiding is an invocation, anything else suppression
    const Bytes data{IPC_SS_CLI_TYPE_CLIR,
                     static_cast<uint8_t>(n == 1 ? IPC_SS_CLI_CLIR_INVOCATION : IPC_SS_CLI_CLIR_SUPPRESSION)};
    request(IPC_SS_CLI, IPC_TYPE_SET, data, [this, n, done](const SsStatus &s, const Message *) {
        if (s.ok)
            clir_ = n;
        done(s);
    });
}

void Ss::queryCallForward(int reason, int serviceClass,
                          std::function<void(const SsStatus &, const std::vector<CallForward> &)> done)
{
    ipc_ss_forwarding_get_data data{};
    data.reason = ssReason(reason);
    data.class_type = ssClassToIpc(serviceClass);
    request(IPC_SS_FORWARDING, IPC_TYPE_GET, toBytes(data), [done](const SsStatus &s, const Message *m) {
        // IpcRxSsCallForward: entries that differ only in their class are
        // one rule for all of those classes
        std::vector<CallForward> rules;
        auto h = m ? m->as<ipc_ss_forwarding_response_header>() : std::nullopt;
        if (!h || h->count < 1 || h->count > IPC_SS_FORWARDING_ENTRY_MAX) {
            done(s, rules);
            return;
        }
        std::vector<ipc_ss_forwarding_entry> entries;
        for (size_t i = 0; i < h->count; i++) {
            const size_t at = sizeof(*h) + i * sizeof(ipc_ss_forwarding_entry);
            ipc_ss_forwarding_entry e{};
            if (at >= m->data.size())
                break;
            std::memcpy(&e, m->data.data() + at, std::min(sizeof(e), m->data.size() - at));
            entries.push_back(e);
        }
        std::vector<bool> taken(entries.size(), false);
        for (size_t i = 0; i < entries.size(); i++) {
            if (taken[i])
                continue;
            const ipc_ss_forwarding_entry &e = entries[i];
            CallForward rule;
            rule.active = e.status == IPC_SS_STATUS_ACTIVE;
            rule.reason = h->reason >= 1 && h->reason <= 6 ? h->reason - 1 : 4;
            rule.serviceClass = ssClassFromIpc(e.class_type);
            const size_t len = std::min<size_t>(e.number_length, sizeof(e.number));
            rule.number = (e.number_type == IPC_SS_FORWARDING_ENTRY_NUMBER_INTL ? "+" : "") +
                          std::string(e.number, strnlen(e.number, len));
            rule.toa = !rule.number.empty() && rule.number[0] == '+' ? 0x91 : 0x81;
            rule.timeSeconds = e.no_reply_time;
            for (size_t j = i + 1; j < entries.size() && rule.serviceClass; j++) {
                const ipc_ss_forwarding_entry &o = entries[j];
                if (!taken[j] && o.number_length && o.number_length == e.number_length &&
                    std::memcmp(o.number, e.number, std::min<size_t>(o.number_length, sizeof(o.number))) == 0 &&
                    o.no_reply_time == e.no_reply_time && o.status == e.status) {
                    rule.serviceClass += ssClassFromIpc(o.class_type);
                    taken[j] = true;
                }
            }
            rules.push_back(rule);
        }
        done(s, rules);
    });
}

void Ss::setCallForward(int mode, const CallForward &rule, Done done)
{
    // IpcTxSsSetCallForward: the number as dialled, + included
    ipc_ss_forwarding_set_data data;
    std::memset(&data, 0, sizeof(data));
    data.mode = ssMode(mode);
    data.reason = ssReason(rule.reason);
    data.class_type = ssClassToIpc(rule.serviceClass);
    data.no_reply_time = static_cast<uint8_t>(rule.timeSeconds);
    if (!rule.number.empty()) {
        const size_t len = std::min<size_t>(rule.number.size(), IPC_SS_FORWARDING_NUMBER_MAX);
        data.number_length = static_cast<uint8_t>(len);
        data.number_type = rule.number[0] == '+' ? 0x91 : 0x81;
        std::memcpy(data.number, rule.number.data(), len);
    }
    request(IPC_SS_FORWARDING, IPC_TYPE_SET, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

void Ss::queryBarring(const std::string &facility, int serviceClass,
                      std::function<void(const SsStatus &, int)> done)
{
    auto code = barringFacility(facility);
    if (!code) {
        done(SsStatus{}, 0);
        return;
    }
    ipc_ss_barring_get_data data{};
    data.facility = *code;
    data.class_type = ssClassToIpc(serviceClass);
    request(IPC_SS_BARRING, IPC_TYPE_GET, toBytes(data), [done](const SsStatus &s, const Message *m) {
        // IpcRxSsCallBarring: the classes barred
        const ClassSum sum = classSum(m, sizeof(ipc_ss_barring_response_header));
        done(s, sum.active ? sum.classes : 0);
    });
}

void Ss::setBarring(const std::string &facility, bool lock, const std::string &password,
                    int serviceClass, Done done)
{
    auto code = barringFacility(facility);
    if (!code) {
        done(SsStatus{});
        return;
    }
    ipc_ss_barring_set_data data{};
    data.facility = *code;
    data.mode = lock ? IPC_SS_MODE_ACTIVATE : IPC_SS_MODE_DEACTIVATE;
    data.class_type = ssClassToIpc(serviceClass);
    copyPassword(data.password, password);
    request(IPC_SS_BARRING, IPC_TYPE_SET, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

void Ss::changeBarringPassword(const std::string &oldPassword, const std::string &newPassword, Done done)
{
    ipc_ss_barring_pw_data data{};
    copyPassword(data.old_password, oldPassword);
    copyPassword(data.new_password, newPassword);
    copyPassword(data.new_password_again, newPassword);
    request(IPC_SS_BARRING_PW, IPC_TYPE_SET, toBytes(data), [done](const SsStatus &s, const Message *) { done(s); });
}

}  // namespace shannond
