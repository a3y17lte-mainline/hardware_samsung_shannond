// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/frame.h"
#include "tests/test.h"

using namespace shannond;

TEST(fmt_round_trip)
{
    Message m;
    m.command = 0x0d1b;
    m.type = 3;
    m.mseq = 7;
    m.aseq = 9;
    m.data = {1, 2, 3};

    const Bytes frame = encodeFmt(m);
    CHECK(frame.size() == 10);
    CHECK(frame[0] == 10 && frame[1] == 0);
    CHECK(frame[2] == 7 && frame[3] == 9);
    CHECK(frame[4] == 0x0d && frame[5] == 0x1b && frame[6] == 3);

    auto back = decodeFmt(frame);
    CHECK(back);
    CHECK(back->command == 0x0d1b && back->type == 3);
    CHECK(back->mseq == 7 && back->aseq == 9);
    CHECK(back->data == m.data);
}

TEST(fmt_rejects_bad_lengths)
{
    CHECK(!decodeFmt(Bytes{1, 2, 3}));
    // Header says 20 bytes, frame has 8
    CHECK(!decodeFmt(Bytes{20, 0, 1, 0, 1, 1, 1, 0}));
    // Header says 3 bytes: shorter than a header
    CHECK(!decodeFmt(Bytes{3, 0, 1, 0, 1, 1, 1}));
    // Trailing bytes beyond the length are ignored
    auto m = decodeFmt(Bytes{8, 0, 1, 0, 1, 1, 1, 0xaa, 0xbb});
    CHECK(m && m->data == Bytes{0xaa});
}

TEST(rfs_round_trip)
{
    RfsMessage m{0x01, 4, {9, 8, 7}};
    const Bytes frame = encodeRfs(m);
    CHECK(frame.size() == 9);
    CHECK(frame[0] == 9 && frame[3] == 0 && frame[4] == 1 && frame[5] == 4);

    auto back = decodeRfs(frame);
    CHECK(back && back->command == 1 && back->id == 4 && back->data == m.data);
    CHECK(!decodeRfs(Bytes{100, 0, 0, 0, 1, 1}));
}

TEST(assembler_joins_pieces_and_splits_reads)
{
    FrameAssembler rfs(kRfsHeaderSize, 4, 0x80000);
    RfsMessage a{0x02, 1, Bytes(3000, 0xaa)};
    RfsMessage b{0x03, 2, Bytes{1, 2}};
    Bytes both = encodeRfs(a);
    const Bytes second = encodeRfs(b);
    both.insert(both.end(), second.begin(), second.end());

    // Three reads: 2048, 2048, the rest; two messages come out
    std::vector<Bytes> out;
    for (size_t at = 0; at < both.size(); at += 2048) {
        auto frames = rfs.add({both.data() + at, std::min<size_t>(2048, both.size() - at)});
        out.insert(out.end(), frames.begin(), frames.end());
    }
    CHECK(out.size() == 2);
    CHECK(decodeRfs(out[0])->data == a.data);
    CHECK(decodeRfs(out[1])->id == 2);
    CHECK(rfs.pending() == 0);

    // A length that cannot be: dropped, reported, and the next one works
    bool bad = false;
    const Bytes junk{0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(rfs.add(junk, &bad).empty() && bad && rfs.pending() == 0);
    CHECK(rfs.add(second, &bad).size() == 1 && !bad);
}
