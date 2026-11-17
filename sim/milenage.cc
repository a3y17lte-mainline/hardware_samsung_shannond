// SPDX-License-Identifier: GPL-2.0-or-later
#include "sim/milenage.h"

#include <algorithm>

#include <openssl/evp.h>

namespace shannond::sim {

namespace {

Block aes(const Block &key, const Block &in)
{
    Block out{};
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, out.data(), &len, in.data(), 16);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

Block xorBlock(const Block &a, const Block &b)
{
    Block out;
    for (int i = 0; i < 16; i++)
        out[i] = a[i] ^ b[i];
    return out;
}

// Cyclic rotation to the left by r bits (r a multiple of 8 here)
Block rot(const Block &x, int r)
{
    Block out;
    int bytes = r / 8;
    for (int i = 0; i < 16; i++)
        out[i] = x[(i + bytes) % 16];
    return out;
}

Block constant(uint8_t last)
{
    Block c{};
    c[15] = last;
    return c;
}

}  // namespace

Block Milenage::opcFrom(const Block &k, const Block &op)
{
    return xorBlock(aes(k, op), op);
}

std::array<uint8_t, 8> Milenage::f1(const Block &rand, const Sqn &sqn, const std::array<uint8_t, 2> &amf,
                                    bool star) const
{
    Block temp = aes(k, xorBlock(rand, opc));
    Block in1;
    for (int i = 0; i < 6; i++)
        in1[i] = in1[i + 8] = sqn[i];
    in1[6] = in1[14] = amf[0];
    in1[7] = in1[15] = amf[1];
    Block out1 = xorBlock(aes(k, xorBlock(temp, rot(xorBlock(in1, opc), 64))), opc);
    std::array<uint8_t, 8> mac;
    std::copy_n(out1.begin() + (star ? 8 : 0), 8, mac.begin());
    return mac;
}

Milenage::Output Milenage::f2345(const Block &rand) const
{
    Block temp = aes(k, xorBlock(rand, opc));
    Block t = xorBlock(temp, opc);
    Block out2 = xorBlock(aes(k, xorBlock(rot(t, 0), constant(1))), opc);
    Block out3 = xorBlock(aes(k, xorBlock(rot(t, 32), constant(2))), opc);
    Block out4 = xorBlock(aes(k, xorBlock(rot(t, 64), constant(4))), opc);
    Block out5 = xorBlock(aes(k, xorBlock(rot(t, 96), constant(8))), opc);
    Output o;
    std::copy_n(out2.begin() + 8, 8, o.res.begin());
    std::copy_n(out2.begin(), 6, o.ak.begin());
    o.ck = out3;
    o.ik = out4;
    std::copy_n(out5.begin(), 6, o.akStar.begin());
    return o;
}

AuthVector makeVector(const Milenage &m, const Block &rand, const Sqn &sqn, const std::array<uint8_t, 2> &amf)
{
    AuthVector v;
    v.rand = rand;
    auto o = m.f2345(rand);
    auto mac = m.f1(rand, sqn, amf);
    for (int i = 0; i < 6; i++)
        v.autn[i] = sqn[i] ^ o.ak[i];
    v.autn[6] = amf[0];
    v.autn[7] = amf[1];
    std::copy(mac.begin(), mac.end(), v.autn.begin() + 8);
    v.xres = o.res;
    v.ck = o.ck;
    v.ik = o.ik;
    return v;
}

std::optional<Sqn> sqnFromAuts(const Milenage &m, const Block &rand, const Bytes &auts)
{
    if (auts.size() != 14)
        return std::nullopt;
    auto o = m.f2345(rand);
    Sqn sqn;
    for (int i = 0; i < 6; i++)
        sqn[i] = auts[i] ^ o.akStar[i];
    auto mac = m.f1(rand, sqn, {0, 0}, true);
    if (!std::equal(mac.begin(), mac.end(), auts.begin() + 6))
        return std::nullopt;
    return sqn;
}

CardResult cardAuthenticate(const Milenage &m, Sqn &sqnMs, const Block &rand, const Block &autn)
{
    CardResult r;
    auto o = m.f2345(rand);
    Sqn sqn;
    for (int i = 0; i < 6; i++)
        sqn[i] = autn[i] ^ o.ak[i];
    std::array<uint8_t, 2> amf{autn[6], autn[7]};
    auto mac = m.f1(rand, sqn, amf);
    if (!std::equal(mac.begin(), mac.end(), autn.begin() + 8)) {
        r.status = 1;
        return r;
    }
    if (!(sqn > sqnMs)) {
        // AUTS = SQN_MS xor AK* || MAC-S (with AMF 0, TS 33.102 6.3.3)
        auto macS = m.f1(rand, sqnMs, {0, 0}, true);
        Bytes auts;
        for (int i = 0; i < 6; i++)
            auts.push_back(sqnMs[i] ^ o.akStar[i]);
        auts.insert(auts.end(), macS.begin(), macS.end());
        r.status = 4;
        r.values.push_back(auts);
        return r;
    }
    sqnMs = sqn;
    r.values.emplace_back(o.res.begin(), o.res.end());
    r.values.emplace_back(o.ck.begin(), o.ck.end());
    r.values.emplace_back(o.ik.begin(), o.ik.end());
    return r;
}

}  // namespace shannond::sim
