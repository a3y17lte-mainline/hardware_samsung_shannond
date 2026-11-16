// SPDX-License-Identifier: GPL-2.0-or-later
//
// The modem's non-volatile data. The modem keeps it on the AP: it reads and
// writes parts of one file through the RFS channel while it runs. The file
// has an MD5 companion: md5(contents + "Samsung_Android_RIL") as 32 hex
// characters.
//
// The store works on whatever file it is given: the daemon is pointed at a
// working copy, never at the EFS partition, until the user decides
// otherwise.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "core/frame.h"

namespace shannond {

class Modem;

class NvStore {
public:
    NvStore(std::string path, size_t size);

    // Checks size and checksum
    bool valid() const;
    std::optional<Bytes> read(size_t offset, size_t length) const;
    bool write(size_t offset, const Bytes &data);

    // Called after every successful write: offset, length
    void onWrite(std::function<void(size_t, size_t)> callback) { onWrite_ = std::move(callback); }

    const std::string &path() const { return path_; }
    std::string md5Path() const { return path_ + ".md5"; }

private:
    std::optional<std::string> checksum() const;
    bool updateChecksum();

    std::string path_;
    size_t size_;
    std::function<void(size_t, size_t)> onWrite_;
};

// Answers the modem's NV item requests on the RFS channel
class RfsService {
public:
    RfsService(Modem &modem, NvStore &nv);

private:
    void readItem(const RfsMessage &request);
    void writeItem(const RfsMessage &request);

    Modem &modem_;
    NvStore &nv_;
};

std::string md5Hex(const Bytes &data, const std::string &secret);

}  // namespace shannond
