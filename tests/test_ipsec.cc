// SPDX-License-Identifier: GPL-2.0-or-later
//
// IMS registration with real IPsec: the UE's LinuxImsNetwork sets up the ESP
// security associations and socket policies, the fake P-CSCF protects its
// side with the mirror image. Needs CAP_NET_ADMIN in its own network
// namespace: run under `unshare -rn` (the test is skipped otherwise).
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/ims/identity.h"
#include "core/ims/linux_network.h"
#include "core/ims/registration.h"
#include "core/ports.h"
#include "sim/fake_modem.h"
#include "sim/fake_pcscf.h"
#include "tests/test.h"

using namespace shannond;
using namespace shannond::ims;

namespace {

constexpr const char *kUe = "2001:db8::1";
constexpr const char *kPcscf = "2001:db8::2";
constexpr uint16_t kPcscfPort = 5060;

bool namespaceReady()
{
    static int ready = -1;
    if (ready >= 0)
        return ready;
    ready = 0;
    if (geteuid() != 0 || !setLinkUp("lo", true))
        return false;
    for (const char *a : {kUe, kPcscf}) {
        uint8_t raw[16];
        inet_pton(AF_INET6, a, raw);
        if (!addAddress("lo", AF_INET6, raw, 128))
            return false;
    }
    // A new address is tentative until the kernel's (immediate, but
    // asynchronous) DAD on loopback finished: wait until it can be bound
    for (const char *a : {kUe, kPcscf}) {
        bool bound = false;
        for (int i = 0; i < 100 && !bound; i++) {
            int fd = socket(AF_INET6, SOCK_DGRAM, 0);
            sockaddr_in6 sa{};
            sa.sin6_family = AF_INET6;
            inet_pton(AF_INET6, a, &sa.sin6_addr);
            bound = bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0;
            close(fd);
            if (!bound)
                usleep(10000);
        }
        if (!bound)
            return false;
    }
    ready = 1;
    return true;
}

}  // namespace

TEST(ipsec_register_over_esp)
{
    if (!namespaceReady()) {
        std::printf("    skipped: needs its own network namespace (unshare -rn)\n");
        return;
    }
    EventLoop loop;
    LinuxImsNetwork ue("", kUe);
    LinuxImsNetwork network("", kPcscf);
    sim::FakePcscf pcscf(loop, kPcscf, kPcscfPort);
    pcscf.ipsec = &network;
    pcscf.alg = "hmac-sha-1-96";
    SipTimers timers;
    timers.t1 = std::chrono::milliseconds(50);
    SipEndpoint endpoint(loop, ue, timers);
    auto card = FakeModem::testMilenage();
    sim::Sqn sqnMs{};
    ImsRegistration reg(loop, endpoint, ue, [&](const Bytes &rand, const Bytes &autn, auto done) {
        sim::Block r, a;
        std::copy_n(rand.begin(), 16, r.begin());
        std::copy_n(autn.begin(), 16, a.begin());
        auto result = sim::cardAuthenticate(card, sqnMs, r, a);
        AkaAnswer answer;
        answer.kind = AkaAnswer::Kind::Ok;
        answer.res = result.values[0];
        answer.ck = result.values[1];
        answer.ik = result.values[2];
        done(answer);
    });
    endpoint.onRequest([&](const SipMessage &request, const Source &source) {
        if (!reg.handleRequest(request, source))
            endpoint.respond(source, request, request.makeResponse(501, "Not Implemented"));
    });
    if (std::getenv("IMS_TEST_LOG")) {
        reg.setLog([](const std::string &l) { std::printf("    %s\n", l.c_str()); });
        endpoint.setLog([](const std::string &l) { std::printf("    %s\n", l.c_str()); });
    }

    RegistrationParams p;
    p.profile = *profileFor("23002");  // O2 CZ: IPv6, integrity only, 6000/5000
    p.profile.localPort = 0;
    p.identity = *identityFromImsi("230020000000001", 2);
    p.pcscfs = {kPcscf};
    p.instance = instanceId("350000000000001");
    reg.start(p);
    CHECK(loop.runUntil([&] { return reg.state() == RegistrationState::Registered; }));
    CHECK(pcscf.protectedWithIpsec);
    // The stock stack's first ports: 6100 server, 6101 client
    CHECK(pcscf.ueServer.ip == kUe && pcscf.ueServer.port == 6100);
    CHECK(loop.runUntil([&] {
        size_t n = 0;
        for (const auto &m : pcscf.requests)
            n += m.method() == "SUBSCRIBE";
        return n == 1;
    }));

    // A plain packet to the protected server port is not taken: the
    // socket policy wants ESP
    size_t before = pcscf.requests.size();
    bool notified = false;
    reg.onChanged([&](RegistrationState, const std::string &) { notified = true; });
    int plain = socket(AF_INET6, SOCK_DGRAM, 0);
    sockaddr_in6 from{}, to{};
    from.sin6_family = to.sin6_family = AF_INET6;
    inet_pton(AF_INET6, kPcscf, &from.sin6_addr);
    inet_pton(AF_INET6, kUe, &to.sin6_addr);
    to.sin6_port = htons(6100);
    bind(plain, reinterpret_cast<sockaddr *>(&from), sizeof(from));
    std::string fake = "NOTIFY sip:x SIP/2.0\r\nVia: SIP/2.0/UDP [2001:db8::2]:9;branch=z9hG4bKp\r\n"
                       "Call-ID: x\r\nCSeq: 1 NOTIFY\r\nContent-Length: 0\r\n\r\n";
    sendto(plain, fake.data(), fake.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
    loop.runUntil([] { return false; }, std::chrono::milliseconds(200));
    close(plain);
    CHECK(pcscf.requests.size() == before);  // no 501 or anything else came back
    CHECK(!notified);

    bool stopped = false;
    reg.stop([&] { stopped = true; });
    CHECK(loop.runUntil([&] { return stopped; }));
    CHECK(pcscf.deregistered == 1);
}
