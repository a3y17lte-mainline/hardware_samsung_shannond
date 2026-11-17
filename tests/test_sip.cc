// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/digest.h"
#include "core/ims/secagree.h"
#include "core/ims/sip.h"
#include "sim/milenage.h"
#include "tests/test.h"

using namespace shannond;
using namespace shannond::ims;

namespace {

template <size_t N>
std::array<uint8_t, N> fromHex(const char *s)
{
    std::array<uint8_t, N> out{};
    for (size_t i = 0; i < N; i++)
        out[i] = static_cast<uint8_t>(std::stoi(std::string(s + 2 * i, 2), nullptr, 16));
    return out;
}

const char kRegister401[] =
    "SIP/2.0 401 Unauthorized\r\n"
    "Via: SIP/2.0/UDP [2001:db8::1]:5060;branch=z9hG4bKabc;rport=5060\r\n"
    "f: <sip:001010123456789@ims.mnc001.mcc001.3gppnetwork.org>;tag=a1\r\n"
    "t: <sip:001010123456789@ims.mnc001.mcc001.3gppnetwork.org>;tag=b2\r\n"
    "i: 1234@2001:db8::1\r\n"
    "CSeq: 1 REGISTER\r\n"
    "WWW-Authenticate: Digest realm=\"ims.mnc001.mcc001.3gppnetwork.org\",\r\n"
    " nonce=\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\",algorithm=AKAv1-MD5,qop=\"auth,auth-int\"\r\n"
    "Security-Server: ipsec-3gpp;q=0.1;alg=hmac-md5-96;spi-c=1111;spi-s=2222;port-c=5062;port-s=5064,"
    " ipsec-3gpp;q=0.5;alg=hmac-sha-1-96;ealg=null;spi-c=3333;spi-s=4444;port-c=6100;port-s=6200\r\n"
    "Content-Length: 0\r\n\r\n";

}  // namespace

TEST(sip_parse_response)
{
    auto m = SipMessage::parse(kRegister401);
    CHECK(m && !m->isRequest() && m->status() == 401);
    CHECK(m->reason() == "Unauthorized");
    CHECK(m->header("From")->find("tag=a1") != std::string::npos);  // compact form expanded
    CHECK(m->callId() == "1234@2001:db8::1");
    auto cseq = m->cseq();
    CHECK(cseq && cseq->first == 1 && cseq->second == "REGISTER");
    CHECK(m->branch() == "z9hG4bKabc");
    CHECK(m->headers("Security-Server").size() == 2);
    // A folded line joins its header
    auto challenge = DigestChallenge::parse(*m->header("WWW-Authenticate"));
    CHECK(challenge && challenge->isAka() && challenge->qopAuth);
    CHECK(challenge->realm == "ims.mnc001.mcc001.3gppnetwork.org");
    auto aka = AkaVector::fromNonce(challenge->nonce);
    CHECK(aka && aka->rand[0] == 0 && aka->rand[15] == 15 && aka->autn[0] == 16 && aka->autn[15] == 31);
}

TEST(sip_parse_request_and_serialize)
{
    SipMessage r = SipMessage::request("INVITE", "tel:+420123456789");
    r.add("Via", "SIP/2.0/TCP [2001:db8::1]:6000;branch=z9hG4bK1");
    r.add("Via", "SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK2");
    r.add("Supported", "100rel, precondition,timer");
    r.add("Content-Type", "application/sdp");
    r.body = "v=0\r\n";
    std::string text = r.serialize();
    CHECK(text.find("Content-Length: 5\r\n\r\nv=0\r\n") != std::string::npos);

    auto m = SipMessage::parse(text);
    CHECK(m && m->isRequest() && m->method() == "INVITE" && m->requestUri() == "tel:+420123456789");
    CHECK(m->headers("Via").size() == 2 && m->branch() == "z9hG4bK1");
    CHECK(m->hasOption("Supported", "precondition") && !m->hasOption("Require", "precondition"));
    CHECK(m->body == "v=0\r\n");

    auto resp = m->makeResponse(180, "Ringing");
    CHECK(resp.headers("Via").size() == 2 && !resp.has("Content-Type"));
}

TEST(sip_parse_rejects_broken)
{
    CHECK(!SipMessage::parse("SIP/2.0 99 Low\r\n\r\n"));
    CHECK(!SipMessage::parse("invite sip:a SIP/2.0\r\n\r\n"));
    CHECK(!SipMessage::parse("INVITE sip:a SIP/2.0\r\nno colon here\r\n\r\n"));
    CHECK(!SipMessage::parse("INVITE sip:a SIP/2.0\r\nContent-Length: 10\r\n\r\nshort"));
    CHECK(!SipMessage::parse("SIP/2.0 200 OK\r\n"));  // no end of headers
    // A longer body than announced is cut
    auto m = SipMessage::parse("SIP/2.0 200 OK\r\nl: 2\r\n\r\nabcd");
    CHECK(m && m->body == "ab");
}

TEST(sip_frame_length)
{
    std::string a = "SIP/2.0 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    CHECK(sipFrameLength(a) == a.size());
    CHECK(sipFrameLength(a.substr(0, a.size() - 1)) == 0u);
    CHECK(sipFrameLength(a + "SIP/2.0") == a.size());
    CHECK(sipFrameLength("\r\n\r\n") == 4u);  // keep-alive
    CHECK(!sipFrameLength("SIP/2.0 200 OK\r\n\r\n"));  // no Content-Length on a stream
}

TEST(sip_uri_and_name_addr)
{
    auto u = Uri::parse("sip:+420123@[2001:db8::5]:5060;user=phone");
    CHECK(u && u->scheme == "sip" && u->user == "+420123" && u->host == "2001:db8::5" && u->port == 5060);
    CHECK(u->params.get("user") == "phone");
    CHECK(u->str() == "sip:+420123@[2001:db8::5]:5060;user=phone");
    auto t = Uri::parse("tel:+420123;phone-context=ims.example");
    CHECK(t && t->user == "+420123" && t->params.has("phone-context"));
    CHECK(!Uri::parse("http://x") && !Uri::parse("sip:[::1"));

    auto n = NameAddr::parse("\"Alice \\\"A\\\"\" <sip:alice@example.com;lr>;tag=x;+sip.instance=\"<urn:a>\"");
    CHECK(n && n->display == "Alice \"A\"" && n->uri == "sip:alice@example.com;lr");
    CHECK(n->params.get("tag") == "x" && n->params.get("+sip.instance") == "\"<urn:a>\"");
    auto plain = NameAddr::parse("sip:bob@example.com;tag=1");
    CHECK(plain && plain->uri == "sip:bob@example.com" && plain->params.get("tag") == "1");

    auto via = Via::parse("SIP/2.0/udp [2001:db8::1]:6000;branch=z9hG4bKx;rport");
    CHECK(via && via->transport == "UDP" && via->host == "2001:db8::1" && via->port == 6000);
    CHECK(via->params.has("rport") && via->str() == "SIP/2.0/UDP [2001:db8::1]:6000;branch=z9hG4bKx;rport");

    auto list = splitList("<sip:a@b;x=\",\">, \"c,d\" <sip:e>,f");
    CHECK(list.size() == 3 && list[1] == "\"c,d\" <sip:e>");
}

TEST(digest_rfc2617)
{
    DigestChallenge c;
    c.realm = "testrealm@host.com";
    c.nonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
    c.opaque = "5ccc069c403ebaf9f0171e9517f40e41";
    c.qopAuth = true;
    c.algorithm = "MD5";
    DigestCredentials cred;
    cred.username = "Mufasa";
    cred.uri = "/dir/index.html";
    cred.method = "GET";
    std::string pw = "Circle Of Life";
    cred.password.assign(pw.begin(), pw.end());
    std::string a = digestAuthorization(c, cred, "0a4f113b", 1);
    CHECK(a.find("response=\"6629fae49393a05397450978507c4ef1\"") != std::string::npos);
    CHECK(a.find("nc=00000001") != std::string::npos);

    CHECK(base64Encode(Bytes{'a', 'b', 'c', 'd'}) == "YWJjZA==");
    CHECK(base64Decode("YWJjZA==") == (Bytes{'a', 'b', 'c', 'd'}));
    CHECK(!base64Decode("YW=Jj"));
}

TEST(milenage_test_set_1)
{
    // 3GPP TS 35.208 4.3, test set 1
    sim::Milenage m;
    m.k = fromHex<16>("465b5ce8b199b49faa5f0a2ee238a6bc");
    m.opc = sim::Milenage::opcFrom(m.k, fromHex<16>("cdc202d5123e20f62b6d676ac72cb318"));
    CHECK(m.opc == fromHex<16>("cd63cb71954a9f4e48a5994e37a02baf"));
    auto rand = fromHex<16>("23553cbe9637a89d218ae64dae47bf35");
    auto sqn = fromHex<6>("ff9bb4d0b607");
    std::array<uint8_t, 2> amf{0xb9, 0xb9};
    CHECK(m.f1(rand, sqn, amf) == fromHex<8>("4a9ffac354dfafb3"));
    CHECK(m.f1(rand, sqn, amf, true) == fromHex<8>("01cfaf9ec4e871e9"));
    auto o = m.f2345(rand);
    CHECK(o.res == fromHex<8>("a54211d5e3ba50bf"));
    CHECK(o.ck == fromHex<16>("b40ba9a3c58b2a05bbf0d987b21bf8cb"));
    CHECK(o.ik == fromHex<16>("f769bcd751044604127672711c6d3441"));
    CHECK(o.ak == fromHex<6>("aa689c648370"));
    CHECK(o.akStar == fromHex<6>("451e8beca43b"));

    // Both sides: a fresh SQN authenticates, a replayed one asks to resync
    auto v = sim::makeVector(m, rand, sqn, amf);
    sim::Sqn ms{};
    auto r = sim::cardAuthenticate(m, ms, v.rand, v.autn);
    CHECK(r.status == 0 && r.values.size() == 3 && ms == sqn);
    CHECK(r.values[0] == Bytes(v.xres.begin(), v.xres.end()));
    r = sim::cardAuthenticate(m, ms, v.rand, v.autn);
    CHECK(r.status == 4 && r.values.size() == 1);
    CHECK(sim::sqnFromAuts(m, v.rand, r.values[0]) == sqn);
    v.autn[15] ^= 1;
    CHECK(sim::cardAuthenticate(m, ms, v.rand, v.autn).status == 1);
}

TEST(security_agreement)
{
    auto m = SipMessage::parse(kRegister401);
    CHECK(m);
    SecurityAlgorithms algs;
    auto chosen = chooseServer(m->headers("Security-Server"), algs);
    CHECK(chosen && chosen->alg == "hmac-sha-1-96" && chosen->spiC == 3333 && chosen->portS == 6200);
    // Only MD5 offered: the other entry
    algs.integrity = {"hmac-md5-96"};
    chosen = chooseServer(m->headers("Security-Server"), algs);
    CHECK(chosen && chosen->spiS == 2222 && chosen->ealg == "null");

    auto offers = clientOffers(SecurityAlgorithms{}, 10, 11, 6000, 5000);
    CHECK(offers.size() == 2);
    CHECK(offers[0].str() ==
          "ipsec-3gpp;alg=hmac-sha-1-96;ealg=null;prot=esp;mod=trans;spi-c=10;spi-s=11;port-c=6000;port-s=5000");

    Bytes ik(16, 7);
    CHECK(integrityKey("hmac-md5-96", ik) == ik);
    CHECK(integrityKey("hmac-sha-1-96", ik).size() == 20);
    CHECK(encryptionKey("null", ik).empty() && encryptionKey("des-ede3-cbc", ik).size() == 24);
    CHECK(!SecurityMechanism::parse("ipsec-3gpp;spi-c=x"));
}
