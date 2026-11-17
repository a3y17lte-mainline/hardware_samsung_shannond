// SPDX-License-Identifier: GPL-2.0-or-later
//
// Circuit switched voice calls. The audio of a call runs from the modem
// through the SoC's audio mixer; the platform switches that path with the
// audio hook. Dialling is only ever done on a front end's request.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/modem.h"

namespace shannond {

enum class CallState { Active, Holding, Dialing, Alerting, Incoming, Waiting };

const char *callStateName(CallState state);

struct Call {
    uint8_t id = 0;
    CallState state = CallState::Active;
    bool incoming = false;
    bool multiparty = false;
    std::string number;
};

constexpr uint8_t kMaxCallVolume = 5;

class Calls {
public:
    using Done = std::function<void(bool ok)>;

    explicit Calls(Modem &modem);

    const std::vector<Call> &list() const { return calls_; }
    // The last call's end, raw from the modem (IPC_CALL_STATUS cause type and cause)
    uint8_t lastEndCause() const { return lastEndCause_; }
    uint8_t lastEndCauseType() const { return lastEndCauseType_; }
    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }
    // Called with true when the first call connects or rings out, false when
    // the last one ends
    void onAudio(std::function<void(bool active)> callback) { audio_ = std::move(callback); }

    // identity: IPC_CALL_IDENTITY (default, hide or show the number)
    void dial(const std::string &number, Done done, uint8_t identity = 0);
    // An emergency call, as the stock RIL places it; categories are the
    // 3GPP TS 24.008 emergency service category bits (0: none given)
    void dialEmergency(const std::string &number, uint8_t categories, Done done);
    // The microphone of a call, muted in the modem
    void setMute(bool mute, Done done);
    void answer(Done done);
    // Ends the call, or rejects the ringing one (one call)
    void hangup(Done done);
    // Ends one call by the modem's id: with other calls around that is a
    // call handling request, as the stock RIL does it
    void hangup(uint8_t id, Done done);
    // 3GPP TS 22.030 call handling (+CHLD): an IPC_SS_MANAGE_CALL
    // operation, on a call (by the modem's id) where one is named
    void manage(uint8_t operation, Done done, uint8_t id = 0, const std::string &number = {});
    void dtmf(char tone, bool start, Done done);
    void refresh();

    // The audio path in the modem (IPC_SND_AUDIO_PATH): its echo and
    // noise tuning for the handset, the speaker or a headset. Sent when a
    // call starts, and at once when it changes during one
    void setAudioPath(uint8_t path);
    // The call volume, 0 to kMaxCallVolume: the modem sets the level
    void setVolume(uint8_t volume);

private:
    void originate(const std::string &number, uint8_t identity, uint8_t subtype, uint8_t category,
                   Done done);
    void status(const Message &message);
    void listReceived(const Message &message);
    void setAudio(bool active);
    void sendAudioPath();
    void sendVolume();
    void notify();

    Modem &modem_;
    std::vector<Call> calls_;
    std::vector<std::function<void()>> changed_;
    std::function<void(bool)> audio_;
    bool audioActive_ = false;
    uint8_t lastEndCause_ = 0;
    uint8_t lastEndCauseType_ = 0;
    uint8_t audioPath_ = 0x01;  // handset
    uint8_t volume_ = 4;
};

}  // namespace shannond
