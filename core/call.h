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

    void dial(const std::string &number, Done done);
    void answer(Done done);
    void hangup(Done done);
    void dtmf(char tone, bool start, Done done);
    void refresh();

    // The audio path in the modem (IPC_SND_AUDIO_PATH): its echo and
    // noise tuning for the handset, the speaker or a headset. Sent when a
    // call starts, and at once when it changes during one
    void setAudioPath(uint8_t path);
    // The call volume, 0 to kMaxCallVolume: the modem sets the level
    void setVolume(uint8_t volume);

private:
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
