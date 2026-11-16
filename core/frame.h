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

// Collects the messages of a channel from its reads. A read can hold a part
// of a message: the modem sends long ones (RFS NV data) as several frames,
// which Samsung's driver hands out one by one, and the reader joins them by
// the length at the start of the header (little endian, header included).
class FrameAssembler {
public:
    FrameAssembler(size_t headerSize, size_t lengthBytes, size_t maxLength)
        : headerSize_(headerSize), lengthBytes_(lengthBytes), maxLength_(maxLength)
    {
    }

    // Adds the bytes of one read; returns the messages it completes. A length
    // outside [header size, maximum] drops what is pending and sets *bad
    std::vector<Bytes> add(std::span<const uint8_t> data, bool *bad = nullptr);

    size_t pending() const { return pending_.size(); }

private:
    size_t headerSize_;
    size_t lengthBytes_;
    size_t maxLength_;
    Bytes pending_;
};

template <class T>
Bytes toBytes(const T &value)
{
    Bytes bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

}  // namespace shannond
