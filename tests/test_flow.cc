// SPDX-License-Identifier: GPL-2.0-or-later
//
// The whole daemon core against the synthetic modem: boot, SIM, LTE
// registration, data with P-CSCF, SMS both ways, calls both ways, a crash.
#include "core/call.h"
#include "core/data.h"
#include "core/network.h"
#include "core/sim.h"
#include "core/sms.h"
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
    Sim sim{modem};
    Network network{modem};
    Data data{modem, DataLinks::none()};
    Sms sms{modem};
    Calls calls{modem};

    bool booted()
    {
        fake.boot();
        return loop.runUntil([&] {
            return sim.state() == SimState::Ready && !sim.imsi().empty() &&
                   registered(network.data().state) && sms.ready() && !sms.smsc().empty() &&
                   !network.imei().empty() && sim.apps().size() == 2;
        });
    }
};

}  // namespace

TEST(boot_brings_sim_registration_and_identities)
{
    Stack s;
    CHECK(s.booted());
    CHECK(s.network.modemReady());
    CHECK(s.network.imei() == "350000000000001");
    CHECK(s.sim.imsi() == "230020000000001");
    CHECK(s.sim.iccid().rfind("8923010", 0) == 0);
    CHECK(s.network.data().tech == AccessTech::Lte);
    CHECK(s.network.voice().state == RegState::Home);
    CHECK(s.loop.runUntil([&] { return s.network.operatorCode() == "23002"; }));
    CHECK(s.loop.runUntil([&] { return s.network.operatorLongName() == "O2.CZ"; }));
    CHECK(s.network.operatorShortName() == "O2.CZ");
    CHECK(s.sim.apps()[0].type == kSimAppUsim && s.sim.apps()[1].type == kSimAppIsim);
    // Packet service was switched on with the modem's start
    CHECK(s.fake.count(IPC_NET_SERVICE_DOMAIN_CONFIG) == 1);
    // No PIN was ever sent
    CHECK(s.fake.count(IPC_SEC_PIN_STATUS) == 0);
}

TEST(limited_service_is_a_refused_registration)
{
    // As the stock RIL reads it; the front end turns it into emergency-only
    Stack s;
    CHECK(s.booted());
    s.fake.limitedService();
    CHECK(s.loop.runUntil([&] { return s.network.voice().state == RegState::Denied; }));
    CHECK(s.network.voice().tech == AccessTech::Gsm);
    CHECK(!registered(s.network.voice().state));
}

TEST(sim_lock_states_as_the_stock_ril_reads_them)
{
    Stack s;
    CHECK(s.booted());

    s.fake.pinStatus(IPC_SEC_PIN_STATUS_LOCK_SC, IPC_SEC_FACILITY_LOCK_TYPE_SC_CARD_BLOCKED);
    CHECK(s.loop.runUntil([&] { return s.sim.state() == SimState::PermanentlyBlocked; }));

    s.fake.pinStatus(IPC_SEC_PIN_STATUS_LOCK_SC, IPC_SEC_FACILITY_LOCK_TYPE_SC_PUK_REQ);
    CHECK(s.loop.runUntil([&] { return s.sim.state() == SimState::PukRequired; }));

    s.fake.pinStatus(IPC_SEC_PIN_STATUS_LOCK_PP, 2);
    CHECK(s.loop.runUntil([&] { return s.sim.state() == SimState::NetworkLocked; }));
    CHECK(s.sim.lockStatus() == IPC_SEC_PIN_STATUS_LOCK_PP && s.sim.facilityLock() == 2);

    s.fake.pinStatus(IPC_SEC_PIN_STATUS_CARD_NOT_PRESENT, 0);
    CHECK(s.loop.runUntil([&] { return s.sim.state() == SimState::Absent; }));
    CHECK(s.sim.apps().empty() && s.sim.imsi().empty());
}

TEST(mode_probe_keeps_the_accepted_modes)
{
    Stack s;
    CHECK(s.booted());
    std::optional<uint8_t> modes;
    s.network.probeModes([&](uint8_t m) { modes = m; });
    CHECK(s.loop.runUntil([&] { return modes.has_value(); }));
    CHECK(*modes == (kModeGsm | kModeUmts | kModeLte));
}

TEST(data_call_comes_up_with_pcscf)
{
    Stack s;
    CHECK(s.booted());

    DataProfile ims;
    ims.apn = "ims";
    ims.ims = true;
    std::optional<std::optional<DataCall>> result;
    s.data.connect(2, ims, [&](std::optional<DataCall> call) { result = call; });
    CHECK(s.loop.runUntil([&] { return result.has_value(); }));
    CHECK(result->has_value());

    const DataCall &call = **result;
    CHECK(call.cid == 2 && call.active && call.iface == "rmnet1");
    CHECK(call.ipv4 && (*call.ipv4)[0] == 10 && (*call.ipv4)[3] == 2);
    CHECK(call.ipv6InterfaceId && (*call.ipv6InterfaceId)[0] == 0x42);
    CHECK(call.dns4.size() == 1);
    CHECK(call.pcscf4.size() == 1 && call.pcscf4[0][3] == 80);
    CHECK(call.pcscf6.size() == 1 && call.pcscf6[0][0] == 0x20);

    bool down = false;
    s.data.onChanged([&](const DataCall &c) { down = c.cid == 2 && !c.active; });
    s.data.disconnect(2);
    CHECK(s.loop.runUntil([&] { return down; }));
    CHECK(s.data.calls().empty());
}

TEST(sms_in_is_acknowledged_with_its_type_and_sms_out_reports)
{
    Stack s;
    CHECK(s.booted());

    Bytes got;
    s.sms.onIncoming([&](const Bytes &pdu) { got = pdu; });
    s.fake.incomingSms(Bytes{0x00, 0x04, 0x0b, 0x91});
    CHECK(s.loop.runUntil([&] { return !got.empty(); }));
    CHECK(got == (Bytes{0x00, 0x04, 0x0b, 0x91}));
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_SMS_DELIVER_REPORT) == 1; }));
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SMS_DELIVER_REPORT) {
            auto report = m.as<ipc_sms_deliver_report_request_data>();
            CHECK(report && report->type == IPC_SMS_TYPE_POINT_TO_POINT && report->id == 5);
        }
    }

    std::optional<std::pair<bool, uint8_t>> sent;
    s.sms.send(Bytes{0x01, 0x00, 0x0b}, [&](bool ok, uint8_t ref) { sent = {ok, ref}; });
    CHECK(s.loop.runUntil([&] { return sent.has_value(); }));
    CHECK(sent->first && sent->second == 7);
    // The SMSC from the SIM went in front of the TPDU
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SMS_SEND_MSG) {
            const size_t h = sizeof(ipc_sms_send_msg_request_header);
            CHECK(m.data[h] == 3 && m.data[h + 1] == 0x91 && m.data[h + 4] == 0x01);
        }
    }
}

TEST(outgoing_call_connects_and_switches_audio)
{
    Stack s;
    CHECK(s.booted());

    std::vector<bool> audio;
    s.calls.onAudio([&](bool on) { audio.push_back(on); });

    bool dialled = false;
    s.calls.dial("800810820", [&](bool ok) { dialled = ok; });
    CHECK(s.loop.runUntil([&] {
        return dialled && !s.calls.list().empty() && s.calls.list()[0].state == CallState::Active;
    }));
    CHECK(s.calls.list()[0].number == "800810820" && !s.calls.list()[0].incoming);
    CHECK(audio.size() == 1 && audio[0]);
    CHECK(s.fake.count(IPC_SND_AUDIO_PATH_CTRL) == 1);

    // The speaker during the call: the modem hears of it at once, and
    // only of a change
    s.calls.setAudioPath(IPC_SND_AUDIO_PATH_SPEAKER);
    s.calls.setAudioPath(IPC_SND_AUDIO_PATH_SPEAKER);
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_SND_AUDIO_PATH_CTRL) == 2; }));

    bool hung = false;
    s.calls.hangup([&](bool ok) { hung = ok; });
    CHECK(s.loop.runUntil([&] { return hung && s.calls.list().empty(); }));
    CHECK(audio.size() == 2 && !audio[1]);
}

TEST(incoming_call_rings_and_is_answered)
{
    Stack s;
    CHECK(s.booted());

    s.fake.incomingCall("123456789");
    CHECK(s.loop.runUntil([&] {
        return !s.calls.list().empty() && s.calls.list()[0].state == CallState::Incoming;
    }));
    CHECK(s.calls.list()[0].incoming && s.calls.list()[0].number == "123456789");

    bool answered = false;
    s.calls.answer([&](bool ok) { answered = ok; });
    CHECK(s.loop.runUntil([&] {
        return answered && s.calls.list()[0].state == CallState::Active;
    }));

    s.fake.remoteHangup();
    CHECK(s.loop.runUntil([&] { return s.calls.list().empty(); }));
}

TEST(sim_services_for_ims)
{
    Stack s;
    CHECK(s.booted());

    std::optional<SimAuthResult> auth;
    s.sim.authenticate(0, Bytes(16, 1), Bytes(16, 2), [&](std::optional<SimAuthResult> r) {
        if (r)
            auth = r;
    });
    CHECK(s.loop.runUntil([&] { return auth.has_value(); }));
    CHECK(auth->status == 0 && auth->values.size() == 3 && auth->values[0].size() == 8);

    std::optional<SimIoResult> io;
    const SimApp isim = s.sim.apps()[1];
    s.sim.io(IPC_SEC_RSIM_COMMAND_READ_BINARY, 0x6f02, {0x3f00, 0x7fff}, 0, 0, 0, {}, &isim,
             [&](std::optional<SimIoResult> r) { io = r; });
    CHECK(s.loop.runUntil([&] { return io.has_value(); }));
    CHECK(io->sw1 == 0x90 && io->data == (Bytes{'a', 'b', 'c'}));
    // The request named the ISIM
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SEC_RSIM_ACCESS) {
            auto h = m.as<ipc_sec_rsim_aid_request_header>();
            CHECK(h && h->app_type == kSimAppIsim && h->aid_length == 7 && h->path_length == 2);
        }
    }

    std::optional<Bytes> apdu;
    s.sim.transmitApdu(Bytes{0x00, 0x70, 0x00, 0x00, 0x01}, [&](std::optional<Bytes> r) { apdu = r; });
    CHECK(s.loop.runUntil([&] { return apdu.has_value(); }));
    CHECK(*apdu == (Bytes{0x01, 0x90, 0x00}));
}

TEST(crash_closes_the_link)
{
    Stack s;
    CHECK(s.booted());

    bool closed = false;
    s.modem.onClosed([&] { closed = true; });
    s.fake.crash();
    CHECK(s.loop.runUntil([&] { return closed; }));
}
