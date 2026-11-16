// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/frame.h"

namespace shannond {

std::vector<Bytes> FrameAssembler::add(std::span<const uint8_t> data, bool *bad)
{
    std::vector<Bytes> frames;

    if (bad)
        *bad = false;
    pending_.insert(pending_.end(), data.begin(), data.end());

    while (pending_.size() >= headerSize_) {
        size_t length = 0;
        for (size_t i = 0; i < lengthBytes_; i++)
            length |= static_cast<size_t>(pending_[i]) << (8 * i);

        if (length < headerSize_ || length > maxLength_) {
            if (bad)
                *bad = true;
            pending_.clear();
            break;
        }
        if (pending_.size() < length)
            break;

        frames.emplace_back(pending_.begin(), pending_.begin() + length);
        pending_.erase(pending_.begin(), pending_.begin() + length);
    }

    return frames;
}

Bytes encodeFmt(const Message &message)
{
    const size_t length = kFmtHeaderSize + message.data.size();
    Bytes frame(length);

    frame[0] = length & 0xff;
    frame[1] = (length >> 8) & 0xff;
    frame[2] = message.mseq;
    frame[3] = message.aseq;
    frame[4] = message.command >> 8;
    frame[5] = message.command & 0xff;
    frame[6] = message.type;
    std::copy(message.data.begin(), message.data.end(), frame.begin() + kFmtHeaderSize);

    return frame;
}

std::optional<Message> decodeFmt(std::span<const uint8_t> frame)
{
    if (frame.size() < kFmtHeaderSize)
        return std::nullopt;

    const size_t length = frame[0] | (frame[1] << 8);
    if (length < kFmtHeaderSize || length > frame.size())
        return std::nullopt;

    Message message;
    message.mseq = frame[2];
    message.aseq = frame[3];
    message.command = (frame[4] << 8) | frame[5];
    message.type = frame[6];
    message.data.assign(frame.begin() + kFmtHeaderSize, frame.begin() + length);

    return message;
}

Bytes encodeRfs(const RfsMessage &message)
{
    const size_t length = kRfsHeaderSize + message.data.size();
    Bytes frame(length);

    frame[0] = length & 0xff;
    frame[1] = (length >> 8) & 0xff;
    frame[2] = (length >> 16) & 0xff;
    frame[3] = (length >> 24) & 0xff;
    frame[4] = message.command;
    frame[5] = message.id;
    std::copy(message.data.begin(), message.data.end(), frame.begin() + kRfsHeaderSize);

    return frame;
}

std::optional<RfsMessage> decodeRfs(std::span<const uint8_t> frame)
{
    if (frame.size() < kRfsHeaderSize)
        return std::nullopt;

    const size_t length = frame[0] | (frame[1] << 8) | (frame[2] << 16) |
                          (static_cast<size_t>(frame[3]) << 24);
    if (length < kRfsHeaderSize || length > frame.size())
        return std::nullopt;

    RfsMessage message;
    message.command = frame[4];
    message.id = frame[5];
    message.data.assign(frame.begin() + kRfsHeaderSize, frame.begin() + length);

    return message;
}

}  // namespace shannond
