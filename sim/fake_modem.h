// SPDX-License-Identifier: GPL-2.0-or-later
//
// A synthetic modem for tests. It sits on the other end of two
// SOCK_SEQPACKET socket pairs, which keep message boundaries like the
// kernel's WWAN ports, and answers in the order the A3's modem did on the
// phone (boot, SIM, LTE registration, data, SMS, calls). The payloads are
// built from the documented layouts, so the tests check the daemon's logic
// against our reading of the protocol, not against the modem itself.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/loop.h"
#include "sim/milenage.h"

namespace shannond {

class FakeModem {
public:
    explicit FakeModem(EventLoop &loop);
    ~FakeModem();

    // The daemon's ends
    int fmtFd() const { return fmt_[0]; }
    int rfsFd() const { return rfs_[0]; }

    // What a modem sends after its stack started and found the SIM
    void boot();
    void incomingSms(const Bytes &pdu, uint8_t type = 0x01 /* point to point */);
    void incomingCall(const std::string &number);
    // A second call while one is up: it waits
    void waitingCall(const std::string &number);
    void remoteHangup();
    // The registration again, as when the cell or its features change
    void networkChanged();
    // Camped on a GSM cell without registering: no SIM, emergency calls only
    void limitedService();
    // The SIM's state changes (IPC_SEC_PIN_STATUS)
    void pinStatus(uint8_t status, uint8_t facilityLock);
    // The modem asks for or changes NV data
    void nvRead(uint32_t offset, uint32_t length);
    void nvWrite(uint32_t offset, const Bytes &data);
    // Close the modem's ends: the daemon sees the hang-up of a crash
    void crash();

    std::vector<Message> received;         // every IPC request
    std::vector<RfsMessage> rfsReceived;   // every RFS answer
    std::set<uint8_t> acceptedModes{0x01, 0x02, 0x08};
    // Send RFS messages in pieces of this size, as the real modem's long
    // ones arrive (0: whole)
    size_t rfsChunk = 0;
    // Whether the answer to a SIM toolkit envelope names its request
    bool envelopeAnswersWithSequence = true;
    // The supplementary services' state on the fake network
    std::string barringPassword = "1234";
    // The result of the next SMS sent (0: delivered to the SMSC)
    uint16_t sendResult = 0;
    // The card's AKA: TS 35.208 test set 1 keys and the highest SQN seen
    sim::Milenage milenage = testMilenage();
    sim::Sqn sqnMs{};
    // Files of the card's applications, by file id: transparent ones and
    // linear fixed ones (records of one length). Others read as "abc".
    std::map<uint16_t, Bytes> simBinary;
    std::map<uint16_t, std::vector<Bytes>> simRecords;
    // What the LTE cell says about voice over PS
    bool volteSupported = true;
    // The IPv4 P-CSCF every data call gets
    std::array<uint8_t, 4> pcscf4{10, 0, 0, 80};
    static sim::Milenage testMilenage();

    size_t count(uint16_t command) const;

    // Any message, for frames recorded from the phone
    void send(uint16_t command, uint8_t type, const Bytes &data, uint8_t aseq = 0);

private:
    void readFmt();
    void readRfs();
    void handle(const Message &request);
    void ack(const Message &request, bool ok = true);
    void respond(const Message &request, const Bytes &data);
    void sendRegistration(uint8_t domain, uint8_t aseq);
    Bytes callList() const;
    void setCallState(uint8_t status);
    void sendUssd(uint8_t state, const std::string &text);

    EventLoop &loop_;
    int fmt_[2];
    int rfs_[2];
    uint8_t seq_ = 0x80;
    uint8_t rfsSeq_ = 0;
    uint8_t smsId_ = 5;

    struct FakeCall {
        uint8_t id;
        uint8_t status;
        bool incoming;
        std::string number;
        bool mpty = false;
    };
    void manageCalls(const Message &request);
    std::vector<FakeCall> calls_;
};

}  // namespace shannond
