// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ports.h"

#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace shannond {

namespace {

std::optional<std::string> readLine(const std::string &path)
{
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line))
        return std::nullopt;
    return line;
}

}  // namespace

std::optional<std::string> findWwanPort(const std::string &type, const std::string &sysfs)
{
    DIR *dir = opendir(sysfs.c_str());
    if (!dir)
        return std::nullopt;

    std::optional<std::string> found;
    while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        auto value = readLine(sysfs + "/" + name + "/type");
        if (value && *value == type && (!found || name < *found))
            found = name;
    }
    closedir(dir);

    return found;
}

std::optional<std::string> wwanDeviceOf(const std::string &port, const std::string &sysfs)
{
    // /sys/class/wwan/<port>/device -> the wwanN device, which links back here
    char target[512];
    const std::string link = sysfs + "/" + port + "/device";
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        std::string path(target);
        auto slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Fall back to the name: wwan0sipc0 belongs to wwan0
    auto end = port.find_first_not_of("0123456789", 4);
    if (port.rfind("wwan", 0) != 0 || end == std::string::npos)
        return std::nullopt;
    return port.substr(0, end);
}

int openPort(const std::string &port)
{
    const std::string path = "/dev/" + port;
    return open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
}

}  // namespace shannond
