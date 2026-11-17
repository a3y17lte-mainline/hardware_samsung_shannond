// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Services.h"

#include "frontends/aidl/Codec.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond::hal {

namespace {

RadioError sendError(const SmsSendResult &r)
{
    // SmsManager::OnSendSmsDone
    switch (r.error) {
    case SmsSendResult::Error::None: return RadioError::NONE;
    case SmsSendResult::Error::Retry: return RadioError::SMS_SEND_FAIL_RETRY;
    case SmsSendResult::Error::FdnBlocked: return RadioError::FDN_CHECK_FAILURE;
    case SmsSendResult::Error::Failed: break;
    }
    return RadioError::GENERIC_FAILURE;
}

}  // namespace

RadioMessaging::RadioMessaging(std::shared_ptr<Stack> stack) : stack_(std::move(stack))
{
    stack_->onReady([this](Core &core) {
        // The framework acknowledges each message once it has stored it
        // (acknowledgeLastIncomingGsmSms); without a framework the core
        // does so itself
        core.sms.onIncoming([this, &core](const Bytes &pdu, bool statusReport) {
            auto i = indication();
            if (!i) {
                core.sms.acknowledge(true, false);
                return;
            }
            if (statusReport)
                i->newSmsStatusReport(kUnsolicited, pdu);
            else
                i->newSms(kUnsolicited, pdu);
        });
        core.sms.onSimFull([this](bool full) {
            if (!full)
                return;
            if (auto i = indication())
                i->simSmsStorageFull(kUnsolicited);
        });
        core.sms.onBroadcast([this](const CellBroadcast &broadcast) {
            if (auto i = indication())
                i->newBroadcastSms(kUnsolicited, broadcast.page);
        });
    });
}

void RadioMessaging::send(int32_t serial, const messaging::GsmSmsMessage &message, bool more)
{
    // more: no link hold across messages yet, each goes on its own
    stack_->post([this, serial, message, more](Core *core) {
        auto respond = [this, more](RadioResponseInfo info,
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

        core->sms.send(*tpdu, [respond, serial](const SmsSendResult &r) {
            messaging::SendSmsResult sent;
            sent.messageRef = r.reference;
            sent.errorCode = r.rpCause;
            respond(responseInfo(serial, sendError(r)), sent);
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
    stack_->post([this, serial, success, cause](Core *core) {
        if (core)
            core->sms.acknowledge(success,
                                  cause == messaging::SmsAcknowledgeFailCause::MEMORY_CAPACITY_EXCEEDED);
        if (auto r = response())
            r->acknowledgeLastIncomingGsmSmsResponse(core ? ok(serial) : notAvailable(serial));
    });
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

::ndk::ScopedAStatus RadioMessaging::setSmscAddress(int32_t serial, const std::string &smsc)
{
    stack_->post([this, serial, smsc](Core *core) {
        auto respond = [this](RadioResponseInfo info) {
            if (auto r = response())
                r->setSmscAddressResponse(info);
        };
        if (!core) {
            respond(notAvailable(serial));
            return;
        }
        const Bytes address = stringToBcdAddress(smsc);
        if (address.empty()) {
            respond(responseInfo(serial, RadioError::INVALID_SMS_FORMAT));
            return;
        }
        core->sms.setSmsc(address, [respond, serial](bool success) {
            respond(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::reportSmsMemoryStatus(int32_t serial, bool available)
{
    // The network holds messages back while the memory is full, and sends
    // them once it has room again
    stack_->post([this, serial, available](Core *core) {
        if (!core) {
            if (auto r = response())
                r->reportSmsMemoryStatusResponse(notAvailable(serial));
            return;
        }
        core->sms.setMemoryAvailable(available, [this, serial](bool success) {
            if (auto r = response())
                r->reportSmsMemoryStatusResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::writeSmsToSim(int32_t serial,
                                                   const messaging::SmsWriteArgs &args)
{
    stack_->post([this, serial, args](Core *core) {
        auto respond = [this](RadioResponseInfo info, int32_t index) {
            if (auto r = response())
                r->writeSmsToSimResponse(info, index);
        };
        if (!core) {
            respond(notAvailable(serial), -1);
            return;
        }
        // The SMSC goes in front with its length, as in a received PDU;
        // none from the framework: the SIM's
        auto tpdu = fromHex(args.pdu);
        Bytes smsc;
        if (!args.smsc.empty()) {
            auto raw = fromHex(args.smsc);
            if (!raw) {
                respond(responseInfo(serial, RadioError::INVALID_ARGUMENTS), -1);
                return;
            }
            smsc = *raw;
            if (!smsc.empty() && smsc[0] + 1u == smsc.size())
                smsc.erase(smsc.begin());
        } else {
            smsc = core->sms.smsc();
        }
        if (!tpdu || tpdu->empty() || args.status < 0 || args.status > 3) {
            respond(responseInfo(serial, RadioError::INVALID_ARGUMENTS), -1);
            return;
        }
        Bytes pdu{static_cast<uint8_t>(smsc.size())};
        pdu.insert(pdu.end(), smsc.begin(), smsc.end());
        pdu.insert(pdu.end(), tpdu->begin(), tpdu->end());
        // SmsWriteArgs.STATUS_* are IPC_SMS_STATUS_* minus one
        core->sms.writeToSim(static_cast<uint8_t>(args.status + 1), pdu,
                             [respond, serial](std::optional<int> index) {
                                 if (index)
                                     respond(ok(serial), *index);
                                 else
                                     respond(responseInfo(serial, RadioError::SIM_FULL), -1);
                             });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::deleteSmsOnSim(int32_t serial, int32_t index)
{
    stack_->post([this, serial, index](Core *core) {
        if (!core) {
            if (auto r = response())
                r->deleteSmsOnSimResponse(notAvailable(serial));
            return;
        }
        core->sms.deleteOnSim(index, [this, serial](bool success) {
            if (auto r = response())
                r->deleteSmsOnSimResponse(success ? ok(serial)
                                                  : responseInfo(serial, RadioError::INVALID_ARGUMENTS));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::setGsmBroadcastActivation(int32_t serial, bool activate)
{
    stack_->post([this, serial, activate](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setGsmBroadcastActivationResponse(notAvailable(serial));
            return;
        }
        core->sms.setBroadcastActivation(activate, [this, serial](bool success) {
            if (auto r = response())
                r->setGsmBroadcastActivationResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::setGsmBroadcastConfig(
        int32_t serial, const std::vector<messaging::GsmBroadcastSmsConfigInfo> &configInfo)
{
    std::vector<BroadcastRange> ranges;
    for (const auto &c : configInfo) {
        BroadcastRange r;
        r.fromId = static_cast<uint16_t>(c.fromServiceId);
        r.toId = static_cast<uint16_t>(c.toServiceId);
        r.fromScheme = static_cast<uint8_t>(c.fromCodeScheme);
        r.toScheme = static_cast<uint8_t>(c.toCodeScheme);
        r.selected = c.selected;
        ranges.push_back(r);
    }
    stack_->post([this, serial, ranges](Core *core) {
        if (!core) {
            if (auto r = response())
                r->setGsmBroadcastConfigResponse(notAvailable(serial));
            return;
        }
        core->sms.setBroadcastConfig(ranges, [this, serial](bool success) {
            if (auto r = response())
                r->setGsmBroadcastConfigResponse(success ? ok(serial) : failed(serial));
        });
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessaging::getGsmBroadcastConfig(int32_t serial)
{
    // What the framework set, as the stock RIL answers from its filter
    stack_->post([this, serial](Core *core) {
        auto r = response();
        if (!r)
            return;
        if (!core) {
            r->getGsmBroadcastConfigResponse(notAvailable(serial), {});
            return;
        }
        std::vector<messaging::GsmBroadcastSmsConfigInfo> configs;
        for (const BroadcastRange &b : core->sms.broadcastConfig()) {
            messaging::GsmBroadcastSmsConfigInfo c;
            c.fromServiceId = b.fromId;
            c.toServiceId = b.toId;
            c.fromCodeScheme = b.fromScheme;
            c.toCodeScheme = b.toScheme;
            c.selected = b.selected;
            configs.push_back(c);
        }
        r->getGsmBroadcastConfigResponse(ok(serial), configs);
    });
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace shannond::hal
