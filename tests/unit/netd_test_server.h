// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file netd_test_server.h
 * @brief Shared fixtures for tests that talk to a fake netd daemon.
 *
 * Two pieces, both reusable across test TUs:
 *
 * - EnvVarGuard — RAII save/restore of one environ entry, so a failed
 *   REQUIRE cannot leak a repointed HELIX_NETD_* into the rest of the suite.
 * - wait_until / b64 / unb64 — the polling wait and base64 helpers every
 *   netd test TU needs; one copy here instead of one per file.
 * - NetdFakeServer — a real AF_UNIX SOCK_STREAM listener with one acceptor
 *   thread. It plays the daemon side of the netd line protocol well enough
 *   to exercise a client backend: it records every complete line any client
 *   ever sent (byte-exact), pushes lines to all connected clients on demand,
 *   and can kill every connection at once (the "daemon died" case). Several
 *   sequential connections are supported (the reconnect tests need them).
 *
 * Everything is bounded: poll timeouts everywhere, so a client that never
 * connects or never speaks cannot wedge the acceptor thread or the suite.
 */

#include "../../include/netd_protocol.h"
#include "hv/base64.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace helix_test {

/// Bounded polling wait on the real clock with a small step. The fake server
/// exposes no condition variable, so tests poll its recorded state.
///
/// The ceiling is deliberately generous: the wait returns the moment the
/// predicate holds, so its size is paid only on a run that was going to fail
/// anyway, while a shard-parallel run on a saturated host can starve a
/// backend's dispatch thread for seconds. A tight ceiling buys nothing and
/// turns that starvation into a red suite.
inline bool wait_until(const std::function<bool()>& pred, int timeout_ms = 30000,
                       int step_ms = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return pred();
}

/// Base64 of a raw string, the way the netd wire protocol encodes fields.
inline std::string b64(const std::string& raw) {
    return hv::Base64Encode(reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<unsigned int>(raw.size()));
}

/// Inverse of b64().
inline std::string unb64(const std::string& encoded) {
    return hv::Base64Decode(encoded.c_str(), static_cast<unsigned int>(encoded.size()));
}

// Saves one environ entry on construction and restores it (set or unset) on
// destruction, so a failed REQUIRE cannot leak a repointed HELIX_NETD_* into
// the rest of the suite. socket_path()/available() read getenv() per call, so
// a test repoints the module within one process by setting the variable.
class EnvVarGuard {
  public:
    explicit EnvVarGuard(const char* key) : key_(key) {
        if (const char* v = ::getenv(key)) {
            was_set_ = true;
            saved_ = v;
        }
    }
    ~EnvVarGuard() {
        if (was_set_) {
            ::setenv(key_.c_str(), saved_.c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }
    EnvVarGuard(const EnvVarGuard&) = delete;
    EnvVarGuard& operator=(const EnvVarGuard&) = delete;

    void set(const std::string& value) {
        ::setenv(key_.c_str(), value.c_str(), 1);
    }
    void unset() {
        ::unsetenv(key_.c_str());
    }

  private:
    std::string key_;
    std::string saved_;
    bool was_set_ = false;
};

/**
 * @brief Fake netd daemon: AF_UNIX stream listener + one acceptor thread.
 *
 * The single thread polls the listening socket and every open client
 * connection. Data arriving on a client is framed with helix::netd's own
 * LineAssembler (this fixture exists to fake THIS daemon, so it shares the
 * wire framing rule) and every complete line is recorded byte-exact. A
 * client that closes is dropped from the connection set automatically.
 *
 * All public methods are thread-safe and bounded; stop() joins the thread,
 * closes everything and unlinks the socket path.
 */
class NetdFakeServer {
  public:
    NetdFakeServer() = default;
    ~NetdFakeServer() {
        stop();
    }
    NetdFakeServer(const NetdFakeServer&) = delete;
    NetdFakeServer& operator=(const NetdFakeServer&) = delete;

    /// Bind and listen on @p path (a stale socket file there is unlinked
    /// first), then start the acceptor thread. False on any OS failure.
    bool start(const std::string& path) {
        path_ = path;

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0)
            return false;
        if (!helix::netd::set_nonblocking(listen_fd_, true)) {
            teardown();
            return false;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) {
            teardown();
            return false;
        }
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        // A leftover socket file from a crashed run would fail the bind.
        ::unlink(path_.c_str());
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            teardown();
            return false;
        }
        if (::listen(listen_fd_, 8) != 0) {
            teardown();
            return false;
        }

        running_.store(true);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    /// Write @p line + '\n' to every currently connected client.
    void push_line(const std::string& line) {
        const std::string framed = line + "\n";
        std::lock_guard<std::mutex> lock(mtx_);
        for (int fd : clients_) {
            // Small lines on a stream socket: the kernel buffer absorbs them
            // even if the client is momentarily not reading. A dead client
            // surfaces as an error here and is reaped by the acceptor thread.
            (void)::send(fd, framed.data(), framed.size(), MSG_NOSIGNAL);
        }
    }

    /// Server-side close of every client connection — the "daemon died"
    /// case. Connected clients see EOF immediately.
    void close_clients() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int fd : clients_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        clients_.clear();
        assemblers_.clear();
    }

    /// Number of currently open client connections.
    size_t connection_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return clients_.size();
    }

    /// Every complete line ever received from any client, byte-exact, in
    /// arrival order, across all connections.
    std::vector<std::string> recorded_lines() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return recorded_;
    }

    size_t recorded_line_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return recorded_.size();
    }

    /// Stop the acceptor thread, close the listener and any remaining
    /// clients, unlink the socket path. Idempotent.
    void stop() {
        running_.store(false);
        if (thread_.joinable())
            thread_.join();
        teardown();
    }

  private:
    static constexpr int kPollMs = 50; ///< Accept/recv poll granularity.

    void serve() {
        std::vector<int> snapshot;
        while (running_.load()) {
            // Poll outside the lock; membership is re-checked under the lock
            // before any fd is touched, so a close_clients() in between can
            // never hand us a recycled fd number.
            {
                std::lock_guard<std::mutex> lock(mtx_);
                snapshot = clients_;
            }
            std::vector<pollfd> pfds;
            pfds.reserve(snapshot.size() + 1);
            pfds.push_back(pollfd{listen_fd_, POLLIN, 0});
            for (int fd : snapshot)
                pfds.push_back(pollfd{fd, POLLIN, 0});

            const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kPollMs);
            if (ready <= 0)
                continue;

            if (pfds[0].revents & POLLIN)
                accept_all();

            for (size_t i = 1; i < pfds.size(); ++i) {
                if (pfds[i].revents == 0)
                    continue;
                drain_client(pfds[i].fd);
            }
        }
    }

    void accept_all() {
        // The listener is non-blocking: draining the backlog ends with EAGAIN,
        // never a blocking accept that would wedge the thread.
        while (true) {
            const int cfd = ::accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0)
                return; // EAGAIN: backlog drained (or transient error)
            // accept() does not inherit the listener's flags, so the client fd
            // needs its own: drain_client() must never park on a peer that
            // announced readability and then sent nothing.
            (void)helix::netd::set_nonblocking(cfd, true);
            std::lock_guard<std::mutex> lock(mtx_);
            clients_.push_back(cfd);
            assemblers_.emplace(cfd, helix::netd::LineAssembler{});
        }
    }

    void drain_client(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        // Membership re-check under the lock (see serve()).
        if (std::find(clients_.begin(), clients_.end(), fd) == clients_.end())
            return;

        char buffer[4096];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            auto it = assemblers_.find(fd);
            if (it != assemblers_.end()) {
                for (std::string& line : it->second.feed(std::string_view(buffer, n)))
                    recorded_.push_back(std::move(line));
            }
            return;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return; // Spurious wakeup: nothing to read, keep the connection.
        // n == 0 (client closed) or a real error: drop the connection.
        ::close(fd);
        clients_.erase(std::find(clients_.begin(), clients_.end(), fd));
        assemblers_.erase(fd);
    }

    void teardown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (int fd : clients_)
                ::close(fd);
            clients_.clear();
            assemblers_.clear();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (!path_.empty()) {
            ::unlink(path_.c_str());
            path_.clear();
        }
    }

    mutable std::mutex mtx_;
    std::string path_;
    int listen_fd_ = -1;
    std::vector<int> clients_;
    std::unordered_map<int, helix::netd::LineAssembler> assemblers_;
    std::vector<std::string> recorded_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace helix_test
