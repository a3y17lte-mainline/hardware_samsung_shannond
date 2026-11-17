// SPDX-License-Identifier: GPL-2.0-or-later
//
// The whole daemon core against the synthetic modem: boot, SIM, LTE
// registration, data with P-CSCF, SMS both ways, calls both ways, a crash.
#include <algorithm>

#include "core/attach.h"
#include "core/call.h"
#include "core/data.h"
#include "core/network.h"
#include "core/sat.h"
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
    Sat sat{modem};

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
    CHECK(s.network.data().volte == true && s.network.data().imsEmergency == false);
    CHECK(s.network.data().tac == 0x5678 && s.network.data().pci == 301);
    CHECK(s.loop.runUntil([&] { return s.network.operatorCode() == "23002"; }));
    CHECK(s.loop.runUntil([&] { return s.network.operatorLongName() == "O2.CZ"; }));
    CHECK(s.network.operatorShortName() == "O2.CZ");
    CHECK(s.sim.apps()[0].type == kSimAppUsim && s.sim.apps()[1].type == kSimAppIsim);
    // Packet service was switched on with the modem's start
    CHECK(s.fake.count(IPC_NET_SERVICE_DOMAIN_CONFIG) == 1);
    // No PIN was ever sent
    CHECK(s.fake.count(IPC_SEC_PIN_STATUS) == 0);
}

TEST(sim_toolkit_commands_wait_for_the_client)
{
    Stack s;
    std::vector<Bytes> commands;
    int ended = 0;
    s.sat.onProactiveCommand([&](const Bytes &c) { commands.push_back(c); });
    s.sat.onSessionEnd([&] { ended++; });
    CHECK(s.booted());

    // SET UP EVENT LIST as the phone's card sends it at every start
    const Bytes frame = {0x12, 0x00, 0xd0, 0x10, 0x81, 0x03, 0x01, 0x05, 0x00, 0x82,
                         0x02, 0x81, 0x82, 0x99, 0x05, 0x01, 0x02, 0x03, 0x09, 0x0a};
    s.fake.send(IPC_SAT_PROACTIVE_CMD, IPC_TYPE_INDI, frame);
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(30));
    CHECK(commands.empty());

    s.sat.clientRunning();
    CHECK(commands.size() == 1 && commands[0] == Bytes(frame.begin() + 2, frame.end()));

    // The terminal response goes out at full size with its length first;
    // the card's 90 00 ends the session, and is no new command
    const Bytes response = {0x81, 0x03, 0x01, 0x05, 0x00, 0x82, 0x02, 0x82, 0x81, 0x83, 0x01, 0x00};
    std::optional<bool> sent;
    s.sat.terminalResponse(response, [&](bool ok) { sent = ok; });
    CHECK(s.loop.runUntil([&] { return sent.has_value() && ended == 1; }));
    CHECK(*sent && commands.size() == 1);
    Message request;
    for (const Message &m : s.fake.received)
        if (m.command == IPC_SAT_PROACTIVE_CMD)
            request = m;
    CHECK(request.type == IPC_TYPE_GET);
    CHECK(request.data.size() == 1 + IPC_SAT_TERMINAL_RESPONSE_MAX && request.data[0] == 12);
    CHECK(std::equal(response.begin(), response.end(), request.data.begin() + 1));
}

TEST(sim_toolkit_envelope_returns_the_cards_answer)
{
    for (bool withSequence : {true, false}) {
        Stack s;
        int ended = 0;
        s.sat.onSessionEnd([&] { ended++; });
        CHECK(s.booted());
        s.fake.envelopeAnswersWithSequence = withSequence;

        // A menu selection the card accepts: the answer, then the end
        std::optional<EnvelopeResult> result;
        s.sat.envelope({0xd3, 0x07, 0x82, 0x02, 0x01, 0x81, 0x90, 0x01, 0x01},
                       [&](std::optional<EnvelopeResult> r) { result = std::move(r); });
        CHECK(s.loop.runUntil([&] { return result.has_value() && ended == 1; }));
        CHECK(result->sw1 == 0x90 && result->sw2 == 0 && result->data == (Bytes{0xaa, 0xbb}));
        size_t size = 0;
        for (const Message &m : s.fake.received)
            if (m.command == IPC_SAT_ENVELOPE_CMD)
                size = m.data.size();
        CHECK(size == 1 + IPC_SAT_ENVELOPE_MAX);
    }
}

TEST(sim_refresh_is_reported_once_the_modem_has_done_it)
{
    Stack s;
    std::vector<SimRefresh> got;
    s.sat.onRefresh([&](const SimRefresh &r) { got.push_back(r); });
    CHECK(s.booted());

    // REFRESH, file change notification, for EF 6F07 and 6F46 under ADF
    const Bytes command = {0xd0, 0x18, 0x81, 0x03, 0x01, 0x01, 0x01, 0x82, 0x02, 0x81, 0x82,
                           0x92, 0x0d, 0x02, 0x3f, 0x00, 0x7f, 0xff, 0x6f, 0x07,
                           0x3f, 0x00, 0x7f, 0xff, 0x6f, 0x46};
    Bytes frame{static_cast<uint8_t>(command.size()), 0x00};
    frame.insert(frame.end(), command.begin(), command.end());
    s.fake.send(IPC_SAT_PROACTIVE_CMD, IPC_TYPE_INDI, frame);
    // A reset notice does not end a file change; the end does
    s.fake.send(IPC_SAT_REFRESH, IPC_TYPE_NOTI, {2});
    s.fake.send(IPC_SAT_REFRESH, IPC_TYPE_NOTI, {1});
    CHECK(s.loop.runUntil([&] { return !got.empty(); }));
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(20));
    CHECK(got.size() == 1 && got[0].type == 0 && got[0].files == (std::vector<uint16_t>{0x6f07, 0x6f46}));
    // Nothing more without a new REFRESH
    s.fake.send(IPC_SAT_REFRESH, IPC_TYPE_NOTI, {1});
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(20));
    CHECK(got.size() == 1);
}

TEST(a_running_modem_is_taken_over)
{
    // No start-up messages: the modem was started by an earlier client
    Stack s;
    adoptRunningModem(s.loop, s.network, s.sim, s.sms, s.data, std::chrono::milliseconds(20));
    CHECK(s.loop.runUntil([&] {
        return s.network.modemReady() && s.sim.state() == SimState::Ready &&
               !s.sim.imsi().empty() && s.sms.ready() && !s.network.imei().empty();
    }));
    // The radio was switched off, and the card's number read from the card
    CHECK(s.loop.runUntil([&] {
        return s.fake.count(IPC_PWR_PHONE_STATE) == 1 && s.fake.count(IPC_SEC_RSIM_ACCESS) == 1;
    }));
}

TEST(a_modem_that_just_started_is_not_probed)
{
    Stack s;
    adoptRunningModem(s.loop, s.network, s.sim, s.sms, s.data, std::chrono::milliseconds(20));
    CHECK(s.booted());
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(60));
    // Only the version request of the normal start, and the radio untouched
    CHECK(s.fake.count(IPC_MISC_ME_VERSION) == 1 && s.fake.count(IPC_PWR_PHONE_STATE) == 0);
    CHECK(s.fake.count(IPC_SEC_RSIM_ACCESS) == 0);
}

TEST(lte_signal_as_the_stock_ril_reads_it)
{
    Stack s;
    CHECK(s.booted());

    // A frame from the phone: -100 dBm, RSRP -100 dBm, RSRQ -13 dB, SNR 4.0 dB
    s.fake.send(IPC_DISP_RSSI_INFO, IPC_TYPE_NOTI,
                {0x64, 0x63, 0x63, 0x0b, 0x64, 0x0d, 0xf0, 0x00, 0x00, 0xff, 0x04, 0xff});
    CHECK(s.loop.runUntil([&] { return s.network.signal() == 100; }));
    const LteSignal &lte = s.network.lteSignal();
    CHECK(lte.strength == 11 && lte.rsrp == 100 && lte.rsrq == 13 && lte.snr == 40 && lte.cqi == 0);

    // Unknown values stay unknown; the short form has no LTE part
    s.fake.send(IPC_DISP_RSSI_INFO, IPC_TYPE_NOTI,
                {0x5a, 0x63, 0x63, 0x0b, 0x00, 0x0d, 0xff, 0xff, 0xff});
    CHECK(s.loop.runUntil([&] { return s.network.signal() == 90; }));
    CHECK(!s.network.lteSignal().rsrp && !s.network.lteSignal().snr && !s.network.lteSignal().cqi);
    s.fake.send(IPC_DISP_RSSI_INFO, IPC_TYPE_NOTI, {0x50});
    CHECK(s.loop.runUntil([&] { return s.network.signal() == 80; }));
    CHECK(!s.network.lteSignal().strength);
}

TEST(screen_state_goes_to_the_modem_as_an_event)
{
    Stack s;
    CHECK(s.booted());
    s.network.setScreenOn(false);
    s.network.setScreenOn(false);
    s.network.setScreenOn(true);
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_MISC_SCREEN_STATE) == 2; }));
    std::vector<Message> sent;
    for (auto &m : s.fake.received)
        if (m.command == IPC_MISC_SCREEN_STATE)
            sent.push_back(m);
    CHECK(sent[0].type == IPC_TYPE_EVENT && sent[0].data == Bytes{0});
    CHECK(sent[1].data == Bytes{1});
    // An event has no answer, and holds nothing up
    std::optional<bool> radio;
    s.network.setRadioPower(true, [&](bool ok) { radio = ok; });
    CHECK(s.loop.runUntil([&] { return radio.has_value(); }));
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

TEST(sms_in_is_acknowledged_by_the_client_and_sms_out_reports)
{
    Stack s;
    CHECK(s.booted());

    std::vector<std::pair<Bytes, bool>> got;
    s.sms.onIncoming([&](const Bytes &pdu, bool report) { got.push_back({pdu, report}); });
    s.fake.incomingSms(Bytes{0x00, 0x04, 0x0b, 0x91});
    s.fake.incomingSms(Bytes{0x00, 0x06, 0x01}, IPC_SMS_TYPE_STATUS_REPORT);
    CHECK(s.loop.runUntil([&] { return got.size() == 2; }));
    CHECK(got[0].first == (Bytes{0x00, 0x04, 0x0b, 0x91}) && !got[0].second);
    CHECK(got[1].second);
    // Nothing goes back before the client has the message
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(20));
    CHECK(s.fake.count(IPC_SMS_DELIVER_REPORT) == 0);

    // In order; a full memory, as the stock RIL reports it
    s.sms.acknowledge(true, false);
    s.sms.acknowledge(false, true);
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_SMS_DELIVER_REPORT) == 2; }));
    std::vector<ipc_sms_deliver_report_request_data> reports;
    for (auto &m : s.fake.received)
        if (auto r = m.as<ipc_sms_deliver_report_request_data>(); r && m.command == IPC_SMS_DELIVER_REPORT)
            reports.push_back(*r);
    CHECK(reports[0].type == IPC_SMS_NETWORK_GSM && reports[0].ack == IPC_SMS_ACK_NO_ERROR &&
          reports[0].id == 5);
    CHECK(reports[1].ack == IPC_SMS_ACK_PDA_FULL_ERROR && reports[1].id == 6);

    std::optional<SmsSendResult> sent;
    s.sms.send(Bytes{0x01, 0x00, 0x0b}, [&](const SmsSendResult &r) { sent = r; });
    CHECK(s.loop.runUntil([&] { return sent.has_value(); }));
    CHECK(sent->ok() && sent->reference == 7);
    // The SMSC from the SIM went in front of the TPDU
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SMS_SEND_MSG) {
            const size_t h = sizeof(ipc_sms_send_msg_request_header);
            CHECK(m.data[h] == 3 && m.data[h + 1] == 0x91 && m.data[h + 4] == 0x01);
        }
    }

    // The network refuses: congestion is worth a retry, a barred number not
    s.fake.sendResult = 0x8000 + 42;
    sent.reset();
    s.sms.send(Bytes{0x01, 0x00, 0x0b}, [&](const SmsSendResult &r) { sent = r; });
    CHECK(s.loop.runUntil([&] { return sent.has_value(); }));
    CHECK(sent->error == SmsSendResult::Error::Retry && sent->rpCause == 42);
    CHECK(smsSendResult(0x8000 + 21, 0).error == SmsSendResult::Error::Failed);
    CHECK(smsSendResult(0x000c, 0).error == SmsSendResult::Error::FdnBlocked);
}

TEST(sms_without_a_client_is_acknowledged_by_the_core)
{
    Stack s;
    CHECK(s.booted());
    s.fake.incomingSms(Bytes{0x00, 0x04, 0x0b, 0x91});
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_SMS_DELIVER_REPORT) == 1; }));
}

TEST(sms_sim_store_smsc_and_memory)
{
    Stack s;
    CHECK(s.booted());

    std::optional<std::optional<int>> stored;
    s.sms.writeToSim(IPC_SMS_STATUS_REC_READ, Bytes{0x00, 0x04, 0x0b},
                     [&](std::optional<int> index) { stored = index; });
    CHECK(s.loop.runUntil([&] { return stored.has_value(); }));
    CHECK(*stored == 4);  // the modem's record 3, counted from 1
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SMS_SAVE_MSG) {
            auto h = m.as<ipc_sms_save_msg_request_header>();
            CHECK(h && h->magic == IPC_SMS_NETWORK_GSM && h->index == IPC_SMS_SIM_INDEX_ANY &&
                  h->status == IPC_SMS_STATUS_REC_READ && h->length == 3);
        }
    }

    std::optional<bool> deleted;
    s.sms.deleteOnSim(4, [&](bool ok) { deleted = ok; });
    CHECK(s.loop.runUntil([&] { return deleted.has_value(); }));
    CHECK(*deleted);
    for (auto &m : s.fake.received)
        if (auto d = m.as<ipc_sms_del_msg_request_data>(); d && m.command == IPC_SMS_DEL_MSG)
            CHECK(d->index == 3);

    std::optional<bool> set;
    s.sms.setSmsc(Bytes{0x91, 0x24, 0x03, 0x21}, [&](bool ok) { set = ok; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    CHECK(*set && s.sms.smsc() == (Bytes{0x91, 0x24, 0x03, 0x21}));
    for (auto &m : s.fake.received) {
        if (m.command == IPC_SMS_SVC_CENTER_ADDR && m.type == IPC_TYPE_SET) {
            CHECK(m.data.size() == sizeof(ipc_sms_svc_center_addr_set_data));
            CHECK(m.data[0] == 4 && m.data[1] == 0x91 && m.data[5] == 0);
        }
    }

    std::optional<bool> memory;
    s.sms.setMemoryAvailable(false, [&](bool ok) { memory = ok; });
    CHECK(s.loop.runUntil([&] { return memory.has_value(); }));
    for (auto &m : s.fake.received)
        if (m.command == IPC_SMS_MEM_STATUS)
            CHECK(m.data == Bytes{IPC_SMS_MEM_STATUS_FULL});

    std::optional<bool> simFull;
    s.sms.onSimFull([&](bool full) { simFull = full; });
    s.fake.send(IPC_SMS_MEM_STATUS, IPC_TYPE_NOTI, {IPC_SMS_NETWORK_GSM, IPC_SMS_MEM_STATUS_FULL});
    CHECK(s.loop.runUntil([&] { return simFull.has_value(); }));
    CHECK(*simFull);
}

TEST(cell_broadcast_channels_and_pages)
{
    Stack s;
    CHECK(s.booted());

    std::vector<Bytes> pages;
    s.sms.onBroadcast([&](const CellBroadcast &b) { pages.push_back(b.page); });

    // Channel 50 and the presidential alert: the warning bits, the listed
    // channel, no padding
    std::optional<bool> set;
    s.sms.setBroadcastConfig({{50, 50, 0, 0xff, true}, {0x1112, 0x1112, 0, 0xff, true}},
                             [&](bool ok) { set = ok; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    Message config;
    for (auto &m : s.fake.received)
        if (m.command == IPC_SMS_CBS_CFG)
            config = m;
    CHECK(config.data.size() == 3 + 2);
    CHECK(config.data[0] == IPC_SMS_CBS_ENABLE &&
          config.data[1] == (IPC_SMS_CBS_SELECT_WARNINGS | IPC_SMS_CBS_SELECT_ALL_WARNINGS));
    CHECK(config.data[2] == 1 && config.data[3] == 0 && config.data[4] == 50);

    // Normal channels only: a list padded to its full length
    set.reset();
    s.sms.setBroadcastConfig({{50, 52, 0, 0xff, true}, {51, 51, 0, 0xff, false}},
                             [&](bool ok) { set = ok; });
    CHECK(s.loop.runUntil([&] { return set.has_value(); }));
    for (auto &m : s.fake.received)
        if (m.command == IPC_SMS_CBS_CFG)
            config = m;
    CHECK(config.data[1] == IPC_SMS_CBS_SELECT_LIST && config.data[2] == IPC_SMS_CBS_LIST_MAX);
    CHECK(config.data.size() == 3 + 2 * IPC_SMS_CBS_LIST_MAX);
    CHECK(config.data[4] == 50 && config.data[6] == 52 && config.data[7] == 0xff);

    // A GSM page on channel 50 passes, one on channel 51 does not
    Bytes page(88, 0x11);
    page[2] = 0;
    page[3] = 50;
    Bytes frame{IPC_SMS_CBS_TYPE_GSM, 88};
    frame.insert(frame.end(), page.begin(), page.end());
    s.fake.send(IPC_SMS_CBS_MSG, IPC_TYPE_NOTI, frame);
    frame[2 + 3] = 51;
    s.fake.send(IPC_SMS_CBS_MSG, IPC_TYPE_NOTI, frame);
    CHECK(s.loop.runUntil([&] { return pages.size() == 1; }));
    s.loop.runUntil([] { return false; }, std::chrono::milliseconds(20));
    CHECK(pages.size() == 1 && pages[0] == page);

    // Activation follows the channels
    std::optional<bool> active;
    s.sms.setBroadcastActivation(true, [&](bool ok) { active = ok; });
    CHECK(s.loop.runUntil([&] { return active.has_value(); }));
    for (auto &m : s.fake.received)
        if (m.command == IPC_SMS_CBS_CFG)
            config = m;
    CHECK(config.data.size() == sizeof(ipc_sms_cbs_activation_data));
    CHECK(config.data[0] == IPC_SMS_CBS_ENABLE && config.data[1] == 1 && config.data[2] == 10);
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

    // The volume keys: one message a step, the level capped
    const size_t volumes = s.fake.count(IPC_SND_SPKR_VOLUME_CTRL);
    s.calls.setVolume(2);
    s.calls.setVolume(2);
    s.calls.setVolume(9);
    CHECK(s.loop.runUntil([&] { return s.fake.count(IPC_SND_SPKR_VOLUME_CTRL) == volumes + 2; }));

    bool hung = false;
    s.calls.hangup([&](bool ok) { hung = ok; });
    CHECK(s.loop.runUntil([&] { return hung && s.calls.list().empty(); }));
    CHECK(audio.size() == 2 && !audio[1]);
}

TEST(numbers_are_dialled_as_the_stock_ril_dials_them)
{
    // The '+' stays in the number, and the prefix byte says international;
    // national numbers have their own prefix byte, and the caller ID setting
    // goes along
    for (const std::string number : {"+420123456789", "800810820"}) {
        Stack s;
        CHECK(s.booted());
        std::optional<bool> dialled;
        s.calls.dial(number, [&](bool ok) { dialled = ok; }, IPC_CALL_IDENTITY_HIDE);
        CHECK(s.loop.runUntil([&] { return dialled.has_value(); }));
        Message out;
        for (auto &m : s.fake.received)
            if (m.command == IPC_CALL_OUTGOING)
                out = m;
        auto o = out.as<ipc_call_originate_data>();
        CHECK(o && out.data.size() == sizeof(*o));
        CHECK(o->call.unknown == 0 && o->call.type == IPC_CALL_TYPE_VOICE &&
              o->call.identity == IPC_CALL_IDENTITY_HIDE && o->category == 0);
        CHECK(o->call.prefix == (number[0] == '+' ? IPC_CALL_PREFIX_INTL : IPC_CALL_PREFIX_NATIONAL));
        CHECK(std::string(reinterpret_cast<const char *>(o->call.number), o->call.number_length) == number);
    }
}

TEST(emergency_call_and_mute_as_the_stock_ril_sends_them)
{
    Stack s;
    CHECK(s.booted());
    std::optional<bool> dialled;
    s.calls.dialEmergency("112", 0x05, [&](bool ok) { dialled = ok; });
    CHECK(s.loop.runUntil([&] { return dialled.has_value(); }));
    Message out;
    for (auto &m : s.fake.received)
        if (m.command == IPC_CALL_OUTGOING)
            out = m;
    auto e = out.as<ipc_call_originate_data>();
    CHECK(e && out.data.size() == sizeof(*e));
    CHECK(e->call.unknown == IPC_CALL_OUTGOING_EMERGENCY && e->call.type == IPC_CALL_TYPE_VOICE &&
          e->call.number_length == 3 && e->call.prefix == IPC_CALL_PREFIX_NATIONAL && e->category == 0x05);

    std::optional<bool> muted;
    s.calls.setMute(true, [&](bool ok) { muted = ok; });
    CHECK(s.loop.runUntil([&] { return muted.has_value(); }));
    for (auto &m : s.fake.received)
        if (m.command == IPC_SND_MIC_MUTE_CTRL)
            CHECK(m.type == IPC_TYPE_SET && m.data == Bytes{1});
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

TEST(two_calls_swap_join_split_and_end)
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

    // A second call waits; the swap puts the first on hold
    auto states = [&] {
        std::vector<CallState> v;
        for (const Call &c : s.calls.list())
            v.push_back(c.state);
        return v;
    };
    s.fake.waitingCall("123456789");
    CHECK(s.loop.runUntil([&] { return states() == std::vector{CallState::Active, CallState::Waiting}; }));

    auto run = [&](uint8_t op, uint8_t id, std::vector<CallState> expect) {
        std::optional<bool> done;
        s.calls.manage(op, [&](bool ok) { done = ok; }, id);
        return s.loop.runUntil([&] { return done.has_value() && *done && states() == expect; });
    };
    CHECK(run(IPC_SS_MANAGE_CALL_SWAP, 0, {CallState::Holding, CallState::Active}));
    CHECK(run(IPC_SS_MANAGE_CALL_CONFERENCE, 0, {CallState::Active, CallState::Active}));
    CHECK(s.calls.list()[0].multiparty && s.calls.list()[1].multiparty);
    CHECK(run(IPC_SS_MANAGE_CALL_SPLIT, 2, {CallState::Holding, CallState::Active}));
    auto m = std::find_if(s.fake.received.rbegin(), s.fake.received.rend(),
                          [](const Message &x) { return x.command == IPC_SS_MANAGE_CALL; });
    CHECK(m != s.fake.received.rend() && m->data.size() == sizeof(ipc_ss_manage_call_data) &&
          m->data[0] == IPC_SS_MANAGE_CALL_SPLIT && m->data[1] == 2);

    // One call by id while two are up; the last one with the plain release
    std::optional<bool> ended;
    s.calls.hangup(1, [&](bool ok) { ended = ok; });
    CHECK(s.loop.runUntil([&] { return ended.has_value() && states() == std::vector{CallState::Active}; }));
    const size_t releases = s.fake.count(IPC_CALL_RELEASE);
    ended.reset();
    s.calls.hangup(2, [&](bool ok) { ended = ok; });
    CHECK(s.loop.runUntil([&] { return ended.has_value() && s.calls.list().empty(); }));
    CHECK(s.fake.count(IPC_CALL_RELEASE) == releases + 1);
    // The audio path stayed up for as long as there was a call
    CHECK(audio == (std::vector<bool>{true, false}));
}

TEST(sim_services_for_ims)
{
    Stack s;
    CHECK(s.booted());

    std::optional<SimAuthResult> auth;
    sim::Sqn sqn{0, 0, 0, 0, 0, 0x20};
    sim::Block rand{};
    rand.fill(1);
    auto vector = sim::makeVector(s.fake.milenage, rand, sqn, {0x80, 0x00});
    s.sim.authenticate(0, Bytes(vector.rand.begin(), vector.rand.end()),
                       Bytes(vector.autn.begin(), vector.autn.end()), [&](std::optional<SimAuthResult> r) {
        if (r)
            auth = r;
    });
    CHECK(s.loop.runUntil([&] { return auth.has_value(); }));
    CHECK(auth->status == 0 && auth->values.size() == 3 && auth->values[0].size() == 8);
    CHECK(auth->values[0] == Bytes(vector.xres.begin(), vector.xres.end()));
    CHECK(auth->values[2] == Bytes(vector.ik.begin(), vector.ik.end()));
    // The same SQN again: the card asks to resynchronise
    auth.reset();
    s.sim.authenticate(0, Bytes(vector.rand.begin(), vector.rand.end()),
                       Bytes(vector.autn.begin(), vector.autn.end()), [&](std::optional<SimAuthResult> r) {
                           if (r)
                               auth = r;
                       });
    CHECK(s.loop.runUntil([&] { return auth.has_value(); }));
    CHECK(auth->status == 4 && !auth->values.empty() && auth->values[0].size() == 14);

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
