// SPDX-License-Identifier: GPL-2.0-or-later
//
// IMS registration of one UE (3GPP TS 24.229 5.1.1, TS 33.203): REGISTER
// with IMS AKA and the security agreement, the IPsec security associations,
// refresh before expiry (re-keyed when the network challenges again), the
// reg event subscription, P-CSCF failover and back-off, deregistration.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/alarm.h"
#include "core/ims/digest.h"
#include "core/ims/endpoint.h"
#include "core/ims/identity.h"
#include "core/ims/profile.h"

namespace shannond::ims {

enum class RegistrationState { Idle, Registering, Registered, Deregistering };
const char *registrationStateName(RegistrationState state);

// The SIM's answer to an AKA challenge
struct AkaAnswer {
    enum class Kind { Ok, Resync, Failed } kind = Kind::Failed;
    Bytes res, ck, ik;
    Bytes auts;
};
using AkaFn = std::function<void(const Bytes &rand, const Bytes &autn, std::function<void(AkaAnswer)> done)>;

struct RegistrationParams {
    ImsProfile profile;
    ImsIdentity identity;
    std::vector<std::string> pcscfs;  // addresses, in order of preference
    std::string instance;             // +sip.instance (IMEI URN)
    std::string userAgent;
    std::function<std::string()> accessNetworkInfo;  // P-Access-Network-Info, may be empty
};

// What a registration gives the rest of the stack
struct RegistrationInfo {
    std::string pcscf;
    std::vector<std::string> serviceRoute;
    std::vector<std::string> associatedUris;  // the IMPUs the network allows
    // The identity for every request but REGISTER: the first associated URI
    // (24.229 5.1.1.2; a temporary IMPU derived from the IMSI is barred)
    std::string defaultImpu;
    std::string contact;  // our Contact (name-addr with the feature tags)
    unsigned expires = 0;
};

class ImsRegistration {
public:
    using Changed = std::function<void(RegistrationState state, const std::string &why)>;

    ImsRegistration(EventLoop &loop, SipEndpoint &endpoint, ImsNetwork &network, AkaFn aka);
    ~ImsRegistration();
    ImsRegistration(const ImsRegistration &) = delete;
    ImsRegistration &operator=(const ImsRegistration &) = delete;

    void setLog(std::function<void(const std::string &)> log) { log_ = std::move(log); }
    void onChanged(Changed changed) { changed_ = std::move(changed); }

    // Registers, and keeps the registration up until stop() or abort()
    void start(RegistrationParams params);
    // Deregisters; done when the network answered or gave up
    void stop(std::function<void()> done = {});
    // The PDN is gone: everything dropped without a message
    void abort();

    RegistrationState state() const { return state_; }
    const RegistrationInfo &info() const { return info_; }
    const RegistrationParams &params() const { return params_; }
    // Where requests to the network go once registered
    Route route() const;
    // Builds the parts every request after registration carries: Route set,
    // P-Access-Network-Info, User-Agent. Requests inside a dialog follow
    // the dialog's route set instead (withRoute false).
    void decorate(SipMessage &request, bool withRoute = true) const;

    // A request from the network: true if it was the registration's (the
    // reg event NOTIFY)
    bool handleRequest(const SipMessage &request, const Source &source);

    // Seconds until the next attempt after n failures (24.229 5.1.1.9 style
    // back-off, the profile's base and maximum)
    std::chrono::seconds backoff(unsigned failures) const;

private:
    struct Attempt;

    void log(const std::string &line);
    void setState(RegistrationState state, const std::string &why);
    void begin();
    void sendRegister(bool protectedFlow, unsigned expires);
    void registerAnswered(const SipMessage *response, bool protectedFlow, unsigned expires);
    void challenged(const SipMessage &response, bool protectedFlow);
    void akaAnswered(AkaAnswer answer, unsigned expires);
    void registered(const SipMessage &response);
    void failed(const std::string &why, bool nextPcscf, std::optional<std::chrono::seconds> retryAfter = {});
    void refresh();
    void subscribe();
    void clearSecurity();
    bool reserveSpis();
    void releaseReserved();
    std::string contactUri() const;
    std::string contactHeader(unsigned expires) const;
    std::string authorization(const std::string &method, const std::string &uri);

    EventLoop &loop_;
    SipEndpoint &endpoint_;
    ImsNetwork &network_;
    AkaFn aka_;
    std::function<void(const std::string &)> log_;
    Changed changed_;
    RegistrationParams params_;
    RegistrationState state_ = RegistrationState::Idle;
    RegistrationInfo info_;

    size_t pcscfIndex_ = 0;
    unsigned failures_ = 0;
    unsigned expires_ = 0;     // asked for
    uint16_t portUc_ = 0, portUs_ = 0;  // our protected ports, for this registration
    std::string callId_;
    std::string fromTag_;
    uint32_t cseq_ = 0;
    std::string pendingBranch_;

    // Security
    uint32_t reservedUc_ = 0, reservedUs_ = 0;  // offered in Security-Client
    std::optional<SaSet> sas_;                  // in use
    std::vector<std::string> securityServer_;   // echoed in Security-Verify
    std::optional<DigestChallenge> challenge_;
    Bytes res_;
    unsigned nc_ = 0;
    std::optional<SecurityMechanism> chosen_;
    std::optional<Bytes> auts_;
    bool akaFailed_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // Reg event subscription
    std::string subCallId_;
    std::string subFromTag_;
    std::string subToTag_;
    uint32_t subCseq_ = 0;

    AlarmTimer retryTimer_;  // wakes a sleeping phone, as the refresh does
    AlarmTimer refreshTimer_;
    AlarmTimer subTimer_;    // the reg event subscription's refresh
    unsigned challenges_ = 0;  // 401s of this attempt
    // Registrations the network ended, one soon after the other
    unsigned ended_ = 0;
    std::optional<std::chrono::steady_clock::time_point> lastEnded_;
    std::function<void()> stopped_;
};

}  // namespace shannond::ims
