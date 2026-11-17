// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/sim.h"

#include <algorithm>
#include <cstdlib>

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

const char *simStateName(SimState state)
{
    switch (state) {
    case SimState::Unknown: return "unknown";
    case SimState::Absent: return "absent";
    case SimState::NotReady: return "not-ready";
    case SimState::PinRequired: return "pin-required";
    case SimState::PukRequired: return "puk-required";
    case SimState::PermanentlyBlocked: return "permanently-blocked";
    case SimState::NetworkLocked: return "network-locked";
    case SimState::Ready: return "ready";
    case SimState::Error: return "error";
    }
    return "?";
}

Sim::Sim(Modem &modem) : modem_(modem)
{
    modem_.onMessage(IPC_SEC_PIN_STATUS, [this](const Message &m) { pinStatus(m); });

    modem_.onMessage(IPC_MISC_ME_IMSI, [this](const Message &m) {
        char *imsi = ipc_misc_me_imsi_imsi_extract(m.data.data(), m.data.size());
        if (imsi) {
            imsi_ = imsi;
            free(imsi);
            notify();
        }
    });

    modem_.onMessage(IPC_SEC_SIM_ICCID, [this](const Message &m) {
        // Length, then the number as on the card
        if (m.data.empty())
            return;
        setIccid(m.data.data() + 1, std::min<size_t>(m.data.size() - 1, m.data[0]));
    });
}

// BCD with swapped nibbles, as in EF ICCID
void Sim::setIccid(const uint8_t *data, size_t size)
{
    std::string iccid;
    for (size_t i = 0; i < size; i++) {
        const uint8_t lo = data[i] & 0x0f, hi = data[i] >> 4;
        if (lo <= 9) iccid += char('0' + lo);
        if (hi <= 9) iccid += char('0' + hi);
    }
    if (!iccid.empty()) {
        iccid_ = iccid;
        notify();
    }
}

void Sim::query()
{
    queried_ = true;
    // The answer goes the way of the modem's own status messages
    modem_.request(IPC_SEC_PIN_STATUS, IPC_TYPE_GET);
}

void Sim::notify()
{
    for (auto &callback : changed_)
        callback();
}

void Sim::setState(SimState state)
{
    if (state == state_)
        return;

    state_ = state;
    modem_.log("SIM %s", simStateName(state));

    if (state == SimState::Ready) {
        modem_.request(IPC_MISC_ME_IMSI, IPC_TYPE_GET);
        refreshApps();
        // The modem announces the card's number only at its start: after
        // a query, read it from the card (EF ICCID, 2FE2 under the MF)
        if (queried_ && iccid_.empty())
            io(0xb0, 0x2fe2, {0x3f00}, 0, 0, 10, {}, nullptr,
               [this](std::optional<SimIoResult> r) {
                   if (r && r->sw1 == 0x90)
                       setIccid(r->data.data(), r->data.size());
               });
    }
    notify();
}

void Sim::pinStatus(const Message &message)
{
    auto status = message.as<ipc_sec_pin_status_response_data>();
    if (!status)
        return;

    lockStatus_ = status->status;
    facilityLock_ = status->facility_lock;

    // As the stock RIL reads it (IpcSimCache::GenerateSimStatus)
    switch (status->status) {
    case IPC_SEC_PIN_STATUS_READY:
    case IPC_SEC_PIN_STATUS_INIT_COMPLETE:
    case IPC_SEC_PIN_STATUS_PB_INIT_COMPLETE:
    case IPC_SEC_PIN_STATUS_LOCK_FD:  // fixed dialling still lets the card work
        setState(SimState::Ready);
        break;
    case IPC_SEC_PIN_STATUS_LOCK_SC:
        if (status->facility_lock == IPC_SEC_FACILITY_LOCK_TYPE_SC_PUK_REQ)
            setState(SimState::PukRequired);
        else if (status->facility_lock == IPC_SEC_FACILITY_LOCK_TYPE_SC_CARD_BLOCKED)
            setState(SimState::PermanentlyBlocked);
        else
            setState(SimState::PinRequired);
        break;
    case IPC_SEC_PIN_STATUS_LOCK_PN:
    case IPC_SEC_PIN_STATUS_LOCK_PU:
    case IPC_SEC_PIN_STATUS_LOCK_PP:
    case IPC_SEC_PIN_STATUS_LOCK_PC:
    case IPC_SEC_PIN_STATUS_SIM_LOCK_REQUIRED:
        setState(SimState::NetworkLocked);
        break;
    case IPC_SEC_PIN_STATUS_CARD_NOT_PRESENT:
        imsi_.clear();
        iccid_.clear();
        apps_.clear();
        setState(SimState::Absent);
        break;
    case IPC_SEC_PIN_STATUS_CARD_ERROR:
    case IPC_SEC_PIN_STATUS_INSIDE_PF_ERROR:
        setState(SimState::Error);
        break;
    default:
        setState(SimState::NotReady);
        break;
    }
}

void Sim::refreshApps(std::function<void(bool)> done)
{
    modem_.request(IPC_SEC_SIM_APPS_INFO, IPC_TYPE_GET, {}, [this, done](const Result &r) {
        if (!r.ok || !r.response) {
            if (done)
                done(false);
            return;
        }

        const Bytes &data = r.response->data;
        const int count = ipc_sec_sim_apps_info_count(data.data(), data.size());
        apps_.clear();
        for (int i = 0; i < count; i++) {
            const ipc_sec_sim_app *a = ipc_sec_sim_apps_info_app(data.data(), data.size(), i);
            SimApp app;
            app.type = a->type;
            app.aid.assign(a->aid, a->aid + std::min<size_t>(a->aid_length, sizeof(a->aid)));
            app.pin1State = a->pin1_state;
            app.pin2State = a->pin2_state;
            apps_.push_back(app);
        }
        notify();
        if (done)
            done(true);
    });
}

void Sim::io(uint8_t command, uint16_t fileId, const std::vector<uint16_t> &path, uint8_t p1,
             uint8_t p2, uint8_t p3, const Bytes &data, const SimApp *app,
             std::function<void(std::optional<SimIoResult>)> done)
{
    ipc_sec_rsim_aid_request_header header;
    if (ipc_sec_rsim_aid_setup(&header, command, app ? app->type : kSimAppNone,
                               app ? app->aid.data() : nullptr, app ? app->aid.size() : 0,
                               fileId, path.data(), path.size(), p1, p2, p3) < 0) {
        done(std::nullopt);
        return;
    }

    Bytes payload = toBytes(header);
    payload.insert(payload.end(), data.begin(), data.end());

    modem_.request(IPC_SEC_RSIM_ACCESS, IPC_TYPE_GET, std::move(payload),
                   [done](const Result &r) {
                       if (!r.ok || !r.response) {
                           done(std::nullopt);
                           return;
                       }
                       auto h = r.response->as<ipc_sec_rsim_access_response_header>();
                       if (!h) {
                           done(std::nullopt);
                           return;
                       }
                       SimIoResult result;
                       result.sw1 = h->sw1;
                       result.sw2 = h->sw2;
                       const size_t offset = sizeof(*h);
                       const size_t length =
                           std::min<size_t>(h->length, r.response->data.size() - offset);
                       result.data.assign(r.response->data.begin() + offset,
                                          r.response->data.begin() + offset + length);
                       done(result);
                   });
}

void Sim::transmitApdu(const Bytes &apdu, std::function<void(std::optional<Bytes>)> done)
{
    ipc_sec_apdu_request_data request;
    if (ipc_sec_apdu_setup(&request, apdu.data(), apdu.size()) < 0) {
        done(std::nullopt);
        return;
    }

    modem_.request(IPC_SEC_GSIM_ACCESS, IPC_TYPE_GET, request, [done](const Result &r) {
        if (!r.ok || !r.response || r.response->data.size() < 2) {
            done(std::nullopt);
            return;
        }
        const Bytes &d = r.response->data;
        const size_t length = std::min<size_t>((d[0] << 8) | d[1], d.size() - 2);
        done(Bytes(d.begin() + 2, d.begin() + 2 + length));
    });
}

void Sim::authenticate(uint8_t type, const Bytes &rand, const Bytes &autn,
                       std::function<void(std::optional<SimAuthResult>)> done)
{
    Bytes payload(ipc_sec_ims_auth_request_size(rand.size(), autn.size()));
    if (ipc_sec_ims_auth_request_setup(payload.data(), payload.size(), type, rand.data(),
                                       rand.size(), autn.data(), autn.size()) < 0) {
        done(std::nullopt);
        return;
    }

    modem_.request(IPC_SEC_IMS_AUTH, IPC_TYPE_GET, std::move(payload), [done](const Result &r) {
        ipc_sec_ims_auth_result parsed;
        if (!r.ok || !r.response ||
            ipc_sec_ims_auth_response_parse(r.response->data.data(), r.response->data.size(),
                                            &parsed) < 0) {
            done(std::nullopt);
            return;
        }
        SimAuthResult result;
        result.type = parsed.type;
        result.status = parsed.status;
        for (unsigned int i = 0; i < parsed.count; i++)
            result.values.emplace_back(parsed.value[i], parsed.value[i] + parsed.length[i]);
        done(result);
    });
}

void Sim::enterPin(const std::string &pin, std::function<void(bool)> done)
{
    ipc_sec_pin_status_request_data request;
    if (ipc_sec_pin_status_setup(&request, IPC_SEC_PIN_TYPE_PIN1, pin.c_str(), nullptr) < 0) {
        done(false);
        return;
    }
    modem_.request(IPC_SEC_PIN_STATUS, IPC_TYPE_SET, request,
                   [done](const Result &r) { done(r.ok); });
}

}  // namespace shannond
