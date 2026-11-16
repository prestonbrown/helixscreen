// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hv/wsdef.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <future>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace helix::test {

/// A one-connection WebSocket server on an ephemeral 127.0.0.1 port. Upgrade
/// answers the handshake and hands back the first client text frame, unmasked.
/// Silent listens but never accepts: the kernel completes the TCP connect and
/// nobody ever answers, like a wedged daemon.
class LoopbackWsServer {
  public:
    enum class Mode { Upgrade, Silent };

    explicit LoopbackWsServer(Mode mode = Mode::Upgrade) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 4);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        frame_ = promise_.get_future();
        if (mode == Mode::Upgrade) {
            thread_ = std::thread([this] { serve_one(); });
        }
    }

    ~LoopbackWsServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        ::close(listen_fd_);
    }

    LoopbackWsServer(const LoopbackWsServer&) = delete;
    LoopbackWsServer& operator=(const LoopbackWsServer&) = delete;

    [[nodiscard]] int port() const {
        return port_;
    }

    /// The first text frame's payload, or nullopt if none arrived in time.
    std::optional<std::string> wait_for_text_frame(std::chrono::milliseconds timeout) {
        if (frame_.wait_for(timeout) != std::future_status::ready) {
            return std::nullopt;
        }
        return frame_.get();
    }

    /// A port nothing listens on: bound once to reserve a number, then closed.
    static int closed_port() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        ::close(fd);
        return ntohs(addr.sin_port);
    }

  private:
    static bool read_some(int fd, std::string& buf, int timeout_ms) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            return false;
        }
        char chunk[1024];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        return true;
    }

    void serve_one() {
        std::optional<std::string> result;
        pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 3000) > 0) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd >= 0) {
                result = handle(fd);
                ::close(fd);
            }
        }
        promise_.set_value(result.value_or(std::string{}));
    }

    static std::optional<std::string> handle(int fd) {
        std::string buf;
        while (buf.find("\r\n\r\n") == std::string::npos) {
            if (!read_some(fd, buf, 2000)) {
                return std::nullopt;
            }
        }
        const std::string head = buf.substr(0, buf.find("\r\n\r\n") + 4);
        buf.erase(0, head.size());
        const std::string key_name = "Sec-WebSocket-Key: ";
        const auto k = head.find(key_name);
        if (k == std::string::npos) {
            return std::nullopt;
        }
        const auto k_end = head.find("\r\n", k);
        const std::string key = head.substr(k + key_name.size(), k_end - k - key_name.size());
        char accept[64] = {0};
        ws_encode_key(key.c_str(), accept);
        const std::string reply = std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                              "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                              "Sec-WebSocket-Accept: ") +
                                  accept + "\r\n\r\n";
        ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);

        // Client frame: FIN|opcode, MASK|len (126 = 16-bit length), mask, data.
        while (buf.size() < 2) {
            if (!read_some(fd, buf, 2000))
                return std::nullopt;
        }
        const auto opcode = static_cast<uint8_t>(buf[0]) & 0x0F;
        size_t len = static_cast<uint8_t>(buf[1]) & 0x7F;
        size_t header = 2;
        if (len == 126) {
            while (buf.size() < 4) {
                if (!read_some(fd, buf, 2000))
                    return std::nullopt;
            }
            len = (static_cast<size_t>(static_cast<uint8_t>(buf[2])) << 8) |
                  static_cast<uint8_t>(buf[3]);
            header = 4;
        }
        const bool masked = (static_cast<uint8_t>(buf[1]) & 0x80) != 0;
        const size_t total = header + (masked ? 4 : 0) + len;
        while (buf.size() < total) {
            if (!read_some(fd, buf, 2000))
                return std::nullopt;
        }
        if (opcode != WS_OPCODE_TEXT || !masked) {
            return std::nullopt;
        }
        std::string payload = buf.substr(header + 4, len);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ buf[header + (i % 4)]);
        }
        // Answer the client's close so it can hang up cleanly.
        std::string rest;
        read_some(fd, rest, 500);
        const char close_frame[2] = {static_cast<char>(0x88), 0};
        ::send(fd, close_frame, sizeof(close_frame), MSG_NOSIGNAL);
        return payload;
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::promise<std::string> promise_;
    std::future<std::string> frame_;
    std::thread thread_;
};

} // namespace helix::test
