// SPDX-License-Identifier: GPL-2.0-or-later
//
// The daemon core on its own thread, for the binder services. Binder calls
// arrive on binder threads and are posted to the core's event loop; the
// core's callbacks run on that loop and answer through the (thread-safe)
// response callbacks.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/call.h"
#include "core/data.h"
#include "core/loop.h"
#include "core/modem.h"
#include "core/network.h"
#include "core/nv.h"
#include "core/sim.h"
#include "core/sms.h"

namespace modemd::hal {

struct Config {
    std::string ipcPort = "wwan0sipc0";
    std::string rfsPort = "wwan0sipcrfs0";
    std::string wwanDevice = "wwan0";
    std::string nv = "/data/vendor/modem/nv/nv_data.bin";
    size_t nvSize = 0x100000;
};

struct Core {
    Core(EventLoop &loop, int fmt, int rfs, const Config &config);

    Modem modem;
    NvStore nv;
    RfsService rfsService;
    Sim sim;
    Network network;
    Data data;
    Sms sms;
    Calls calls;
    uint8_t supportedModes = 0;  // what the modem accepted when probed
};

class Stack {
public:
    explicit Stack(Config config);
    ~Stack();

    // Opens the ports (which starts the modem) and runs the core
    void start();

    bool connected() const { return ready_; }

    // Run fn on the core's thread with the core, or right away with nullptr
    // while the modem is not up
    void post(std::function<void(Core *)> fn);
    // Called on the core's thread once the modem's stack is up
    void onReady(std::function<void(Core &)> fn);

private:
    void run();

    Config config_;
    EventLoop loop_;
    std::unique_ptr<Core> core_;
    std::atomic<bool> ready_{false};
    std::vector<std::function<void(Core &)>> readyCallbacks_;
    std::thread thread_;
};

}  // namespace modemd::hal
