// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/ims.h"

#include <arpa/inet.h>

#include "core/ims/isim.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond::ims {

namespace {

std::string ipv4String(const std::array<uint8_t, 4> &a)
{
    char buf[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, a.data(), buf, sizeof(buf));
}

std::string ipv6String(const std::array<uint8_t, 16> &a)
{
    char buf[INET6_ADDRSTRLEN];
    return inet_ntop(AF_INET6, a.data(), buf, sizeof(buf));
}

}  // namespace

Ims::Ims(Modem &modem, Sim &sim, Network &network, Data &data, NetworkFactory factory)
    : Ims(modem, sim, network, data, std::move(factory), Options{})
{
}

Ims::Ims(Modem &modem, Sim &sim, Network &network, Data &data, NetworkFactory factory, Options options)
    : modem_(modem), loop_(modem.loop()), sim_(sim), network_(network), data_(data), factory_(std::move(factory)),
      options_(std::move(options))
{
    voice_ = std::make_unique<VoiceEngine>(modem_);
    calls_ = std::make_unique<ImsCalls>(loop_, *voice_);
    calls_->setLog([this](const std::string &l) { log(l); });
    auto alive = alive_;
    sim_.onChanged([this, alive]() {
        if (*alive)
            evaluate();
    });
    network_.onChanged([this, alive]() {
        if (*alive)
            evaluate();
    });
    data_.onChanged([this, alive](const DataCall &call) {
        if (*alive)
            dataChanged(call);
    });
}

Ims::~Ims()
{
    *alive_ = false;
    loop_.cancel(timer_);
    calls_.reset();
    voice_.reset();
    registration_.reset();
    endpoint_.reset();
    imsNetwork_.reset();
}

void Ims::log(const std::string &line)
{
    modem_.log("IMS: %s", line.c_str());
}

void Ims::notify()
{
    for (auto &c : changed_)
        c();
}

RegistrationState Ims::state() const
{
    return registration_ ? registration_->state() : RegistrationState::Idle;
}

std::string Ims::status() const
{
    static const char *phases[] = {"off", "PDN", "address", "identity", "running", "stopping"};
    std::string s = std::string(enabled_ ? "on" : "off") + ", " + phases[static_cast<int>(phase_)];
    if (registration_)
        s += std::string(", ") + registrationStateName(registration_->state());
    return s + " (" + why_ + ")";
}

void Ims::setEnabled(bool enabled)
{
    if (enabled == enabled_)
        return;
    enabled_ = enabled;
    log(enabled ? "VoLTE on" : "VoLTE off");
    // Switched on by hand: no wait for the retry after an earlier stop
    if (enabled && phase_ == Phase::Off && timer_) {
        loop_.cancel(timer_);
        timer_ = 0;
    }
    evaluate();
}

std::optional<ImsProfile> Ims::currentProfile() const
{
    if (options_.profile)
        return options_.profile;
    if (const ImsProfile *p = profileFor(network_.operatorCode()))
        return *p;
    return std::nullopt;
}

bool Ims::wanted(std::string &why) const
{
    const shannond::Registration &data = network_.data();
    if (!enabled_)
        why = "VoLTE off";
    else if (sim_.state() != SimState::Ready || sim_.imsi().empty())
        why = "SIM not ready";
    else if (!shannond::registered(data.state))
        why = "not registered";
    else if (data.tech != AccessTech::Lte)
        why = "not on LTE";
    else if (!options_.ignoreVops && data.volte != true)
        why = "the cell offers no voice over PS";
    else if (!currentProfile())
        why = "no IMS profile for " + network_.operatorCode();
    else
        return true;
    return false;
}

void Ims::evaluate()
{
    std::string why;
    const bool want = wanted(why);
    if (!want) {
        if (phase_ != Phase::Off && phase_ != Phase::Stopping) {
            log("stopping: " + why);
            stop(true);
        }
        if (why_ != why) {
            why_ = why;
            notify();
        }
        return;
    }
    if (phase_ != Phase::Off || timer_)
        return;
    profile_ = currentProfile();
    if (options_.ownPdn) {
        connectPdn(*profile_);
        return;
    }
    for (const auto &[cid, call] : data_.calls())
        if (call.ims && call.active) {
            pdnUp(call);
            return;
        }
    why_ = "waiting for the IMS PDN";
    notify();
}

void Ims::connectPdn(const ImsProfile &profile)
{
    phase_ = Phase::Pdn;
    why_ = "IMS PDN coming up";
    notify();
    DataProfile p;
    p.apn = profile.apn;
    p.protocol = profile.ipv6 ? IpProtocol::Ipv6 : IpProtocol::Ipv4v6;
    p.ims = true;
    auto alive = alive_;
    const unsigned cid = options_.pdnCid;
    data_.connect(cid, p, [this, alive, cid](std::optional<DataCall> call) {
        if (!*alive)
            return;
        if (phase_ != Phase::Pdn) {  // stopped meanwhile
            if (call)
                data_.disconnect(cid);
            return;
        }
        if (!call) {
            phase_ = Phase::Off;
            why_ = "IMS PDN failed (cause " + std::to_string(data_.lastFailCause(cid)) + "), again in " +
                   std::to_string(options_.pdnRetry.count()) + " s";
            log(why_);
            timer_ = loop_.after(options_.pdnRetry, [this, alive]() {
                if (!*alive)
                    return;
                timer_ = 0;
                evaluate();
            });
            notify();
            return;
        }
        pdnUp(*call);
    });
}

void Ims::pdnUp(const DataCall &call)
{
    pdn_ = call;
    imsNetwork_ = factory_ ? factory_(call) : nullptr;
    if (!imsNetwork_) {
        why_ = "no sockets for the IMS PDN";
        stop(false);
        return;
    }
    endpoint_ = std::make_unique<SipEndpoint>(loop_, *imsNetwork_, profile_->timers);
    endpoint_->setLog([this](const std::string &l) { log(l); });
    endpoint_->onRequest([this](const SipMessage &request, const Source &source) {
        if (registration_ && registration_->handleRequest(request, source))
            return;
        if (calls_->handleRequest(request, source))
            return;
        if (requestHandler_)
            requestHandler_(request, source);
        else
            endpoint_->respond(source, request, request.makeResponse(501, "Not Implemented"));
    });
    phase_ = Phase::Address;
    why_ = "waiting for an address on " + call.iface;
    log("IMS PDN up on " + call.iface + ", " + std::to_string(call.pcscf4.size() + call.pcscf6.size()) +
        " P-CSCF");
    notify();
    waitForAddress(std::chrono::steady_clock::now() + options_.addressWait);
}

void Ims::waitForAddress(std::chrono::steady_clock::time_point deadline)
{
    if (phase_ != Phase::Address)
        return;
    if (!imsNetwork_->localAddress().empty()) {
        readIdentity();
        return;
    }
    auto alive = alive_;
    if (std::chrono::steady_clock::now() > deadline) {
        why_ = "no address on the IMS PDN";
        log(why_);
        stop(false);
        return;
    }
    timer_ = loop_.after(std::chrono::milliseconds(500), [this, alive, deadline]() {
        if (!*alive)
            return;
        timer_ = 0;
        waitForAddress(deadline);
    });
}

void Ims::readIdentity()
{
    phase_ = Phase::Identity;
    why_ = "reading the IMS identity";
    auto alive = alive_;
    readImsIdentity(sim_, network_.operatorCode(), [this, alive](std::optional<ImsIdentity> id) {
        if (!*alive || phase_ != Phase::Identity)
            return;
        if (!id) {
            why_ = "no IMS identity";
            log(why_);
            stop(false);
            return;
        }
        log(std::string("identity from the ") + (id->fromIsim ? "ISIM" : "IMSI"));
        startRegistration(*id);
    });
}

void Ims::startRegistration(const ImsIdentity &identity)
{
    phase_ = Phase::Running;
    registration_ = std::make_unique<ImsRegistration>(
        loop_, *endpoint_, *imsNetwork_,
        [this](const Bytes &rand, const Bytes &autn, std::function<void(AkaAnswer)> done) {
            authenticate(rand, autn, std::move(done));
        });
    registration_->setLog([this](const std::string &l) { log(l); });
    registration_->onChanged(
        [this](RegistrationState state, const std::string &why) { registrationChanged(state, why); });

    RegistrationParams p;
    p.profile = *profile_;
    p.identity = identity;
    // P-CSCFs of the local address's IP version first
    const bool v6 = imsNetwork_->localAddress().find(':') != std::string::npos;
    std::vector<std::string> v4s, v6s;
    for (const auto &a : pdn_->pcscf4)
        v4s.push_back(ipv4String(a));
    for (const auto &a : pdn_->pcscf6)
        v6s.push_back(ipv6String(a));
    p.pcscfs = v6 ? v6s : v4s;
    p.instance = instanceId(network_.imei());
    p.userAgent = userAgentFor(*profile_, options_.device);
    p.accessNetworkInfo = [this]() { return accessNetworkInfo(); };
    if (p.pcscfs.empty()) {
        why_ = std::string("the IMS PDN gave no ") + (v6 ? "IPv6" : "IPv4") + " P-CSCF";
        log(why_);
        stop(false);
        return;
    }
    registration_->start(p);
}

void Ims::registrationChanged(RegistrationState state, const std::string &why)
{
    if (phase_ == Phase::Running)
        why_ = why;
    if (state == RegistrationState::Registered && !calls_->attached() && endpoint_ && registration_) {
        VoiceEngineConfig c;
        c.portBase = profile_->audioPort;
        c.rtpTimeout = profile_->rtpTimeout;
        c.rtcpTimeout = profile_->rtcpTimeout;
        c.rtcpOnCall = profile_->rtcpOnCall;
        c.rtcpRs = profile_->media.bandwidthRs;
        c.rtcpRr = profile_->media.bandwidthRr;
        voice_->setConfig(c);
        calls_->attach(*endpoint_, *registration_, profile_->apn);
    } else if (state != RegistrationState::Registered && calls_->attached()) {
        calls_->detach();
    }
    tellModem(state == RegistrationState::Registered);
    notify();
}

void Ims::tellModem(bool registered)
{
    if (modemTold_ == registered)
        return;
    modemTold_ = registered;
    // As the stock RIL reports every change of the IMS registration
    ipc_ims_reg_state_data d{};
    d.status = registered ? IPC_IMS_REG_STATUS_REGISTERED : IPC_IMS_REG_STATUS_NOT_REGISTERED;
    d.features = registered ? IPC_IMS_FEATURE_VOICE : 0;
    d.rat = IPC_IMS_RAT_LTE;
    modem_.request(IPC_IMS_REG_STATE, IPC_TYPE_SET, d, [this](const Result &r) {
        if (!r.ok)
            log("the modem did not take the registration state");
    });
}

void Ims::stop(bool deregister)
{
    loop_.cancel(timer_);
    timer_ = 0;
    const bool wasPdn = phase_ == Phase::Pdn;
    phase_ = Phase::Stopping;
    // Calls end first: their BYEs and CANCELs go out while we still can
    if (calls_->attached()) {
        for (const auto &c : calls_->calls())
            calls_->hangup(c.id);
        calls_->detach();
    }
    if (registration_ && deregister && registration_->state() == RegistrationState::Registered) {
        auto alive = alive_;
        registration_->stop([this, alive]() {
            if (*alive)
                loop_.post([this, alive]() {
                    if (*alive)
                        finishStop();
                });
        });
        return;
    }
    if (wasPdn) {  // the connect callback disconnects what comes up
        phase_ = Phase::Off;
        notify();
        return;
    }
    finishStop();
}

void Ims::finishStop()
{
    calls_->detach();
    registration_.reset();
    endpoint_.reset();
    imsNetwork_.reset();
    if (options_.ownPdn && pdn_)
        data_.disconnect(pdn_->cid);
    pdn_.reset();
    tellModem(false);
    phase_ = Phase::Off;
    std::string why;
    if (!wanted(why))
        why_ = why;
    notify();
    // Conditions may hold again (or still): look once more, later
    auto alive = alive_;
    timer_ = loop_.after(options_.pdnRetry, [this, alive]() {
        if (!*alive)
            return;
        timer_ = 0;
        evaluate();
    });
}

void Ims::dataChanged(const DataCall &call)
{
    if (pdn_ && call.cid == pdn_->cid) {
        if (!call.active && phase_ != Phase::Stopping && phase_ != Phase::Off) {
            why_ = "IMS PDN lost";
            log(why_);
            if (registration_)
                registration_->abort();
            stop(false);
            return;
        }
        if (call.active)
            pdn_ = call;
        return;
    }
    if (!options_.ownPdn && call.ims && call.active && phase_ == Phase::Off)
        evaluate();
}

std::string Ims::accessNetworkInfo() const
{
    const shannond::Registration &d = network_.data();
    if (d.tech != AccessTech::Lte || network_.operatorCode().empty())
        return {};
    return ims::accessNetworkInfo(network_.operatorCode(), d.tac ? d.tac : d.area, d.cell);
}

void Ims::authenticate(const Bytes &rand, const Bytes &autn, std::function<void(AkaAnswer)> done)
{
    // On the ISIM if the card has one, else on the USIM (33.203 6.1)
    sim_.authenticate(sim_.akaType(), rand, autn, [this, done](std::optional<SimAuthResult> r) {
        // Lengths only: the values are keys
        std::string shape = r ? "status " + std::to_string(r->status) + ", lengths" : "no answer";
        if (r)
            for (const auto &v : r->values)
                shape += " " + std::to_string(v.size());
        log("AKA answer from the SIM: " + shape);
        AkaAnswer a;
        if (r && r->status == 0 && r->values.size() >= 3) {
            a.kind = AkaAnswer::Kind::Ok;
            a.res = r->values[0];
            a.ck = r->values[1];
            a.ik = r->values[2];
        } else if (r && (r->status == 4 || r->status == 5) && !r->values.empty() && r->values[0].size() == 14) {
            a.kind = AkaAnswer::Kind::Resync;
            a.auts = r->values[0];
        }
        done(a);
    });
}

}  // namespace shannond::ims
