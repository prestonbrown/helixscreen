// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_transfer_range.cpp
 * @brief A server that ignores byte ranges must not turn a bounded download
 *        into a whole-file payload
 *
 * download_file_partial asks for the first N bytes, download_file_tail for the
 * last N. A server that ignores Range answers 200 with the entire file, and
 * both functions used to forward that body unclamped: the thumbnail-header
 * fetch on a metadata-less server (one 100 KB request per list entry) then
 * parsed a whole multi-MB gcode file per entry on the single slow-lane worker.
 * The clamps live in the two shared transfer functions; these tests drive the
 * real HTTP client against a local responder that honours or ignores Range.
 */

#include "moonraker_client_mock.h"
#include "moonraker_file_transfer_api.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

std::string make_payload(size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; i++) {
        s[i] = static_cast<char>((i * 7) & 0xFF);
    }
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// One request per connection on loopback. honour_range decides whether a
/// Range header slices the body (206) or is answered with the whole file
/// (200) - the two server behaviours a transfer has to survive.
class RangeResponder {
  public:
    RangeResponder(bool honour_range, const std::string& body)
        : body_(body), honour_range_(honour_range) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 4) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this] { serve(); });
    }

    ~RangeResponder() {
        stop_ = true;
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

  private:
    void serve() {
        while (!stop_) {
            int conn = ::accept(listen_fd_, nullptr, nullptr);
            if (conn < 0) {
                return; // listener closed by the destructor
            }
            handle(conn);
            ::close(conn);
        }
    }

    void handle(int conn) {
        std::string headers;
        char buf[2048];
        while (headers.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }
            headers.append(buf, static_cast<size_t>(n));
            if (headers.size() > 8192) {
                return; // not a request we can answer
            }
        }

        const std::string lowered = to_lower(headers);
        size_t pos = lowered.find("range: bytes=");
        std::string range;
        if (pos != std::string::npos) {
            size_t end = headers.find("\r\n", pos);
            range = headers.substr(pos + 13,
                                   end == std::string::npos ? std::string::npos : end - pos - 13);
        }

        std::string status = "200 OK";
        std::string content = body_;
        if (honour_range_ && !range.empty()) {
            size_t dash = range.find('-');
            if (dash == 0) { // suffix: "bytes=-N" means the last N bytes
                size_t n = static_cast<size_t>(atoi(range.c_str() + 1));
                n = std::min(n, body_.size());
                content = body_.substr(body_.size() - n);
                status = "206 Partial Content";
            } else if (dash != std::string::npos) { // "bytes=a-b"
                size_t a = static_cast<size_t>(atoi(range.c_str()));
                size_t b = static_cast<size_t>(atoi(range.c_str() + dash + 1));
                b = std::min(b, body_.size() - 1);
                if (a <= b) {
                    content = body_.substr(a, b - a + 1);
                    status = "206 Partial Content";
                }
            }
        }

        std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\n";
        resp += "Content-Length: " + std::to_string(content.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += content;
        ::send(conn, resp.data(), resp.size(), 0);
    }

    std::string body_;
    bool honour_range_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

struct TransferOutcome {
    bool ok = false;
    std::string body;
    std::string error;
    std::thread::id callback_thread{};
};

/// Run one transfer and wait for its executor-thread callback. The transfer
/// APIs are fire-and-forget onto HttpExecutor::slow(), so the test synchronizes
/// through a promise instead of polling.
TransferOutcome await(const std::function<void(MoonrakerFileTransferAPI::StringCallback,
                                               MoonrakerFileTransferAPI::ErrorCallback)>& call) {
    std::promise<TransferOutcome> promise;
    auto future = promise.get_future();
    call(
        [&promise](const std::string& body) {
            promise.set_value(TransferOutcome{true, body, "", std::this_thread::get_id()});
        },
        [&promise](const MoonrakerError& error) {
            promise.set_value(
                TransferOutcome{false, "", error.message, std::this_thread::get_id()});
        });
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return future.get();
}

/// The API stores a reference to the base URL string; keep storage alive for
/// the API's lifetime.
struct TransferHarness {
    MoonrakerClientMock client;
    std::string base_url;
    std::unique_ptr<MoonrakerFileTransferAPI> api;

    TransferHarness(const std::string& url) : base_url(url) {
        api = std::make_unique<MoonrakerFileTransferAPI>(client, base_url);
    }
};

constexpr size_t PAYLOAD_SIZE = 64 * 1024;
constexpr size_t PARTIAL_BYTES = 4096;
constexpr size_t TAIL_BYTES = 1024;

} // namespace

TEST_CASE("partial download clamps a Range-ignoring 200 body", "[api][transfer][range]") {
    const std::string payload = make_payload(PAYLOAD_SIZE);
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", PARTIAL_BYTES, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == PARTIAL_BYTES);
    REQUIRE(out.body == payload.substr(0, PARTIAL_BYTES));
}

TEST_CASE("tail download clamps a Range-ignoring 200 body to the last bytes",
          "[api][transfer][range]") {
    const std::string payload = make_payload(PAYLOAD_SIZE);
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_tail("gcodes", "some_file.gcode", TAIL_BYTES, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == TAIL_BYTES);
    REQUIRE(out.body == payload.substr(payload.size() - TAIL_BYTES));
}

TEST_CASE("range-honouring 206 responses pass through unclamped", "[api][transfer][range]") {
    const std::string payload = make_payload(PAYLOAD_SIZE);
    RangeResponder server(/*honour_range=*/true, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", PARTIAL_BYTES, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == PARTIAL_BYTES);
    REQUIRE(out.body == payload.substr(0, PARTIAL_BYTES));
}

TEST_CASE("transfer callbacks run on the executor, not the caller", "[api][transfer][range]") {
    const std::string payload = make_payload(1024);
    RangeResponder server(/*honour_range=*/true, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", 512, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.callback_thread != std::this_thread::get_id());
}
