// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_diag_upload_gate.cpp
 * @brief The diagnostic-upload gate: decision semantics, both behaviours of
 *        the bundle-upload and crash-auto-send chokepoints, and the payload
 *        channel marker (prestonbrown/helixscreen#1410).
 *
 * Every test points the two worker URLs at a loopback CountingWorkerStub via
 * HELIX_BUNDLE_WORKER_URL / HELIX_CRASH_WORKER_URL. libhv honours no proxy
 * environment, so this repointing is the ONLY thing standing between an
 * accidentally ungated run (a reverted gate, a future mutation) and a real
 * upload to crash.helixscreen.org — pipe B would auto-file a live GitHub
 * issue. Do not remove the guards.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "netd_test_server.h"
#include "remote_control_server.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/debug_bundle_collector.h"
#include "system/diag_upload_gate.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// A loopback HTTP listener that counts every request served and always
/// answers 200 with the JSON body it was built with — the bundle worker's
/// {"share_code": ...} shape or the crash worker's {"issue_number": ...}
/// shape. "Did anything leave the process" is then a request count.
class CountingWorkerStub {
  public:
    explicit CountingWorkerStub(std::string json_body) : body_(std::move(json_body)) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0; // kernel picks a free port
        REQUIRE(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(listen(listen_fd_, 4) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        REQUIRE(getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";

        // Bound receive timeout so the accept loop re-checks stop_ instead of
        // blocking forever.
        struct timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        thread_ = std::thread([this] { serve(); });
    }

    ~CountingWorkerStub() {
        stop();
    }

    const std::string& url() const {
        return url_;
    }

    int request_count() const {
        return count_.load();
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }

  private:
    void serve() {
        while (!stopped_) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue; // SO_RCVTIMEO expiry or a spurious wakeup
            }

            // Accepted sockets do not inherit SO_RCVTIMEO on Linux; without
            // this a keep-alive client that sent its request would leave recv
            // blocked forever waiting for bytes that are never coming.
            struct timeval tv {};
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Read the request head; the body is never parsed.
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < (1U << 20)) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                req.append(buf, static_cast<size_t>(n));
            }

            ++count_;

            const std::string http = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/json\r\n" +
                                     std::string("Content-Length: ") +
                                     std::to_string(body_.size()) +
                                     "\r\n"
                                     "Connection: close\r\n"
                                     "\r\n" +
                                     body_;
            send(fd, http.data(), http.size(), 0);
            close(fd);
        }
    }

    std::string body_;
    std::string url_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<int> count_{0};
    std::atomic<bool> stopped_{false};
};

/// Everything a gating test needs: both worker URLs repointed at counting
/// stubs, the opt-in env pinned to a known state, and the CrashReporter /
/// CrashHistory singletons isolated in a temp directory.
struct DiagUploadGateFixture : public HelixTestFixture {
    DiagUploadGateFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_diag_upload_test_" + std::to_string(::getpid()) + "_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);

        crash_reporter_.init(temp_dir_.string());
        helix::CrashHistory::instance().shutdown();
        helix::CrashHistory::instance().init(temp_dir_.string());

        bundle_url_.set(bundle_stub_.url());
        crash_url_.set(crash_stub_.url());
        opt_in_.unset();

        // The gate tests are about whether and where bytes leave the process, not
        // what the bundle holds; the real collector walks /proc and asks systemd.
        helix::DebugBundleCollector::set_collect_override_for_test(
            [](const helix::BundleOptions&) { return json{{"diag_upload_marked", false}}; });
    }

    ~DiagUploadGateFixture() {
        helix::DebugBundleCollector::set_collect_override_for_test(nullptr);
        helix::CrashHistory::instance().shutdown();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    CrashReporter crash_reporter_;
    CountingWorkerStub bundle_stub_{"{\"share_code\":\"TESTCODE\"}"};
    CountingWorkerStub crash_stub_{"{\"issue_number\":42,\"issue_url\":\"https://x\"}"};
    helix_test::EnvVarGuard bundle_url_{"HELIX_BUNDLE_WORKER_URL"};
    helix_test::EnvVarGuard crash_url_{"HELIX_CRASH_WORKER_URL"};
    helix_test::EnvVarGuard opt_in_{"HELIX_DIAGNOSTIC_UPLOADS"};
    fs::path temp_dir_;
};

/// Drive the UpdateQueue while waiting for a worker-lane callback to land.
bool settle_done(const std::atomic<bool>& done) {
    return helix_test::wait_until([&done] {
        helix::ui::UpdateQueue::instance().drain();
        return done.load();
    });
}

} // namespace

// ============================================================================
// Decision semantics [diag-uploads]
// ============================================================================

TEST_CASE("diag upload gate: unmarked build defaults off, env overrides both ways",
          "[diag-uploads]") {
    DiagUploadGateFixture fx;

    // The test binary is an unmarked build (plain `make test`), so the
    // compile default is OFF — exactly the state a self-compiled build ships.
    REQUIRE_FALSE(helix::diag::marked_build());
    CHECK_FALSE(helix::diag::uploads_enabled());

    fx.opt_in_.set("1");
    CHECK(helix::diag::uploads_enabled());

    fx.opt_in_.set("0");
    CHECK_FALSE(helix::diag::uploads_enabled());

    // Only exact "1"/"0" are switches; anything else falls back to the
    // compile default (same rule as HELIX_HOT_RELOAD).
    fx.opt_in_.set("yes");
    CHECK_FALSE(helix::diag::uploads_enabled());
}

// ============================================================================
// Pipe A: DebugBundleCollector::upload_async [diag-uploads][debug-bundle]
// ============================================================================

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "upload_async: unmarked build refuses without collecting or uploading",
                 "[diag-uploads][debug-bundle]") {
    bool called = false;
    helix::BundleResult got;

    helix::DebugBundleCollector::upload_async(helix::BundleOptions{},
                                              [&](const helix::BundleResult& r) {
                                                  called = true;
                                                  got = r;
                                              });

    // The refusal is synchronous on the caller's thread — nothing was ever
    // submitted to the worker lane.
    REQUIRE(called);
    CHECK(got.uploads_disabled);
    CHECK_FALSE(got.success);
    CHECK_FALSE(got.error_message.empty());
    CHECK(bundle_stub_.request_count() == 0);
}

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "upload_async: opted-in build uploads through the worker URL",
                 "[diag-uploads][debug-bundle]") {
    opt_in_.set("1");

    std::atomic<bool> done{false};
    helix::BundleResult got;

    helix::DebugBundleCollector::upload_async(helix::BundleOptions{},
                                              [&](const helix::BundleResult& r) {
                                                  got = r;
                                                  done = true;
                                              });

    REQUIRE(settle_done(done));
    CHECK(got.success);
    CHECK(got.share_code == "TESTCODE");
    CHECK(bundle_stub_.request_count() == 1);
}

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "upload_async: worker URL is captured at submit; guard death cannot retarget",
                 "[diag-uploads][debug-bundle]") {
    opt_in_.set("1");

    std::atomic<bool> done{false};
    helix::BundleResult got;

    helix::DebugBundleCollector::upload_async(helix::BundleOptions{},
                                              [&](const helix::BundleResult& r) {
                                                  got = r;
                                                  done = true;
                                              });

    // What an EnvVarGuard's death restores once the binary-wide loopback pin
    // is in place: if the worker lane resolves the URL at execution time
    // rather than submit time, this retargets the in-flight upload.
    bundle_url_.set("http://127.0.0.1:9/");

    REQUIRE(settle_done(done));
    CHECK(got.success);
    CHECK(bundle_stub_.request_count() == 1);
}

// ============================================================================
// Pipe B: CrashReporter::try_auto_send [diag-uploads][crash_reporter]
// ============================================================================

TEST_CASE_METHOD(DiagUploadGateFixture, "try_auto_send: unmarked build refuses and falls back",
                 "[diag-uploads][crash_reporter]") {
    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "test";

    CHECK_FALSE(crash_reporter_.try_auto_send(report));
    CHECK(crash_stub_.request_count() == 0);
}

TEST_CASE_METHOD(DiagUploadGateFixture, "try_auto_send: opted-in build sends to the worker URL",
                 "[diag-uploads][crash_reporter]") {
    opt_in_.set("1");

    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "test";

    REQUIRE(crash_reporter_.try_auto_send(report));
    CHECK(crash_stub_.request_count() == 1);
}

// ============================================================================
// Payload channel marker [diag-uploads]
// ============================================================================

TEST_CASE("payload marker: bundle and crash report both carry diag_upload_marked",
          "[diag-uploads][slow]") {
    DiagUploadGateFixture fx;

    // Real collection on purpose: the marker must survive the whole collector.
    const json bundle = helix::DebugBundleCollector::collect(helix::BundleOptions{});
    REQUIRE(bundle.contains("diag_upload_marked"));
    CHECK(bundle["diag_upload_marked"] == json(helix::diag::marked_build()));

    CrashReporter::CrashReport report;
    const json report_json = fx.crash_reporter_.report_to_json(report);
    REQUIRE(report_json.contains("diag_upload_marked"));
    CHECK(report_json["diag_upload_marked"] == json(helix::diag::marked_build()));
}

// ============================================================================
// Pipe C: the `ctl log` RPC through the real dispatch machinery [diag-uploads]
// ============================================================================

namespace {

/// One newline-framed JSON-RPC round trip over the server's unix socket.
/// Empty string on any transport failure — the REQUIREs on the parsed
/// response then fail with a readable cause instead of hanging.
std::string rpc_roundtrip(int fd, const std::string& request) {
    const std::string line = request + "\n";
    if (send(fd, line.data(), line.size(), 0) != static_cast<ssize_t>(line.size())) {
        return "";
    }

    struct timeval tv {};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string response;
    char buf[4096];
    while (response.find('\n') == std::string::npos) {
        struct pollfd pfd {
            fd, POLLIN, 0
        };
        if (poll(&pfd, 1, 5000) <= 0) {
            return "";
        }
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return "";
        }
        response.append(buf, static_cast<size_t>(n));
    }
    response.erase(response.find('\n'));
    return response;
}

} // namespace

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "log RPC: unmarked build gets a real JSON-RPC error, not a served result",
                 "[diag-uploads][remote]") {
    const std::string sock_path = (temp_dir_ / "ctl.sock").string();
    ::unlink(sock_path.c_str());

    helix::RemoteConfig config;
    config.socket_path = sock_path;
    helix::RemoteControlServer server;
    REQUIRE(server.start(config));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    REQUIRE(sock_path.size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Gate OFF (the default this binary compiles to): dispatch() must answer
    // with a top-level JSON-RPC error and no result key. A handler that
    // RETURNS an error object instead of throwing ships it inside
    // {"result": ...} — exactly the shape these two assertions reject.
    const std::string refused =
        rpc_roundtrip(fd, R"({"jsonrpc":"2.0","method":"log","params":{"lines":3},"id":1})");
    REQUIRE_FALSE(refused.empty());
    const json refused_json = json::parse(refused);
    REQUIRE(refused_json.contains("error"));
    REQUIRE_FALSE(refused_json.contains("result"));
    const std::string message = refused_json["error"].value("message", "");
    CHECK(message.find("disabled") != std::string::npos);

    // Gate ON (the env opt-in a rig carries): the same request serves the
    // ring, proving the refusal was the gate and not a broken handler.
    opt_in_.set("1");
    const std::string served =
        rpc_roundtrip(fd, R"({"jsonrpc":"2.0","method":"log","params":{"lines":3},"id":2})");
    REQUIRE_FALSE(served.empty());
    const json served_json = json::parse(served);
    REQUIRE(served_json.contains("result"));
    CHECK(served_json["result"].contains("lines"));

    close(fd);
    server.stop();
}
