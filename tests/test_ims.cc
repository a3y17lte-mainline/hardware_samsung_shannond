// SPDX-License-Identifier: GPL-2.0-or-later
//
// IMS registration against the fake P-CSCF (sim/fake_pcscf.h), over real
// sockets on the loopback interface, with Milenage on both sides.
#include "core/data.h"
#include "core/ims/identity.h"
#include "core/ims/ims.h"
#include "core/modem.h"
#include "core/network.h"
#include "core/sim.h"
#include "core/ims/registration.h"
#include "sim/fake_modem.h"
#include "sim/fake_pcscf.h"
#include "tests/test.h"

#include <cstdio>
#include <cstdlib>

extern "C" {
#include <samsung-ipc.h>
}

using namespace shannond;
using namespace shannond::ims;

namespace {

constexpr uint16_t kPcscfPort = 45060;

struct Bench {
    EventLoop loop;
    sim::FakeImsNetwork net{"127.0.0.1"};
    sim::FakePcscf pcscf{loop, "127.0.0.2", kPcscfPort};
    SipEndpoint endpoint{loop, net, timers()};
    sim::Milenage card = FakeModem::testMilenage();
    sim::Sqn sqnMs{};
    std::vector<std::string> log;
    ImsRegistration reg{loop, endpoint, net, [this](const Bytes &rand, const Bytes &autn, auto done) {
                         sim::Block r, a;
                         std::copy_n(rand.begin(), 16, r.begin());
                         std::copy_n(autn.begin(), 16, a.begin());
                         auto result = sim::cardAuthenticate(card, sqnMs, r, a);
                         AkaAnswer answer;
                         if (result.status == 0) {
                             answer.kind = AkaAnswer::Kind::Ok;
                             answer.res = result.values[0];
                             answer.ck = result.values[1];
                             answer.ik = result.values[2];
                         } else if (result.status == 4) {
                             answer.kind = AkaAnswer::Kind::Resync;
                             answer.auts = result.values[0];
                         }
                         loop.post([done, answer]() { done(answer); });
                     }};

    Bench()
    {
        endpoint.onRequest([this](const SipMessage &request, const Source &source) {
            if (!reg.handleRequest(request, source))
                endpoint.respond(source, request, request.makeResponse(501, "Not Implemented"));
        });
        const bool print = std::getenv("IMS_TEST_LOG");
        reg.setLog([this, print](const std::string &l) {
            log.push_back(l);
            if (print)
                std::printf("    %s\n", l.c_str());
        });
        if (print)
            endpoint.setLog([](const std::string &l) { std::printf("    %s\n", l.c_str()); });
    }

    static SipTimers timers()
    {
        SipTimers t;
        t.t1 = std::chrono::milliseconds(10);
        t.t2 = std::chrono::milliseconds(40);
        return t;
    }

    RegistrationParams params()
    {
        RegistrationParams p;
        p.profile = defaultProfile();
        p.profile.pcscfPort = kPcscfPort;
        p.profile.localPort = 0;
        p.profile.portBase = 45000;
        p.profile.retryBase = 1;
        p.profile.retryMax = 1;
        p.identity = *identityFromImsi("230020000000001", 2);
        p.pcscfs = {"127.0.0.2"};
        p.instance = instanceId("350000000000001");
        p.userAgent = userAgentFor(p.profile, DeviceInfo{});
        p.accessNetworkInfo = [] { return accessNetworkInfo("23002", 0x5678, 0x0abcdef); };
        return p;
    }

    bool waitFor(RegistrationState state, std::chrono::milliseconds limit = std::chrono::seconds(5))
    {
        return loop.runUntil([&] { return reg.state() == state; }, limit);
    }

    size_t count(const std::string &method) const
    {
        size_t n = 0;
        for (const auto &m : pcscf.requests)
            n += m.method() == method;
        return n;
    }
};

}  // namespace

TEST(ims_identity)
{
    auto id = identityFromImsi("230020123456789", 2);
    CHECK(id && id->domain == "ims.mnc002.mcc230.3gppnetwork.org");
    CHECK(id->impi == "230020123456789@ims.mnc002.mcc230.3gppnetwork.org");
    CHECK(id->impu() == "sip:230020123456789@ims.mnc002.mcc230.3gppnetwork.org");
    CHECK(identityFromImsi("310150123456789", 3)->domain == "ims.mnc150.mcc310.3gppnetwork.org");
    CHECK(!identityFromImsi("2300x", 2) && !identityFromImsi("230020123456789", 4));
    CHECK(isimTlv(Bytes{0x80, 3, 'a', 'b', 'c', 0xff}) == "abc");
    CHECK(!isimTlv(Bytes{0x80, 9, 'a'}) && !isimTlv(Bytes{0xff, 0xff}));
    CHECK(mncLengthFromAd(Bytes{0, 0, 0, 2}) == 2 && mncLengthFromAd(Bytes{0, 0}) == 0);
    CHECK(instanceId("35000000000001") == "<urn:gsma:imei:35000000-000001-0>");
    CHECK(accessNetworkInfo("23002", 0x1a2b, 0x12345) ==
          "3GPP-E-UTRAN-FDD;utran-cell-id-3gpp=230021a2b0012345");
    CHECK(profileFor("23002") && profileFor("23002")->ipsec && !profileFor("23001"));
    // The stock User-Agent: the O2 profile has no template of its own, so
    // the default one with the last three characters of the firmware
    CHECK(userAgentFor(*profileFor("23002"), DeviceInfo{}) == "SM-A320FL-TK2 Samsung IMS 6.0");
    CHECK(userAgentFor(defaultProfile(), DeviceInfo{}) == "SM-A320FL-A320FLXXS9CTK2 Samsung IMS 6.0");
}

TEST(ims_register_and_deregister)
{
    Bench b;
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));

    // The security agreement: our SPIs and ports, the P-CSCF's, MD5 keys
    CHECK(b.net.installed.size() == 1);
    const SaSet &s = b.net.installed[0];
    CHECK(s.spiPc == b.pcscf.spiC && s.spiPs == b.pcscf.spiS);
    CHECK(s.portPc == b.pcscf.portC && s.portPs == b.pcscf.portS);
    // The stock port counter: server port on a multiple of 100, client + 1
    CHECK(s.portUs > 45000 && s.portUs % 100 == 0 && s.portUc == s.portUs + 1);
    // Stock SPIs: spi-c in [100, 100099], spi-s = spi-c + 1
    CHECK(s.spiUc >= 100 && s.spiUc < 100100 && s.spiUs == s.spiUc + 1);
    CHECK(s.alg == "hmac-md5-96" && s.integrityKey.size() == 16 && s.encryptionKey.empty());
    // client UDP, server UDP, server TCP, and a TCP client connection if
    // a request was too long for UDP
    CHECK(b.net.protectedSockets >= 3);
    CHECK(b.net.reserved.empty());
    // Security-Verify repeats the P-CSCF's Security-Server
    CHECK(b.pcscf.lastSecurityVerify.find("spi-c=20480") != std::string::npos);
    // Via and Contact carry the protected server port
    CHECK(b.pcscf.ueServer.port == s.portUs);
    CHECK(b.pcscf.lastContact.find(":" + std::to_string(s.portUs) + ">") != std::string::npos);
    bool stockAgent = false;
    for (const auto &m : b.pcscf.requests)
        stockAgent |= m.header("User-Agent") == "SM-A320FL-A320FLXXS9CTK2 Samsung IMS 6.0";
    CHECK(stockAgent);
    CHECK(b.pcscf.lastContact.find("+sip.instance=\"<urn:gsma:imei:35000000-000000-0>\"") != std::string::npos);
    CHECK(b.pcscf.lastContact.find("icsi.mmtel") != std::string::npos);

    const auto &info = b.reg.info();
    CHECK(info.expires == 3600 && info.serviceRoute.size() == 1 && info.associatedUris.size() == 3);
    // After registration the first associated URI speaks for us, not the
    // IMSI-derived identity (barred by the fake network, as by O2)
    CHECK(info.defaultImpu == "sip:+420600000001@ims.mnc002.mcc230.3gppnetwork.org");
    // The protected REGISTER carried the cell; the unprotected one did not
    bool sawPani = false, plainPani = false;
    for (const auto &m : b.pcscf.requests)
        if (m.method() == "REGISTER" && m.has("P-Access-Network-Info"))
            (m.has("Security-Verify") ? sawPani : plainPani) = true;
    CHECK(sawPani && !plainPani);
    CHECK(b.loop.runUntil([&] { return b.count("SUBSCRIBE") == 1; }));
    // Its route: the P-CSCF's protected port first, then the Service-Route
    for (const auto &m : b.pcscf.requests)
        if (m.method() == "SUBSCRIBE") {
            auto routes = m.headers("Route");
            CHECK(routes.size() == 2);
            CHECK(routes[0] == "<sip:127.0.0.2:" + std::to_string(b.pcscf.portS) + ";lr>");
            CHECK(routes[1] == "<sip:orig@scscf.ims.example;lr>");
        }

    bool stopped = false;
    b.reg.stop([&] { stopped = true; });
    CHECK(b.loop.runUntil([&] { return stopped; }));
    CHECK(b.reg.state() == RegistrationState::Idle);
    CHECK(b.pcscf.deregistered == 1);
    CHECK(b.net.installed.empty() && b.net.removed.size() == 1);
}

TEST(ims_register_resynchronises)
{
    Bench b;
    b.sqnMs = {0, 0, 0, 0, 1, 0};  // the card is ahead of the network
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));
    CHECK(b.pcscf.resyncs == 1 && b.pcscf.challenges == 2);
}

TEST(ims_register_fails_over)
{
    Bench b;
    auto p = b.params();
    p.pcscfs = {"127.0.0.3", "127.0.0.2"};  // nobody at the first
    b.reg.start(p);
    CHECK(b.waitFor(RegistrationState::Registered, std::chrono::seconds(8)));
    CHECK(b.reg.info().pcscf == "127.0.0.2");
}

TEST(ims_register_interval_too_brief)
{
    Bench b;
    b.pcscf.minExpires = 700000;
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));
    CHECK(b.pcscf.requests.back().method() == "SUBSCRIBE" || b.pcscf.registered == 1);
    bool asked = false;
    for (const auto &m : b.pcscf.requests)
        asked |= m.method() == "REGISTER" && m.header("Expires") == "700000";
    CHECK(asked);
}

TEST(ims_register_rejected_backs_off)
{
    Bench b;
    b.pcscf.failStatus = 403;
    b.reg.start(b.params());
    CHECK(b.loop.runUntil([&] { return b.count("REGISTER") >= 2; }, std::chrono::seconds(4)));
    CHECK(b.reg.state() == RegistrationState::Registering);
    CHECK(b.net.reserved.size() <= 2);  // no SPIs leak across attempts
    b.pcscf.failStatus = 0;
    CHECK(b.waitFor(RegistrationState::Registered, std::chrono::seconds(4)));
}

TEST(ims_refresh_with_new_keys)
{
    Bench b;
    b.pcscf.grantExpires = 4;  // refresh after 2 s
    b.pcscf.challengeRefresh = true;
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));
    // As the stock stack: same ports, so the security associations stay;
    // the SPIs offered with the refresh are given back
    const SaSet first = b.net.installed.at(0);
    CHECK(b.loop.runUntil([&] { return b.pcscf.registered == 2; }, std::chrono::seconds(6)));
    CHECK(b.loop.runUntil([&] { return b.net.reserved.empty(); }));
    CHECK(b.reg.state() == RegistrationState::Registered);
    CHECK(b.net.installed.size() == 1 && b.net.removed.empty());
    CHECK(b.net.installed[0].spiUc == first.spiUc && b.pcscf.ueServer.port == first.portUs);
    CHECK(b.pcscf.challenges == 2);
}

TEST(ims_network_ends_registration)
{
    Bench b;
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));
    CHECK(b.loop.runUntil([&] { return b.count("SUBSCRIBE") == 1; }));
    b.pcscf.terminateRegistration();
    CHECK(b.loop.runUntil([&] { return b.reg.state() != RegistrationState::Registered; }));
    CHECK(b.waitFor(RegistrationState::Registered, std::chrono::seconds(4)));
    CHECK(b.pcscf.registered == 2);
}

TEST(ims_register_over_tcp)
{
    Bench b;
    b.endpoint.udpLimit = 100;  // everything is "too long" for UDP
    b.reg.start(b.params());
    CHECK(b.waitFor(RegistrationState::Registered));
    CHECK(b.pcscf.ueServer.port == b.net.installed.at(0).portUs);
    CHECK(b.loop.runUntil([&] { return b.count("SUBSCRIBE") == 1; }));
}

namespace {

Bytes tlv(const std::string &s, size_t pad = 0)
{
    Bytes b{0x80, static_cast<uint8_t>(s.size())};
    b.insert(b.end(), s.begin(), s.end());
    if (b.size() < pad)
        b.resize(pad, 0xff);
    return b;
}

}  // namespace

TEST(ims_brings_up_pdn_registers_and_tells_the_modem)
{
    EventLoop loop;
    FakeModem fake{loop};
    Modem modem{loop, fake.fmtFd(), fake.rfsFd()};
    Sim sim{modem};
    Network network{modem};
    Data data{modem, DataLinks::none()};
    sim::FakePcscf pcscf{loop, "127.0.0.2", kPcscfPort};
    fake.pcscf4 = {127, 0, 0, 2};
    // An ISIM with its own identities; the tel: URI is listed first
    fake.simBinary[0x6f02] = tlv("isim-user@ims.example.net");
    fake.simBinary[0x6f03] = tlv("ims.example.net");
    fake.simRecords[0x6f04] = {tlv("tel:+420600000001", 40), tlv("sip:isim-user@ims.example.net", 40)};

    Ims::Options o;
    o.profile = defaultProfile();
    o.profile->pcscfPort = kPcscfPort;
    o.profile->localPort = 0;
    o.profile->portBase = 45000;
    o.profile->timers.t1 = std::chrono::milliseconds(10);
    o.pdnRetry = std::chrono::seconds(1);
    sim::FakeImsNetwork *net = nullptr;
    Ims ims(modem, sim, network, data,
            [&](const DataCall &) {
                auto n = std::make_unique<sim::FakeImsNetwork>("127.0.0.1");
                net = n.get();
                return n;
            },
            o);

    if (std::getenv("IMS_TEST_LOG"))
        modem.setLog([](const std::string &l) { std::printf("    %s\n", l.c_str()); });
    fake.boot();
    CHECK(loop.runUntil([&] { return sim.state() == SimState::Ready && network.data().volte == true; }));
    CHECK(!ims.registered());
    ims.setEnabled(true);
    CHECK(loop.runUntil([&] { return ims.registered(); }));

    // The PDN: the lte_ims profile, then IPv6 with a P-CSCF request on the
    // IMS context (cid 1, as stock)
    bool profile = false, defined = false;
    for (const auto &m : fake.received)
        if (m.command == IPC_GPRS_SET_APN_INFO) {
            auto p = m.as<ipc_gprs_set_apn_info_data>();
            profile = p && p->profile_id == IPC_GPRS_PROFILE_IMS && std::string(p->name) == "lte_ims" &&
                      p->protocol == IPC_GPRS_PDP_TYPE_IPV6 && p->pcscf == IPC_GPRS_PCSCF_REQUEST_IPV6 &&
                      std::string(p->apn) == "ims" && !defined;
        } else if (m.command == IPC_GPRS_DEFINE_PDP_CONTEXT) {
            auto d = m.as<ipc_gprs_define_pdp_context_ext_data>();
            defined = d && d->cid == 1 && d->type == IPC_GPRS_PDP_TYPE_IPV6 &&
                      d->pcscf == IPC_GPRS_PCSCF_REQUEST_IPV6 && std::string(d->apn) == "ims";
        }
    CHECK(profile && defined);
    // Activated as the IMS PDN (mode 3), else the modem's voice engine
    // refuses it (0x8001, seen on the phone)
    CHECK(fake.imsContexts.count(1) == 1);
    // The ISIM's identities: the SIP IMPU registers, the IMPI authenticates
    bool fromIsim = false;
    for (const auto &m : pcscf.requests)
        if (m.method() == "REGISTER")
            fromIsim = m.header("From")->find("<sip:isim-user@ims.example.net>") == 0 &&
                       m.header("Authorization")->find("username=\"isim-user@ims.example.net\"") !=
                           std::string::npos &&
                       m.requestUri() == "sip:ims.example.net";
    CHECK(fromIsim);
    CHECK(net && net->installed.size() == 1);
    // The modem heard about it as the stock RIL tells it
    auto regState = [&]() -> std::optional<ipc_ims_reg_state_data> {
        std::optional<ipc_ims_reg_state_data> last;
        for (const auto &m : fake.received)
            if (m.command == IPC_IMS_REG_STATE)
                last = m.as<ipc_ims_reg_state_data>();
        return last;
    };
    CHECK(loop.runUntil([&] { return regState().has_value(); }));
    CHECK(regState()->status == IPC_IMS_REG_STATUS_REGISTERED && regState()->features == IPC_IMS_FEATURE_VOICE &&
          regState()->rat == IPC_IMS_RAT_LTE);
    // Calls are possible: the voice engine is on, with the PDN's address
    CHECK(ims.calls().attached());
    CHECK(loop.runUntil([&] { return fake.count(IPC_IMS_ENGINE) == 1 && fake.count(IPC_IMS_PDN) == 1; }));

    // A cell without voice over PS: deregistered, the PDN goes, the modem
    // hears it
    fake.volteSupported = false;
    fake.networkChanged();
    CHECK(loop.runUntil([&] { return regState()->status == IPC_IMS_REG_STATUS_NOT_REGISTERED; }));
    auto released = [&] {
        for (const auto &m : fake.received)
            if (m.command == IPC_GPRS_PDP_CONTEXT)
                if (auto s = m.as<ipc_gprs_pdp_context_request_set_data>(); s && s->cid == 1 && !s->enable)
                    return true;
        return false;
    };
    CHECK(loop.runUntil(released));
    CHECK(pcscf.deregistered == 1);
    CHECK(net == nullptr || ims.endpoint() == nullptr);
    CHECK(!ims.calls().attached() && !ims.calls().dial("+420603123456"));
    CHECK(ims.status().find("no voice over PS") != std::string::npos);
}

TEST(ims_srvcc_call_list)
{
    // The list the modem gets when SRVCC starts, as the stock RIL builds it
    auto call = [](int id, ImsCallState state, bool incoming, const char *number) {
        ImsCallInfo c;
        c.id = id;
        c.state = state;
        c.incoming = incoming;
        c.number = number;
        return c;
    };
    // One call: transaction 0, whatever its state
    Bytes one = srvccCallList({call(7, ImsCallState::Alerting, false, "+420603")});
    CHECK(one == Bytes({1, 0x00, IPC_CALL_TYPE_VOICE, 0, IPC_CALL_TERM_MO, IPC_CALL_LIST_ENTRY_STATUS_ALERTING, 0, 7,
                        0x81, '+', '4', '2', '0', '6', '0', '3'}));
    // Held and active: the active one is transaction 0
    Bytes two = srvccCallList({call(1, ImsCallState::Held, true, "1"), call(2, ImsCallState::Active, false, "2")});
    CHECK(two == Bytes({2, 0x00, 1, 1, IPC_CALL_TERM_MT, IPC_CALL_LIST_ENTRY_STATUS_HOLDING, 0, 1, 0x81, '1', 0x00, 1,
                        0, IPC_CALL_TERM_MO, IPC_CALL_LIST_ENTRY_STATUS_ACTIVE, 0, 1, 0x81, '2'}));
    // No active call: the held one is 0; a ringing call next to it waits
    Bytes held = srvccCallList({call(1, ImsCallState::Incoming, true, "1"), call(2, ImsCallState::Held, false, "2")});
    CHECK(held[3] == 1 && held[5] == IPC_CALL_LIST_ENTRY_STATUS_WAITING && held[12] == 0);
    // Three: active 0, held 1, the third 2
    Bytes three = srvccCallList({call(1, ImsCallState::Incoming, true, ""), call(2, ImsCallState::Held, false, ""),
                                 call(3, ImsCallState::Active, false, "")});
    CHECK(three.size() == 1 + 3 * 8 && three[3] == 2 && three[11] == 1 && three[19] == 0);
    // A ringing call alone is incoming
    CHECK(srvccCallList({call(1, ImsCallState::Incoming, true, "")})[5] == IPC_CALL_LIST_ENTRY_STATUS_INCOMING);
}

TEST(ims_srvcc)
{
    EventLoop loop;
    FakeModem fake{loop};
    Modem modem{loop, fake.fmtFd(), fake.rfsFd()};
    Sim sim{modem};
    Network network{modem};
    Data data{modem, DataLinks::none()};
    sim::FakePcscf pcscf{loop, "127.0.0.2", kPcscfPort};
    fake.pcscf4 = {127, 0, 0, 2};
    fake.simBinary[0x6fad] = Bytes{0, 0, 0, 2};

    Ims::Options o;
    o.profile = defaultProfile();
    o.profile->pcscfPort = kPcscfPort;
    o.profile->localPort = 0;
    o.profile->portBase = 45000;
    o.profile->timers.t1 = std::chrono::milliseconds(10);
    Ims ims(modem, sim, network, data,
            [](const DataCall &) { return std::make_unique<sim::FakeImsNetwork>("127.0.0.1"); }, o);
    std::vector<SrvccState> states;
    ims.onSrvcc([&](SrvccState s) { states.push_back(s); });
    std::vector<ImsCallInfo> changes;
    ims.calls().onChanged([&](const ImsCallInfo &c) { changes.push_back(c); });
    fake.boot();
    CHECK(loop.runUntil([&] { return sim.state() == SimState::Ready && network.data().volte == true; }));
    ims.setEnabled(true);
    CHECK(loop.runUntil([&] { return ims.registered() && ims.calls().attached(); }));
    auto lists = [&] {
        std::vector<Bytes> out;
        for (const auto &m : fake.received)
            if (m.command == IPC_CALL_LIST && m.type == IPC_TYPE_SET)
                out.push_back(m.data);
        return out;
    };
    auto srvcc = [&](uint8_t state) {
        fake.send(IPC_CALL_SRVCC, IPC_TYPE_NOTI, Bytes{state});
        loop.runUntil([] { return false; }, std::chrono::milliseconds(100));
    };

    // Without a VoLTE call the modem's SRVCC is none of ours (as stock)
    srvcc(IPC_CALL_SRVCC_STARTED);
    CHECK(states.empty() && lists().empty());

    // A call (the far end never answers): the list when SRVCC starts, the
    // call stays on IMS when it fails
    auto id = ims.calls().dial("+420603123456");
    CHECK(id);
    CHECK(loop.runUntil([&] { return !changes.empty(); }));
    srvcc(IPC_CALL_SRVCC_STARTED);
    CHECK(states == std::vector<SrvccState>{SrvccState::Started});
    CHECK(lists().size() == 1 && lists()[0] == srvccCallList(ims.calls().calls()));
    CHECK(lists()[0][5] == IPC_CALL_LIST_ENTRY_STATUS_DIALING);
    srvcc(IPC_CALL_SRVCC_FAILED);
    CHECK(states.back() == SrvccState::Failed && ims.calls().calls().size() == 1);

    // Completed: the call is the modem's now
    srvcc(IPC_CALL_SRVCC_STARTED);
    srvcc(IPC_CALL_SRVCC_COMPLETED);
    CHECK(states.back() == SrvccState::Completed && ims.calls().calls().empty());
    CHECK(changes.back().state == ImsCallState::Ended && changes.back().handedOver);
    CHECK(ims.registered());
}

TEST(ims_registers_on_a_card_without_isim)
{
    EventLoop loop;
    FakeModem fake{loop};
    fake.hasIsim = false;
    Modem modem{loop, fake.fmtFd(), fake.rfsFd()};
    Sim sim{modem};
    Network network{modem};
    Data data{modem, DataLinks::none()};
    sim::FakePcscf pcscf{loop, "127.0.0.2", kPcscfPort};
    fake.pcscf4 = {127, 0, 0, 2};
    fake.simBinary[0x6fad] = Bytes{0, 0, 0, 2};  // EF_AD: two digit MNC

    Ims::Options o;
    o.profile = defaultProfile();
    o.profile->pcscfPort = kPcscfPort;
    o.profile->localPort = 0;
    o.profile->portBase = 45000;
    o.profile->timers.t1 = std::chrono::milliseconds(10);
    Ims ims(modem, sim, network, data,
            [](const DataCall &) { return std::make_unique<sim::FakeImsNetwork>("127.0.0.1"); }, o);
    fake.boot();
    CHECK(loop.runUntil([&] {
        return sim.state() == SimState::Ready && network.data().volte == true && !sim.apps().empty();
    }));
    CHECK(sim.apps().size() == 1);
    ims.setEnabled(true);
    CHECK(loop.runUntil([&] { return ims.registered(); }));

    // The identity from the IMSI, AKA on the USIM
    bool imsiIdentity = false;
    for (const auto &m : pcscf.requests)
        if (m.method() == "REGISTER")
            imsiIdentity = m.header("From")->find("<sip:230020000000001@ims.mnc002.mcc230.3gppnetwork.org>") == 0;
    CHECK(imsiIdentity);
    bool usim = false;
    for (const auto &m : fake.received)
        if (m.command == IPC_SEC_IMS_AUTH && !m.data.empty())
            usim = m.data[0] == IPC_SEC_IMS_AUTH_TYPE_USIM;
    CHECK(usim);
}
