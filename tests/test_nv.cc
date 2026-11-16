// SPDX-License-Identifier: GPL-2.0-or-later
//
// The NV store and the RFS service, on a temporary file.
#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include "core/modem.h"
#include "core/nv.h"
#include "sim/fake_modem.h"
#include "tests/test.h"

extern "C" {
#include <samsung-ipc.h>
}

using namespace shannond;

namespace {

constexpr size_t kSize = 0x1000;

struct TempNv {
    std::string dir;
    std::string path;

    TempNv()
    {
        char tmpl[] = "/tmp/shannond-nv-XXXXXX";
        dir = mkdtemp(tmpl);
        path = dir + "/nv_data.bin";
        Bytes data(kSize);
        for (size_t i = 0; i < kSize; i++)
            data[i] = i & 0xff;
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<char *>(data.data()), kSize);
        std::ofstream(path + ".md5") << md5Hex(data, "Samsung_Android_RIL");
    }

    ~TempNv()
    {
        unlink((path + ".md5").c_str());
        unlink(path.c_str());
        rmdir(dir.c_str());
    }
};

}  // namespace

TEST(nv_store_reads_writes_and_keeps_the_checksum)
{
    TempNv tmp;
    NvStore nv(tmp.path, kSize);
    CHECK(nv.valid());

    auto part = nv.read(0x10, 4);
    CHECK(part && *part == (Bytes{0x10, 0x11, 0x12, 0x13}));
    CHECK(!nv.read(kSize - 2, 4));

    size_t written = 0;
    nv.onWrite([&](size_t, size_t length) { written += length; });
    CHECK(nv.write(0x20, Bytes{0xde, 0xad}));
    CHECK(written == 2);
    CHECK(nv.valid());
    CHECK(*nv.read(0x20, 2) == (Bytes{0xde, 0xad}));
    CHECK(!nv.write(kSize - 1, Bytes{1, 2}));
}

TEST(nv_store_detects_a_bad_checksum)
{
    TempNv tmp;
    std::ofstream(tmp.path + ".md5") << "00000000000000000000000000000000";
    NvStore nv(tmp.path, kSize);
    CHECK(!nv.valid());
    // The wrong size is not valid either
    NvStore wrong(tmp.path, kSize * 2);
    CHECK(!wrong.valid());
}

TEST(rfs_service_answers_reads_and_writes)
{
    TempNv tmp;
    EventLoop loop;
    FakeModem fake(loop);
    Modem modem(loop, fake.fmtFd(), fake.rfsFd());
    NvStore nv(tmp.path, kSize);
    RfsService rfs(modem, nv);

    fake.nvRead(0x100, 8);
    CHECK(loop.runUntil([&] { return fake.rfsReceived.size() == 1; }));
    {
        const RfsMessage &answer = fake.rfsReceived[0];
        CHECK(answer.command == (IPC_RFS_NV_READ_ITEM & 0xff) && answer.id == 1);
        ipc_rfs_nv_read_item_response_header h;
        CHECK(answer.data.size() == sizeof(h) + 8);
        std::memcpy(&h, answer.data.data(), sizeof(h));
        CHECK(h.confirm == 1 && h.offset == 0x100 && h.length == 8);
        CHECK(answer.data[sizeof(h)] == 0x00 && answer.data[sizeof(h) + 7] == 0x07);
    }

    fake.nvWrite(0x200, Bytes{1, 2, 3});
    CHECK(loop.runUntil([&] { return fake.rfsReceived.size() == 2; }));
    {
        ipc_rfs_nv_write_item_response_data w;
        std::memcpy(&w, fake.rfsReceived[1].data.data(), sizeof(w));
        CHECK(w.confirm == 1 && w.offset == 0x200 && w.length == 3);
        CHECK(*nv.read(0x200, 3) == (Bytes{1, 2, 3}));
        CHECK(nv.valid());
    }

    // Beyond the end: refused, nothing written
    fake.nvRead(kSize - 4, 8);
    CHECK(loop.runUntil([&] { return fake.rfsReceived.size() == 3; }));
    CHECK(fake.rfsReceived[2].data[0] == 0);

    // A message longer than a FMT frame can be (NV data comes in big
    // writes): read whole, then refused for its range
    fake.nvWrite(0, Bytes(0x18000, 0x5a));
    CHECK(loop.runUntil([&] { return fake.rfsReceived.size() == 4; }));
    CHECK(fake.rfsReceived[3].data[0] == 0);
    CHECK(nv.valid());

    // In 2 KiB pieces, as the modem sends long messages: joined again
    fake.rfsChunk = 2048;
    Bytes data(3000);  // two pieces, within the 4 KiB test NV file
    for (size_t i = 0; i < data.size(); i++)
        data[i] = static_cast<uint8_t>(i);
    fake.nvWrite(0x100, data);
    CHECK(loop.runUntil([&] { return fake.rfsReceived.size() == 5; }));
    {
        ipc_rfs_nv_write_item_response_data w;
        std::memcpy(&w, fake.rfsReceived[4].data.data(), sizeof(w));
        CHECK(w.confirm == 1 && w.offset == 0x100 && w.length == data.size());
        CHECK(*nv.read(0x100, data.size()) == data);
    }
}
