// SPDX-License-Identifier: GPL-2.0-or-later
//
// shannond for Linux: the core on the kernel's WWAN ports, controlled with
// one-line commands on a Unix socket (try "help"). Events go to every
// connected client. Nothing is dialled, sent or entered without a command:
// the SIM in the phone is the user's own.
//
//   shannond [--nv FILE] [--nv-size N] [--socket PATH] [--apn APN]
//          [--audio-hook PATH] [--radio-off] [--ims-ignore-vops]
//
// VoLTE starts only with "ims on": it brings up the IMS PDN and registers
// with the network as the SIM's subscriber.
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <getopt.h>
#include <map>
#include <memory>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/call.h"
#include "core/data.h"
#include "core/ims/ims.h"
#include "core/ims/linux_network.h"
#include "core/network.h"
#include "core/nv.h"
#include "core/ports.h"
#include "core/sim.h"
#include "core/sms.h"

using namespace shannond;

namespace {

struct Options {
    std::string nv = "/var/lib/modem/nv/nv_data.bin";
    size_t nvSize = 0x100000;
    std::string socket = "/run/shannond.sock";
    std::string apn;
    std::string audioHook = "/usr/libexec/samsungipcd/voicecall.sh";
    bool radio = true;
    bool imsIgnoreVops = false;
};

ims::Ims::Options imsOptions(const Options &options)
{
    ims::Ims::Options o;
    o.ownPdn = true;  // nobody else brings up the IMS PDN on Linux
    o.ignoreVops = options.imsIgnoreVops;
    return o;
}

void logLine(const std::string &line)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::fprintf(stderr, "[%5ld.%03ld] %s\n", ts.tv_sec, ts.tv_nsec / 1000000, line.c_str());
}

std::string hex(const Bytes &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::optional<Bytes> unhex(const std::string &s)
{
    if (s.size() % 2)
        return std::nullopt;
    Bytes bytes;
    for (size_t i = 0; i < s.size(); i += 2) {
        char *end;
        const std::string pair = s.substr(i, 2);
        const long v = std::strtol(pair.c_str(), &end, 16);
        if (*end)
            return std::nullopt;
        bytes.push_back(static_cast<uint8_t>(v));
    }
    return bytes;
}

std::string ip4(const std::array<uint8_t, 4> &a)
{
    return std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) + "." +
           std::to_string(a[3]);
}

std::string ip6(const std::array<uint8_t, 16> &a)
{
    char s[64];
    std::snprintf(s, sizeof(s), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                  a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12],
                  a[13], a[14], a[15]);
    return s;
}

// Runs the audio hook for the length of a call: it switches the mixer to
// the modem and back when a line arrives on its stdin
class AudioHook {
public:
    explicit AudioHook(std::string path) : path_(std::move(path)) {}
    ~AudioHook() { stop(); }

    void start()
    {
        if (pid_ > 0 || path_.empty() || access(path_.c_str(), X_OK) < 0)
            return;
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) < 0)
            return;
        pid_ = fork();
        if (pid_ == 0) {
            dup2(pipefd[0], STDIN_FILENO);
            execl(path_.c_str(), path_.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        close(pipefd[0]);
        fd_ = pipefd[1];
    }

    void stop()
    {
        if (fd_ >= 0) {
            (void)!write(fd_, "\n", 1);
            close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

private:
    std::string path_;
    pid_t pid_ = -1;
    int fd_ = -1;
};

class Daemon {
public:
    Daemon(EventLoop &loop, int fmt, int rfs, const Options &options, const std::string &wwan)
        : loop_(loop), options_(options), modem_(loop, fmt, rfs), nv_(options.nv, options.nvSize),
          rfs_(modem_, nv_), sim_(modem_), network_(modem_),
          data_(modem_, DataLinks::wwan(wwan)), sms_(modem_), calls_(modem_),
          hook_(options.audioHook),
          ims_(modem_, sim_, network_, data_,
               [](const DataCall &pdn) { return std::make_unique<ims::LinuxImsNetwork>(pdn.iface); },
               imsOptions(options))
    {
        modem_.setLog(logLine);
        modem_.onClosed([this] {
            event("modem gone");
            loop_.stop();
        });
        nv_.onWrite([this](size_t offset, size_t length) {
            modem_.log("NV write: %zu bytes at %#zx", length, offset);
        });

        network_.onReady([this] {
            network_.probeModes();
            if (!options_.apn.empty()) {
                DataProfile p;
                p.apn = options_.apn;
                data_.setAttachProfile(p);
            }
            if (options_.radio)
                network_.setRadioPower(true);
        });
        network_.onChanged([this] {
            const std::string line = std::string("reg ") + regStateName(network_.data().state) + " " +
                                     accessTechName(network_.data().tech) + " voice " +
                                     regStateName(network_.voice().state);
            if (line != lastReg_) {
                lastReg_ = line;
                event(line);
            }
        });
        sim_.onChanged([this] { event(std::string("sim ") + simStateName(sim_.state())); });
        sms_.onIncoming([this](const Bytes &pdu, bool statusReport) {
            event((statusReport ? "sms-report " : "sms ") + hex(pdu));
            sms_.acknowledge(true, false);
        });
        calls_.onChanged([this] { event("calls " + callList()); });
        calls_.onAudio([this](bool on) {
            if (on)
                hook_.start();
            else
                hook_.stop();
        });
        data_.onChanged([this](const DataCall &c) {
            event("data " + std::to_string(c.cid) + (c.active ? " up " + c.iface : " down"));
        });
        ims_.onChanged([this] {
            std::string line = "ims " + ims_.status();
            if (line != lastIms_) {
                lastIms_ = line;
                event(line);
            }
        });

        if (!nv_.valid())
            modem_.log("warning: %s has no valid checksum", options_.nv.c_str());
    }

    void client(int fd)
    {
        clients_[fd] = "";
        loop_.watch(fd, EPOLLIN, [this, fd](uint32_t) { readClient(fd); });
        reply(fd, "shannond ready; try help");
    }

private:
    void event(const std::string &line)
    {
        logLine("event: " + line);
        for (auto &[fd, buffer] : clients_)
            reply(fd, "event " + line);
    }

    void reply(int fd, const std::string &line)
    {
        const std::string out = line + "\n";
        (void)!send(fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    }

    void readClient(int fd)
    {
        char buffer[1024];
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            loop_.unwatch(fd);
            close(fd);
            clients_.erase(fd);
            return;
        }
        std::string &pending = clients_[fd];
        pending.append(buffer, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            command(fd, line);
            if (!clients_.count(fd))
                return;
        }
    }

    std::string callList()
    {
        std::string s;
        for (auto &c : calls_.list())
            s += std::to_string(c.id) + ":" + callStateName(c.state) + ":" + (c.incoming ? "in" : "out") +
                 ":" + c.number + " ";
        return s.empty() ? "none" : s;
    }

    void command(int fd, const std::string &line)
    {
        std::istringstream in(line);
        std::string cmd, arg1, arg2, arg3;
        in >> cmd >> arg1 >> arg2 >> arg3;
        auto done = [this, fd, line](bool ok) { reply(fd, (ok ? "ok " : "error ") + line); };

        if (cmd == "help") {
            reply(fd, "status | radio on|off | data connect CID APN [ims] | data disconnect CID | "
                      "data list | sms send TPDU-HEX | call dial NUMBER | call answer | "
                      "call hangup | call list | dtmf TONE | sim apps | sim auth RAND AUTN | "
                      "sim apdu HEX | ims on|off|status | quit");
        } else if (cmd == "status") {
            reply(fd, "sim " + std::string(simStateName(sim_.state())) + ", data " +
                          regStateName(network_.data().state) + " on " +
                          accessTechName(network_.data().tech) + ", voice " +
                          regStateName(network_.voice().state) + ", operator " +
                          network_.operatorCode() + ", signal " + std::to_string(network_.signal()) +
                          ", radio " + (network_.radioOn() ? "on" : "off"));
        } else if (cmd == "radio" && (arg1 == "on" || arg1 == "off")) {
            network_.setRadioPower(arg1 == "on", done);
        } else if (cmd == "data" && arg1 == "connect" && !arg2.empty() && !arg3.empty()) {
            DataProfile p;
            p.apn = arg3;
            std::string flag;
            in >> flag;
            p.ims = flag == "ims";
            data_.connect(std::stoul(arg2), p, [this, fd](std::optional<DataCall> c) {
                if (!c) {
                    reply(fd, "error data connect");
                    return;
                }
                std::string s = "ok data " + std::to_string(c->cid) + " " + c->iface;
                if (c->ipv4)
                    s += " ipv4 " + ip4(*c->ipv4);
                for (auto &d : c->dns4)
                    s += " dns " + ip4(d);
                for (auto &p4 : c->pcscf4)
                    s += " pcscf " + ip4(p4);
                for (auto &p6 : c->pcscf6)
                    s += " pcscf " + ip6(p6);
                if (c->mtu)
                    s += " mtu " + std::to_string(c->mtu);
                reply(fd, s);
            });
        } else if (cmd == "data" && arg1 == "disconnect" && !arg2.empty()) {
            data_.disconnect(std::stoul(arg2), done);
        } else if (cmd == "data" && arg1 == "list") {
            std::string s = "data:";
            for (auto &[cid, c] : data_.calls())
                s += " " + std::to_string(cid) + ":" + c.iface;
            reply(fd, s);
        } else if (cmd == "sms" && arg1 == "send") {
            auto tpdu = unhex(arg2);
            if (!tpdu) {
                reply(fd, "error bad hex");
                return;
            }
            sms_.send(*tpdu, [this, fd](const SmsSendResult &r) {
                reply(fd, r.ok() ? "ok sms ref " + std::to_string(r.reference)
                                 : "error sms cause " + std::to_string(r.rpCause));
            });
        } else if (cmd == "call" && arg1 == "dial" && !arg2.empty()) {
            calls_.dial(arg2, done);
        } else if (cmd == "call" && arg1 == "answer") {
            calls_.answer(done);
        } else if (cmd == "call" && arg1 == "hangup") {
            calls_.hangup(done);
        } else if (cmd == "call" && arg1 == "list") {
            reply(fd, "calls " + callList());
        } else if (cmd == "dtmf" && arg1.size() == 1) {
            const char tone = arg1[0];
            calls_.dtmf(tone, true, [this, tone, done](bool ok) {
                loop_.after(std::chrono::milliseconds(150),
                            [this, tone, done] { calls_.dtmf(tone, false, done); });
                if (!ok)
                    done(false);
            });
        } else if (cmd == "sim" && arg1 == "apps") {
            std::string s = "apps:";
            for (auto &a : sim_.apps())
                s += " type " + std::to_string(a.type) + " aid " + hex(a.aid);
            reply(fd, s);
        } else if (cmd == "sim" && arg1 == "auth") {
            auto rand = unhex(arg2), autn = unhex(arg3);
            if (!rand || !autn) {
                reply(fd, "error bad hex");
                return;
            }
            sim_.authenticate(0, *rand, *autn, [this, fd](std::optional<SimAuthResult> r) {
                if (!r) {
                    reply(fd, "error auth");
                    return;
                }
                std::string s = "ok auth status " + std::to_string(r->status);
                for (auto &v : r->values)
                    s += " " + hex(v);
                reply(fd, s);
            });
        } else if (cmd == "sim" && arg1 == "apdu") {
            auto apdu = unhex(arg2);
            if (!apdu) {
                reply(fd, "error bad hex");
                return;
            }
            sim_.transmitApdu(*apdu, [this, fd](std::optional<Bytes> r) {
                reply(fd, r ? "ok apdu " + hex(*r) : "error apdu");
            });
        } else if (cmd == "quit") {
            loop_.unwatch(fd);
            close(fd);
            clients_.erase(fd);
        } else if (cmd == "ims" && (arg1 == "on" || arg1 == "off")) {
            ims_.setEnabled(arg1 == "on");
            reply(fd, "ok ims " + ims_.status());
        } else if (cmd == "ims" && (arg1 == "status" || arg1.empty())) {
            reply(fd, "ims " + ims_.status());
        } else {
            reply(fd, "error unknown command: " + line);
        }
    }

    EventLoop &loop_;
    const Options &options_;
    Modem modem_;
    NvStore nv_;
    RfsService rfs_;
    Sim sim_;
    Network network_;
    Data data_;
    Sms sms_;
    Calls calls_;
    AudioHook hook_;
    ims::Ims ims_;
    std::map<int, std::string> clients_;
    std::string lastReg_;
    std::string lastIms_;
};

int listenSocket(const std::string &path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (fd < 0 || path.size() >= sizeof(addr.sun_path))
        return -1;
    std::strcpy(addr.sun_path, path.c_str());
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0600);
    return fd;
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    static const option longOptions[] = {
        {"nv", required_argument, nullptr, 'n'},
        {"nv-size", required_argument, nullptr, 'z'},
        {"socket", required_argument, nullptr, 's'},
        {"apn", required_argument, nullptr, 'a'},
        {"audio-hook", required_argument, nullptr, 'h'},
        {"radio-off", no_argument, nullptr, 'r'},
        {"ims-ignore-vops", no_argument, nullptr, 'v'},
        {},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (c) {
        case 'n': options.nv = optarg; break;
        case 'z': options.nvSize = std::stoul(optarg, nullptr, 0); break;
        case 's': options.socket = optarg; break;
        case 'a': options.apn = optarg; break;
        case 'h': options.audioHook = optarg; break;
        case 'r': options.radio = false; break;
        case 'v': options.imsIgnoreVops = true; break;
        default:
            std::fprintf(stderr, "usage: shannond [--nv FILE] [--nv-size N] [--socket PATH] "
                                 "[--apn APN] [--audio-hook PATH] [--radio-off] [--ims-ignore-vops]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    auto ipcPort = findWwanPort(kPortTypeIpc);
    auto rfsPort = findWwanPort(kPortTypeRfs);
    if (!ipcPort || !rfsPort) {
        logLine("no Samsung IPC WWAN ports (is exynos_modem loaded without legacy_abi?)");
        return 1;
    }
    auto wwan = wwanDeviceOf(*ipcPort).value_or("wwan0");

    logLine("opening " + *ipcPort + " (this starts the modem)");
    const int fmt = openPort(*ipcPort);
    if (fmt < 0) {
        logLine("cannot open " + *ipcPort + ": " + std::strerror(errno));
        return 1;
    }
    const int rfs = openPort(*rfsPort);
    if (rfs < 0) {
        logLine("cannot open " + *rfsPort + ": " + std::strerror(errno));
        return 1;
    }

    EventLoop loop;
    Daemon daemon(loop, fmt, rfs, options, wwan);

    const int listener = listenSocket(options.socket);
    if (listener < 0) {
        logLine("cannot listen on " + options.socket);
        return 1;
    }
    loop.watch(listener, EPOLLIN, [&](uint32_t) {
        const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0)
            daemon.client(fd);
    });

    logLine("shannond running on " + *ipcPort + " + " + *rfsPort + ", socket " + options.socket);
    loop.run();

    // The modem went away: exit, the service manager starts us again and
    // the next open restarts the modem
    unlink(options.socket.c_str());
    return 3;
}
