// SPDX-License-Identifier: GPL-2.0-or-later
//
// SDP offer/answer and the modem voice engine's frames (against the fake
// modem, which accepts every setting).
#include "core/ims/sdp.h"
#include "core/ims/voice_engine.h"
#include "core/modem.h"
#include "sim/fake_modem.h"
#include "tests/test.h"

#include <cstring>

extern "C" {
#include <samsung-ipc.h>
}

using namespace shannond;
using namespace shannond::ims;

namespace {

// An offer as a VoLTE network sends it for an incoming call: AMR-WB and AMR,
// both packetizations, preconditions with nothing reserved on its side yet,
// and a video stream we do not take
const char kNetworkOffer[] =
    "v=0\r\n"
    "o=- 3815 3815 IN IP6 2a00:1028::10\r\n"
    "s=-\r\n"
    "c=IN IP6 2a00:1028::10\r\n"
    "b=AS:41\r\n"
    "b=RS:0\r\n"
    "b=RR:0\r\n"
    "t=0 0\r\n"
    "m=audio 40000 RTP/AVP 104 110 102 108 105 100\r\n"
    "b=AS:41\r\n"
    "a=rtpmap:104 AMR-WB/16000/1\r\n"
    "a=fmtp:104 mode-change-capability=2;max-red=0\r\n"
    "a=rtpmap:110 AMR-WB/16000/1\r\n"
    "a=fmtp:110 octet-align=1;mode-change-capability=2;max-red=0\r\n"
    "a=rtpmap:102 AMR/8000/1\r\n"
    "a=fmtp:102 mode-set=0,2,4,7;mode-change-capability=2;max-red=0\r\n"
    "a=rtpmap:108 AMR/8000/1\r\n"
    "a=fmtp:108 octet-align=1\r\n"
    "a=rtpmap:105 telephone-event/16000\r\n"
    "a=fmtp:105 0-15\r\n"
    "a=rtpmap:100 telephone-event/8000\r\n"
    "a=fmtp:100 0-15\r\n"
    "a=ptime:20\r\n"
    "a=maxptime:240\r\n"
    "a=curr:qos local none\r\n"
    "a=curr:qos remote none\r\n"
    "a=des:qos mandatory local sendrecv\r\n"
    "a=des:qos optional remote sendrecv\r\n"
    "a=sendrecv\r\n"
    "m=video 0 RTP/AVP 99\r\n"
    "a=rtpmap:99 H264/90000\r\n"
    "a=sendonly\r\n";

}  // namespace

TEST(sdp_parse_network_offer)
{
    auto sdp = Sdp::parse(kNetworkOffer);
    CHECK(sdp);
    CHECK(sdp->address == "2a00:1028::10" && sdp->port == 40000);
    CHECK(sdp->codecs.size() == 6 && sdp->codecs[0].payloadType == 104 && sdp->codecs[5].payloadType == 100);
    const Codec *voice = sdp->voice();
    CHECK(voice && voice->name == "AMR-WB" && voice->rate == 16000 && !voice->octetAlign());
    CHECK(sdp->codecs[1].octetAlign());
    CHECK(sdp->codecs[2].highestMode() == 7 && !sdp->codecs[0].highestMode());
    const Codec *dtmf = sdp->dtmf();
    CHECK(dtmf && dtmf->payloadType == 105);
    CHECK(sdp->ptime == 20 && sdp->maxptime == 240 && sdp->bandwidthAs == 41);
    // The video stream's sendonly does not leak into ours
    CHECK(sdp->direction == Direction::SendRecv);
    CHECK(sdp->qos.present && !sdp->qos.localCurrent && sdp->qos.localDesired == "mandatory" &&
          sdp->qos.remoteDesired == "optional");
    CHECK(!preconditionsMet(*sdp, true));
}

TEST(sdp_answer_keeps_offer_codec)
{
    MediaCapabilities caps;
    auto offer = Sdp::parse(kNetworkOffer);
    CHECK(offer);
    auto answer = makeAnswer(caps, *offer, "2001:db8::5", 1234, 77);
    CHECK(answer);
    CHECK(answer->codecs.size() == 2 && answer->codecs[0].payloadType == 104 && answer->codecs[1].payloadType == 105);
    CHECK(answer->qos.present && answer->qos.localDesired == "mandatory");
    // Its side is not reserved yet; nor is ours until the bearer comes
    setPreconditions(*answer, false, offer->qos.localCurrent);
    std::string text = answer->str();
    CHECK(text.find("c=IN IP6 2001:db8::5\r\n") != std::string::npos);
    CHECK(text.find("m=audio 1234 RTP/AVP 104 105\r\n") != std::string::npos);
    CHECK(text.find("a=curr:qos local none\r\n") != std::string::npos);
    CHECK(text.find("a=sendrecv\r\n") != std::string::npos);

    // A rejected or codec-less offer gets no answer
    Sdp none = *offer;
    none.codecs.erase(none.codecs.begin(), none.codecs.begin() + 4);
    CHECK(!makeAnswer(caps, none, "2001:db8::5", 1234, 77));
}

TEST(sdp_offer_round_trip)
{
    MediaCapabilities caps;
    Sdp offer = makeOffer(caps, "2001:db8::5", 1234, 99);
    std::string text = offer.str();
    CHECK(text.find("m=audio 1234 RTP/AVP 116 107 118 96 111 110\r\n") != std::string::npos);
    CHECK(text.find("a=fmtp:107 octet-align=1; mode-change-capability=2; max-red=220\r\n") != std::string::npos);
    CHECK(text.find("a=rtpmap:111 telephone-event/16000\r\n") != std::string::npos);
    CHECK(text.find("a=des:qos optional remote sendrecv\r\n") != std::string::npos);
    CHECK(text.find("b=AS:38\r\n") != std::string::npos);

    auto parsed = Sdp::parse(text);
    CHECK(parsed && parsed->codecs.size() == 6 && parsed->port == 1234 && parsed->address == "2001:db8::5");
    CHECK(parsed->codecs[1].octetAlign() && !parsed->codecs[0].octetAlign());
    CHECK(parsed->dtmf()->payloadType == 111);

    // The far end reserved its side and asks for confirmation
    Sdp reply = *parsed;
    reply.qos.localCurrent = true;
    reply.qos.localDesired = "mandatory";
    CHECK(preconditionsMet(reply, true) && !preconditionsMet(reply, false));
    // Hold: our sendonly is their recvonly
    CHECK(reverse(Direction::SendOnly) == Direction::RecvOnly && reverse(Direction::Inactive) == Direction::Inactive);
}

TEST(sdp_rejects_broken)
{
    CHECK(!Sdp::parse(""));
    CHECK(!Sdp::parse("v=0\r\nm=video 4000 RTP/AVP 99\r\n"));
    // Garbage numbers are ignored, not trusted
    auto s = Sdp::parse("v=0\r\nc=IN IP4 10.0.0.1\r\nm=audio 99999 RTP/AVP 300 x 96\r\na=rtpmap:96 AMR/8000/1\r\n");
    CHECK(s && s->port == 0 && s->codecs.size() == 1 && s->address == "10.0.0.1");
}

namespace {

struct EngineBench {
    EventLoop loop;
    FakeModem fake{loop};
    Modem modem{loop, fake.fmtFd(), fake.rfsFd()};
    VoiceEngine engine{modem};

    std::vector<const Message *> sent(uint16_t command) const
    {
        std::vector<const Message *> out;
        for (const Message &m : fake.received)
            if (m.command == command)
                out.push_back(&m);
        return out;
    }

    MediaSetup setup() const
    {
        MediaSetup s;
        s.localAddress = "2001:db8::5";
        s.localPort = 1234;
        s.remoteAddress = "2a00:1028::10";
        s.remotePort = 40000;
        s.codec.payloadType = 104;
        s.codec.name = "AMR-WB";
        s.codec.rate = 16000;
        s.codec.fmtp = "mode-change-capability=2";
        s.codecRxPayloadType = 116;
        Codec dtmf;
        dtmf.payloadType = 105;
        dtmf.name = "telephone-event";
        dtmf.rate = 16000;
        s.dtmf = dtmf;
        s.dtmfRxPayloadType = 111;
        return s;
    }
};

}  // namespace

TEST(voice_engine_call)
{
    EngineBench b;
    std::optional<bool> prepared;
    b.engine.prepare("2001:db8::5", "ims", [&](bool ok) { prepared = ok; });
    CHECK(b.loop.runUntil([&] { return prepared.has_value(); }) && *prepared);
    auto engine = b.sent(IPC_IMS_ENGINE);
    CHECK(engine.size() == 1 && engine[0]->data == Bytes({IPC_IMS_ENGINE_ON, 0xd2}));
    auto pdn = b.sent(IPC_IMS_PDN);
    CHECK(pdn.size() == 1);
    auto p = pdn[0]->as<ipc_ims_pdn_data>();
    CHECK(p && p->family == IPC_IMS_ADDRESS_IPV6 && p->ifname_length == 7 &&
          std::memcmp(p->ifname, "lte_ims", 7) == 0 && p->cid == 1 && p->address[0] == 0x20);

    // A second registration does not switch the engine on again
    prepared.reset();
    b.engine.prepare("2001:db8::5", "ims", [&](bool ok) { prepared = ok; });
    CHECK(b.loop.runUntil([&] { return prepared.has_value(); }) && *prepared);
    CHECK(b.sent(IPC_IMS_ENGINE).size() == 1 && b.sent(IPC_IMS_PDN).size() == 2);

    CHECK(b.engine.allocatePort() == 1234 && b.engine.allocatePort() == 1236);
    b.engine.releasePort(1234);
    CHECK(b.engine.allocatePort() == 1234);

    std::optional<std::optional<int>> opened;
    b.engine.open(b.setup(), [&](std::optional<int> ch) { opened = ch; });
    CHECK(b.loop.runUntil([&] { return opened.has_value(); }) && *opened == 0);

    auto channel = b.sent(IPC_IMS_CHANNEL);
    CHECK(channel.size() == 2);
    auto create = channel[0]->as<ipc_ims_channel_data>();
    CHECK(create && create->operation == IPC_IMS_CHANNEL_CREATE && create->local_rtp_port == 0xffff);
    auto change = channel[1]->as<ipc_ims_channel_data>();
    CHECK(change && change->operation == IPC_IMS_CHANNEL_CHANGE && change->local_rtp_port == 1234 &&
          change->local_rtcp_port == 1235 && change->remote_rtp_port == 40000 && change->remote_rtcp_port == 40001 &&
          change->remote_family == IPC_IMS_ADDRESS_IPV6 && change->remote[0] == 0x2a && change->stream_type == 1);

    auto codec = b.sent(IPC_IMS_CODEC);
    CHECK(codec.size() == 1);
    auto c = codec[0]->as<ipc_ims_codec_data>();
    CHECK(c && c->changed == 0x21ff && c->codec == IPC_IMS_CODEC_AMR_WB && c->payload_type == 104 &&
          c->frequency == 16000 && c->bitrate == IPC_IMS_BITRATE_AMR_WB_23850 && c->mode_set == 8 &&
          c->packetization == 2 && c->unknown3 == 116 && c->ptime == 20 && c->max_rx_ptime == 240);

    auto option = b.sent(IPC_IMS_OPTION);
    CHECK(option.size() == 1);
    auto o = option[0]->as<ipc_ims_option_data>();
    CHECK(o && o->rtp_timeout == 20 && o->rtcp_on_call == 1 && (o->flags & IPC_IMS_OPTION_RTP_TIMEOUT) &&
          o->rtcp_rr == 1125);

    auto dtmf = b.sent(IPC_IMS_DTMF);
    CHECK(dtmf.size() == 1 && dtmf[0]->data == Bytes({0, 105, 0, 0x80, 0x3e, 111}));

    auto media = b.sent(IPC_IMS_RTP_MEDIA);
    CHECK(media.size() == 1 && media[0]->data == Bytes({1, 0, 3, 1}));
    // In stock's order: PDN, channel, codec, options, DTMF, start
    const auto &all = b.fake.received;
    std::vector<uint16_t> order;
    for (const Message &m : all)
        if ((m.command >> 8) == IPC_GROUP_IMS)
            order.push_back(m.command);
    CHECK((order == std::vector<uint16_t>{IPC_IMS_ENGINE, IPC_IMS_PDN, IPC_IMS_PDN, IPC_IMS_PDN, IPC_IMS_CHANNEL,
                                          IPC_IMS_CHANNEL, IPC_IMS_CODEC, IPC_IMS_OPTION, IPC_IMS_DTMF,
                                          IPC_IMS_RTP_MEDIA}));

    // Hold: only the media state changes
    MediaSetup held = b.setup();
    held.active = false;
    std::optional<bool> updated;
    b.engine.update(0, held, [&](bool ok) { updated = ok; });
    CHECK(b.loop.runUntil([&] { return updated.has_value(); }) && *updated);
    CHECK(b.sent(IPC_IMS_CHANNEL).size() == 2 && b.sent(IPC_IMS_CODEC).size() == 1);
    CHECK(b.sent(IPC_IMS_RTP_MEDIA).back()->data == Bytes({3, 0, 0, 1}));

    // A re-INVITE to a new address and codec
    MediaSetup moved = b.setup();
    moved.remotePort = 40010;
    moved.codec.name = "AMR";
    moved.codec.rate = 8000;
    moved.codec.fmtp = "mode-set=0,2,4,7; octet-align=1";
    updated.reset();
    b.engine.update(0, moved, [&](bool ok) { updated = ok; });
    CHECK(b.loop.runUntil([&] { return updated.has_value(); }) && *updated);
    CHECK(b.sent(IPC_IMS_CHANNEL).size() == 3 && b.sent(IPC_IMS_CODEC).size() == 2);
    auto amr = b.sent(IPC_IMS_CODEC).back()->as<ipc_ims_codec_data>();
    CHECK(amr && amr->codec == IPC_IMS_CODEC_AMR && amr->bitrate == IPC_IMS_BITRATE_AMR_12200 &&
          amr->packetization == 1 && amr->frequency == 8000);
    CHECK(b.sent(IPC_IMS_RTP_MEDIA).back()->data == Bytes({3, 0, 3, 1}));

    std::optional<bool> sentDigit;
    b.engine.dtmf(0, '#', [&](bool ok) { sentDigit = ok; });
    CHECK(b.loop.runUntil([&] { return sentDigit.has_value(); }) && *sentDigit);
    auto exec = b.sent(IPC_IMS_DTMF).back();
    CHECK(exec->type == IPC_TYPE_EXEC && exec->data == Bytes({0, 11, 0, 1}));
    sentDigit.reset();
    b.engine.dtmf(0, 'x', [&](bool ok) { sentDigit = ok; });
    CHECK(sentDigit == false);

    // A dedicated bearer for voice
    std::vector<std::tuple<int, int, bool>> bearers;
    b.engine.onEvents({[&](int id, int qci, bool added) { bearers.emplace_back(id, qci, added); }, {}});
    ipc_ims_dedicated_bearer_header bearer{};
    bearer.bearer_id = 6;
    bearer.qci = 1;
    bearer.event = IPC_IMS_BEARER_ADDED;
    b.fake.send(IPC_IMS_DEDICATED_BEARER, IPC_TYPE_NOTI, toBytes(bearer));
    CHECK(b.loop.runUntil([&] { return !bearers.empty(); }));
    CHECK(bearers[0] == std::make_tuple(6, 1, true));

    std::optional<bool> closed;
    b.engine.close(0, [&](bool ok) { closed = ok; });
    CHECK(b.loop.runUntil([&] { return closed.has_value(); }) && *closed);
    CHECK(b.sent(IPC_IMS_RTP_MEDIA).back()->data == Bytes({2, 0, 0, 1}));
    auto del = b.sent(IPC_IMS_CHANNEL).back()->as<ipc_ims_channel_data>();
    CHECK(del && del->operation == IPC_IMS_CHANNEL_DELETE && del->channel == 0);
}

TEST(voice_engine_rejects_unknown_codec)
{
    EngineBench b;
    MediaSetup s = b.setup();
    s.codec.name = "PCMU";
    std::optional<std::optional<int>> opened;
    b.engine.open(s, [&](std::optional<int> ch) { opened = ch; });
    CHECK(opened.has_value() && !*opened);
    CHECK(b.fake.received.empty());
}
