// SPDX-License-Identifier: GPL-2.0-or-later
//
// SDP (RFC 4566) for a VoLTE voice call: one audio stream with AMR-WB / AMR
// and telephone-event, the offer/answer rules of RFC 3264, and the QoS
// preconditions of RFC 3312 as 3GPP TS 24.229 and GSMA IR.92 use them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shannond::ims {

enum class Direction { SendRecv, SendOnly, RecvOnly, Inactive };
const char *directionName(Direction d);
// The same seen from the other side (sendonly <-> recvonly)
Direction reverse(Direction d);

struct Codec {
    uint8_t payloadType = 0;
    std::string name;  // "AMR-WB", "AMR", "telephone-event"
    unsigned rate = 0;  // clock rate
    std::string fmtp;   // parameters, as written
    bool octetAlign() const;
    // The highest mode of mode-set, if given
    std::optional<int> highestMode() const;
};

// The QoS precondition state (RFC 3312 5): per side, whether resources
// are reserved in each direction
struct Preconditions {
    bool present = false;
    // curr: what is reserved (local is ours as the author of the SDP)
    bool localCurrent = false;   // sendrecv reserved
    bool remoteCurrent = false;
    // des: strength wanted, "mandatory", "optional", "none"
    std::string localDesired = "mandatory";
    std::string remoteDesired = "mandatory";
    bool confirmRemote = false;  // conf:qos remote sendrecv asked
};

struct Sdp {
    std::string origin = "-";  // o= user
    uint64_t sessionId = 0;
    uint64_t version = 0;
    std::string address;  // c=, numeric
    uint16_t port = 0;    // 0: the stream is rejected
    std::vector<Codec> codecs;  // in order of preference
    Direction direction = Direction::SendRecv;
    unsigned ptime = 20;
    unsigned maxptime = 240;
    unsigned bandwidthAs = 0, bandwidthRs = 0, bandwidthRr = 0;
    std::optional<uint16_t> rtcpPort;
    Preconditions qos;

    std::string str() const;
    static std::optional<Sdp> parse(const std::string &text);

    const Codec *voice() const;    // the first AMR-WB or AMR codec
    const Codec *dtmf() const;     // telephone-event, at the voice codec's rate if there
};

// What we can do (from the profile)
struct MediaCapabilities {
    // Payload types: bandwidth-efficient and octet-aligned for each codec
    uint8_t amrWbBe = 116, amrWbOa = 107, amrBe = 118, amrOa = 96;
    uint8_t dtmfWb = 111, dtmfNb = 110;
    unsigned ptime = 20, maxptime = 240;
    unsigned bandwidthAs = 38, bandwidthRs = 375, bandwidthRr = 1125;
    unsigned maxRed = 220;
    bool preconditions = true;
};

// Our offer
Sdp makeOffer(const MediaCapabilities &caps, const std::string &address, uint16_t port, uint64_t sessionId);
// Our answer to an offer: the first of its voice codecs we support (its
// payload type kept), its telephone-event at that rate; nullopt if none
std::optional<Sdp> makeAnswer(const MediaCapabilities &caps, const Sdp &offer, const std::string &address,
                              uint16_t port, uint64_t sessionId);

// The precondition lines of an SDP we send: ours is the "local" side
void setPreconditions(Sdp &sdp, bool localReserved, bool remoteReserved, bool confirm = false);
// Whether both sides' resources are reserved, as an SDP from the other
// side says (its "local" is the remote)
bool preconditionsMet(const Sdp &theirs, bool ourResources);

}  // namespace shannond::ims
