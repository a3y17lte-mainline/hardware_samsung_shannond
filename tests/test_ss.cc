// SPDX-License-Identifier: GPL-2.0-or-later
//
// Supplementary services and USSD against the synthetic modem, and the
// text codings they use.
#include "core/network.h"
#include "core/ss.h"
#include "core/text.h"
#include "sim/fake_modem.h"
#include "tests/test.h"

extern "C" {
#include <samsung-ipc.h>
}

using namespace shannond;

namespace {

struct Stack {
    EventLoop loop;
    FakeModem fake{loop};
    Modem modem{loop, fake.fmtFd(), fake.rfsFd()};
    Ss ss{modem};

    Message last(uint16_t command)
    {
        Message found;
        for (const Message &m : fake.received)
            if (m.command == command)
                found = m;
        return found;
    }
};

}  // namespace

TEST(gsm7_packing)
{
    // 23.038 examples: "hellohello" packs to E8 32 9B FD 46 97 D9 EC 37
    CHECK(packGsm7("hellohello") == (Bytes{0xe8, 0x32, 0x9b, 0xfd, 0x46, 0x97, 0xd9, 0xec, 0x37}));
    CHECK(unpackGsm7(Bytes{0xe8, 0x32, 0x9b, 0xfd, 0x46, 0x97, 0xd9, 0xec, 0x37}) == "hellohello");
    // USSD codes, the extension table, and text outside the alphabet
    CHECK(unpackGsm7(*packGsm7("*101#")) == "*101#");
    CHECK(unpackGsm7(*packGsm7("€ [x] ä@")) == "€ [x] ä@");
    CHECK(!packGsm7("č"));
    // Seven septets leave seven bits: a CR fills them and is dropped again
    const Bytes seven = *packGsm7("1234567");
    CHECK(seven.size() == 7 && (seven[6] >> 1) == 0x0d);
    CHECK(unpackGsm7(seven) == "1234567");
}

TEST(ussd_codings)
{
    CHECK(ussdCoding(0x0f) == TextCoding::Gsm7);
    CHECK(ussdCoding(0x48) == TextCoding::Ucs2);
    CHECK(ussdCoding(0x44) == TextCoding::EightBit);
    CHECK(ussdCoding(0x11) == TextCoding::Ucs2WithLanguage);
    CHECK(ussdCoding(0xf4) == TextCoding::EightBit);
    CHECK(ussdCoding(0x80) == TextCoding::Unknown);
    CHECK(decodeUssd(0x48, utf8ToUcs2("Kredit: 100 Kč")) == "Kredit: 100 Kč");
    CHECK(decodeUssd(0x11, Bytes{'c', 's', 0x00, 0x4b}) == "K");
    CHECK(decodeUssd(0x80, Bytes{1, 2}).empty());
}

TEST(ussd_menu_and_reply)
{
    Stack s;
    std::vector<std::pair<UssdMode, std::string>> got;
    s.ss.onUssd([&](UssdMode mode, const std::string &text) { got.push_back({mode, text}); });

    std::optional<SsStatus> sent;
    s.ss.sendUssd("*101#", [&](const SsStatus &st) { sent = st; });
    CHECK(s.loop.runUntil([&] { return sent.has_value() && got.size() == 1; }));
    CHECK(sent->ok && got[0].first == UssdMode::Request && got[0].second == "1 Credit 2 Data");
    auto req = s.last(IPC_SS_USSD).as<ipc_ss_ussd_request_data>();
    CHECK(req && req->action == IPC_SS_USSD_SEND && req->type == IPC_SS_USSD_TYPE_REQUEST &&
          req->dcs == 0x0f && Bytes(req->data, req->data + req->length) == *packGsm7("*101#"));
    CHECK(s.last(IPC_SS_USSD).data.size() == sizeof(ipc_ss_ussd_request_data));

    // The network asked: the answer is a response
    sent.reset();
    s.ss.sendUssd("1", [&](const SsStatus &st) { sent = st; });
    CHECK(s.loop.runUntil([&] { return sent.has_value() && got.size() == 2; }));
    CHECK(s.last(IPC_SS_USSD).as<ipc_ss_ussd_request_data>()->type == IPC_SS_USSD_TYPE_RESPONSE);
    CHECK(got[1].first == UssdMode::Notify && got[1].second == "Credit: 100 CZK");

    // Text outside GSM 7-bit goes as UCS2; cancel is its own action
    sent.reset();
    s.ss.sendUssd("ž", [&](const SsStatus &st) { sent = st; });
    CHECK(s.loop.runUntil([&] { return sent.has_value(); }));
    req = s.last(IPC_SS_USSD).as<ipc_ss_ussd_request_data>();
    CHECK(req->dcs == 0x48 && req->length == 2 && req->data[0] == 0x01 && req->data[1] == 0x7e);
    sent.reset();
    s.ss.cancelUssd([&](const SsStatus &st) { sent = st; });
    CHECK(s.loop.runUntil([&] { return sent.has_value(); }));
    CHECK(s.last(IPC_SS_USSD).data[0] == IPC_SS_USSD_CANCEL);
}

TEST(call_waiting_and_line_identity)
{
    Stack s;
    std::optional<std::pair<bool, int>> waiting;
    s.ss.queryCallWaiting(0, [&](const SsStatus &st, bool on, int classes) {
        if (st.ok)
            waiting = {on, classes};
    });
    CHECK(s.loop.runUntil([&] { return waiting.has_value(); }));
    CHECK(waiting->first && waiting->second == 1);
    CHECK(s.last(IPC_SS_WAITING).data == Bytes{IPC_SS_CLASS_ALL});

    std::optional<SsStatus> set;
    s.ss.setCallWaiting(false, 1, [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(set->ok && s.last(IPC_SS_WAITING).data == (Bytes{IPC_SS_CLASS_VOICE, IPC_SS_MODE_DEACTIVATE}));

    std::optional<int> clip;
    s.ss.queryClip([&](const SsStatus &, int status) { clip = status; });
    CHECK(s.loop.runUntil([&] { return clip.has_value(); }));
    CHECK(*clip == 0);

    set.reset();
    s.ss.setClir(1, [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(s.last(IPC_SS_CLI).data == (Bytes{IPC_SS_CLI_TYPE_CLIR, IPC_SS_CLI_CLIR_INVOCATION}));
    std::optional<std::pair<int, int>> clir;
    s.ss.queryClir([&](const SsStatus &, int n, int m) { clir = {n, m}; });
    CHECK(s.loop.runUntil([&] { return clir.has_value(); }));
    CHECK(clir->first == 1 && clir->second == 4);
}

TEST(call_forwarding_rules_are_merged_by_number)
{
    Stack s;
    std::optional<std::vector<CallForward>> rules;
    s.ss.queryCallForward(1, 0, [&](const SsStatus &st, const std::vector<CallForward> &r) {
        if (st.ok)
            rules = r;
    });
    CHECK(s.loop.runUntil([&] { return rules.has_value(); }));
    auto get = s.last(IPC_SS_FORWARDING).as<ipc_ss_forwarding_get_data>();
    CHECK(get && get->reason == 2 && get->class_type == IPC_SS_CLASS_ALL);
    CHECK(rules->size() == 2);
    const CallForward &r = (*rules)[0];
    CHECK(r.active && r.reason == 1 && r.serviceClass == 3 && r.number == "+420123456" && r.toa == 0x91 &&
          r.timeSeconds == 20);
    CHECK((*rules)[1].serviceClass == 4 && (*rules)[1].number == "+420999");

    CallForward rule;
    rule.reason = 2;
    rule.serviceClass = 1;
    rule.number = "+420777";
    rule.timeSeconds = 30;
    std::optional<SsStatus> set;
    s.ss.setCallForward(3, rule, [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    auto data = s.last(IPC_SS_FORWARDING).as<ipc_ss_forwarding_set_data>();
    CHECK(data && data->mode == IPC_SS_MODE_REGISTRATION && data->reason == 3 &&
          data->class_type == IPC_SS_CLASS_VOICE && data->no_reply_time == 30 && data->number_length == 7 &&
          data->number_type == 0x91 && std::string(data->number, 7) == "+420777");
}

TEST(barring_with_password)
{
    Stack s;
    std::optional<int> classes;
    s.ss.queryBarring("AI", 0, [&](const SsStatus &st, int c) {
        if (st.ok)
            classes = c;
    });
    CHECK(s.loop.runUntil([&] { return classes.has_value(); }));
    CHECK(*classes == 1 && s.last(IPC_SS_BARRING).data == (Bytes{4, IPC_SS_CLASS_ALL}));

    std::optional<SsStatus> set;
    s.ss.setBarring("AO", true, "0000", 1, [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(!set->ok && set->code == 0x8126);
    set.reset();
    s.ss.setBarring("AO", true, "1234", 1, [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(set->ok);
    CHECK(!Ss::isBarringFacility("SC") && Ss::isBarringFacility("AB"));

    set.reset();
    s.ss.changeBarringPassword("1234", "4321", [&](const SsStatus &st) { set = st; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(s.last(IPC_SS_BARRING_PW).data == (Bytes{'1', '2', '3', '4', '4', '3', '2', '1', '4', '3', '2', '1'}));
}

TEST(notifications_during_calls)
{
    Stack s;
    std::vector<SsNotification> got;
    s.ss.onNotification([&](const SsNotification &n) { got.push_back(n); });
    // MO: the call is waiting at the other side (code 3); MT: forwarded
    // call (0x61) with the number and index 1; a Samsung-only code dropped
    s.fake.send(IPC_SS_INFO, IPC_TYPE_NOTI, {0x02, 1, 0, 0, 0, 0});
    s.fake.send(IPC_SS_INFO, IPC_TYPE_NOTI, {0x61, 0, 0, 1, 4, 0x81, '1', '2', '3', '4'});
    s.fake.send(IPC_SS_INFO, IPC_TYPE_NOTI, {0x37, 1, 0, 0, 0, 0});
    CHECK(s.loop.runUntil([&] { return got.size() == 2; }));
    CHECK(!got[0].mt && got[0].code == 3);
    CHECK(got[1].mt && got[1].code == 2 && got[1].index == 1 && got[1].number == "1234" && got[1].toa == 0x81);
}
