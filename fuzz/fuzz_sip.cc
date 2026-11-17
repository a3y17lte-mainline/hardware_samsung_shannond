// SPDX-License-Identifier: GPL-2.0-or-later
//
// libFuzzer target for what the IMS stack reads from the network: SIP
// messages and their header values, digest challenges, security-server
// lists, and SDP bodies.
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/ims/digest.h"
#include "core/ims/sdp.h"
#include "core/ims/secagree.h"
#include "core/ims/sip.h"

using namespace shannond::ims;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const std::string text(reinterpret_cast<const char *>(data), size);
    (void)sipFrameLength(text);
    if (auto m = SipMessage::parse(text)) {
        (void)m->serialize();
        (void)m->cseq();
        (void)m->branch();
        for (const char *h : {"From", "To", "Contact", "Record-Route", "P-Asserted-Identity"})
            for (const auto &v : m->headers(h))
                (void)NameAddr::parse(v);
        for (const auto &v : m->headers("Via"))
            (void)Via::parse(v);
        if (auto c = m->header("WWW-Authenticate"))
            if (auto d = DigestChallenge::parse(*c))
                (void)AkaVector::fromNonce(d->nonce);
        for (const auto &v : m->headers("Security-Server"))
            (void)SecurityMechanism::parse(v);
        if (auto sdp = Sdp::parse(m->body))
            (void)sdp->str();
    }
    if (auto sdp = Sdp::parse(text)) {
        (void)sdp->str();
        if (const Codec *c = sdp->voice()) {
            (void)c->highestMode();
            (void)c->octetAlign();
        }
        (void)sdp->dtmf();
        (void)makeAnswer(MediaCapabilities{}, *sdp, "2001:db8::1", 1234, 1);
    }
    (void)Uri::parse(text);
    return 0;
}
