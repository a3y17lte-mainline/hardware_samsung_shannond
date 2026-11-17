// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where a call's voice runs. On Shannon modems RTP, the codec and the
// jitter buffer run in the modem (VoiceEngine, IPC group 0x16); an AP media
// engine (for Wi-Fi calling) would be a second implementation.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/ims/sdp.h"

namespace shannond::ims {

// One call's audio, from the negotiated SDP
struct MediaSetup {
    std::string localAddress;
    uint16_t localPort = 0;
    std::string remoteAddress;
    uint16_t remotePort = 0;
    uint16_t remoteRtcpPort = 0;  // 0: remote RTP + 1
    // As the other side receives them (its SDP's payload types); ours, what
    // we receive with, differ when we made the offer (0: the same)
    Codec codec;
    uint8_t codecRxPayloadType = 0;
    std::optional<Codec> dtmf;
    uint8_t dtmfRxPayloadType = 0;
    unsigned ptime = 20;
    unsigned maxptime = 240;
    bool active = true;  // sending and receiving (else held)
};

// Events from the media side
struct MediaEvents {
    // A dedicated bearer came or went (QCI 1 carries voice)
    std::function<void(int bearerId, int qci, bool added)> bearer;
    // No RTP (or RTCP) for the configured time: the call is dead
    std::function<void(int channel)> timeout;
};

class MediaEngine {
public:
    using Done = std::function<void(bool ok)>;

    virtual ~MediaEngine() = default;

    // Once per registration: the engine on and the PDN it uses
    virtual void prepare(const std::string &localAddress, const std::string &pdnName, Done done) = 0;
    // A free local RTP port on the PDN for a new call
    virtual uint16_t allocatePort() = 0;
    virtual void releasePort(uint16_t port) = 0;
    // A call's stream: open (and start), change, stop
    virtual void open(const MediaSetup &setup, std::function<void(std::optional<int> channel)> done) = 0;
    virtual void update(int channel, const MediaSetup &setup, Done done) = 0;
    virtual void close(int channel, Done done) = 0;
    // A DTMF digit (0-9, *, #, A-D) as an RTP event (RFC 4733)
    virtual void dtmf(int channel, char digit, Done done) = 0;
    void onEvents(MediaEvents events) { events_ = std::move(events); }

protected:
    MediaEvents events_;
};

}  // namespace shannond::ims
