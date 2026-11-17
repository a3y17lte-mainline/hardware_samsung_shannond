// SPDX-License-Identifier: GPL-2.0-or-later
//
// Starting while the modem already runs.
#pragma once

#include <chrono>

#include "core/data.h"
#include "core/loop.h"
#include "core/network.h"
#include "core/sim.h"
#include "core/sms.h"

namespace shannond {

// The modem keeps running when its client goes away, and tells only the
// client that started it that its stack is up, which card it has and that
// SMS is ready. If none of that came after `wait`, ask the modem whether it
// runs; if it does, take it over: radio off, the card's state asked for,
// old data interfaces removed. The modules must outlive the loop.
void adoptRunningModem(EventLoop &loop, Network &network, Sim &sim, Sms &sms, Data &data,
                       std::chrono::milliseconds wait = std::chrono::seconds(3));

}  // namespace shannond
