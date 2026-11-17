// SPDX-License-Identifier: GPL-2.0-or-later
//
// Supplementary services (3GPP TS 22.004: call waiting, line identity,
// forwarding, barring) and USSD, as the stock RIL drives the modem. Every
// request here goes to the network; none is made unless a front end asks.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/modem.h"

namespace shannond {

// Android's UssdModeType: the modem's state minus one
enum class UssdMode { Notify = 0, Request, NetworkRelease, OtherClient, NotSupported, Timeout };

// The outcome of a request: refused by the modem or the network with the
// modem's GEN_PHONE_RES code, or not answered at all
struct SsStatus {
    bool ok = false;
    bool timeout = false;
    uint16_t code = 0;
};

// One forwarding rule (Android's CallForwardInfo)
struct CallForward {
    bool active = false;
    int reason = 0;        // 0 unconditional, 1 busy, 2 no reply, 3 not reachable, 4 all, 5 all conditional
    int serviceClass = 0;  // 27.007 class bits
    int toa = 0x81;
    std::string number;
    int timeSeconds = 0;
};

// A notification during a call (Android's SuppSvcNotification)
struct SsNotification {
    bool mt = false;  // about an incoming call
    int code = 0;     // 27.007 +CSSI (MO) or +CSSU (MT) code
    int index = 0;
    int toa = 0;
    std::string number;
};

class Ss {
public:
    using Done = std::function<void(const SsStatus &)>;

    explicit Ss(Modem &modem);

    // USSD
    void onUssd(std::function<void(UssdMode, const std::string &)> callback)
    {
        ussd_ = std::move(callback);
    }
    // A new request, or the answer to the network's question
    void sendUssd(const std::string &text, Done done);
    void cancelUssd(Done done);

    // Notifications during calls
    void onNotification(std::function<void(const SsNotification &)> callback)
    {
        notification_ = std::move(callback);
    }

    // Service classes are 27.007 bits (1 voice, 2 data, 4 fax, 8 SMS, ...);
    // 0 is all of them
    void queryCallWaiting(int serviceClass, std::function<void(const SsStatus &, bool enabled, int classes)> done);
    void setCallWaiting(bool enable, int serviceClass, Done done);

    // Android's ClipStatus: 0 provisioned, 1 not provisioned, 2 unknown
    void queryClip(std::function<void(const SsStatus &, int status)> done);
    // CLIR: n is the setting for outgoing calls (0 network default, 1 hide,
    // 2 show), kept here; m the network's state (27.007 +CLIR)
    int clirSetting() const { return clir_; }
    void queryClir(std::function<void(const SsStatus &, int n, int m)> done);
    void setClir(int n, Done done);

    // mode: 0 disable, 1 enable, 2 interrogate, 3 register, 4 erase (Android's)
    void queryCallForward(int reason, int serviceClass,
                          std::function<void(const SsStatus &, const std::vector<CallForward> &)> done);
    void setCallForward(int mode, const CallForward &rule, Done done);

    // Facilities as in 27.007 +CLCK (AO, OI, OX, AI, IR, AB, AG, AC)
    void queryBarring(const std::string &facility, int serviceClass,
                      std::function<void(const SsStatus &, int classes)> done);
    void setBarring(const std::string &facility, bool lock, const std::string &password,
                    int serviceClass, Done done);
    void changeBarringPassword(const std::string &oldPassword, const std::string &newPassword, Done done);

    static bool isBarringFacility(const std::string &facility);

private:
    void ussdReceived(const Message &message);
    void infoReceived(const Message &message);
    void request(uint16_t command, uint8_t type, Bytes data, std::function<void(const SsStatus &, const Message *)> done);

    Modem &modem_;
    std::function<void(UssdMode, const std::string &)> ussd_;
    std::function<void(const SsNotification &)> notification_;
    bool ussdReplyPending_ = false;
    int clir_ = 0;
};

// Service class conversions of the stock RIL (ConvertSsClassTypeToIpc and
// ConvertIpcToSsClassType)
uint8_t ssClassToIpc(int serviceClass);
int ssClassFromIpc(uint8_t ipcClass);

}  // namespace shannond
