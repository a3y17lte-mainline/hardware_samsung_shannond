// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio HAL's string and SIM encodings (frontends/aidl/Codec), which
// need no Android headers.
#include "frontends/aidl/Codec.h"
#include "tests/test.h"

using namespace shannond;
using namespace shannond::hal;

TEST(codec_hex)
{
    CHECK(toHex(Bytes{0x00, 0x9f, 0xa0, 0xff}) == "009FA0FF");
    CHECK(fromHex("009fA0Ff") == (Bytes{0x00, 0x9f, 0xa0, 0xff}));
    CHECK(fromHex("") == Bytes{});
    CHECK(!fromHex("abc"));
    CHECK(!fromHex("zz"));
}

TEST(codec_base64)
{
    // RFC 4648 section 10
    const char *vectors[][2] = {{"", ""},         {"f", "Zg=="},     {"fo", "Zm8="},
                                {"foo", "Zm9v"},  {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                {"foobar", "Zm9vYmFy"}};
    for (auto &v : vectors) {
        const std::string plain = v[0];
        const Bytes bytes(plain.begin(), plain.end());
        CHECK(toBase64(bytes) == v[1]);
        CHECK(fromBase64(v[1]) == bytes);
    }
    CHECK(!fromBase64("Zm9v!"));
}

TEST(codec_bcd_address)
{
    // International, +420 123 456 (odd digit count: F filler)
    CHECK(bcdAddressToString(Bytes{0x91, 0x24, 0x10, 0x32, 0x54, 0xf6}) == "+420123456");
    CHECK(bcdAddressToString(Bytes{0x81, 0x21, 0x43}) == "1234");
    CHECK(bcdAddressToString(Bytes{}).empty());
}

TEST(codec_fcp_transparent)
{
    // EF AD: transparent, 4 bytes
    const Bytes fcp{0x62, 0x14, 0x82, 0x02, 0x41, 0x21, 0x83, 0x02, 0x6f, 0xad, 0x8a, 0x01,
                    0x05, 0x8b, 0x03, 0x6f, 0x06, 0x02, 0x80, 0x02, 0x00, 0x04};
    auto sim = fcpToSimResponse(fcp, 0x6fad);
    CHECK(sim && sim->size() == 15);
    CHECK((*sim)[2] == 0x00 && (*sim)[3] == 0x04);
    CHECK((*sim)[4] == 0x6f && (*sim)[5] == 0xad);
    CHECK((*sim)[6] == 0x04);   // EF
    CHECK((*sim)[13] == 0x00);  // transparent
}

TEST(codec_fcp_linear_fixed)
{
    // EF MSISDN: linear fixed, two records of 28 bytes, no file size tag
    const Bytes fcp{0x62, 0x0b, 0x82, 0x05, 0x42, 0x21, 0x00, 0x1c, 0x02, 0x83, 0x02, 0x6f, 0x40};
    auto sim = fcpToSimResponse(fcp, 0x6f40);
    CHECK(sim);
    CHECK((*sim)[2] == 0x00 && (*sim)[3] == 0x38);  // 2 * 28
    CHECK((*sim)[13] == 0x01);
    CHECK((*sim)[14] == 0x1c);
    CHECK(!fcpToSimResponse(Bytes{0x6f, 0x00}, 0x6f40));
}

TEST(codec_path_and_cla)
{
    CHECK(parsePath("3F007FFF") == (std::vector<uint16_t>{0x3f00, 0x7fff}));
    CHECK(parsePath("").empty());
    CHECK(channelCla(0x00, 0) == 0x00);
    CHECK(channelCla(0x00, 1) == 0x01);
    CHECK(channelCla(0x80, 3) == 0x83);
    CHECK(channelCla(0x00, 4) == 0x40);
    CHECK(channelCla(0x80, 19) == 0xcf);
}

TEST(codec_aka)
{
    Bytes challenge{16};
    for (int i = 0; i < 16; i++)
        challenge.push_back(static_cast<uint8_t>(i));
    challenge.push_back(16);
    for (int i = 0; i < 16; i++)
        challenge.push_back(static_cast<uint8_t>(0xa0 + i));
    auto c = parseAkaChallenge(challenge);
    CHECK(c && c->rand.size() == 16 && c->autn.size() == 16);
    CHECK(c->rand[15] == 15 && c->autn[0] == 0xa0);
    CHECK(!parseAkaChallenge(Bytes{16, 1, 2}));
    CHECK(!parseAkaChallenge(Bytes{}));

    auto ok = akaAnswer(0, {Bytes{1, 2}, Bytes{3}, Bytes{4, 5, 6}});
    CHECK(ok.sw1 == 0x90 && ok.sw2 == 0x00);
    CHECK(ok.data == (Bytes{0xdb, 2, 1, 2, 1, 3, 3, 4, 5, 6}));

    auto sync = akaAnswer(4, {Bytes{9, 9}});
    CHECK(sync.sw1 == 0x90);
    CHECK(sync.data == (Bytes{0xdc, 2, 9, 9}));

    auto bad = akaAnswer(1, {});
    CHECK(bad.sw1 == 0x98 && bad.sw2 == 0x62 && bad.data.empty());
}
