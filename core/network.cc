// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/network.h"

#include <cstdlib>

extern "C" {
#include <samsung-ipc.h>
}

namespace modemd {

namespace {

// Access technology values of the Shannon registration message
AccessTech techFromRaw(uint8_t raw)
{
    switch (raw) {
    case 0x01: return AccessTech::Gsm;
    case 0x02: return AccessTech::Gprs;
    case 0x03: return AccessTech::Edge;
    case 0x04:
    case 0x05: return AccessTech::Umts;
    case 0x21: return AccessTech::Lte;
    case 0x30: return AccessTech::Iwlan;
    default: return AccessTech::Unknown;
    }
}

RegState stateFromRaw(uint8_t raw)
{
    switch (raw) {
    case IPC_NET_REGISTRATION_STATUS_NONE: return RegState::NotRegistered;
    case IPC_NET_REGISTRATION_STATUS_HOME: return RegState::Home;
    case IPC_NET_REGISTRATION_STATUS_SEARCHING: return RegState::Searching;
    case IPC_NET_REGISTRATION_STATUS_EMERGENCY: return RegState::Denied;
    case IPC_NET_REGISTRATION_STATUS_ROAMING: return RegState::Roaming;
    default: return RegState::Unknown;
    }
}

}  // namespace

const char *regStateName(RegState state)
{
    switch (state) {
    case RegState::Unknown: return "unknown";
    case RegState::NotRegistered: return "not-registered";
    case RegState::Searching: return "searching";
    case RegState::Home: return "home";
    case RegState::Roaming: return "roaming";
    case RegState::Denied: return "denied";
    case RegState::EmergencyOnly: return "emergency-only";
    }
    return "?";
}

const char *accessTechName(AccessTech tech)
{
    switch (tech) {
    case AccessTech::Unknown: return "unknown";
    case AccessTech::Gsm: return "gsm";
    case AccessTech::Gprs: return "gprs";
    case AccessTech::Edge: return "edge";
    case AccessTech::Umts: return "umts";
    case AccessTech::Lte: return "lte";
    case AccessTech::Iwlan: return "iwlan";
    }
    return "?";
}

bool registered(RegState state)
{
    return state == RegState::Home || state == RegState::Roaming;
}

Network::Network(Modem &modem) : modem_(modem)
{
    // The modem announces itself once its stack runs
    modem_.onMessage(IPC_PWR_PHONE_PWR_UP, [this](const Message &) { powerUp(); });
    modem_.onMessage(IPC_PWR_PHONE_STATE, [this](const Message &m) {
        if (auto s = m.as<ipc_pwr_phone_state_response_data>()) {
            radioOn_ = s->state == IPC_PWR_PHONE_STATE_RESPONSE_NORMAL;
            notify();
        }
    });
    modem_.onMessage(IPC_NET_REGIST, [this](const Message &m) { regist(m); });
    modem_.onMessage(IPC_NET_SERVING_NETWORK, [this](const Message &m) {
        if (auto s = m.as<ipc_net_serving_network_data>()) {
            std::string plmn(s->plmn, sizeof(s->plmn));
            while (!plmn.empty() && (plmn.back() == '#' || plmn.back() == '\0'))
                plmn.pop_back();
            plmn_ = plmn;
            notify();
        }
    });
    modem_.onMessage(IPC_DISP_RSSI_INFO, [this](const Message &m) {
        if (auto r = m.as<ipc_disp_rssi_info_data>()) {
            rssi_ = r->rssi;
            notify();
        }
    });
    modem_.onMessage(IPC_MISC_ME_SN, [this](const Message &m) {
        auto r = m.as<ipc_misc_me_sn_response_data>();
        if (r && r->type == IPC_MISC_ME_SN_SERIAL_NUM) {
            char *sn = ipc_misc_me_sn_extract(&*r);
            if (sn) {
                imei_ = sn;
                free(sn);
            }
        }
    });
}

void Network::notify()
{
    for (auto &callback : changed_)
        callback();
}

void Network::powerUp()
{
    if (ready_)
        return;
    ready_ = true;
    modem_.log("modem stack is up");

    ipc_misc_me_sn_request_data sn{};
    sn.type = IPC_MISC_ME_SN_SERIAL_NUM;
    modem_.request(IPC_MISC_ME_SN, IPC_TYPE_GET, sn);

    // The modem starts with circuit switched service only; data needs PS too
    ipc_net_service_domain_config_data domain;
    ipc_net_service_domain_config_setup(&domain, IPC_NET_SERVICE_DOMAIN_CONFIG_CS_PS);
    modem_.request(IPC_NET_SERVICE_DOMAIN_CONFIG, IPC_TYPE_SET, domain);

    refresh();

    for (auto &callback : readyCallbacks_)
        callback();
}

void Network::refresh()
{
    for (uint8_t domain : {IPC_NET_SERVICE_DOMAIN_GSM, IPC_NET_SERVICE_DOMAIN_GPRS}) {
        ipc_net_regist_request_data request;
        ipc_net_regist_setup(&request, domain);
        modem_.request(IPC_NET_REGIST, IPC_TYPE_GET, request);
    }
    modem_.request(IPC_NET_SERVING_NETWORK, IPC_TYPE_GET);
}

void Network::regist(const Message &message)
{
    auto r = message.as<ipc_net_regist_response_data>();
    if (!r)
        return;

    Registration reg;
    reg.state = stateFromRaw(r->status);
    reg.rawTech = r->act;
    reg.tech = techFromRaw(r->act);
    reg.area = r->lac;
    reg.cell = r->cid;
    reg.rejectCause = r->fail_cause;

    // One record per domain; on LTE the modem also sends a combined one
    if (r->domain != IPC_NET_SERVICE_DOMAIN_GPRS)
        voice_ = reg;
    if (r->domain != IPC_NET_SERVICE_DOMAIN_GSM)
        data_ = reg;

    modem_.log("registration domain %u: %s on %s (raw %#x), cause %u", r->domain,
               regStateName(reg.state), accessTechName(reg.tech), reg.rawTech, reg.rejectCause);
    notify();
}

void Network::setRadioPower(bool on, std::function<void(bool)> done)
{
    ipc_pwr_phone_state_request_data state{};
    state.state = on ? IPC_PWR_PHONE_STATE_REQUEST_NORMAL : IPC_PWR_PHONE_STATE_REQUEST_LPM;

    auto send = [this, state, done]() {
        modem_.request(IPC_PWR_PHONE_STATE, IPC_TYPE_EXEC, state, [done](const Result &r) {
            if (done)
                done(r.ok);
        });
    };

    if (on)  // as the working open clients do: power-up first, then the state
        modem_.request(IPC_PWR_PHONE_PWR_UP, IPC_TYPE_EXEC, {}, [send](const Result &) { send(); });
    else
        send();
}

void Network::probeModes(std::function<void(uint8_t)> done)
{
    probeNext(0, 0, std::move(done));
}

void Network::probeNext(uint8_t bit, uint8_t accepted, std::function<void(uint8_t)> done)
{
    // Bits beyond the LTE one were refused by every modem tried so far
    if (bit > 3) {
        if (!accepted) {
            if (done)
                done(0);
            return;
        }
        ipc_net_mode_sel_data all{};
        all.mode_sel = accepted;
        modem_.request(IPC_NET_MODE_SEL, IPC_TYPE_SET, all, [this, accepted, done](const Result &r) {
            modes_ = r.ok ? accepted : 0;
            modem_.log("network modes %#x%s", accepted, r.ok ? "" : " refused");
            if (done)
                done(modes_);
        });
        return;
    }

    ipc_net_mode_sel_data one{};
    one.mode_sel = 1 << bit;
    modem_.request(IPC_NET_MODE_SEL, IPC_TYPE_SET, one,
                   [this, bit, accepted, done](const Result &r) {
                       probeNext(bit + 1, r.ok ? accepted | (1 << bit) : accepted, done);
                   });
}

}  // namespace modemd
