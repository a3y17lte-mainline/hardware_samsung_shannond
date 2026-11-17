// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/sdp.h"

#include <charconv>
#include <sstream>

#include "core/ims/sip.h"

namespace shannond::ims {

namespace {

std::vector<std::string_view> words(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t start = s.find_first_not_of(' ', pos);
        if (start == std::string_view::npos)
            break;
        size_t end = s.find(' ', start);
        out.push_back(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        pos = end == std::string_view::npos ? s.size() : end;
    }
    return out;
}

template <class T>
bool number(std::string_view s, T &out)
{
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

std::string addressLine(const std::string &address)
{
    return std::string(address.find(':') != std::string::npos ? "IN IP6 " : "IN IP4 ") + address;
}

Codec amr(uint8_t pt, bool wideband, bool octetAlign, unsigned maxRed)
{
    Codec c;
    c.payloadType = pt;
    c.name = wideband ? "AMR-WB" : "AMR";
    c.rate = wideband ? 16000 : 8000;
    c.fmtp = std::string(octetAlign ? "octet-align=1; " : "") + "mode-change-capability=2; max-red=" +
             std::to_string(maxRed);
    return c;
}

Codec telephoneEvent(uint8_t pt, unsigned rate)
{
    Codec c;
    c.payloadType = pt;
    c.name = "telephone-event";
    c.rate = rate;
    c.fmtp = "0-15";
    return c;
}

}  // namespace

const char *directionName(Direction d)
{
    switch (d) {
    case Direction::SendRecv: return "sendrecv";
    case Direction::SendOnly: return "sendonly";
    case Direction::RecvOnly: return "recvonly";
    case Direction::Inactive: return "inactive";
    }
    return "sendrecv";
}

Direction reverse(Direction d)
{
    if (d == Direction::SendOnly)
        return Direction::RecvOnly;
    if (d == Direction::RecvOnly)
        return Direction::SendOnly;
    return d;
}

bool Codec::octetAlign() const
{
    return Params::parse(fmtp).get("octet-align") == "1";
}

std::optional<int> Codec::highestMode() const
{
    // As the stock stack reads mode-set: its last mode
    auto set = Params::parse(fmtp).get("mode-set");
    if (!set || set->empty())
        return std::nullopt;
    char last = set->back();
    if (last < '0' || last > '9')
        return std::nullopt;
    return last - '0';
}

std::string Sdp::str() const
{
    std::ostringstream o;
    o << "v=0\r\n";
    o << "o=" << origin << " " << sessionId << " " << version << " " << addressLine(address) << "\r\n";
    o << "s=-\r\n";
    o << "c=" << addressLine(address) << "\r\n";
    o << "t=0 0\r\n";
    for (const std::string &m : rejectedBefore)
        o << m << "\r\n";
    o << "m=audio " << port << " RTP/AVP";
    for (const Codec &c : codecs)
        o << " " << unsigned(c.payloadType);
    o << "\r\n";
    // On the stream (TS 26.114 6.2.5): the bearer's bit rates come from here
    if (bandwidthAs)
        o << "b=AS:" << bandwidthAs << "\r\n";
    if (bandwidthRs)
        o << "b=RS:" << bandwidthRs << "\r\n";
    if (bandwidthRr)
        o << "b=RR:" << bandwidthRr << "\r\n";
    for (const Codec &c : codecs) {
        o << "a=rtpmap:" << unsigned(c.payloadType) << " " << c.name << "/" << c.rate
          << (c.name == "telephone-event" ? "" : "/1") << "\r\n";
        if (!c.fmtp.empty())
            o << "a=fmtp:" << unsigned(c.payloadType) << " " << c.fmtp << "\r\n";
    }
    if (ptime)
        o << "a=ptime:" << ptime << "\r\n";
    if (maxptime)
        o << "a=maxptime:" << maxptime << "\r\n";
    if (rtcpPort)
        o << "a=rtcp:" << *rtcpPort << "\r\n";
    if (qos.present) {
        o << "a=curr:qos local " << (qos.localCurrent ? "sendrecv" : "none") << "\r\n";
        o << "a=curr:qos remote " << (qos.remoteCurrent ? "sendrecv" : "none") << "\r\n";
        o << "a=des:qos " << qos.localDesired << " local sendrecv\r\n";
        o << "a=des:qos " << qos.remoteDesired << " remote sendrecv\r\n";
        if (qos.confirmRemote)
            o << "a=conf:qos remote sendrecv\r\n";
    }
    o << "a=" << directionName(direction) << "\r\n";
    for (const std::string &m : rejectedAfter)
        o << m << "\r\n";
    return o.str();
}

std::optional<Sdp> Sdp::parse(const std::string &text)
{
    Sdp sdp;
    bool audio = false, inMedia = false, skipping = false, sawVersion = false;
    std::vector<uint8_t> order;
    std::string sessionAddress;
    sdp.ptime = 0;
    sdp.maxptime = 0;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        if (raw.size() < 2 || raw[1] != '=')
            continue;
        const char type = raw[0];
        std::string_view v = std::string_view(raw).substr(2);
        if (skipping && type != 'm')
            continue;  // a stream we do not use
        if (type == 'v') {
            sawVersion = true;
        } else if (type == 'o') {
            auto w = words(v);
            if (w.size() >= 6) {
                sdp.origin = std::string(w[0]);
                number(w[1], sdp.sessionId);
                number(w[2], sdp.version);
            }
        } else if (type == 'c') {
            auto w = words(v);
            if (w.size() >= 3) {
                if (inMedia)
                    sdp.address = std::string(w[2]);
                else
                    sessionAddress = std::string(w[2]);
            }
        } else if (type == 'm') {
            auto w = words(v);
            // Only the first audio stream is ours; the others are kept as
            // rejected lines
            skipping = audio || !(w.size() >= 3 && w[0] == "audio");
            inMedia = !skipping;
            if (skipping && w.size() >= 3) {
                std::string line = "m=" + std::string(w[0]) + " 0";
                for (size_t i = 2; i < w.size(); i++)
                    line += " " + std::string(w[i]);
                (audio ? sdp.rejectedAfter : sdp.rejectedBefore).push_back(line);
            }
            if (!skipping) {
                audio = true;
                number(w[1], sdp.port);
                for (size_t i = 3; i < w.size(); i++) {
                    unsigned pt;
                    if (number(w[i], pt) && pt < 128)
                        order.push_back(static_cast<uint8_t>(pt));
                }
            }
        } else if (type == 'b' && (inMedia || !audio)) {
            unsigned n = 0;
            if (v.starts_with("AS:") && number(v.substr(3), n))
                sdp.bandwidthAs = n;
            else if (v.starts_with("RS:") && number(v.substr(3), n))
                sdp.bandwidthRs = n;
            else if (v.starts_with("RR:") && number(v.substr(3), n))
                sdp.bandwidthRr = n;
        } else if (type == 'a' && (inMedia || !audio)) {
            if (v.starts_with("rtpmap:")) {
                auto w = words(v.substr(7));
                unsigned pt;
                if (w.size() >= 2 && number(w[0], pt) && pt < 128) {
                    Codec c;
                    c.payloadType = static_cast<uint8_t>(pt);
                    std::string_view enc = w[1];
                    size_t slash = enc.find('/');
                    c.name = std::string(enc.substr(0, slash));
                    if (slash != std::string_view::npos) {
                        std::string_view rate = enc.substr(slash + 1);
                        number(rate.substr(0, rate.find('/')), c.rate);
                    }
                    sdp.codecs.push_back(c);
                }
            } else if (v.starts_with("fmtp:")) {
                std::string_view rest = v.substr(5);
                size_t space = rest.find(' ');
                unsigned pt;
                if (space != std::string_view::npos && number(rest.substr(0, space), pt))
                    for (Codec &c : sdp.codecs)
                        if (c.payloadType == pt)
                            c.fmtp = std::string(trim(rest.substr(space + 1)));
            } else if (v.starts_with("ptime:")) {
                number(v.substr(6), sdp.ptime);
            } else if (v.starts_with("maxptime:")) {
                number(v.substr(9), sdp.maxptime);
            } else if (v.starts_with("rtcp:")) {
                uint16_t port;
                auto w = words(v.substr(5));
                if (!w.empty() && number(w[0], port))
                    sdp.rtcpPort = port;
            } else if (v == "sendrecv") {
                sdp.direction = Direction::SendRecv;
            } else if (v == "sendonly") {
                sdp.direction = Direction::SendOnly;
            } else if (v == "recvonly") {
                sdp.direction = Direction::RecvOnly;
            } else if (v == "inactive") {
                sdp.direction = Direction::Inactive;
            } else if (v.starts_with("curr:qos ")) {
                auto w = words(v.substr(9));
                if (w.size() >= 2) {
                    sdp.qos.present = true;
                    const bool reserved = w[1] == "sendrecv";
                    if (w[0] == "local")
                        sdp.qos.localCurrent = reserved;
                    else if (w[0] == "remote")
                        sdp.qos.remoteCurrent = reserved;
                }
            } else if (v.starts_with("des:qos ")) {
                auto w = words(v.substr(8));
                if (w.size() >= 3) {
                    sdp.qos.present = true;
                    if (w[1] == "local")
                        sdp.qos.localDesired = std::string(w[0]);
                    else if (w[1] == "remote")
                        sdp.qos.remoteDesired = std::string(w[0]);
                }
            } else if (v.starts_with("conf:qos remote")) {
                sdp.qos.confirmRemote = true;
            }
        }
    }
    if (!sawVersion || !audio)
        return std::nullopt;
    if (sdp.address.empty())
        sdp.address = sessionAddress;
    // Codecs in the m= line's order; static types without rtpmap are left out
    std::vector<Codec> ordered;
    for (uint8_t pt : order)
        for (const Codec &c : sdp.codecs)
            if (c.payloadType == pt)
                ordered.push_back(c);
    sdp.codecs = std::move(ordered);
    if (!sdp.ptime)
        sdp.ptime = 20;
    return sdp;
}

const Codec *Sdp::voice() const
{
    for (const Codec &c : codecs)
        if (iequals(c.name, "AMR-WB") || iequals(c.name, "AMR"))
            return &c;
    return nullptr;
}

const Codec *Sdp::dtmf() const
{
    const Codec *v = voice();
    const Codec *any = nullptr;
    for (const Codec &c : codecs)
        if (iequals(c.name, "telephone-event")) {
            if (v && c.rate == v->rate)
                return &c;
            if (!any)
                any = &c;
        }
    return any;
}

Sdp makeOffer(const MediaCapabilities &caps, const std::string &address, uint16_t port, uint64_t sessionId)
{
    Sdp s;
    s.sessionId = sessionId;
    s.version = sessionId;
    s.address = address;
    s.port = port;
    // The profile's order: AMR-WB bandwidth-efficient, octet-aligned, then AMR
    s.codecs = {amr(caps.amrWbBe, true, false, caps.maxRed), amr(caps.amrWbOa, true, true, caps.maxRed),
                amr(caps.amrBe, false, false, caps.maxRed), amr(caps.amrOa, false, true, caps.maxRed),
                telephoneEvent(caps.dtmfWb, 16000), telephoneEvent(caps.dtmfNb, 8000)};
    s.ptime = caps.ptime;
    s.maxptime = caps.maxptime;
    s.bandwidthAs = caps.bandwidthAs;
    s.bandwidthRs = caps.bandwidthRs;
    s.bandwidthRr = caps.bandwidthRr;
    if (caps.preconditions) {
        // IR.92 2.4.1: our resources mandatory, the far end's optional
        s.qos.present = true;
        s.qos.localDesired = "mandatory";
        s.qos.remoteDesired = "optional";
    }
    return s;
}

std::optional<Sdp> makeAnswer(const MediaCapabilities &caps, const Sdp &offer, const std::string &address,
                              uint16_t port, uint64_t sessionId)
{
    const Codec *voice = offer.voice();
    if (!voice || offer.port == 0)
        return std::nullopt;
    Sdp s;
    s.sessionId = sessionId;
    s.version = sessionId;
    s.address = address;
    s.port = port;
    s.codecs.push_back(*voice);
    // The answer echoes the offer's parameters for the chosen codec
    if (const Codec *dtmf = offer.dtmf())
        s.codecs.push_back(*dtmf);
    s.ptime = offer.ptime ? offer.ptime : caps.ptime;
    s.maxptime = caps.maxptime;
    s.bandwidthAs = caps.bandwidthAs;
    s.bandwidthRs = caps.bandwidthRs;
    s.bandwidthRr = caps.bandwidthRr;
    s.direction = reverse(offer.direction);
    s.rejectedBefore = offer.rejectedBefore;
    s.rejectedAfter = offer.rejectedAfter;
    if (offer.qos.present && caps.preconditions) {
        s.qos.present = true;
        s.qos.localDesired = "mandatory";
        s.qos.remoteDesired = "mandatory";
        s.qos.remoteCurrent = offer.qos.localCurrent;
    }
    return s;
}

void setPreconditions(Sdp &sdp, bool localReserved, bool remoteReserved, bool confirm)
{
    if (!sdp.qos.present)
        return;
    sdp.qos.localCurrent = localReserved;
    sdp.qos.remoteCurrent = remoteReserved;
    sdp.qos.confirmRemote = confirm;
}

bool preconditionsMet(const Sdp &theirs, bool ourResources)
{
    if (!theirs.qos.present)
        return true;
    // Their "local" is their side; a side whose desire is not mandatory
    // does not hold the call
    const bool theirsOk = theirs.qos.localCurrent || theirs.qos.localDesired != "mandatory";
    return theirsOk && ourResources;
}

}  // namespace shannond::ims
