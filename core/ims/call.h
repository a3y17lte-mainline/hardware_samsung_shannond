// SPDX-License-Identifier: GPL-2.0-or-later
//
// VoLTE calls (3GPP TS 24.229 5.1.3-5.1.5, GSMA IR.92 2.2-2.4): the INVITE
// dialogs with reliable provisional answers (RFC 3262), UPDATE (RFC 3311),
// QoS preconditions tied to the dedicated bearer (RFC 3312), hold and
// resume, CANCEL and BYE, and the session timer (RFC 4028). The voice runs
// in a MediaEngine (the modem's voice engine). No Android types.
//
// Outgoing: INVITE with our SDP offer (our resources not reserved yet);
// a reliable 183 brings the answer: PRACK, then UPDATE once the voice
// bearer is up; 180, then 200 and ACK.
// Incoming: 100, then a reliable 183 with the answer asking the network to
// confirm its resources; when both sides' resources are there (the
// network's UPDATE, our bearer), 180 and the phone rings; 200 on answer.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ims/endpoint.h"
#include "core/ims/media.h"
#include "core/ims/profile.h"
#include "core/ims/registration.h"
#include "core/ims/sdp.h"
#include "core/loop.h"

namespace shannond::ims {

enum class ImsCallState {
    Setup,     // incoming, resources not ready: not shown yet
    Dialing,   // outgoing, no ringing yet
    Alerting,  // outgoing, the far end rings
    Incoming,  // the phone rings
    Active,
    Held,      // we put it on hold
    Ended,
};
const char *callStateName(ImsCallState state);

struct ImsCallInfo {
    int id = 0;
    ImsCallState state = ImsCallState::Setup;
    bool incoming = false;
    std::string number;     // the other side: a number, else its URI
    bool remoteHeld = false;  // the other side holds the call
    // Once ended: the SIP status that ended it (0: BYE or our hangup), and
    // whether to try the call again on CS
    int endStatus = 0;
    std::string endReason;
    bool retryOnCs = false;
    // Ended here by SRVCC: the call goes on as a CS call in the modem
    bool handedOver = false;
    // Outgoing, before the answer: the network plays the ringback (early
    // media), so the phone plays none of its own
    bool earlyMedia = false;
};

class ImsCalls {
public:
    using Changed = std::function<void(const ImsCallInfo &call)>;

    ImsCalls(EventLoop &loop, MediaEngine &media);
    ~ImsCalls();
    ImsCalls(const ImsCalls &) = delete;
    ImsCalls &operator=(const ImsCalls &) = delete;

    void setLog(std::function<void(const std::string &)> log) { log_ = std::move(log); }
    void onChanged(Changed changed) { changed_.push_back(std::move(changed)); }
    // Call audio needed (any call active or held) or not
    void onAudio(std::function<void(bool)> audio) { audio_ = std::move(audio); }

    // The SIP side while registered. The PDN's name is the modem's data
    // profile for the voice engine ("ims").
    void attach(SipEndpoint &endpoint, ImsRegistration &registration, const std::string &pdnName);
    // Registration or PDN gone: every call ends here at once
    void detach();
    // SRVCC completed: the calls continue on CS. The sessions end here
    // without a BYE, and the call audio stays up a moment for the CS call
    // that takes over.
    void handover();
    bool attached() const { return endpoint_ != nullptr; }

    // Requests the registration does not take; false if not a call's
    bool handleRequest(const SipMessage &request, const Source &source);
    // A 2xx of an INVITE whose transaction already ended
    void handleStrayResponse(const SipMessage &response);

    // Front-end requests
    // number: with #31# in front the call hides our number (CLIR)
    std::optional<int> dial(const std::string &number);
    bool answer(int id);
    bool hangup(int id);  // ends, cancels or rejects
    bool hold(int id);
    bool resume(int id);
    bool dtmf(int id, char digit);

    std::vector<ImsCallInfo> calls() const;  // without calls in Setup
    bool any() const;

    // The request URI for a dialled number (24.229 5.1.2A.1.5)
    static std::string targetUri(const std::string &number, const std::string &domain);

private:
    struct Session;

    void log(const std::string &line);
    Session *find(int id);
    Session *byCallId(const std::string &callId);
    void notify(Session &s);
    void setState(Session &s, ImsCallState state);
    void end(Session &s, int status, const std::string &reason, bool retryOnCs = false);
    void reap();
    void updateAudio();

    // Building and sending
    SipMessage inDialog(Session &s, const std::string &method);
    void addCommon(SipMessage &m, Session &s, bool body);
    std::string contact() const;
    Sdp nextSdp(Session &s);
    void sendInDialog(Session &s, SipMessage m, SipEndpoint::ResponseHandler handler);
    void sendBye(Session &s);
    void ack(Session &s, uint32_t cseq);
    std::string ackVia() const;  // the Via of an ACK for a 2xx

    // Outgoing
    void inviteAnswered(int id, const SipMessage *response);
    void earlyDialog(Session &s, const SipMessage &response);
    void prack(Session &s, const SipMessage &response);
    void sendQosUpdate(Session &s);
    void cancel(Session &s);
    void sendInvite(Session &s, SipMessage invite);
    void updateEarlyMedia(Session &s, const SipMessage &response);
    void forkAnswered(Session &s, const SipMessage &response);

    // Incoming
    void incoming(const SipMessage &request, const Source &source);
    void respondInvite(Session &s, int status, const std::string &reason, bool reliable, bool body);
    void retransmitReliable(int id);
    void startOkRetransmit(Session &s);
    void retransmitOk(int id);
    void checkIncomingReady(Session &s);
    void inDialogRequest(Session &s, const SipMessage &request, const Source &source);
    void offerReceived(Session &s, const SipMessage &request, const Source &source);

    // Both
    void reinvite(Session &s, bool holdCall);
    void bearer(int bearerId, int qci, bool added);
    void remoteAnswer(Session &s, const Sdp &answer);
    void syncMedia(Session &s);
    MediaSetup mediaSetup(const Session &s) const;
    void sessionTimer(Session &s, const SipMessage &message, bool weAreUac);
    void startSessionTimer(Session &s, unsigned interval, bool weRefresh);
    void refreshSession(int id);

    EventLoop &loop_;
    MediaEngine &media_;
    SipEndpoint *endpoint_ = nullptr;
    ImsRegistration *registration_ = nullptr;
    ImsProfile profile_;
    std::string pdnName_;
    std::map<int, std::unique_ptr<Session>> sessions_;
    int nextId_ = 1;
    bool voiceBearer_ = false;  // a QCI 1 dedicated bearer is up
    int voiceBearerId_ = 0;
    // After the last call ended its bearer is on its way out: not counted
    // for a new call until then (the release normally comes within it)
    std::chrono::steady_clock::time_point bearerStaleUntil_{};
    bool haveBearer() const;
    void recheckBearer();
    bool audioOn_ = false;
    EventLoop::TimerId handoverHold_ = 0;
    std::vector<Changed> changed_;
    std::function<void(bool)> audio_;
    std::function<void(const std::string &)> log_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace shannond::ims
