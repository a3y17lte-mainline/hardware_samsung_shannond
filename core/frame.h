// SPDX-License-Identifier: GPL-2.0-or-later
//
// Samsung IPC frames as the modem's control ports carry them: one frame per
// read or write.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace shannond {

using Bytes = std::vector<uint8_t>;

// A message of the IPC (FMT) channel. The command is group << 8 | index.
struct Message {
    uint16_t command = 0;
    uint8_t type = 0;
    uint8_t mseq = 0;
    uint8_t aseq = 0;
    Bytes data;

    // The payload as a structure, if it is long enough
    template <class T>
    std::optional<T> as() const
    {
        if (data.size() < sizeof(T))
            return std::nullopt;
        T value;
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }
};

// A message of the remote file system (RFS) channel
struct RfsMessage {
    uint8_t command = 0;  // index in group 0x42
    uint8_t id = 0;
    Bytes data;
};

constexpr size_t kFmtHeaderSize = 7;
constexpr size_t kRfsHeaderSize = 6;
constexpr uint8_t kRfsGroup = 0x42;

Bytes encodeFmt(const Message &message);
std::optional<Message> decodeFmt(std::span<const uint8_t> frame);
Bytes encodeRfs(const RfsMessage &message);
std::optional<RfsMessage> decodeRfs(std::span<const uint8_t> frame);

template <class T>
Bytes toBytes(const T &value)
{
    Bytes bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

}  // namespace shannond
