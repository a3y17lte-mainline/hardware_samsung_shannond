// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/secagree.h"

#include <charconv>
#include <cstdlib>

#include "core/ims/sip.h"

namespace shannond::ims {

namespace {

template <class T>
bool parseNumber(std::string_view s, T &out)
{
    s = trim(s);
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

double preference(const std::string &q)
{
    if (q.empty())
        return 0.001;  // lower than any explicit preference
    return std::strtod(q.c_str(), nullptr);
}

}  // namespace

std::string SecurityMechanism::str() const
{
    std::string out = mechanism;
    if (!q.empty())
        out += ";q=" + q;
    out += ";alg=" + alg;
    if (!ealg.empty())
        out += ";ealg=" + ealg;
    out += ";prot=" + prot + ";mod=" + mod;
    out += ";spi-c=" + std::to_string(spiC) + ";spi-s=" + std::to_string(spiS);
    out += ";port-c=" + std::to_string(portC) + ";port-s=" + std::to_string(portS);
    return out;
}

std::optional<SecurityMechanism> SecurityMechanism::parse(std::string_view value)
{
    value = trim(value);
    size_t semi = value.find(';');
    SecurityMechanism m;
    m.mechanism = std::string(trim(value.substr(0, semi)));
    m.ealg.clear();
    if (m.mechanism.empty())
        return std::nullopt;
    if (semi == std::string_view::npos)
        return m;
    Params p = Params::parse(value.substr(semi + 1));
    m.q = p.get("q").value_or("");
    m.alg = p.get("alg").value_or("");
    m.ealg = p.get("ealg").value_or("null");  // absent: no encryption (33.203 7.1)
    m.prot = p.get("prot").value_or("esp");
    m.mod = p.get("mod").value_or("trans");
    auto number = [&](const char *name, auto &out) {
        auto v = p.get(name);
        return !v || parseNumber(*v, out);
    };
    if (!number("spi-c", m.spiC) || !number("spi-s", m.spiS) || !number("port-c", m.portC) ||
        !number("port-s", m.portS))
        return std::nullopt;
    return m;
}

std::vector<SecurityMechanism> clientOffers(const SecurityAlgorithms &algorithms, uint32_t spiC, uint32_t spiS,
                                            uint16_t portC, uint16_t portS)
{
    std::vector<SecurityMechanism> out;
    for (const auto &ealg : algorithms.encryption)
        for (const auto &alg : algorithms.integrity) {
            SecurityMechanism m;
            m.alg = alg;
            m.ealg = ealg;
            m.spiC = spiC;
            m.spiS = spiS;
            m.portC = portC;
            m.portS = portS;
            out.push_back(m);
        }
    return out;
}

std::optional<SecurityMechanism> chooseServer(const std::vector<std::string> &securityServer,
                                              const SecurityAlgorithms &algorithms)
{
    std::optional<SecurityMechanism> best;
    for (const auto &value : securityServer) {
        auto m = SecurityMechanism::parse(value);
        if (!m || !iequals(m->mechanism, "ipsec-3gpp") || !iequals(m->prot, "esp") || !iequals(m->mod, "trans"))
            continue;
        bool alg = false, ealg = false;
        for (const auto &a : algorithms.integrity)
            alg |= iequals(a, m->alg);
        for (const auto &e : algorithms.encryption)
            ealg |= iequals(e, m->ealg);
        if (!alg || !ealg || !m->spiC || !m->spiS || !m->portC || !m->portS)
            continue;
        if (!best || preference(m->q) > preference(best->q))
            best = m;
    }
    return best;
}

bool knownIntegrity(std::string_view alg)
{
    return iequals(alg, "hmac-md5-96") || iequals(alg, "hmac-sha-1-96");
}

bool knownEncryption(std::string_view ealg)
{
    return iequals(ealg, "null") || iequals(ealg, "aes-cbc") || iequals(ealg, "des-ede3-cbc");
}

Bytes integrityKey(const std::string &alg, const Bytes &ik)
{
    if (ik.size() != 16)
        return {};
    Bytes key = ik;
    if (iequals(alg, "hmac-sha-1-96"))
        key.insert(key.end(), 4, 0);  // IK_ESP = IK || 32 zero bits
    return key;
}

Bytes encryptionKey(const std::string &ealg, const Bytes &ck)
{
    if (iequals(ealg, "aes-cbc"))
        return ck.size() == 16 ? ck : Bytes{};
    if (iequals(ealg, "des-ede3-cbc")) {
        // CK_ESP = CK1 || CK2 || CK1 (33.203 6.1)
        if (ck.size() != 16)
            return {};
        Bytes key = ck;
        key.insert(key.end(), ck.begin(), ck.begin() + 8);
        return key;
    }
    return {};
}

}  // namespace shannond::ims
