// SPDX-License-Identifier: GPL-2.0-or-later
//
// The modem's voice engine (IPC group 0x16), driven as the stock IMS stack
// drives it (libsec-ims: CPVEInterface, CpAudioEngineClient, OemIPCHandler;
// work/notes/ril/voice-engine.md):
//   engine on (0x1609) and the PDN (0x1606, "lte_" + PDN name), once;
//   per call: the PDN again, a channel (0x1601 create, then change with
//   the addresses), the codec (0x1602), the options (0x1605), the DTMF
//   codec (0x1604), start (0x1603); hold/resume = 0x1603 modify; end =
//   0x1603 stop and 0x1601 delete.
#pragma once

#include <map>
#include <memory>
#include <set>

#include "core/ims/media.h"
#include "core/modem.h"

namespace shannond::ims {

struct VoiceEngineConfig {
    uint16_t portBase = 1234;     // the profile's audio_port_start
    unsigned rtpTimeout = 20;     // seconds, the profile's rtp_timeout
    unsigned rtcpTimeout = 20;
    bool rtcpOnCall = true;       // enable_rtcp_on_active_call (O2)
    unsigned rtcpRs = 375, rtcpRr = 1125;
};

class VoiceEngine : public MediaEngine {
public:
    VoiceEngine(Modem &modem, VoiceEngineConfig config = {});
    ~VoiceEngine();
    VoiceEngine(const VoiceEngine &) = delete;
    VoiceEngine &operator=(const VoiceEngine &) = delete;

    // The profile's settings, before the first call
    void setConfig(VoiceEngineConfig config) { config_ = config; }

    void prepare(const std::string &localAddress, const std::string &pdnName, Done done) override;
    uint16_t allocatePort() override;
    void releasePort(uint16_t port) override;
    void open(const MediaSetup &setup, std::function<void(std::optional<int> channel)> done) override;
    void update(int channel, const MediaSetup &setup, Done done) override;
    void close(int channel, Done done) override;
    void dtmf(int channel, char digit, Done done) override;

private:
    struct Channel {
        MediaSetup setup;
        bool started = false;
    };

    void send(uint16_t command, uint8_t type, Bytes data, Done done);
    Bytes pdnFrame() const;
    Bytes channelFrame(int channel, uint8_t operation, const MediaSetup *setup) const;
    Bytes codecFrame(int channel, const MediaSetup &setup) const;
    Bytes optionFrame(int channel) const;
    Bytes dtmfFrame(int channel, const MediaSetup &setup) const;
    Bytes mediaFrame(int channel, uint8_t operation, const MediaSetup &setup) const;
    void configure(int channel, const MediaSetup &setup, Done done);
    void media(const Message &m);

    Modem &modem_;
    VoiceEngineConfig config_;
    std::string localAddress_;
    std::string pdnName_;
    bool engineOn_ = false;
    std::map<int, Channel> channels_;
    std::set<uint16_t> ports_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace shannond::ims
