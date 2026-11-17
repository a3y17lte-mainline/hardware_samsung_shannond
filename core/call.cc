// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/call.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

const char *callStateName(CallState state)
{
    switch (state) {
    case CallState::Active: return "active";
    case CallState::Holding: return "holding";
    case CallState::Dialing: return "dialing";
    case CallState::Alerting: return "alerting";
    case CallState::Incoming: return "incoming";
    case CallState::Waiting: return "waiting";
    }
    return "?";
}

Calls::Calls(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_CALL_INCOMING, [this](const Message &) { refresh(); });
    modem_.onMessage(IPC_CALL_STATUS, [this](const Message &m) { status(m); });
    modem_.onMessage(IPC_CALL_LIST, [this](const Message &m) { listReceived(m); });
}

void Calls::notify()
{
    for (auto &callback : changed_)
        callback();
}

void Calls::refresh()
{
    modem_.request(IPC_CALL_LIST, IPC_TYPE_GET);
}

void Calls::status(const Message &message)
{
    auto s = message.as<ipc_call_status_data>();
    if (!s)
        return;

    modem_.log("call %u status %u, end cause %u", s->id, s->status, s->end_cause);
    if (s->status == IPC_CALL_STATUS_RELEASED) {
        lastEndCause_ = s->end_cause;
        lastEndCauseType_ = s->reason;
    }
    if (s->status == IPC_CALL_STATUS_CONNECTED || s->status == IPC_CALL_STATUS_DIALING)
        setAudio(true);
    refresh();
}

void Calls::listReceived(const Message &message)
{
    const unsigned int count = ipc_call_list_count_extract(message.data.data(), message.data.size());
    std::vector<Call> calls;

    for (unsigned int i = 0; i < count; i++) {
        ipc_call_list_entry *entry =
            ipc_call_list_entry_extract(message.data.data(), message.data.size(), i);
        if (!entry)
            break;

        Call call;
        call.id = entry->id;
        call.incoming = entry->term == IPC_CALL_TERM_MT;
        call.multiparty = entry->mpty;
        switch (entry->status) {
        case IPC_CALL_LIST_ENTRY_STATUS_HOLDING: call.state = CallState::Holding; break;
        case IPC_CALL_LIST_ENTRY_STATUS_DIALING: call.state = CallState::Dialing; break;
        case IPC_CALL_LIST_ENTRY_STATUS_ALERTING: call.state = CallState::Alerting; break;
        case IPC_CALL_LIST_ENTRY_STATUS_INCOMING: call.state = CallState::Incoming; break;
        case IPC_CALL_LIST_ENTRY_STATUS_WAITING: call.state = CallState::Waiting; break;
        default: call.state = CallState::Active; break;
        }
        if (char *number = ipc_call_list_entry_number_extract(entry)) {
            call.number = number;
            free(number);
        }
        calls.push_back(call);
    }

    calls_ = std::move(calls);
    if (calls_.empty() && !imsAudio_)
        setAudio(false);
    notify();
}

void Calls::setImsAudio(bool active)
{
    imsAudio_ = active;
    if (active)
        setAudio(true);
    else if (calls_.empty())
        setAudio(false);
}

void Calls::setAudio(bool active)
{
    if (active == audioActive_)
        return;
    audioActive_ = active;

    if (active) {
        sendAudioPath();
        // The microphone's state at the start of a call, as stock
        setMute(micMute_, [](bool) {});
    }
    if (audio_)
        audio_(active);
}

void Calls::sendAudioPath()
{
    ipc_snd_audio_path_ctrl_data path{};
    path.path = audioPath_;
    modem_.request(IPC_SND_AUDIO_PATH_CTRL, IPC_TYPE_SET, path);
    sendVolume();
}

void Calls::sendVolume()
{
    // The modem keeps a volume per kind of output
    ipc_snd_spkr_volume_ctrl_data volume{};
    switch (audioPath_) {
    case IPC_SND_AUDIO_PATH_SPEAKER: volume.type = IPC_SND_VOLUME_TYPE_SPEAKER; break;
    case IPC_SND_AUDIO_PATH_HEADSET:
    case IPC_SND_AUDIO_PATH_HEADPHONE: volume.type = IPC_SND_VOLUME_TYPE_HEADSET; break;
    default: volume.type = IPC_SND_VOLUME_TYPE_VOICE; break;
    }
    volume.volume = volume_;
    modem_.request(IPC_SND_SPKR_VOLUME_CTRL, IPC_TYPE_SET, volume);
}

void Calls::setVolume(uint8_t volume)
{
    volume = volume > kMaxCallVolume ? kMaxCallVolume : volume;
    if (volume == volume_)
        return;
    volume_ = volume;
    if (audioActive_)
        sendVolume();
}

void Calls::setAudioPath(uint8_t path)
{
    if (path == audioPath_)
        return;
    audioPath_ = path;
    if (audioActive_)
        sendAudioPath();
}

void Calls::originate(const std::string &number, uint8_t identity, uint8_t subtype,
                      uint8_t category, Done done)
{
    // IpcTxCallOriginate: the number as dialled, '+' included, and the
    // prefix byte saying whether it is international
    const bool intl = !number.empty() && number[0] == '+';
    const std::string dialled = number.substr(0, IPC_CALL_OUTGOING_NUMBER_MAX);
    ipc_call_originate_data data;
    std::memset(&data, 0, sizeof(data));
    if (dialled.empty() ||
        ipc_call_outgoing_setup(&data.call, IPC_CALL_TYPE_VOICE, identity,
                                intl ? IPC_CALL_PREFIX_INTL : IPC_CALL_PREFIX_NATIONAL,
                                dialled.c_str()) < 0) {
        done(false);
        return;
    }
    data.call.unknown = subtype;
    data.category = category;
    modem_.request(IPC_CALL_OUTGOING, IPC_TYPE_EXEC, data, [done](const Result &r) { done(r.ok); });
}

void Calls::dial(const std::string &number, Done done, uint8_t identity)
{
    originate(number, identity, 0, 0, std::move(done));
}

void Calls::dialEmergency(const std::string &number, uint8_t categories, Done done)
{
    // The emergency call type (0x0106) and the categories in the last byte
    originate(number, IPC_CALL_IDENTITY_DEFAULT, IPC_CALL_OUTGOING_EMERGENCY, categories & 0x7f,
              std::move(done));
}

void Calls::setMicMute(bool mute)
{
    if (mute == micMute_)
        return;
    micMute_ = mute;
    if (audioActive_)
        setMute(mute, [](bool) {});
}

void Calls::setMute(bool mute, Done done)
{
    // IpcTxSndSetMute: the framework's value as it is
    micMute_ = mute;
    ipc_snd_mic_mute_ctrl_data data{};
    data.mute = mute ? 1 : 0;
    modem_.request(IPC_SND_MIC_MUTE_CTRL, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

void Calls::answer(Done done)
{
    modem_.request(IPC_CALL_ANSWER, IPC_TYPE_EXEC, {}, [done](const Result &r) { done(r.ok); });
}

void Calls::hangup(Done done)
{
    modem_.request(IPC_CALL_RELEASE, IPC_TYPE_EXEC, {}, [done](const Result &r) { done(r.ok); });
}

void Calls::hangup(uint8_t id, Done done)
{
    // CallManager::DoHangup: the plain release for the only call
    if (calls_.size() <= 1) {
        hangup(std::move(done));
        return;
    }
    manage(IPC_SS_MANAGE_CALL_RELEASE_ONE, std::move(done), id);
}

void Calls::manage(uint8_t operation, Done done, uint8_t id, const std::string &number)
{
    // IpcTxCallSetManageCall: the modem's call ids as they are (the stock
    // RIL shifts them only for Intel modems)
    ipc_ss_manage_call_data data;
    std::memset(&data, 0, sizeof(data));
    data.operation = operation;
    data.id = id;
    const size_t len = std::min<size_t>(number.size(), IPC_SS_MANAGE_CALL_NUMBER_MAX);
    data.number_length = static_cast<uint8_t>(len);
    std::memcpy(data.number, number.data(), len);
    modem_.request(IPC_SS_MANAGE_CALL, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

void Calls::dtmf(char tone, bool start, Done done)
{
    ipc_call_cont_dtmf_data data{};
    data.status = start ? IPC_CALL_DTMF_STATUS_START : IPC_CALL_DTMF_STATUS_STOP;
    data.tone = tone;
    modem_.request(IPC_CALL_CONT_DTMF, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

}  // namespace shannond
