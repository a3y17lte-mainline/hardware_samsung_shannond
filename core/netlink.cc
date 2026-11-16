// SPDX-License-Identifier: GPL-2.0-or-later
//
// Just enough rtnetlink to add, remove and switch WWAN links.
#include "core/ports.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/wwan.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace modemd {

namespace {

class Request {
public:
    Request(uint16_t type, uint16_t flags)
    {
        std::memset(buffer_, 0, sizeof(buffer_));
        header()->nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
        header()->nlmsg_type = type;
        header()->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
        header()->nlmsg_seq = 1;
        info()->ifi_family = AF_UNSPEC;
    }

    nlmsghdr *header() { return reinterpret_cast<nlmsghdr *>(buffer_); }
    ifinfomsg *info() { return static_cast<ifinfomsg *>(NLMSG_DATA(header())); }

    rtattr *add(uint16_t type, const void *data, size_t length)
    {
        auto *attr = reinterpret_cast<rtattr *>(buffer_ + NLMSG_ALIGN(header()->nlmsg_len));
        attr->rta_type = type;
        attr->rta_len = RTA_LENGTH(length);
        if (length)
            std::memcpy(RTA_DATA(attr), data, length);
        header()->nlmsg_len = NLMSG_ALIGN(header()->nlmsg_len) + RTA_ALIGN(attr->rta_len);
        return attr;
    }

    rtattr *begin(uint16_t type) { return add(type, nullptr, 0); }
    void end(rtattr *nest)
    {
        nest->rta_len = reinterpret_cast<uint8_t *>(header()) + header()->nlmsg_len -
                        reinterpret_cast<uint8_t *>(nest);
    }

    bool send(std::string *error)
    {
        const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0)
            return fail(error, errno);

        sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        if (sendto(fd, buffer_, header()->nlmsg_len, 0, reinterpret_cast<sockaddr *>(&addr),
                   sizeof(addr)) < 0) {
            const int err = errno;
            close(fd);
            return fail(error, err);
        }

        uint8_t reply[4096];
        const ssize_t n = recv(fd, reply, sizeof(reply), 0);
        close(fd);
        if (n < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr))))
            return fail(error, EIO);

        auto *h = reinterpret_cast<nlmsghdr *>(reply);
        if (h->nlmsg_type != NLMSG_ERROR)
            return fail(error, EIO);
        const int code = -static_cast<nlmsgerr *>(NLMSG_DATA(h))->error;
        return code == 0 || fail(error, code);
    }

private:
    static bool fail(std::string *error, int code)
    {
        if (error)
            *error = std::strerror(code);
        return false;
    }

    alignas(nlmsghdr) uint8_t buffer_[1024];
};

}  // namespace

bool createWwanLink(const std::string &wwanDevice, const std::string &name, unsigned int linkId,
                    std::string *error)
{
    Request request(RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    request.add(IFLA_IFNAME, name.c_str(), name.size() + 1);
    request.add(IFLA_PARENT_DEV_NAME, wwanDevice.c_str(), wwanDevice.size() + 1);

    rtattr *linkinfo = request.begin(IFLA_LINKINFO);
    request.add(IFLA_INFO_KIND, "wwan", 5);
    rtattr *data = request.begin(IFLA_INFO_DATA);
    const uint32_t id = linkId;
    request.add(IFLA_WWAN_LINK_ID, &id, sizeof(id));
    request.end(data);
    request.end(linkinfo);

    return request.send(error);
}

bool deleteLink(const std::string &name, std::string *error)
{
    Request request(RTM_DELLINK, 0);
    request.add(IFLA_IFNAME, name.c_str(), name.size() + 1);
    return request.send(error);
}

bool setLinkUp(const std::string &name, bool up, std::string *error)
{
    Request request(RTM_NEWLINK, 0);
    request.info()->ifi_change = IFF_UP;
    request.info()->ifi_flags = up ? IFF_UP : 0;
    request.add(IFLA_IFNAME, name.c_str(), name.size() + 1);
    return request.send(error);
}

bool linkExists(const std::string &name)
{
    return if_nametoindex(name.c_str()) != 0;
}

}  // namespace modemd
