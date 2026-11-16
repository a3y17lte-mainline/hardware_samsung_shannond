// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/nv.h"

#include <cstdio>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/modem.h"

extern "C" {
#include <samsung-ipc.h>
}

namespace shannond {

namespace {

constexpr char kSecret[] = "Samsung_Android_RIL";

std::optional<Bytes> readFile(const std::string &path)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;

    Bytes data;
    uint8_t buffer[65536];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
        data.insert(data.end(), buffer, buffer + n);
    close(fd);

    if (n < 0)
        return std::nullopt;
    return data;
}

// Write to a temporary file and rename, so a crash never leaves half a file
bool writeFileAtomic(const std::string &path, const void *data, size_t size)
{
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;

    const auto *p = static_cast<const uint8_t *>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, p + done, size - done);
        if (n <= 0) {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += n;
    }

    if (fsync(fd) < 0 || close(fd) < 0) {
        unlink(tmp.c_str());
        return false;
    }

    return rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

std::string md5Hex(const Bytes &data, const std::string &secret)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestUpdate(ctx, secret.data(), secret.size());
    EVP_DigestFinal_ex(ctx, digest, &length);
    EVP_MD_CTX_free(ctx);

    std::string hex;
    char byte[3];
    for (unsigned int i = 0; i < length; i++) {
        snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }
    return hex;
}

NvStore::NvStore(std::string path, size_t size) : path_(std::move(path)), size_(size) {}

std::optional<std::string> NvStore::checksum() const
{
    auto data = readFile(path_);
    if (!data || data->size() != size_)
        return std::nullopt;
    return md5Hex(*data, kSecret);
}

bool NvStore::valid() const
{
    auto sum = checksum();
    auto stored = readFile(md5Path());
    if (!sum || !stored || stored->size() < 32)
        return false;
    return std::string(stored->begin(), stored->begin() + 32) == *sum;
}

std::optional<Bytes> NvStore::read(size_t offset, size_t length) const
{
    if (offset > size_ || length > size_ - offset)
        return std::nullopt;

    const int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;

    Bytes data(length);
    const ssize_t n = pread(fd, data.data(), length, offset);
    close(fd);

    if (n != static_cast<ssize_t>(length))
        return std::nullopt;
    return data;
}

bool NvStore::updateChecksum()
{
    auto sum = checksum();
    return sum && writeFileAtomic(md5Path(), sum->data(), sum->size());
}

bool NvStore::write(size_t offset, const Bytes &data)
{
    if (offset > size_ || data.size() > size_ - offset)
        return false;

    auto contents = readFile(path_);
    if (!contents || contents->size() != size_)
        return false;

    std::copy(data.begin(), data.end(), contents->begin() + offset);
    if (!writeFileAtomic(path_, contents->data(), contents->size()) || !updateChecksum())
        return false;

    if (onWrite_)
        onWrite_(offset, data.size());
    return true;
}

RfsService::RfsService(Modem &modem, NvStore &nv) : modem_(modem), nv_(nv)
{
    modem_.onRfs(IPC_RFS_NV_READ_ITEM & 0xff, [this](const RfsMessage &m) { readItem(m); });
    modem_.onRfs(IPC_RFS_NV_WRITE_ITEM & 0xff, [this](const RfsMessage &m) { writeItem(m); });
}

void RfsService::readItem(const RfsMessage &request)
{
    ipc_rfs_nv_read_item_request_data req{};
    ipc_rfs_nv_read_item_response_header header{};
    RfsMessage response;
    std::optional<Bytes> data;

    response.command = request.command;
    response.id = request.id;

    if (request.data.size() >= sizeof(req)) {
        std::memcpy(&req, request.data.data(), sizeof(req));
        data = nv_.read(req.offset, req.length);
    }

    header.offset = req.offset;
    header.length = req.length;
    header.confirm = data ? 1 : 0;
    response.data = toBytes(header);
    if (data)
        response.data.insert(response.data.end(), data->begin(), data->end());
    else
        modem_.log("NV read of %u bytes at %#x failed", req.length, req.offset);

    modem_.sendRfs(response);
}

void RfsService::writeItem(const RfsMessage &request)
{
    ipc_rfs_nv_write_item_request_header req{};
    ipc_rfs_nv_write_item_response_data resp{};
    RfsMessage response;
    bool ok = false;

    response.command = request.command;
    response.id = request.id;

    if (request.data.size() >= sizeof(req)) {
        std::memcpy(&req, request.data.data(), sizeof(req));
        if (request.data.size() - sizeof(req) >= req.length) {
            Bytes data(request.data.begin() + sizeof(req),
                       request.data.begin() + sizeof(req) + req.length);
            ok = nv_.write(req.offset, data);
        }
    }

    if (!ok)
        modem_.log("NV write of %u bytes at %#x failed", req.length, req.offset);

    resp.confirm = ok ? 1 : 0;
    resp.offset = req.offset;
    resp.length = req.length;
    response.data = toBytes(resp);
    modem_.sendRfs(response);
}

}  // namespace shannond
