// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/attach.h"

namespace shannond {

void adoptRunningModem(EventLoop &loop, Network &network, Sim &sim, Sms &sms, Data &data,
                       std::chrono::milliseconds wait)
{
    loop.after(wait, [&network, &sim, &sms, &data] {
        if (network.modemReady())
            return;
        network.probeRunning([&sim, &sms, &data](bool running) {
            if (!running)
                return;
            data.reset();
            sim.query();
            sms.adopt();
        });
    });
}

}  // namespace shannond
