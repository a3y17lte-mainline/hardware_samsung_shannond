// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include "frontends/aidl/Codec.h"

namespace shannond::hal {

RadioMessaging::RadioMessaging(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        // The core acknowledges every delivered SMS to the network itself:
        // the framework's acknowledgement only confirms it stored the message
        core.sms.onIncoming([this](const Bytes &pdu) {
            if (auto i = indication())
                i->newSms(kUnsolicited, pdu);
        });
    });
}

void RadioMessaging::send(int32_t serial, const messaging::GsmSmsMessage &message, bool more)
{
    // more: no link hold across messages yet, each goes on its own
    stack_->post([this, serial, message, more](Core *core) {
        auto respond = [this, serial, more](RadioResponseInfo info,
                                            const messaging::SendSmsResult &result) {
            if (auto r = response()) {
                if (more)
                    r->sendSmsExpectMoreResponse(info, result);
                else
                    r->sendSmsResponse(info, result);
            }
        };
        messaging::SendSmsResult result;
        result.messageRef = 0;
        result.errorCode = -1;
        if (!core) {
            respond(notAvailable(serial), result);
            return;
        }

        const auto tpdu = fromHex(message.pdu);
        if (!tpdu || tpdu->empty()) {
            respond(responseInfo(serial, RadioError::INVALID_ARGUMENTS), result);
            return;
        }
        // smscPdu: a length byte, then the address; empty for the SIM's SMSC
        std::optional<Bytes> smsc;
        if (!message.smscPdu.empty()) {
            auto raw = fromHex(message.smscPdu);
            if (raw && raw->size() > 1 && (*raw)[0] + 1u == raw->size())
                smsc = Bytes(raw->begin() + 1, raw->end());
        }

        core->sms.send(*tpdu, [respond, serial](bool success, uint8_t reference) {
            messaging::SendSmsResult sent;
            sent.messageRef = reference;
            sent.errorCode = -1;
            respond(success ? ok(serial) : responseInfo(serial, RadioError::NETWORK_REJECT), sent);
        }, smsc ? &*smsc : nullptr);
    });
}

::ndk::ScopedAStatus RadioMessaging::sendSms(int32_t serial, const messaging::GsmSmsMessage &message)
{
    send(serial, message, false);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::sendSmsExpectMore(int32_t serial,
                                                       const messaging::GsmSmsMessage &message)
{
    send(serial, message, true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::acknowledgeLastIncomingGsmSms(
        int32_t serial, bool success, messaging::SmsAcknowledgeFailCause cause)
{
    (void)success;
    (void)cause;  // already acknowledged by the core (see the constructor)
    if (auto r = response())
        r->acknowledgeLastIncomingGsmSmsResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::getSmscAddress(int32_t serial)
{
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core || core->sms.smsc().empty())
            r->getSmscAddressResponse(notAvailable(serial), "");
        else
            r->getSmscAddressResponse(ok(serial), bcdAddressToString(core->sms.smsc()));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::reportSmsMemoryStatus(int32_t serial, bool available)
{
    (void)available;
    if (auto r = response())
        r->reportSmsMemoryStatusResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

// Cell broadcast is not decoded yet; accepting the configuration keeps the
// framework from retrying it

::ndk::ScopedAStatus RadioMessaging::setGsmBroadcastActivation(int32_t serial, bool activate)
{
    (void)activate;
    if (auto r = response())
        r->setGsmBroadcastActivationResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::setGsmBroadcastConfig(
        int32_t serial, const std::vector<messaging::GsmBroadcastSmsConfigInfo> &configInfo)
{
    (void)configInfo;
    if (auto r = response())
        r->setGsmBroadcastConfigResponse(ok(serial));
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
