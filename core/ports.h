// SPDX-License-Identifier: GPL-2.0-or-later
//
// The modem's control ports and packet data links on the mainline kernel.
#pragma once

#include <optional>
#include <cstdint>
#include <string>

namespace shannond {

// WWAN port types of the Exynos modem driver
constexpr char kPortTypeIpc[] = "SIPC";
constexpr char kPortTypeRfs[] = "SIPC-RFS";

// The device name (e.g. "wwan0sipc0") of the first port of a type
std::optional<std::string> findWwanPort(const std::string &type,
                                        const std::string &sysfs = "/sys/class/wwan");
// The WWAN device (e.g. "wwan0") a port belongs to
std::optional<std::string> wwanDeviceOf(const std::string &port,
                                        const std::string &sysfs = "/sys/class/wwan");

// Opens /dev/<port>, non-blocking. Opening the IPC port starts the modem
// and can take tens of seconds.
int openPort(const std::string &port);

// Packet data contexts as WWAN links (rtnetlink). Link id N is the modem's
// packet data channel N, which the modem uses for context id N + 1.
bool createWwanLink(const std::string &wwanDevice, const std::string &name, unsigned int linkId,
                    std::string *error = nullptr);
bool deleteLink(const std::string &name, std::string *error = nullptr);
bool setLinkUp(const std::string &name, bool up, std::string *error = nullptr);
// Sets an address on a link: 4 bytes for AF_INET, 16 for AF_INET6
bool addAddress(const std::string &name, int family, const uint8_t *address,
                unsigned int prefixLength, std::string *error = nullptr);
bool linkExists(const std::string &name);

// A routing table of its own for a link: a default route through the device
// (no gateway: raw IP), and a rule that sends the sockets bound to the device
// (SO_BINDTODEVICE) to that table. For a link nobody else routes: Android's
// netd ends its rules with "unreachable" and knows nothing of our IMS PDN.
bool addDefaultRoute(const std::string &name, int family, uint32_t table, std::string *error = nullptr);
bool addDeviceRule(const std::string &name, int family, uint32_t table, uint32_t priority,
                   std::string *error = nullptr);
bool deleteDeviceRule(const std::string &name, int family, uint32_t table, uint32_t priority,
                      std::string *error = nullptr);

}  // namespace shannond
