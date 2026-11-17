// SPDX-License-Identifier: GPL-2.0-or-later
//
// Radio power, network registration, operator and signal.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/modem.h"

namespace shannond {

enum class RegState { Unknown, NotRegistered, Searching, Home, Roaming, Denied, EmergencyOnly };
enum class AccessTech { Unknown, Gsm, Gprs, Edge, Umts, Lte, Iwlan };

const char *regStateName(RegState state);
const char *accessTechName(AccessTech tech);
bool registered(RegState state);

struct Registration {
    RegState state = RegState::Unknown;
    AccessTech tech = AccessTech::Unknown;
    uint8_t rawTech = 0;
    uint16_t area = 0;      // LAC or TAC
    uint32_t cell = 0;
    uint8_t rejectCause = 0;
};

// Mode bits of IPC_NET_MODE_SEL on Shannon
constexpr uint8_t kModeGsm = 0x01;
constexpr uint8_t kModeUmts = 0x02;
constexpr uint8_t kModeLte = 0x08;

// LTE signal as the modem reports it; a field is empty while unknown
struct LteSignal {
    std::optional<int> strength;  // 0 to 31
    std::optional<int> rsrp;      // -dBm
    std::optional<int> rsrq;      // -dB
    std::optional<int> snr;       // 0.1 dB
    std::optional<int> cqi;
};

class Network {
public:
    explicit Network(Modem &modem);

    bool modemReady() const { return ready_; }
    const std::string &imei() const { return imei_; }
    const Registration &voice() const { return voice_; }   // circuit switched
    const Registration &data() const { return data_; }     // packet switched
    const std::string &operatorCode() const { return plmn_; }  // MCC MNC
    // The names the network sent for itself, empty if it sent none
    const std::string &operatorLongName() const { return longName_; }
    const std::string &operatorShortName() const { return shortName_; }
    int signal() const { return rssi_; }  // raw IPC_DISP_RSSI_INFO: -dBm, -1 if unknown
    // The LTE measurements that come with it, in Android's units
    const LteSignal &lteSignal() const { return lte_; }
    const std::string &version() const { return version_; }  // modem software
    bool radioOn() const { return radioOn_; }
    uint8_t modes() const { return modes_; }

    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }
    void onReady(std::function<void()> callback) { readyCallbacks_.push_back(std::move(callback)); }

    // Whether the modem is running although it never announced itself (a
    // client that starts after the modem did). If so, the stack counts as
    // up and the radio is switched off, which ends whatever registration,
    // data contexts and calls an earlier client left
    void probeRunning(std::function<void(bool running)> done);

    void setRadioPower(bool on, std::function<void(bool ok)> done = {});
    // Asks for the modes in order and keeps the ones the modem accepts
    void probeModes(std::function<void(uint8_t modes)> done = {});
    void setModes(uint8_t modes, std::function<void(bool ok)> done = {});
    void refresh();
    // Whether anyone looks at the screen: the modem sends less while not
    void setScreenOn(bool on);

private:
    void powerUp();
    void regist(const Message &message);
    void probeNext(uint8_t bit, uint8_t accepted, std::function<void(uint8_t)> done);
    void notify();

    Modem &modem_;
    bool ready_ = false;
    bool radioOn_ = false;
    std::string imei_;
    std::string version_;
    Registration voice_;
    Registration data_;
    std::string plmn_;
    std::string longName_;
    std::string shortName_;
    int rssi_ = -1;
    LteSignal lte_;
    uint8_t modes_ = 0;
    std::optional<bool> screenOn_;
    std::vector<std::function<void()>> changed_;
    std::vector<std::function<void()>> readyCallbacks_;
};

}  // namespace shannond
