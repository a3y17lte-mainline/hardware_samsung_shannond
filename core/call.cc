// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/call.h"

#include <cstdlib>

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
    if (calls_.empty())
        setAudio(false);
    notify();
}

void Calls::setAudio(bool active)
{
    if (active == audioActive_)
        return;
    audioActive_ = active;

    if (active)
        sendAudioPath();
    if (audio_)
        audio_(active);
}

void Calls::sendAudioPath()
{
    ipc_snd_audio_path_ctrl_data path{};
    path.path = audioPath_;
    modem_.request(IPC_SND_AUDIO_PATH_CTRL, IPC_TYPE_SET, path);

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

void Calls::setAudioPath(uint8_t path)
{
    if (path == audioPath_)
        return;
    audioPath_ = path;
    if (audioActive_)
        sendAudioPath();
}

void Calls::dial(const std::string &number, Done done)
{
    ipc_call_outgoing_data data;
    const bool intl = !number.empty() && number[0] == '+';
    if (ipc_call_outgoing_setup(&data, IPC_CALL_TYPE_VOICE, IPC_CALL_IDENTITY_DEFAULT,
                                intl ? IPC_CALL_PREFIX_INTL : IPC_CALL_PREFIX_NONE,
                                intl ? number.c_str() + 1 : number.c_str()) < 0) {
        done(false);
        return;
    }
    modem_.request(IPC_CALL_OUTGOING, IPC_TYPE_EXEC, data, [done](const Result &r) { done(r.ok); });
}

void Calls::answer(Done done)
{
    modem_.request(IPC_CALL_ANSWER, IPC_TYPE_EXEC, {}, [done](const Result &r) { done(r.ok); });
}

void Calls::hangup(Done done)
{
    modem_.request(IPC_CALL_RELEASE, IPC_TYPE_EXEC, {}, [done](const Result &r) { done(r.ok); });
}

void Calls::dtmf(char tone, bool start, Done done)
{
    ipc_call_cont_dtmf_data data{};
    data.status = start ? IPC_CALL_DTMF_STATUS_START : IPC_CALL_DTMF_STATUS_STOP;
    data.tone = tone;
    modem_.request(IPC_CALL_CONT_DTMF, IPC_TYPE_SET, data, [done](const Result &r) { done(r.ok); });
}

}  // namespace shannond
