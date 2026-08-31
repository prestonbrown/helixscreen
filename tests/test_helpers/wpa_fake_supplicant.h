// tests/test_helpers/wpa_fake_supplicant.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file wpa_fake_supplicant.h
 * @brief A fake wpa_supplicant control socket, for protocol-level tests of
 *        WifiBackendWpaSupplicant.
 *
 * The backend's control and monitor connections are AF_UNIX SOCK_DGRAM
 * wpa_ctrl handles: the client binds its own (abstract) address, sends
 * command datagrams to the daemon's socket path, and — after ATTACH — the
 * daemon pushes unsolicited events back to the monitor client's address.
 * This fake is one bound DGRAM endpoint with a recvfrom loop that answers
 * the command protocol and remembers ATTACH senders so tests can push
 * events at them.
 *
 * Built for the scan-watchdog tests (prestonbrown/helixscreen#1407) — the
 * first protocol-level harness for this backend; the #1036 test used a
 * bound-but-silent socket, which can only pin the failure path.
 */

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace helix_test {

class WpaFakeSupplicant {
  public:
    /// Binds <dir>/wlan0. Call start() before pointing a backend at it.
    explicit WpaFakeSupplicant(std::string dir) : dir_(std::move(dir)) {}
    ~WpaFakeSupplicant() {
        stop();
    }

    WpaFakeSupplicant(const WpaFakeSupplicant&) = delete;
    WpaFakeSupplicant& operator=(const WpaFakeSupplicant&) = delete;

    bool start() {
        const std::string path = dir_ + "/wlan0";
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd_ < 0)
            return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        running_ = true;
        thread_ = std::thread([this]() { serve(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false))
            return;
        ::shutdown(fd_, SHUT_RDWR);
        if (thread_.joinable())
            thread_.join();
        ::close(fd_);
        fd_ = -1;
        ::unlink((dir_ + "/wlan0").c_str());
    }

    /// Push an unsolicited event to every ATTACHed monitor client, the way
    /// wpa_supplicant broadcasts CTRL-EVENT-* lines.
    void push_event(const std::string& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : monitors_) {
            ::sendto(fd_, event.data(), event.size(), 0, reinterpret_cast<const sockaddr*>(&m.addr),
                     m.len);
        }
    }

    /// Commands received so far, in order (SCAN, ATTACH, PING, ...).
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

  private:
    struct Peer {
        sockaddr_storage addr{};
        socklen_t len{};
    };

    void serve() {
        char buf[4096];
        while (running_) {
            sockaddr_storage peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t n =
                ::recvfrom(fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (n <= 0) {
                if (!running_)
                    return;
                continue;
            }
            const std::string cmd(buf, static_cast<size_t>(n));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                commands_.push_back(cmd);
                if (cmd == "ATTACH") {
                    Peer p;
                    p.addr = peer;
                    p.len = peer_len;
                    monitors_.push_back(p);
                }
            }
            reply(peer, peer_len, answer_to(cmd));
        }
    }

    static std::string answer_to(const std::string& cmd) {
        if (cmd == "PING")
            return "PONG";
        if (cmd == "LIST_NETWORKS") {
            // Well-formed empty table: header row only.
            return "network id / ssid / bssid / flags\n";
        }
        // SCAN answers "OK\nOK\n" from real wpa_supplicant (request ack +
        // event-armed ack); classify_scan_reply tolerates the plain form.
        return "OK\n";
    }

    void reply(const sockaddr_storage& peer, socklen_t len, const std::string& msg) {
        ::sendto(fd_, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&peer), len);
    }

    std::string dir_;
    int fd_{-1};
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::vector<Peer> monitors_;
    std::vector<std::string> commands_;
};

/// RAII HELIX_WPA_SOCKET_DIR pin, so a failing assertion cannot leak the env
/// into later tests (same reason helix_test::EnvVarGuard exists for the netd
/// socket).
struct WpaSocketDirGuard {
    explicit WpaSocketDirGuard(const std::string& dir) {
        ::setenv("HELIX_WPA_SOCKET_DIR", dir.c_str(), 1);
    }
    ~WpaSocketDirGuard() {
        ::unsetenv("HELIX_WPA_SOCKET_DIR");
    }
};

} // namespace helix_test
