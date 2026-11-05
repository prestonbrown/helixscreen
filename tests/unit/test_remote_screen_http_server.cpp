// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_remote_screen_http_server.cpp
 * @brief ScreenHttpServer's view routes over a real loopback socket.
 */

#include "../test_helpers/raw_http_client.h"
#include "remote_screen_control.h"
#include "remote_screen_http_server.h"
#include "remote_screen_server_sink.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::test::raw_http;
using namespace std::chrono_literals;

namespace {

struct Frame {
    std::vector<uint8_t> px;
    RemoteScreenFrame f;
    Frame(int w, int h) : px(static_cast<size_t>(w) * h * 4, 0x7F) {
        f.px_map = px.data();
        f.px_map_len = px.size();
        f.x2 = w - 1;
        f.y2 = h - 1;
        f.disp_w = w;
        f.disp_h = h;
        f.src_stride = static_cast<uint32_t>(w * 4);
        f.src_format = RemoteScreenPixelFormat::BGRA8888;
    }
};

struct ServerFixture {
    std::shared_ptr<ScreenFrameStore> store = std::make_shared<ScreenFrameStore>();
    std::atomic<int> redraws{0};
    std::unique_ptr<ScreenHttpServer> server;

    explicit ServerFixture(ScreenServerConfig cfg = {}) {
        ScreenServerHooks hooks;
        hooks.request_full_redraw = [this] { redraws.fetch_add(1); };
        hooks.is_printing = [] { return false; };
        server = std::make_unique<ScreenHttpServer>(store, hooks);
        cfg.bind_host = "127.0.0.1";
        cfg.port = 0;
        REQUIRE(server->start(cfg));
        REQUIRE(server->port() > 0);
    }

    void capture(int w = 32, int h = 24) {
        Frame frame(w, h);
        store->on_frame(frame.f);
    }
};

} // namespace

TEST_CASE("http: snapshot returns image/jpeg with no-store after a frame",
          "[remote_screen][http]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: image/jpeg"));
    CHECK(r.has_header("cache-control: no-store"));
    REQUIRE(r.body.size() > 4);
    CHECK(static_cast<uint8_t>(r.body[0]) == 0xFF);
    CHECK(static_cast<uint8_t>(r.body[1]) == 0xD8);
}

TEST_CASE("http: snapshot with no frame yet is 503 with Retry-After 1", "[remote_screen][http]") {
    ServerFixture fx;
    const auto start = std::chrono::steady_clock::now();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(r.status == 503);
    CHECK(r.has_header("retry-after: 1"));
    CHECK(took < std::chrono::milliseconds(kFirstFrameWaitMs + 300));
}

TEST_CASE("http: first snapshot calls request_full_redraw once", "[remote_screen][http]") {
    ServerFixture fx;
    // The redraw lands while the first request waits for it.
    std::thread flusher([&] {
        while (fx.redraws.load() == 0) {
            std::this_thread::sleep_for(2ms);
        }
        fx.store->note_full_invalidate();
        fx.capture();
    });
    auto first = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    flusher.join();
    CHECK(first.status == 200);
    auto second = raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    CHECK(second.status == 200);
    CHECK(fx.redraws.load() == 1);
}

TEST_CASE("http: snapshot?ts=123 is served like snapshot.jpg", "[remote_screen][http]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    auto r = raw_http(fx.server->port(), "GET", "/snapshot.jpg?ts=123");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: image/jpeg"));
}

TEST_CASE("http: info reports w, h and control", "[remote_screen][http]") {
    ScreenServerConfig cfg;
    cfg.control_allowed = true;
    ServerFixture fx(cfg);
    fx.store->note_request();
    fx.capture(48, 20);
    auto r = raw_http(fx.server->port(), "GET", "/info");
    REQUIRE(r.status == 200);
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j["w"] == 48);
    CHECK(j["h"] == 20);
    CHECK(j["control"] == true);

    fx.server->set_control_allowed(false);
    auto off =
        nlohmann::json::parse(raw_http(fx.server->port(), "GET", "/info").body, nullptr, false);
    CHECK(off["control"] == false);
}

TEST_CASE("http: index serves the page file, 404 when it is missing", "[remote_screen][http]") {
    const std::string page = "/tmp/helix_remote_screen_page_test.html";
    {
        std::ofstream out(page);
        out << "<html>screen</html>";
    }
    ScreenServerConfig cfg;
    cfg.page_path = page;
    ServerFixture fx(cfg);
    auto r = raw_http(fx.server->port(), "GET", "/");
    CHECK(r.status == 200);
    CHECK(r.has_header("content-type: text/html"));
    CHECK(r.body == "<html>screen</html>");
    std::remove(page.c_str());

    auto missing = raw_http(fx.server->port(), "GET", "/");
    CHECK(missing.status == 404);
}

TEST_CASE("http: stop() while a request waits returns promptly", "[remote_screen][http]") {
    ServerFixture fx;
    std::thread client([&] {
        // No frame ever arrives, so the handler sits in its first-frame wait.
        (void)raw_http(fx.server->port(), "GET", "/snapshot.jpg");
    });
    while (fx.redraws.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }
    const auto start = std::chrono::steady_clock::now();
    fx.server->stop();
    // Well inside the first-frame wait: stop() wakes the handler instead of waiting it out.
    CHECK(std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds(kFirstFrameWaitMs / 2));
    client.join();
}

TEST_CASE("http: a port already in use fails start cleanly", "[remote_screen][http]") {
    ServerFixture first;
    auto store = std::make_shared<ScreenFrameStore>();
    ScreenHttpServer second(store, {});
    ScreenServerConfig cfg;
    cfg.bind_host = "127.0.0.1";
    cfg.port = first.server->port();
    CHECK_FALSE(second.start(cfg));
    CHECK(second.port() == 0);
}

// ---------------------------------------------------------------------------
// Control routes
// ---------------------------------------------------------------------------

namespace {

struct ControlFixture {
    std::shared_ptr<ScreenFrameStore> store = std::make_shared<ScreenFrameStore>();
    std::shared_ptr<PairingStore> pairing;
    std::vector<TouchEvent> touches;
    std::mutex touches_mutex;
    std::unique_ptr<ScreenHttpServer> server;

    ControlFixture(bool control, bool lan_mode) {
        if (lan_mode) {
            pairing = std::make_shared<PairingStore>("123456", std::vector<std::string>{},
                                                     PairingStore::Persist{});
        }
        ScreenServerHooks hooks;
        hooks.request_full_redraw = [] {};
        hooks.is_printing = [] { return false; };
        hooks.apply_touch = [this](const TouchEvent& e) {
            std::lock_guard<std::mutex> lock(touches_mutex);
            touches.push_back(e);
            return true;
        };
        hooks.pairing = pairing;
        server = std::make_unique<ScreenHttpServer>(store, hooks);
        ScreenServerConfig cfg;
        cfg.bind_host = "127.0.0.1";
        cfg.port = 0;
        cfg.control_allowed = control;
        cfg.lan_mode = lan_mode;
        REQUIRE(server->start(cfg));
        Frame frame(64, 48);
        store->on_frame(frame.f);
    }

    std::string host() const {
        return "127.0.0.1:" + std::to_string(server->port());
    }

    helix::test::RawHttpReply touch(const std::string& body, bool header = true,
                                    const std::string& origin = "", const std::string& token = "",
                                    const std::string& type = "application/json") {
        std::vector<std::pair<std::string, std::string>> h = {{"Content-Type", type}};
        if (header) {
            h.push_back({"X-Helix-Touch", "1"});
        }
        if (!origin.empty()) {
            h.push_back({"Origin", origin});
        }
        if (!token.empty()) {
            h.push_back({"X-Helix-Pair", token});
        }
        return raw_http(server->port(), "POST", "/touch", h, body);
    }

    size_t touch_count() {
        std::lock_guard<std::mutex> lock(touches_mutex);
        return touches.size();
    }
};

const std::string kDown = R"({"type":"down","x":10,"y":10})";

} // namespace

TEST_CASE("http: touch is refused unless every gate passes", "[remote_screen][http][control]") {
    SECTION("control off is 403") {
        ControlFixture fx(false, false);
        CHECK(fx.touch(kDown).status == 403);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("missing X-Helix-Touch is 403") {
        ControlFixture fx(true, false);
        CHECK(fx.touch(kDown, false).status == 403);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("a form content type is 403") {
        ControlFixture fx(true, false);
        CHECK(fx.touch(kDown, true, "", "", "text/plain").status == 403);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("a foreign Origin is 403") {
        ControlFixture fx(true, false);
        CHECK(fx.touch(kDown, true, "http://evil.example").status == 403);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("unpaired in LAN mode is 401") {
        ControlFixture fx(true, true);
        CHECK(fx.touch(kDown).status == 401);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("a point outside the image is 400") {
        ControlFixture fx(true, false);
        CHECK(fx.touch(R"({"type":"down","x":64,"y":0})").status == 400);
        CHECK(fx.touch_count() == 0);
    }
    SECTION("behind nginx with every gate passed, the event is applied") {
        ControlFixture fx(true, false);
        auto r = fx.touch(kDown, true, "http://" + fx.host());
        CHECK(r.status == 204);
        REQUIRE(fx.touch_count() == 1);
        CHECK(fx.touches[0].type == TouchEvent::Type::Down);
        CHECK(fx.touches[0].x == 10);
    }
}

TEST_CASE("http: pair returns a token that, sent as X-Helix-Pair, allows touch",
          "[remote_screen][http][control]") {
    // A header, not a cookie: Mainsail opened by host name frames the screen from the
    // printer's IP, a different site, and a cross-site frame gets no cookies over HTTP.
    ControlFixture fx(true, true);
    const std::vector<std::pair<std::string, std::string>> h = {
        {"Content-Type", "application/json"}, {"X-Helix-Touch", "1"}};

    auto info =
        nlohmann::json::parse(raw_http(fx.server->port(), "GET", "/info").body, nullptr, false);
    CHECK(info["paired"] == false);

    auto wrong = raw_http(fx.server->port(), "POST", "/pair", h, R"({"code":"000000"})");
    CHECK(wrong.status == 403);

    auto ok = raw_http(fx.server->port(), "POST", "/pair", h, R"({"code":"123456"})");
    REQUIRE(ok.status == 200);
    CHECK_FALSE(ok.has_header("set-cookie"));
    CHECK(ok.has_header("cache-control: no-store"));
    auto body = nlohmann::json::parse(ok.body, nullptr, false);
    REQUIRE(body.is_object());
    const std::string token = body.value("token", "");
    REQUIRE(token.size() == 32);
    REQUIRE(fx.pairing->is_paired(token));

    CHECK(fx.touch(kDown, true, "", "not-a-token").status == 401);
    CHECK(fx.touch(kDown, true, "", token).status == 204);
    CHECK(fx.touch_count() == 1);

    auto paired_info = nlohmann::json::parse(
        raw_http(fx.server->port(), "GET", "/info", {{"X-Helix-Pair", token}}).body, nullptr,
        false);
    CHECK(paired_info["paired"] == true);
}

TEST_CASE("http: pairing past the attempt limit is 429 with Retry-After",
          "[remote_screen][http][control]") {
    ControlFixture fx(true, true);
    const std::vector<std::pair<std::string, std::string>> h = {
        {"Content-Type", "application/json"}, {"X-Helix-Touch", "1"}};
    for (int i = 0; i < kPairAttemptsPerWindow; ++i) {
        raw_http(fx.server->port(), "POST", "/pair", h, R"({"code":"000000"})");
    }
    auto limited = raw_http(fx.server->port(), "POST", "/pair", h, R"({"code":"123456"})");
    CHECK(limited.status == 429);
    CHECK(limited.has_header("retry-after: "));
}

// ---------------------------------------------------------------------------
// MJPEG stream
// ---------------------------------------------------------------------------

namespace {

/// Opens stream.mjpg and reads until @p parts JPEG parts have arrived (or the
/// server closes, or @p timeout passes). Returns the raw bytes read.
std::string read_stream(int port, int parts, std::chrono::milliseconds timeout,
                        std::atomic<bool>* closed_by_server = nullptr) {
    // Empty on a failed request, for the caller to assert on: some callers are client
    // threads, where a Catch2 macro races the main thread's assertion state.
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const std::string req = "GET /stream.mjpg HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::send(fd, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
        if (fd >= 0) {
            ::close(fd);
        }
        return {};
    }
    timeval tv{0, 100 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[8192];
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
            if (closed_by_server) {
                *closed_by_server = true;
            }
            break;
        }
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
        }
        size_t count = 0;
        for (size_t at = raw.find("\r\n\r\n\xFF\xD8"); at != std::string::npos;
             at = raw.find("\r\n\r\n\xFF\xD8", at + 1)) {
            ++count;
        }
        if (static_cast<int>(count) >= parts && raw.rfind("\xFF\xD9") != std::string::npos) {
            break;
        }
    }
    ::close(fd);
    return raw;
}

} // namespace

TEST_CASE("stream: first two parts are JPEGs with the boundary", "[remote_screen][http]") {
    ServerFixture fx;
    std::atomic<bool> stop_feeding{false};
    // A changing screen: each new generation is a new part.
    std::thread feeder([&] {
        int shade = 0;
        while (!stop_feeding.load()) {
            Frame frame(32, 24);
            std::fill(frame.px.begin(), frame.px.end(), static_cast<uint8_t>(shade++ * 40));
            fx.store->on_frame(frame.f);
            std::this_thread::sleep_for(50ms);
        }
    });
    const std::string raw = read_stream(fx.server->port(), 2, 5s);
    stop_feeding = true;
    feeder.join();

    const std::string lower = helix::test::raw_http_lower(raw.substr(0, raw.find("\r\n\r\n")));
    CHECK(lower.find("multipart/x-mixed-replace; boundary=helixframe") != std::string::npos);
    size_t parts = 0;
    for (size_t at = raw.find("--helixframe\r\nContent-Type: image/jpeg\r\n");
         at != std::string::npos;
         at = raw.find("--helixframe\r\nContent-Type: image/jpeg\r\n", at + 1)) {
        ++parts;
    }
    CHECK(parts >= 2);
}

TEST_CASE("stream: stop() ends an open stream within 1s", "[remote_screen][http]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    std::atomic<bool> closed{false};
    std::thread client([&] { read_stream(fx.server->port(), 1000, 5s, &closed); });
    std::this_thread::sleep_for(300ms);
    const auto start = std::chrono::steady_clock::now();
    fx.server->stop();
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    client.join();
    CHECK(closed.load());
}

TEST_CASE("stream: an open stream keeps wants_frames true past kViewerIdleMs",
          "[remote_screen][http]") {
    auto fake_now = std::make_shared<std::atomic<int64_t>>(3600LL * 1000000000LL);
    auto store = std::make_shared<ScreenFrameStore>([fake_now] {
        return std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(fake_now->load()));
    });
    ScreenServerHooks hooks;
    hooks.request_full_redraw = [] {};
    hooks.is_printing = [] { return false; };
    ScreenHttpServer server(store, hooks);
    ScreenServerConfig cfg;
    cfg.bind_host = "127.0.0.1";
    cfg.port = 0;
    REQUIRE(server.start(cfg));
    Frame frame(16, 16);
    store->on_frame(frame.f);

    std::atomic<bool> done{false};
    std::thread client([&] {
        read_stream(server.port(), 1000, 3s);
        done = true;
    });
    std::this_thread::sleep_for(300ms);
    REQUIRE(store->wants_frames());
    // Ten seconds pass with no snapshot request: the open stream alone keeps the viewer.
    fake_now->fetch_add(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::milliseconds(kViewerIdleMs + 1000))
                            .count());
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    bool alive = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (store->wants_frames()) {
            alive = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    CHECK(alive);
    server.stop();
    client.join();
}

// ---------------------------------------------------------------------------
// Who counts as a viewer
// ---------------------------------------------------------------------------

TEST_CASE("viewer: a plain snapshot captures frames but is no page viewer; a marked poll is",
          "[remote_screen][http][remote_viewer]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    REQUIRE(raw_http(fx.server->port(), "GET", "/snapshot.jpg").status == 200);
    CHECK(fx.store->wants_frames());
    CHECK_FALSE(fx.store->page_viewer_present());
    // Another parameter's value does not count either.
    REQUIRE(
        raw_http(fx.server->port(), "GET", std::string("/snapshot.jpg?") + kViewerPageParam + "=0")
            .status == 200);
    CHECK_FALSE(fx.store->page_viewer_present());

    REQUIRE(raw_http(fx.server->port(), "GET",
                     std::string("/snapshot.jpg?") + kViewerPageParam + "=1&t=123")
                .status == 200);
    CHECK(fx.store->page_viewer_present());
}

TEST_CASE("viewer: a touch is no page viewer", "[remote_screen][http][remote_viewer]") {
    ControlFixture fx(true, false);
    REQUIRE(fx.touch(kDown, true, "http://" + fx.host()).status == 204);
    CHECK(fx.store->wants_frames());
    CHECK_FALSE(fx.store->page_viewer_present());
}

TEST_CASE("viewer: an open stream is no page viewer", "[remote_screen][http][remote_viewer]") {
    ServerFixture fx;
    fx.store->note_request();
    fx.capture();
    const std::string got = read_stream(fx.server->port(), 1, 3s);
    REQUIRE(got.find("image/jpeg") != std::string::npos);
    CHECK(fx.store->wants_frames());
    CHECK_FALSE(fx.store->page_viewer_present());
}

TEST_CASE("viewer: the served page marks its snapshot polls",
          "[remote_screen][http][remote_viewer]") {
    ScreenServerConfig cfg;
    cfg.page_path = "assets/remote_screen/index.html";
    ServerFixture fx(cfg);
    auto r = raw_http(fx.server->port(), "GET", "/");
    REQUIRE(r.status == 200);
    CHECK(r.body.find(std::string("\"snapshot.jpg?") + kViewerPageParam + "=1") !=
          std::string::npos);
}
