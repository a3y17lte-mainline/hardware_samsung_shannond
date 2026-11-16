// SPDX-License-Identifier: GPL-2.0-or-later
//
// The SIM card: its state, identities, file access, logical channels and
// the authentication IMS needs.
//
// Nothing here enters a PIN on its own. Wrong PINs block the card, so PIN
// entry is only ever a direct request of the user through a front end.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/modem.h"

namespace shannond {

enum class SimState { Unknown, Absent, NotReady, PinRequired, PukRequired, NetworkLocked, Ready, Error };

const char *simStateName(SimState state);

struct SimApp {
    uint8_t type = 0;
    Bytes aid;
    uint8_t pin1State = 0;
    uint8_t pin2State = 0;
};

struct SimIoResult {
    uint8_t sw1 = 0;
    uint8_t sw2 = 0;
    Bytes data;
};

struct SimAuthResult {
    uint8_t type = 0;
    uint8_t status = 0;
    std::vector<Bytes> values;  // RES, CK, IK for AKA
};

class Sim {
public:
    explicit Sim(Modem &modem);

    SimState state() const { return state_; }
    const std::string &imsi() const { return imsi_; }
    const std::string &iccid() const { return iccid_; }
    const std::vector<SimApp> &apps() const { return apps_; }

    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }

    void refreshApps(std::function<void(bool ok)> done = {});

    // READ BINARY, READ RECORD, GET RESPONSE, ... on a file, in the USIM or,
    // with app, in that application (the ISIM)
    void io(uint8_t command, uint16_t fileId, const std::vector<uint16_t> &path, uint8_t p1,
            uint8_t p2, uint8_t p3, const Bytes &data, const SimApp *app,
            std::function<void(std::optional<SimIoResult>)> done);

    // An APDU on a logical channel; the answer is the response data with
    // SW1 SW2 at the end
    void transmitApdu(const Bytes &apdu, std::function<void(std::optional<Bytes>)> done);

    // AKA with RAND and AUTN (type 0)
    void authenticate(uint8_t type, const Bytes &rand, const Bytes &autn,
                      std::function<void(std::optional<SimAuthResult>)> done);

    // Only on the user's explicit request
    void enterPin(const std::string &pin, std::function<void(bool ok)> done);

private:
    void pinStatus(const Message &message);
    void setState(SimState state);
    void notify();

    Modem &modem_;
    SimState state_ = SimState::Unknown;
    std::string imsi_;
    std::string iccid_;
    std::vector<SimApp> apps_;
    std::vector<std::function<void()>> changed_;
};

}  // namespace shannond
