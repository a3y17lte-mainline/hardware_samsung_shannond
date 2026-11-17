// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/voice_engine.h"

#include <arpa/inet.h>

#include <memory>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond::ims {

namespace {

// 0x1603 operations and states, as the stock stack sends them
constexpr uint8_t kMediaStart = 1, kMediaStop = 2, kMediaModify = 3;
constexpr uint8_t kMediaSendRecv = 3, kMediaHeld = 0;
// 0x1601 stream type of a voice channel
constexpr uint8_t kStreamAudio = 1;
// 0x1602: every setting of the frame is valid (stock's mask for a voice codec)
constexpr uint32_t kCodecAllSettings = 0x21ff;
// 0x1602 packetization (stock: octet-aligned 1, bandwidth-efficient 2)
constexpr uint8_t kOctetAligned = 1, kBandwidthEfficient = 2;
// 0x1604 DTMF execute: one tone of the default length
constexpr uint8_t kDtmfSingle = 0, kDtmfPlay = 1;
constexpr unsigned kChannels = 8;
// The modem's data profile flag of 0x1606 (stock passes 1)
constexpr uint8_t kPdnFlag = 1;

struct Address {
    uint8_t family = 0;
    uint8_t bytes[16] = {};
};

bool parseAddress(const std::string &text, Address &out)
{
    if (inet_pton(AF_INET6, text.c_str(), out.bytes) == 1) {
        out.family = IPC_IMS_ADDRESS_IPV6;
        return true;
    }
    if (inet_pton(AF_INET, text.c_str(), out.bytes) == 1) {
        out.family = IPC_IMS_ADDRESS_IPV4;
        return true;
    }
    return false;
}

template <class T>
Bytes frame(const T &data)
{
    return toBytes(data);
}

// The codec's mode (bit rate) of the frame: the highest of mode-set, else
// the codec's highest
struct CodecInfo {
    uint8_t id = 0;
    unsigned mode = 0;
    uint32_t bitrate = 0;
};

std::optional<CodecInfo> codecInfo(const Codec &c)
{
    CodecInfo info;
    if (c.name == "AMR-WB") {
        info.id = IPC_IMS_CODEC_AMR_WB;
        info.mode = c.highestMode().value_or(8);
        if (info.mode > 8)
            return std::nullopt;
        info.bitrate = IPC_IMS_BITRATE_AMR_WB_6600 << info.mode;
    } else if (c.name == "AMR") {
        info.id = IPC_IMS_CODEC_AMR;
        info.mode = c.highestMode().value_or(7);
        if (info.mode > 7)
            return std::nullopt;
        info.bitrate = IPC_IMS_BITRATE_AMR_4750 << info.mode;
    } else {
        return std::nullopt;
    }
    return info;
}

// RFC 4733 event code of a DTMF digit
int dtmfCode(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    switch (digit) {
    case '*': return 10;
    case '#': return 11;
    case 'A': case 'a': return 12;
    case 'B': case 'b': return 13;
    case 'C': case 'c': return 14;
    case 'D': case 'd': return 15;
    }
    return -1;
}

bool sameAddresses(const MediaSetup &a, const MediaSetup &b)
{
    return a.localAddress == b.localAddress && a.localPort == b.localPort &&
           a.remoteAddress == b.remoteAddress && a.remotePort == b.remotePort &&
           a.remoteRtcpPort == b.remoteRtcpPort;
}

bool sameCodec(const MediaSetup &a, const MediaSetup &b)
{
    return a.codec.name == b.codec.name && a.codec.payloadType == b.codec.payloadType &&
           a.codec.fmtp == b.codec.fmtp && a.codecRxPayloadType == b.codecRxPayloadType &&
           a.ptime == b.ptime && a.maxptime == b.maxptime;
}

bool sameDtmf(const MediaSetup &a, const MediaSetup &b)
{
    if (a.dtmf.has_value() != b.dtmf.has_value())
        return false;
    return !a.dtmf || (a.dtmf->payloadType == b.dtmf->payloadType && a.dtmf->rate == b.dtmf->rate &&
                       a.dtmfRxPayloadType == b.dtmfRxPayloadType);
}

}  // namespace

VoiceEngine::VoiceEngine(Modem &modem, VoiceEngineConfig config) : modem_(modem), config_(config)
{
    auto alive = alive_;
    modem_.onMessage(IPC_IMS_DEDICATED_BEARER, [this, alive](const Message &m) {
        if (!*alive)
            return;
        auto h = m.as<ipc_ims_dedicated_bearer_header>();
        if (!h)
            return;
        const bool added = h->event == IPC_IMS_BEARER_ADDED;
        if (!added && h->event != IPC_IMS_BEARER_RELEASED)
            return;
        modem_.log("ims: dedicated bearer %u qci %u %s", h->bearer_id, h->qci, added ? "added" : "released");
        if (events_.bearer)
            events_.bearer(h->bearer_id, h->qci, added);
    });
    modem_.onMessage(IPC_IMS_OPTION, [this, alive](const Message &m) {
        if (*alive)
            media(m);
    });
}

VoiceEngine::~VoiceEngine()
{
    *alive_ = false;
}

void VoiceEngine::media(const Message &m)
{
    auto h = m.as<ipc_ims_media_event_header>();
    if (!h)
        return;
    // The event numbers are not mapped yet (voice-engine.md): logged, so a
    // call on the phone shows which one an RTP timeout is
    modem_.log("ims: media event %u on channel %u (%zu bytes)", h->event, h->channel, m.data.size());
}

void VoiceEngine::send(uint16_t command, uint8_t type, Bytes data, Done done)
{
    auto alive = alive_;
    modem_.request(command, type, std::move(data), [this, alive, command, done](const Result &r) {
        if (!*alive)
            return;
        if (!r.ok)
            modem_.log("ims: voice engine %s failed (0x%04x%s)", commandName(command), r.code,
                       r.timeout ? ", timeout" : "");
        if (done)
            done(r.ok);
    });
}

Bytes VoiceEngine::pdnFrame() const
{
    Address a;
    ipc_ims_pdn_data d{};
    if (!parseAddress(localAddress_, a) ||
        ipc_ims_pdn_setup(&d, a.family, a.bytes, ("lte_" + pdnName_).c_str(), kPdnFlag) < 0)
        return {};
    return frame(d);
}

Bytes VoiceEngine::channelFrame(int channel, uint8_t operation, const MediaSetup *setup) const
{
    ipc_ims_channel_data d{};
    ipc_ims_channel_setup(&d, uint8_t(channel), operation);
    if (setup) {
        Address local, remote;
        if (!parseAddress(setup->localAddress, local) || !parseAddress(setup->remoteAddress, remote) ||
            local.family != remote.family)
            return {};
        const uint16_t remoteRtcp = setup->remoteRtcpPort ? setup->remoteRtcpPort : uint16_t(setup->remotePort + 1);
        // Ports in host order, as the stock stack writes them
        if (ipc_ims_channel_set_addresses(&d, local.family, local.bytes, remote.bytes, setup->localPort,
                                          setup->remotePort, uint16_t(setup->localPort + 1), remoteRtcp) < 0)
            return {};
        d.stream_type = kStreamAudio;
    }
    return frame(d);
}

Bytes VoiceEngine::codecFrame(int channel, const MediaSetup &setup) const
{
    auto info = codecInfo(setup.codec);
    if (!info)
        return {};
    ipc_ims_codec_data d{};
    d.channel = uint8_t(channel);
    d.changed = kCodecAllSettings;
    d.codec = info->id;
    d.payload_type = setup.codec.payloadType;
    d.frequency = uint16_t(setup.codec.rate);
    d.bitrate = info->bitrate;
    d.ptime = uint8_t(setup.ptime);
    d.max_rx_ptime = uint8_t(setup.maxptime > 255 ? 255 : setup.maxptime);
    d.channels = 1;
    d.packetization = setup.codec.octetAlign() ? kOctetAligned : kBandwidthEfficient;
    d.mode_set = uint16_t(info->mode);
    // The byte after the mode set: the receive payload type
    d.unknown3 = setup.codecRxPayloadType ? setup.codecRxPayloadType : setup.codec.payloadType;
    return frame(d);
}

Bytes VoiceEngine::optionFrame(int channel) const
{
    ipc_ims_option_data d{};
    d.channel = uint8_t(channel);
    d.flags = IPC_IMS_OPTION_RTP_TIMEOUT | IPC_IMS_OPTION_RTCP_TIMEOUT;
    d.rtp_timeout = config_.rtpTimeout;
    d.rtcp_timeout = config_.rtcpTimeout;
    if (config_.rtcpOnCall) {
        d.flags |= IPC_IMS_OPTION_RTCP | IPC_IMS_OPTION_RTCP_ON_CALL;
        d.rtcp = 1;
        d.rtcp_on_call = 1;
        d.rtcp_rs = config_.rtcpRs;
        d.rtcp_rr = config_.rtcpRr;
    }
    return frame(d);
}

Bytes VoiceEngine::dtmfFrame(int channel, const MediaSetup &setup) const
{
    // 0x1604 set (stock SAE_SetDTMFOptions): @7 channel, @8 payload type,
    // @9 0, @0x0a u16, @0x0c payload type. Which payload type is the
    // receive one and the u16 being the clock rate are read from the
    // callers' argument order, not from a capture.
    struct __attribute__((packed)) {
        uint8_t channel;
        uint8_t txPayloadType;
        uint8_t zero;
        uint16_t rate;
        uint8_t rxPayloadType;
    } d{};
    d.channel = uint8_t(channel);
    d.txPayloadType = setup.dtmf->payloadType;
    d.rate = uint16_t(setup.dtmf->rate);
    d.rxPayloadType = setup.dtmfRxPayloadType ? setup.dtmfRxPayloadType : setup.dtmf->payloadType;
    return frame(d);
}

Bytes VoiceEngine::mediaFrame(int channel, uint8_t operation, const MediaSetup &setup) const
{
    ipc_ims_rtp_media_data d{};
    d.operation = operation;
    d.channel = uint8_t(channel);
    d.status = operation == kMediaStop ? kMediaHeld : (setup.active ? kMediaSendRecv : kMediaHeld);
    // The last byte: 1 for an IPv6 media address
    Address a;
    d.unknown = parseAddress(setup.remoteAddress, a) && a.family == IPC_IMS_ADDRESS_IPV6 ? 1 : 0;
    return frame(d);
}

void VoiceEngine::prepare(const std::string &localAddress, const std::string &pdnName, Done done)
{
    localAddress_ = localAddress;
    pdnName_ = pdnName;
    auto pdn = pdnFrame();
    if (pdn.empty()) {
        modem_.log("ims: voice engine: bad PDN address or name");
        if (done)
            done(false);
        return;
    }
    auto sendPdn = [this, pdn, done]() { send(IPC_IMS_PDN, IPC_TYPE_SET, pdn, done); };
    if (engineOn_) {
        sendPdn();
        return;
    }
    // Engine on, once; the second byte is what stock passes: the low byte of
    // the profile's first audio port
    ipc_ims_engine_data e{};
    e.engine = IPC_IMS_ENGINE_ON;
    e.mode = uint8_t(config_.portBase & 0xff);
    send(IPC_IMS_ENGINE, IPC_TYPE_SET, frame(e), [this, sendPdn, done](bool ok) {
        if (!ok) {
            if (done)
                done(false);
            return;
        }
        engineOn_ = true;
        sendPdn();
    });
}

uint16_t VoiceEngine::allocatePort()
{
    // Even RTP ports from the profile's first, RTCP on the next one
    for (uint32_t port = config_.portBase & ~1u; port + 1 <= 0xffff; port += 2) {
        if (!ports_.count(uint16_t(port))) {
            ports_.insert(uint16_t(port));
            return uint16_t(port);
        }
    }
    return 0;
}

void VoiceEngine::releasePort(uint16_t port)
{
    ports_.erase(port);
}

// Frames sent one after the other; the first failure ends the run
namespace {
struct Steps : std::enable_shared_from_this<Steps> {
    std::vector<std::pair<uint16_t, std::pair<uint8_t, Bytes>>> frames;
    std::function<void(uint16_t, uint8_t, Bytes, MediaEngine::Done)> send;
    MediaEngine::Done done;
    size_t next = 0;

    void run()
    {
        if (next == frames.size()) {
            if (done)
                done(true);
            return;
        }
        auto &[command, typed] = frames[next++];
        auto self = shared_from_this();
        send(command, typed.first, typed.second, [self](bool ok) {
            if (!ok) {
                if (self->done)
                    self->done(false);
                return;
            }
            self->run();
        });
    }
};
}  // namespace

void VoiceEngine::open(const MediaSetup &setup, std::function<void(std::optional<int> channel)> done)
{
    int channel = -1;
    for (unsigned i = 0; i < kChannels; i++) {
        if (!channels_.count(int(i))) {
            channel = int(i);
            break;
        }
    }
    auto pdn = pdnFrame();
    auto change = channel < 0 ? Bytes{} : channelFrame(channel, IPC_IMS_CHANNEL_CHANGE, &setup);
    auto codec = channel < 0 ? Bytes{} : codecFrame(channel, setup);
    if (channel < 0 || pdn.empty() || change.empty() || codec.empty()) {
        modem_.log("ims: voice engine: cannot open a channel (%s)",
                   channel < 0 ? "none free" : codec.empty() ? "codec" : "addresses");
        done(std::nullopt);
        return;
    }
    channels_[channel] = Channel{setup, false};

    // As stock's CreateChannel and StartChannel: the PDN, the channel, its
    // addresses, codec, options, DTMF, then start
    auto steps = std::make_shared<Steps>();
    steps->send = [this](uint16_t c, uint8_t t, Bytes d, Done k) { send(c, t, std::move(d), std::move(k)); };
    steps->frames.push_back({IPC_IMS_PDN, {IPC_TYPE_SET, pdn}});
    steps->frames.push_back({IPC_IMS_CHANNEL, {IPC_TYPE_SET, channelFrame(channel, IPC_IMS_CHANNEL_CREATE, nullptr)}});
    steps->frames.push_back({IPC_IMS_CHANNEL, {IPC_TYPE_SET, change}});
    steps->frames.push_back({IPC_IMS_CODEC, {IPC_TYPE_SET, codec}});
    steps->frames.push_back({IPC_IMS_OPTION, {IPC_TYPE_SET, optionFrame(channel)}});
    if (setup.dtmf)
        steps->frames.push_back({IPC_IMS_DTMF, {IPC_TYPE_SET, dtmfFrame(channel, setup)}});
    steps->frames.push_back({IPC_IMS_RTP_MEDIA, {IPC_TYPE_SET, mediaFrame(channel, kMediaStart, setup)}});
    steps->done = [this, channel, done](bool ok) {
        if (!ok) {
            // Leave nothing behind in the modem
            send(IPC_IMS_CHANNEL, IPC_TYPE_SET, channelFrame(channel, IPC_IMS_CHANNEL_DELETE, nullptr),
                 [this, channel](bool) { channels_.erase(channel); });
            done(std::nullopt);
            return;
        }
        channels_[channel].started = true;
        modem_.log("ims: voice channel %d started", channel);
        done(channel);
    };
    steps->run();
}

void VoiceEngine::update(int channel, const MediaSetup &setup, Done done)
{
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        if (done)
            done(false);
        return;
    }
    const MediaSetup old = it->second.setup;
    auto steps = std::make_shared<Steps>();
    steps->send = [this](uint16_t c, uint8_t t, Bytes d, Done k) { send(c, t, std::move(d), std::move(k)); };
    if (!sameAddresses(old, setup)) {
        auto change = channelFrame(channel, IPC_IMS_CHANNEL_CHANGE, &setup);
        if (change.empty()) {
            if (done)
                done(false);
            return;
        }
        steps->frames.push_back({IPC_IMS_CHANNEL, {IPC_TYPE_SET, change}});
    }
    if (!sameCodec(old, setup)) {
        auto codec = codecFrame(channel, setup);
        if (codec.empty()) {
            if (done)
                done(false);
            return;
        }
        steps->frames.push_back({IPC_IMS_CODEC, {IPC_TYPE_SET, codec}});
    }
    if (setup.dtmf && !sameDtmf(old, setup))
        steps->frames.push_back({IPC_IMS_DTMF, {IPC_TYPE_SET, dtmfFrame(channel, setup)}});
    steps->frames.push_back({IPC_IMS_RTP_MEDIA, {IPC_TYPE_SET, mediaFrame(channel, kMediaModify, setup)}});
    it->second.setup = setup;
    steps->done = std::move(done);
    steps->run();
}

void VoiceEngine::close(int channel, Done done)
{
    auto it = channels_.find(channel);
    if (it == channels_.end()) {
        if (done)
            done(true);
        return;
    }
    const MediaSetup setup = it->second.setup;
    const bool started = it->second.started;
    channels_.erase(it);
    auto remove = [this, channel, done](bool) {
        send(IPC_IMS_CHANNEL, IPC_TYPE_SET, channelFrame(channel, IPC_IMS_CHANNEL_DELETE, nullptr), done);
    };
    if (started)
        send(IPC_IMS_RTP_MEDIA, IPC_TYPE_SET, mediaFrame(channel, kMediaStop, setup), remove);
    else
        remove(true);
}

void VoiceEngine::dtmf(int channel, char digit, Done done)
{
    const int code = dtmfCode(digit);
    if (code < 0 || !channels_.count(channel)) {
        if (done)
            done(false);
        return;
    }
    // 0x1604 execute: @7 channel, @8 event, @9 mode, @0x0a operation
    const uint8_t d[4] = {uint8_t(channel), uint8_t(code), kDtmfSingle, kDtmfPlay};
    send(IPC_IMS_DTMF, IPC_TYPE_EXEC, Bytes(d, d + sizeof(d)), std::move(done));
}

}  // namespace shannond::ims
