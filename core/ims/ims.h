// SPDX-License-Identifier: GPL-2.0-or-later
//
// When to run IMS and with what: once VoLTE is wanted, the card is ready,
// the phone is on LTE where the network supports voice over PS, and the
// operator has a profile, the IMS PDN comes up (ours, or the one Android's
// telephony brought up), the identities are read from the card and the UE
// registers. The modem is told about the registration (IPC_IMS_REG_STATE)
// as the stock RIL tells it. No Android types: the front ends supply the
// sockets (ImsNetwork) for the PDN.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/data.h"
#include "core/ims/call.h"
#include "core/ims/endpoint.h"
#include "core/ims/profile.h"
#include "core/ims/registration.h"
#include "core/ims/voice_engine.h"
#include "core/network.h"
#include "core/sim.h"

namespace shannond::ims {

// SRVCC as the modem reports it (IPC_CALL_SRVCC): the network moves the
// VoLTE calls to CS when LTE coverage ends
enum class SrvccState { Started, Completed, Failed };

// The IMS call list the modem gets when SRVCC starts (IPC_CALL_LIST, set),
// as the stock RIL builds it: each call with the transaction identifier it
// will have on CS (3GPP TS 24.237 12.2.4: active 0, held 1, a third call 2)
Bytes srvccCallList(const std::vector<ImsCallInfo> &calls);

class Ims {
public:
    using NetworkFactory = std::function<std::unique_ptr<ImsNetwork>(const DataCall &pdn)>;

    struct Options {
        // Connect the IMS PDN ourselves (Linux) or wait for one (Android,
        // where telephony brings it up)
        bool ownPdn = true;
        // As the stock RIL (GetCidFromApnType: IMS 1, internet 2): the
        // modem's voice engine takes the IMS PDN only on the context of
        // the IMS profile's id
        unsigned pdnCid = 1;
        // The phone as its stock firmware names itself (User-Agent)
        DeviceInfo device;
        // A profile for any network (trying an unknown one by hand)
        std::optional<ImsProfile> profile;
        // Register even if the cell did not say it supports voice over PS
        bool ignoreVops = false;
        std::chrono::milliseconds addressWait{30000};
        std::chrono::seconds pdnRetry{30};
    };

    Ims(Modem &modem, Sim &sim, Network &network, Data &data, NetworkFactory factory);
    Ims(Modem &modem, Sim &sim, Network &network, Data &data, NetworkFactory factory, Options options);
    ~Ims();
    Ims(const Ims &) = delete;
    Ims &operator=(const Ims &) = delete;

    // The user's VoLTE switch
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    RegistrationState state() const;
    bool registered() const { return state() == RegistrationState::Registered; }
    // A line for status output: what IMS waits for or is doing
    std::string status() const;

    // VoLTE calls (attached to the SIP side while registered)
    ImsCalls &calls() { return *calls_; }
    ImsRegistration *registration() { return registration_.get(); }
    SipEndpoint *endpoint() { return endpoint_.get(); }
    // Requests neither the registration nor the calls take
    void onRequest(SipEndpoint::RequestHandler handler) { requestHandler_ = std::move(handler); }
    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }
    // SRVCC of our calls (not reported without a VoLTE call, as stock). On
    // Completed the calls have ended here and are in the modem's CS call list.
    void onSrvcc(std::function<void(SrvccState)> callback) { srvcc_.push_back(std::move(callback)); }

    // The SIM's AKA (also what the tests replace)
    void authenticate(const Bytes &rand, const Bytes &autn, std::function<void(AkaAnswer)> done);

private:
    enum class Phase { Off, Pdn, Address, Identity, Running, Stopping };

    void log(const std::string &line);
    void evaluate();
    std::optional<ImsProfile> currentProfile() const;
    bool wanted(std::string &why) const;
    void connectPdn(const ImsProfile &profile);
    void pdnUp(const DataCall &call);
    void waitForAddress(std::chrono::steady_clock::time_point deadline);
    void readIdentity();
    void startRegistration(const ImsIdentity &identity);
    void stop(bool deregister);
    void finishStop();
    void dataChanged(const DataCall &call);
    void registrationChanged(RegistrationState state, const std::string &why);
    void tellModem(bool registered);
    void srvcc(const Message &message);
    std::string accessNetworkInfo() const;
    void notify();

    Modem &modem_;
    EventLoop &loop_;
    Sim &sim_;
    Network &network_;
    Data &data_;
    NetworkFactory factory_;
    Options options_;
    bool enabled_ = false;
    Phase phase_ = Phase::Off;
    std::string why_ = "off";
    std::optional<ImsProfile> profile_;
    std::optional<DataCall> pdn_;
    std::unique_ptr<ImsNetwork> imsNetwork_;
    std::unique_ptr<SipEndpoint> endpoint_;
    std::unique_ptr<ImsRegistration> registration_;
    bool modemTold_ = false;  // the modem starts with no IMS registration
    EventLoop::TimerId timer_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    SipEndpoint::RequestHandler requestHandler_;
    std::unique_ptr<VoiceEngine> voice_;
    std::unique_ptr<ImsCalls> calls_;
    std::vector<std::function<void()>> changed_;
    std::vector<std::function<void(SrvccState)>> srvcc_;
};

}  // namespace shannond::ims
