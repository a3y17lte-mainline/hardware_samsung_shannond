// SPDX-License-Identifier: GPL-2.0-or-later
//
// The request tracker against a raw peer on a socket pair.
#include <atomic>
#include <sys/socket.h>
#include <thread>
#include <poll.h>
#include <unistd.h>

#include "core/modem.h"
#include "tests/test.h"

extern "C" {
#include <samsung-ipc.h>
}

using namespace shannond;

namespace {

struct Pair {
    int fd[2];
    Pair() { socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd); }
    ~Pair()
    {
        close(fd[0]);
        if (fd[1] >= 0)
            close(fd[1]);
    }

    std::optional<Message> receive()
    {
        uint8_t buffer[4096];
        const ssize_t n = read(fd[1], buffer, sizeof(buffer));
        if (n <= 0)
            return std::nullopt;
        return decodeFmt({buffer, static_cast<size_t>(n)});
    }

    // Whether a message is waiting, without blocking on an empty socket
    bool waiting()
    {
        pollfd p{fd[1], POLLIN, 0};
        return poll(&p, 1, 20) > 0;
    }

    void send(const Message &m)
    {
        const Bytes frame = encodeFmt(m);
        (void)!write(fd[1], frame.data(), frame.size());
    }
};

Message genRes(const Message &request, uint16_t code)
{
    ipc_gen_phone_res_data res{};
    res.group = request.command >> 8;
    res.index = request.command & 0xff;
    res.type = request.type;
    res.code = code;
    Message m;
    m.command = IPC_GEN_PHONE_RES;
    m.type = IPC_TYPE_RESP;
    m.aseq = request.mseq;
    m.data = toBytes(res);
    return m;
}

}  // namespace

TEST(set_completes_with_gen_phone_res)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    std::optional<Result> result;
    modem.request(IPC_NET_MODE_SEL, IPC_TYPE_SET, Bytes{8}, [&](const Result &r) { result = r; });

    auto request = pair.receive();
    CHECK(request && request->command == IPC_NET_MODE_SEL && request->mseq != 0);
    CHECK(request->data == Bytes{8});

    pair.send(genRes(*request, IPC_GEN_PHONE_RES_CODE_SUCCESS));
    CHECK(loop.runUntil([&] { return result.has_value(); }));
    CHECK(result->ok && result->code == IPC_GEN_PHONE_RES_CODE_SUCCESS);
    CHECK(modem.pendingCount() == 0);
}

TEST(error_code_fails_the_request)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    std::optional<Result> result;
    modem.request(IPC_NET_MODE_SEL, IPC_TYPE_SET, Bytes{0x10}, [&](const Result &r) { result = r; });
    auto request = pair.receive();
    pair.send(genRes(*request, 0x8001));

    CHECK(loop.runUntil([&] { return result.has_value(); }));
    CHECK(!result->ok && result->code == 0x8001);
}

TEST(get_completes_with_its_response_and_listeners_see_it)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    int seen = 0;
    modem.onMessage(IPC_NET_SERVING_NETWORK, [&](const Message &) { seen++; });

    Bytes payload;
    modem.request(IPC_NET_SERVING_NETWORK, IPC_TYPE_GET, {}, [&](const Result &r) {
        if (r.ok && r.response)
            payload = r.response->data;
    });
    auto request = pair.receive();

    Message response;
    response.command = IPC_NET_SERVING_NETWORK;
    response.type = IPC_TYPE_RESP;
    response.aseq = request->mseq;
    response.data = {1, 2, 3};
    pair.send(response);

    CHECK(loop.runUntil([&] { return !payload.empty(); }));
    CHECK(payload == (Bytes{1, 2, 3}));
    CHECK(seen == 1);
}

TEST(answers_are_matched_by_sequence_not_order)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    // Different groups are with the modem together
    std::vector<int> order;
    modem.request(IPC_NET_MODE_SEL, IPC_TYPE_SET, Bytes{1}, [&](const Result &) { order.push_back(1); });
    modem.request(IPC_SND_MIC_MUTE_CTRL, IPC_TYPE_SET, Bytes{2}, [&](const Result &) { order.push_back(2); });
    auto first = pair.receive();
    auto second = pair.receive();
    CHECK(first && second && first->mseq != second->mseq);

    pair.send(genRes(*second, IPC_GEN_PHONE_RES_CODE_SUCCESS));
    pair.send(genRes(*first, IPC_GEN_PHONE_RES_CODE_SUCCESS));
    CHECK(loop.runUntil([&] { return order.size() == 2; }));
    CHECK(order[0] == 2 && order[1] == 1);
}

TEST(one_request_per_group_at_a_time)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    std::vector<Bytes> answers;
    for (uint8_t i = 1; i <= 3; i++)
        modem.request(IPC_SEC_RSIM_ACCESS, IPC_TYPE_GET, Bytes{i}, [&](const Result &r) {
            answers.push_back(r.ok ? r.response->data : Bytes{});
        });

    // The second request leaves only when the first is answered; answers
    // without a sequence number go to the request that is out
    for (uint8_t i = 1; i <= 3; i++) {
        auto request = pair.receive();
        CHECK(request && request->data == Bytes{i});
        CHECK(!pair.waiting());

        Message response;
        response.command = IPC_SEC_RSIM_ACCESS;
        response.type = IPC_TYPE_RESP;
        response.aseq = i == 2 ? 0 : request->mseq;
        response.data = {static_cast<uint8_t>(0x10 + i)};
        pair.send(response);
        CHECK(loop.runUntil([&] { return answers.size() == i; }));
    }
    CHECK(answers == (std::vector<Bytes>{{0x11}, {0x12}, {0x13}}));
}

TEST(unanswered_request_times_out)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    std::optional<Result> result;
    modem.request(IPC_MISC_ME_SN, IPC_TYPE_GET, {}, [&](const Result &r) { result = r; },
                  std::chrono::milliseconds(30));
    CHECK(loop.runUntil([&] { return result.has_value(); }, std::chrono::seconds(1)));
    CHECK(!result->ok && result->timeout);
}

TEST(notifications_reach_listeners)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    Bytes got;
    modem.onMessage(IPC_DISP_RSSI_INFO, [&](const Message &m) { got = m.data; });

    Message n;
    n.command = IPC_DISP_RSSI_INFO;
    n.type = IPC_TYPE_NOTI;
    n.data = {42};
    pair.send(n);
    CHECK(loop.runUntil([&] { return !got.empty(); }));
    CHECK(got == Bytes{42});
}

TEST(hang_up_fails_pending_requests)
{
    EventLoop loop;
    Pair pair;
    Modem modem(loop, pair.fd[0], -1);

    bool closed = false;
    std::optional<Result> result;
    modem.onClosed([&] { closed = true; });
    modem.request(IPC_MISC_ME_SN, IPC_TYPE_GET, {}, [&](const Result &r) { result = r; });

    close(pair.fd[1]);
    pair.fd[1] = -1;
    CHECK(loop.runUntil([&] { return closed; }));
    CHECK(result && !result->ok && !result->timeout);
}

TEST(post_runs_on_the_loop_from_another_thread)
{
    EventLoop loop;
    std::atomic<int> value{0};
    std::thread other([&] { loop.post([&] { value = 42; }); });
    other.join();
    CHECK(loop.runUntil([&] { return value == 42; }));
}
