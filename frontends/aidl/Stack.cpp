// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontends/aidl/Stack.h"

#include "core/attach.h"

#include <android-base/logging.h>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "core/ims/linux_network.h"
#include "core/ports.h"

namespace shannond::hal {

namespace {

// A timed kernel wake lock (needs CAP_BLOCK_SUSPEND): it ends by itself, so
// a crash of this service cannot keep the phone awake
void stayAwake(std::chrono::seconds time)
{
    const int fd = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    const std::string line =
            "shannond-ims " + std::to_string(std::chrono::nanoseconds(time).count());
    (void)!write(fd, line.data(), line.size());
    close(fd);
}

}  // namespace

void Core::imsAwake()
{
    // A few seconds for a SIP exchange (a registration refresh, a call
    // coming in); through a call, and while a request of ours waits for
    // its answer (the SIP timers stand still in suspend), renewed, since
    // nothing else holds the system awake for the SIP side of it
    const auto held = [this] { return ims.calls().any() || (ims.endpoint() && ims.endpoint()->busy()); };
    const bool call = held();
    stayAwake(std::chrono::seconds(call ? 60 : 5));
    if (call && !wakeTimer_)
        wakeTimer_ = modem.loop().after(std::chrono::seconds(30), [this, held] {
            wakeTimer_ = 0;
            if (held())
                imsAwake();
        });
}

Core::Core(EventLoop &loop, int fmt, int rfs, const Config &config)
    : modem(loop, fmt, rfs),
      nv(config.nv, config.nvSize),
      rfsService(modem, nv),
      sim(modem),
      network(modem),
      data(modem, DataLinks::wwan(config.wwanDevice)),
      sms(modem),
      calls(modem),
      sat(modem),
      ss(modem),
      ims(modem, sim, network, data,
          [](const DataCall &pdn) { return std::make_unique<ims::LinuxImsNetwork>(pdn.iface); })
{
    // A VoLTE call uses the modem's call audio like a CS call
    ims.calls().onAudio([this](bool on) { calls.setImsAudio(on); });
    // After SRVCC the calls are the modem's CS calls: fetch them
    ims.onSrvcc([this](ims::SrvccState state) {
        if (state == ims::SrvccState::Completed)
            calls.refresh();
    });
    ims.onActivity([this] { imsAwake(); });
}

Stack::Stack(Config config) : config_(std::move(config)) {}

Stack::~Stack()
{
    loop_.stop();
    if (thread_.joinable())
        thread_.detach();
}

void Stack::start()
{
    thread_ = std::thread([this] { run(); });
}

void Stack::post(std::function<void(Core *)> fn)
{
    if (!ready_) {
        fn(nullptr);
        return;
    }
    loop_.post([this, fn = std::move(fn)] { fn(core_.get()); });
}

void Stack::onReady(std::function<void(Core &)> fn)
{
    // Registered before start(); runs on the core's thread
    readyCallbacks_.push_back(std::move(fn));
}

void Stack::run()
{
    // The ports appear once the driver is bound; opening the IPC port
    // starts the modem, which takes up to about 20 s
    int fmt = -1, rfs = -1;
    for (;;) {
        fmt = openPort(config_.ipcPort);
        if (fmt >= 0)
            break;
        LOG(INFO) << "waiting for " << config_.ipcPort;
        sleep(2);
    }
    rfs = openPort(config_.rfsPort);
    if (rfs < 0)
        LOG(FATAL) << "cannot open " << config_.rfsPort;

    core_ = std::make_unique<Core>(loop_, fmt, rfs, config_);
    core_->modem.setLog([](const std::string &line) { LOG(INFO) << line; });
    core_->modem.onClosed([] {
        // The modem crashed or was stopped: init restarts the service, and
        // opening the port again restarts the modem
        LOG(ERROR) << "modem link closed, exiting";
        std::_Exit(1);
    });
    if (!core_->nv.valid())
        LOG(WARNING) << config_.nv << " has no valid checksum";

    core_->network.onReady([this] {
        core_->network.probeModes([this](uint8_t modes) { core_->supportedModes = modes; });
        ready_ = true;
        for (auto &fn : readyCallbacks_)
            fn(*core_);
    });

    // After a restart of this service the modem is already running
    adoptRunningModem(loop_, core_->network, core_->sim, core_->sms, core_->data);

    LOG(INFO) << "modem link open on " << config_.ipcPort << " + " << config_.rfsPort;
    loop_.run();
}

}  // namespace shannond::hal
