// SPDX-License-Identifier: GPL-2.0-or-later
//
// The radio HAL for the Galaxy A3 2017 (Shannon 315 on the mainline WWAN
// driver): one process serving every radio interface for SIM slot 1, with
// the modem daemon core underneath.
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>

#include "frontends/aidl/Services.h"

using namespace modemd::hal;

namespace {

template <typename Service>
void publish(const std::shared_ptr<Service> &service, const char *instance)
{
    const std::string name = std::string(Service::descriptor) + "/" + instance;
    const binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), name.c_str());
    CHECK_EQ(status, STATUS_OK) << "cannot register " << name;
    LOG(INFO) << "registered " << name;
}

}  // namespace

int main(int, char *argv[])
{
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::RADIO));
    ABinderProcess_setThreadPoolMaxThreadCount(2);

    auto stack = std::make_shared<Stack>(Config{});

    // The services hook into the core's notifications when the modem is
    // up, so they all exist before the stack starts
    auto config = ndk::SharedRefBase::make<RadioConfig>(stack);
    auto modem = ndk::SharedRefBase::make<RadioModem>(stack);
    auto sim = ndk::SharedRefBase::make<RadioSim>(stack);
    auto network = ndk::SharedRefBase::make<RadioNetwork>(stack);
    auto data = ndk::SharedRefBase::make<RadioData>(stack);
    auto messaging = ndk::SharedRefBase::make<RadioMessaging>(stack);
    auto voice = ndk::SharedRefBase::make<RadioVoice>(stack);

    stack->start();

    publish(config, "default");
    publish(modem, "slot1");
    publish(sim, "slot1");
    publish(network, "slot1");
    publish(data, "slot1");
    publish(messaging, "slot1");
    publish(voice, "slot1");

    ABinderProcess_startThreadPool();
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // not reached
}
