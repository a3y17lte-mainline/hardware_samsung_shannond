// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/data.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sys/socket.h>

#include "core/ports.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(60);

std::string ifaceName(unsigned int cid)
{
    return "rmnet" + std::to_string(cid - 1);
}

template <size_t N>
bool zero(const unsigned char *p)
{
    for (size_t i = 0; i < N; i++)
        if (p[i])
            return false;
    return true;
}

template <size_t N>
std::array<uint8_t, N> arr(const unsigned char *p)
{
    std::array<uint8_t, N> a;
    std::memcpy(a.data(), p, N);
    return a;
}

}  // namespace

DataLinks DataLinks::wwan(const std::string &wwanDevice)
{
    DataLinks links;
    links.up = [wwanDevice](DataCall &call) {
        const std::string iface = ifaceName(call.cid);
        call.iface = iface;
        // A link left from an earlier call still has that call's addresses
        if (linkExists(iface))
            deleteLink(iface);
        if (!createWwanLink(wwanDevice, iface, call.cid - 1))
            return false;

        // A point to point link: the address alone, no subnet
        if (call.ipv4 && !addAddress(iface, AF_INET, call.ipv4->data(), 32))
            return false;
        if (call.ipv6InterfaceId) {
            // The link local address the network expects; the prefix comes
            // by router advertisement
            std::array<uint8_t, 16> ll{0xfe, 0x80};
            std::copy(call.ipv6InterfaceId->begin(), call.ipv6InterfaceId->end(), ll.begin() + 8);
            if (!addAddress(iface, AF_INET6, ll.data(), 64))
                return false;
        }
        return setLinkUp(iface, true);
    };
    links.down = [](unsigned int, const std::string &iface) { deleteLink(iface); };
    return links;
}

DataLinks DataLinks::none()
{
    DataLinks links;
    links.up = [](DataCall &call) {
        call.iface = ifaceName(call.cid);
        return true;
    };
    links.down = [](unsigned int, const std::string &) {};
    return links;
}

Data::Data(Modem &modem, DataLinks links) : modem_(modem), links_(std::move(links))
{
    modem_.onMessage(IPC_GPRS_IP_CONFIGURATION, [this](const Message &m) { ipConfiguration(m); });
    modem_.onMessage(IPC_GPRS_CALL_STATUS, [this](const Message &m) { callStatus(m); });
}

void Data::notify(const DataCall &call)
{
    for (auto &callback : changed_)
        callback(call);
}

void Data::setAttachProfile(const DataProfile &profile, std::function<void(bool)> done)
{
    ipc_gprs_lte_attach_apn_info_data attach;
    if (ipc_gprs_lte_attach_apn_info_setup(&attach, 1, profile.apn.c_str(),
                                           profile.username.c_str(),
                                           profile.password.c_str()) < 0) {
        if (done)
            done(false);
        return;
    }
    modem_.request(IPC_GPRS_LTE_ATTACH_APN_INFO, IPC_TYPE_SET, attach, [done](const Result &r) {
        if (done)
            done(r.ok);
    });
}

void Data::connect(unsigned int cid, const DataProfile &profile, Done done)
{
    if (cid == 0 || cid > kDataChannels || pending_.count(cid)) {
        done(std::nullopt);
        return;
    }

    failCauses_.erase(cid);
    pending_[cid] = std::move(done);
    calls_[cid] = DataCall{};
    calls_[cid].cid = cid;
    activate(cid, profile, 0);

    modem_.loop().after(kConnectTimeout, [this, cid] {
        if (pending_.count(cid)) {
            modem_.log("data call %u: no IP configuration", cid);
            complete(cid, false);
        }
    });
}

void Data::activate(unsigned int cid, const DataProfile &profile, int attempt)
{
    ipc_gprs_define_pdp_context_data define;
    ipc_gprs_define_pdp_context_setup(&define, 1, cid, profile.apn.c_str());

    modem_.request(IPC_GPRS_DEFINE_PDP_CONTEXT, IPC_TYPE_SET, define,
                   [this, cid, profile, attempt](const Result &r) {
        if (!pending_.count(cid))
            return;
        if (!r.ok) {
            modem_.log("data call %u: context definition refused (%#x)", cid, r.code);
            complete(cid, false);
            return;
        }

        ipc_gprs_pdp_context_request_set_data set;
        ipc_gprs_pdp_context_request_set_setup(&set, 1, cid, profile.username.c_str(),
                                               profile.password.c_str());
        modem_.request(IPC_GPRS_PDP_CONTEXT, IPC_TYPE_SET, set,
                       [this, cid, profile, attempt](const Result &r2) {
            if (!pending_.count(cid) || r2.ok)
                return;
            // The first activation after a new attach profile can fail while
            // the modem re-registers; one retry, as seen on the A3
            if (attempt == 0) {
                modem_.log("data call %u: activation refused, retrying", cid);
                modem_.loop().after(std::chrono::seconds(2),
                                    [this, cid, profile] { activate(cid, profile, 1); });
                return;
            }
            complete(cid, false);
        });
    });
}

void Data::complete(unsigned int cid, bool ok)
{
    auto it = pending_.find(cid);
    if (it == pending_.end())
        return;

    Done done = std::move(it->second);
    pending_.erase(it);

    if (ok) {
        done(calls_[cid]);
    } else {
        calls_.erase(cid);
        done(std::nullopt);
    }
}

void Data::ipConfiguration(const Message &message)
{
    ipc_gprs_ip_configuration_ext config;
    if (ipc_gprs_ip_configuration_ext_parse(message.data.data(), message.data.size(), &config) < 0) {
        modem_.log("short IP configuration (%zu bytes)", message.data.size());
        return;
    }

    const unsigned int cid = config.cid;
    DataCall &call = calls_[cid];
    call.cid = cid;

    if (config.flags & IPC_GPRS_IP_CONFIGURATION_FLAG_IPV4)
        call.ipv4 = arr<4>(config.ipv4);
    // fe80:: plus the interface identifier the network assigned
    if (!zero<8>(config.ipv6 + 8))
        call.ipv6InterfaceId = arr<8>(config.ipv6 + 8);
    call.dns4.clear();
    for (auto &d : config.ipv4_dns)
        if (!zero<4>(d))
            call.dns4.push_back(arr<4>(d));
    call.dns6.clear();
    for (auto &d : config.ipv6_dns)
        if (!zero<16>(d))
            call.dns6.push_back(arr<16>(d));
    call.pcscf4.clear();
    for (unsigned int i = 0; i < config.pcscf_ipv4_count; i++)
        call.pcscf4.push_back(arr<4>(config.pcscf_ipv4[i]));
    call.pcscf6.clear();
    for (unsigned int i = 0; i < config.pcscf_ipv6_count; i++)
        call.pcscf6.push_back(arr<16>(config.pcscf_ipv6[i]));
    call.mtu = config.mtu;

    if (!call.ipv4 && !call.ipv6InterfaceId) {
        modem_.log("data call %u: IP configuration without an address", cid);
        return;
    }

    if (!links_.up || !links_.up(call)) {
        modem_.log("data call %u: no network interface", cid);
        complete(cid, false);
        return;
    }

    call.active = true;
    modem_.log("data call %u up on %s, %zu P-CSCF", cid, call.iface.c_str(),
               call.pcscf4.size() + call.pcscf6.size());
    notify(call);
    complete(cid, true);
}

void Data::callStatus(const Message &message)
{
    ipc_gprs_call_status_ext_data status;
    if (ipc_gprs_call_status_ext_extract(message.data.data(), message.data.size(), &status) < 0)
        return;

    if (status.dedicated_bearer_changed)
        modem_.log("data call %u: dedicated bearer changed", status.cid);

    if (status.status == IPC_GPRS_STATUS_ENABLED)
        return;

    // The context went down, by our request or the network's
    auto it = calls_.find(status.cid);
    if (it == calls_.end())
        return;

    DataCall call = it->second;
    call.active = false;
    call.failCause = status.fail_cause;
    failCauses_[call.cid] = status.fail_cause;
    if (!call.iface.empty() && links_.down)
        links_.down(call.cid, call.iface);
    calls_.erase(it);

    modem_.log("data call %u down, cause %#x", call.cid, call.failCause);
    if (pending_.count(call.cid))
        complete(call.cid, false);
    notify(call);
}

void Data::disconnect(unsigned int cid, std::function<void(bool)> done)
{
    ipc_gprs_pdp_context_request_set_data set;
    ipc_gprs_pdp_context_request_set_setup(&set, 0, cid, "", "");
    modem_.request(IPC_GPRS_PDP_CONTEXT, IPC_TYPE_SET, set, [done](const Result &r) {
        if (done)
            done(r.ok);
    });
}

}  // namespace shannond
