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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <system_error>
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
        for (auto& t : late_threads_) {
            if (t.joinable())
                t.join();
        }
        late_threads_.clear();
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

    /// Answer @p cmd with @p reply from now on, in place of the built-in one.
    void set_reply(const std::string& cmd, const std::string& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[cmd] = reply;
    }

    /// After answering @p cmd, send @p extra as a second datagram to the same
    /// client: the shape of a late reply to an earlier, timed-out request.
    void set_trailing_datagram(const std::string& cmd, const std::string& extra) {
        std::lock_guard<std::mutex> lock(mutex_);
        trailing_[cmd] = extra;
    }

    /// Datagrams sent through set_trailing_datagram() so far.
    int trailing_sent() const {
        return trailing_sent_.load();
    }

    /// Answer the next @p cmd only after @p delay_ms, past the client's
    /// timeout, to the address that asked. Later @p cmd requests answer at once.
    void delay_next_reply(const std::string& cmd, int delay_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        delayed_[cmd] = delay_ms;
    }

    /// Delayed replies sent so far, and how many reached a live client address.
    int late_replies_sent() const {
        return late_sent_.load();
    }
    int late_replies_delivered() const {
        return late_delivered_.load();
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
            int delay_ms = -1;
            std::string trailing;
            bool has_trailing = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto d = delayed_.find(cmd);
                if (d != delayed_.end()) {
                    delay_ms = d->second;
                    delayed_.erase(d);
                }
                const auto t = trailing_.find(cmd);
                if (t != trailing_.end()) {
                    trailing = t->second;
                    has_trailing = true;
                }
            }
            if (delay_ms >= 0) {
                late_threads_.emplace_back(
                    [this, peer, peer_len, delay_ms, msg = answer_to(cmd)]() {
                        for (int waited = 0; waited < delay_ms && running_; waited += 10)
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        const ssize_t sent =
                            ::sendto(fd_, msg.data(), msg.size(), 0,
                                     reinterpret_cast<const sockaddr*>(&peer), peer_len);
                        late_sent_.fetch_add(1);
                        if (sent >= 0)
                            late_delivered_.fetch_add(1);
                    });
                continue;
            }
            reply(peer, peer_len, answer_to(cmd));
            if (has_trailing) {
                reply(peer, peer_len, trailing);
                trailing_sent_.fetch_add(1);
            }
        }
    }

    std::string answer_to(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = replies_.find(cmd);
            if (it != replies_.end())
                return it->second;
        }
        if (cmd == "PING")
            return "PONG";
        if (cmd == "LIST_NETWORKS") {
            // Well-formed empty table: header row only.
            return "network id / ssid / bssid / flags\n";
        }
        if (cmd == "ADD_NETWORK") {
            // The supplicant allocates the next network id; 0 is the first.
            return "0\n";
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
    std::map<std::string, std::string> replies_;
    std::map<std::string, std::string> trailing_;
    std::map<std::string, int> delayed_;
    std::vector<std::thread> late_threads_;
    std::atomic<int> trailing_sent_{0};
    std::atomic<int> late_sent_{0};
    std::atomic<int> late_delivered_{0};
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

/// Hermetic /sys/class/net for the same tests: creates
/// <base>/sys/class/net/wlan0/wireless (wlan0 to match the fake control
/// socket's name), pins HELIX_WPA_NET_SYSFS to the net dir and
/// HELIX_WPA_SYS_ROOT to <base>/sys (no rfkill switch unless a case adds one), so the
/// backend's hardware probe finds a radio on machines without one (CI
/// runners). The dtor unpins and removes the whole <base>/sys tree, including
/// anything a case added under it.
struct WpaNetSysfsGuard {
    explicit WpaNetSysfsGuard(const std::string& base_dir) {
        sys_dir_ = base_dir + "/sys";
        const std::string net_dir = sys_dir_ + "/class/net";
        std::error_code ec;
        std::filesystem::create_directories(net_dir + "/wlan0/wireless", ec);
        ::setenv("HELIX_WPA_NET_SYSFS", net_dir.c_str(), 1);
        ::setenv("HELIX_WPA_SYS_ROOT", sys_dir_.c_str(), 1);
    }
    ~WpaNetSysfsGuard() {
        ::unsetenv("HELIX_WPA_SYS_ROOT");
        ::unsetenv("HELIX_WPA_NET_SYSFS");
        std::error_code ec;
        std::filesystem::remove_all(sys_dir_, ec);
    }

  private:
    std::string sys_dir_;
};

/// Hermetic process table for the same tests: pins HELIX_WPA_PROC_ROOT to an
/// empty <base>/proc, so the backend's wpa_supplicant daemon scans read no
/// real PIDs. A real /proc makes start()'s duration a property of the host:
/// on one holding ~95k zombie processes each scan costs a second, and init
/// overruns the 5 s wait start() gives it.
struct WpaProcRootGuard {
    explicit WpaProcRootGuard(const std::string& base_dir) {
        proc_dir_ = base_dir + "/proc";
        std::error_code ec;
        std::filesystem::create_directories(proc_dir_, ec);
        ::setenv("HELIX_WPA_PROC_ROOT", proc_dir_.c_str(), 1);
    }
    ~WpaProcRootGuard() {
        ::unsetenv("HELIX_WPA_PROC_ROOT");
        std::error_code ec;
        std::filesystem::remove_all(proc_dir_, ec);
    }

  private:
    std::string proc_dir_;
};

} // namespace helix_test
