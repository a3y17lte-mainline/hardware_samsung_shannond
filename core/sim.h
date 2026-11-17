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

enum class SimState {
    Unknown,
    Absent,
    NotReady,
    PinRequired,
    PukRequired,
    PermanentlyBlocked,  // PUK attempts used up
    NetworkLocked,       // a personalisation lock: lockStatus() says which
    Ready,
    Error,
};

const char *simStateName(SimState state);

// The modem's application types (IPC_SEC_SIM_APPS_INFO and the file access
// request), which differ from Android's
constexpr uint8_t kSimAppUsim = 0;
constexpr uint8_t kSimAppIsim = 1;
constexpr uint8_t kSimAppNone = 6;  // a file access without an application

struct SimApp {
    uint8_t type = 0;  // kSimApp*
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
    // The raw lock of the last IPC_SEC_PIN_STATUS (IPC_SEC_PIN_STATUS_LOCK_*,
    // SIM_LOCK_REQUIRED) and its facility lock type (1 = code, 2 = PUK)
    uint8_t lockStatus() const { return lockStatus_; }
    uint8_t facilityLock() const { return facilityLock_; }

    void onChanged(std::function<void()> callback) { changed_.push_back(std::move(callback)); }

    void refreshApps(std::function<void(bool ok)> done = {});

    // READ BINARY, READ RECORD, GET RESPONSE, ... on a file, in the USIM or,
    // with app, in that application (the ISIM)
    // Asks the modem for the card's state: for a start without the
    // modem's own start-up messages
    void query();

    void io(uint8_t command, uint16_t fileId, const std::vector<uint16_t> &path, uint8_t p1,
            uint8_t p2, uint8_t p3, const Bytes &data, const SimApp *app,
            std::function<void(std::optional<SimIoResult>)> done);

    // An APDU on a logical channel; the answer is the response data with
    // SW1 SW2 at the end
    void transmitApdu(const Bytes &apdu, std::function<void(std::optional<Bytes>)> done);

    // The AKA type for an application, as the stock RIL picks it: IMS AKA
    // (IPC_SEC_IMS_AUTH_TYPE_IMS) only for the card's ISIM, else USIM AKA.
    // An empty AID: the ISIM if the card has one (for IMS)
    uint8_t akaType(const Bytes &aid = {}) const;
    // AKA with RAND and AUTN
    void authenticate(uint8_t type, const Bytes &rand, const Bytes &autn,
                      std::function<void(std::optional<SimAuthResult>)> done);

    // Only on the user's explicit request
    void enterPin(const std::string &pin, std::function<void(bool ok)> done);

private:
    void pinStatus(const Message &message);
    void setIccid(const uint8_t *data, size_t size);
    void readIccid();
    void setState(SimState state);
    void notify();

    Modem &modem_;
    SimState state_ = SimState::Unknown;
    std::string imsi_;
    std::string iccid_;
    bool queried_ = false;
    std::vector<SimApp> apps_;
    uint8_t lockStatus_ = 0;
    uint8_t facilityLock_ = 0;
    std::vector<std::function<void()>> changed_;
};

}  // namespace shannond
