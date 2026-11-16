// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_transfer_range.cpp
 * @brief A server that ignores byte ranges must not turn a bounded download
 *        into a whole-file payload
 *
 * download_file_partial asks for the first N bytes, download_file_tail for the
 * last N. A server that ignores Range answers 200 with the entire file: both
 * functions clamp what they hand on, and the head path also aborts the
 * transfer once N bytes have arrived, so the excess never crosses the wire or
 * occupies the single slow-lane worker. These tests drive the real HTTP client
 * against a local responder that honours or ignores Range and counts the body
 * bytes it managed to push.
 *
 * Stays in the default sweep, no [slow]: everything here is self-contained
 * loopback threads (the responder plus HttpExecutor::slow), each wait is
 * bounded by await()'s 10 s promise timeout, and the whole tag runs in
 * well under a second.
 */

#include "moonraker_client_mock.h"
#include "moonraker_file_transfer_api.h"
#include "thumbnail_rules.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Not every platform has MSG_NOSIGNAL; the loopback responder needs it so a
// client hangup fails send() instead of killing the test process with SIGPIPE.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

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

    /// Body bytes the responder pushed before send failed or completed. Only
    /// meaningful once wait_writes_done() has returned.
    [[nodiscard]] size_t content_written() const {
        return content_written_.load();
    }

    /// Wait until the write loop finished for the current/last connection, so
    /// content_written() reads its final value. Returns false on timeout.
    bool wait_writes_done(int timeout_ms) const {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!writes_done_.load()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
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
        handle_request(conn);
        writes_done_ = true;
    }

    void handle_request(int conn) {
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

        // Cap the send buffer so a client that stops reading stalls this loop
        // after a bounded amount, instead of letting kernel autotuning buffer
        // the whole body.
        int sndbuf = kSendBuffer;
        ::setsockopt(conn, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\n";
        resp += "Content-Length: " + std::to_string(content.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        if (::send(conn, resp.data(), resp.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(resp.size())) {
            return;
        }

        // Body in chunks, counting what actually left: a client hangup makes
        // send fail once the socket buffers fill or the peer resets, so
        // content_written() measures the bytes the client let cross.
        size_t off = 0;
        while (off < content.size()) {
            size_t n = std::min(kBodyChunk, content.size() - off);
            ssize_t w = ::send(conn, content.data() + off, n, MSG_NOSIGNAL);
            if (w <= 0) {
                return;
            }
            off += static_cast<size_t>(w);
            content_written_ += static_cast<size_t>(w);
        }
    }

    std::string body_;
    bool honour_range_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<size_t> content_written_{0};
    std::atomic<bool> writes_done_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    static constexpr size_t kBodyChunk = 16 * 1024;
    static constexpr int kSendBuffer = 32 * 1024;
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

constexpr size_t kPayloadSize = 64 * 1024;
constexpr size_t kPartialBytes = 4096;
constexpr size_t kTailBytes = 1024;

/// The most the kernel can hold between the server's writes and the client's
/// reads: the server's send buffer plus the client's receive buffer, each at the
/// size it may autotune to. A client that hangs up at max_bytes still lets the
/// server push that much before the close lands. Each side is capped at 16 MB
/// so a host tuned for huge windows does not cost a huge payload.
size_t kernel_buffer_ceiling() {
    auto autotune_max = [](const char* path, size_t fallback) {
        std::ifstream in(path);
        size_t min = 0, def = 0, max = 0;
        if (in >> min >> def >> max) {
            return std::min<size_t>(max, 16 * 1024 * 1024);
        }
        return fallback;
    };
    // macOS has no /proc: kern.ipc.maxsockbuf bounds each side at 8 MB.
    return autotune_max("/proc/sys/net/ipv4/tcp_rmem", 8 * 1024 * 1024) +
           autotune_max("/proc/sys/net/ipv4/tcp_wmem", 8 * 1024 * 1024);
}

} // namespace

TEST_CASE("partial download clamps a Range-ignoring 200 body", "[api][transfer][range]") {
    const std::string payload = make_payload(kPayloadSize);
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", kPartialBytes, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == kPartialBytes);
    REQUIRE(out.body == payload.substr(0, kPartialBytes));
}

TEST_CASE("partial download aborts a Range-ignoring 200 at max_bytes", "[api][transfer][range]") {
    // Larger than every byte the kernel could hold on the client's behalf, so
    // only a client that kept reading past max_bytes lets the whole body cross.
    const std::string payload = make_payload(2 * kernel_buffer_ceiling());
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", kPartialBytes, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == kPartialBytes);
    REQUIRE(out.body == payload.substr(0, kPartialBytes));

    // The client must hang up once max_bytes arrived: the server got to push
    // the abort point plus kernel-buffer slack, not the whole file.
    REQUIRE(server.wait_writes_done(2000));
    REQUIRE(server.content_written() < payload.size());
}

TEST_CASE("tail download clamps a Range-ignoring 200 body to the last bytes",
          "[api][transfer][range]") {
    const std::string payload = make_payload(kPayloadSize);
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_tail("gcodes", "some_file.gcode", kTailBytes, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == kTailBytes);
    REQUIRE(out.body == payload.substr(payload.size() - kTailBytes));
}

TEST_CASE("range-honouring 206 responses pass through unclamped", "[api][transfer][range]") {
    const std::string payload = make_payload(kPayloadSize);
    RangeResponder server(/*honour_range=*/true, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_partial("gcodes", "some_file.gcode", kPartialBytes, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == kPartialBytes);
    REQUIRE(out.body == payload.substr(0, kPartialBytes));
}

TEST_CASE("a ranged download returns the bytes at the offset whether or not the server slices",
          "[api][transfer][range]") {
    const std::string payload = make_payload(kPayloadSize);
    const bool honour = GENERATE(true, false);
    CAPTURE(honour);
    RangeResponder server(honour, payload);
    TransferHarness harness(server.base_url());

    // Offsets off the payload's 256-byte period, one inside the first body
    // chunk and one several chunks in, so a skipped or misplaced skip shows.
    const uint64_t offset = GENERATE(uint64_t{12345}, uint64_t{40001});
    CAPTURE(offset);
    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_range("gcodes", "a.gcode.3mf", offset, 3000, ok, err);
    });

    REQUIRE(out.ok);
    REQUIRE(out.body.size() == 3000);
    CHECK(out.body == payload.substr(static_cast<size_t>(offset), 3000));
}

TEST_CASE("a ranged download that runs past the end returns what the file has",
          "[api][transfer][range]") {
    const std::string payload = make_payload(kPayloadSize);
    const bool honour = GENERATE(true, false);
    CAPTURE(honour);
    RangeResponder server(honour, payload);
    TransferHarness harness(server.base_url());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_range("gcodes", "a.gcode.3mf", kPayloadSize - 100, 1000, ok,
                                         err);
    });
    REQUIRE(out.ok);
    CHECK(out.body == payload.substr(kPayloadSize - 100));
}

TEST_CASE("a ranged download of nothing is refused before it is sent", "[api][transfer][range]") {
    RangeResponder server(true, make_payload(1024));
    TransferHarness harness(server.base_url());
    const auto out = await([&](auto ok, auto err) {
        harness.api->download_file_range("gcodes", "a.gcode.3mf", 10, 0, ok, err);
    });
    CHECK_FALSE(out.ok);
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

// A thumbnail lands by rename, so a reader that opened the cache file before
// the download keeps reading the complete old image instead of a file being
// truncated and rewritten under it.
static std::string read_fixture(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

TEST_CASE("thumbnail download replaces the cache file instead of rewriting it",
          "[api][transfer][thumbnail]") {
    const std::string payload = read_fixture("assets/images/benchy_thumbnail_white.png");
    REQUIRE(payload.size() > 1024);
    RangeResponder server(/*honour_range=*/false, payload);
    TransferHarness harness(server.base_url());

    char dir_tmpl[] = "/tmp/helix_thumb_dl_XXXXXX";
    REQUIRE(::mkdtemp(dir_tmpl) != nullptr);
    const std::string dir = dir_tmpl;
    const std::string cache_path = dir + "/thumb.png";
    const std::string old_bytes = "previous thumbnail";
    {
        std::ofstream out(cache_path, std::ios::binary);
        out << old_bytes;
    }
    std::ifstream reader(cache_path, std::ios::binary);
    REQUIRE(reader.good());

    const auto out = await([&](auto ok, auto err) {
        harness.api->download_thumbnail(".thumbs/a.png", cache_path, ok, err);
    });
    REQUIRE(out.ok);

    std::string held((std::istreambuf_iterator<char>(reader)), std::istreambuf_iterator<char>());
    CHECK((held == old_bytes));
    std::ifstream fresh(cache_path, std::ios::binary);
    std::string landed((std::istreambuf_iterator<char>(fresh)), std::istreambuf_iterator<char>());
    CHECK((landed == payload));
    CHECK_FALSE(std::filesystem::exists(cache_path + ".tmp"));

    std::filesystem::remove_all(dir);
}

// The cache names the file .png and LVGL picks its decoder by that name, so
// what lands there is PNG whatever the server sent, or nothing.
TEST_CASE("thumbnail download stores PNG bytes whatever the server's format",
          "[api][transfer][thumbnail][rules]") {
    char dir_tmpl[] = "/tmp/helix_thumb_fmt_XXXXXX";
    REQUIRE(::mkdtemp(dir_tmpl) != nullptr);
    const std::string dir = dir_tmpl;
    const std::string cache_path = dir + "/thumb.png";

    SECTION("a JPEG is re-encoded") {
        RangeResponder server(
            false, read_fixture("assets/test_timelapse/benchy_timelapse_20260310.thumb.jpg"));
        TransferHarness harness(server.base_url());
        const auto out = await([&](auto ok, auto err) {
            harness.api->download_thumbnail(".thumbs/a.jpg", cache_path, ok, err);
        });
        REQUIRE(out.ok);
        CHECK(helix::sniff_image_format(read_fixture(cache_path.c_str())) ==
              helix::ImageFormat::Png);
    }

    SECTION("anything else fails and leaves the cache file alone") {
        {
            std::ofstream old(cache_path, std::ios::binary);
            old << "previous thumbnail";
        }
        RangeResponder server(false, make_payload(kPayloadSize));
        TransferHarness harness(server.base_url());
        const auto out = await([&](auto ok, auto err) {
            harness.api->download_thumbnail(".thumbs/a.png", cache_path, ok, err);
        });
        CHECK_FALSE(out.ok);
        CHECK(read_fixture(cache_path.c_str()) == "previous thumbnail");
    }

    std::filesystem::remove_all(dir);
}
