// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WifiBackendNetd unit tests — the backend driven directly (not through
 * WifiBackend::create(), which a later phase wires) against a fake netd
 * daemon (netd_test_server.h) repointed via HELIX_NETD_SOCKET.
 *
 * Timing knobs are injected at construction (reconnect 200 ms, scan
 * watchdog 300 ms, ack wait 500 ms) so every wait in this file is bounded
 * and fast. Assertions are byte-exact wherever the feature produces bytes
 * (wire lines, merged scan rows, event counts) so a mutation anywhere in
 * the pipeline shows up red here.
 */

#include "../../include/wifi_backend.h"
#include "netd_test_server.h"

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include "../../include/wifi_backend_netd.h"
#endif

#include "hv/base64.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

#if !defined(__APPLE__) && !defined(__ANDROID__)

// Shared netd test helpers (b64/unb64/wait_until) live in netd_test_server.h.
using helix_test::b64;
using helix_test::unb64;
using helix_test::wait_until;

// ============================================================================
// Fixture: fake daemon + repointed env + event-counting backend handle.
// Event counting follows the WiFiBackendTestFixture cv/timeout shape from
// test_wifi_backends.cpp; assertions on counts stay on the main thread.
// ============================================================================
class NetdBackendFixture {
  public:
    static constexpr int kReconnectMs = 200;
    static constexpr int kWatchdogMs = 300;

    NetdBackendFixture() {
        char dir_template[] = "/tmp/helix_netd_backend_XXXXXX";
        char* dir = ::mkdtemp(dir_template);
        REQUIRE(dir != nullptr);
        dir_ = dir;

        sock_env_.set(dir_ + "/netd.sock");
        bin_env_.unset();

        server_ = std::make_unique<helix_test::NetdFakeServer>();
        REQUIRE(server_->start(dir_ + "/netd.sock"));

        // Through the base interface everywhere: the drift test drives every
        // pure virtual through std::unique_ptr<WifiBackend>.
        backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, kWatchdogMs);
    }

    ~NetdBackendFixture() {
        if (backend_)
            backend_->stop();
        backend_.reset(); // full teardown incl. event-loop join
        server_.reset();  // closes clients, unlinks the socket
        ::rmdir(dir_.c_str());
    }

    void count_event(const std::string& name) {
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            counts_[name] += 1;
        }
        event_cv_.notify_all();
    }

    int event_count(const std::string& name) {
        std::lock_guard<std::mutex> lock(event_mutex_);
        return counts_[name];
    }

    bool wait_for_event(const std::string& name, int target, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(event_mutex_);
        return event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this, &name, target] { return counts_[name] >= target; });
    }

    void register_standard_events() {
        for (const char* name : {"SCAN_COMPLETE", "CONNECTED", "DISCONNECTED", "AUTH_FAILED"}) {
            backend_->register_event_callback(
                name, [this, name](const std::string&) { count_event(name); });
        }
    }

    /// Wait until the fake server has recorded @p line from the backend.
    bool line_recorded(const std::string& line) {
        const auto lines = server_->recorded_lines();
        return std::find(lines.begin(), lines.end(), line) != lines.end();
    }

    helix_test::EnvVarGuard sock_env_{"HELIX_NETD_SOCKET"};
    helix_test::EnvVarGuard bin_env_{"HELIX_NETD_BIN"};
    std::unique_ptr<helix_test::NetdFakeServer> server_;
    std::unique_ptr<WifiBackend> backend_;

  private:
    std::string dir_;
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    std::map<std::string, int> counts_;
};

// ============================================================================
// 1. Interface drift: every pure virtual through the base pointer, and the
//    capability answers this backend must pin.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd backend interface surface", "[netd][wifi]") {
    // Not running before start(): operations report NOT_INITIALIZED, not a
    // crash or a silent success.
    REQUIRE_FALSE(backend_->is_running());
    REQUIRE(backend_->trigger_scan().result == WiFiResult::NOT_INITIALIZED);

    std::vector<WiFiNetwork> networks;
    REQUIRE(backend_->get_scan_results(networks).result == WiFiResult::NOT_INITIALIZED);
    REQUIRE(networks.empty());

    // Empty SSID is rejected before anything else, running or not.
    REQUIRE(backend_->connect_network("", "pw").result == WiFiResult::INVALID_PARAMETERS);
    REQUIRE(backend_->connect_network("SomeNet", "pw").result == WiFiResult::NOT_INITIALIZED);
    REQUIRE(backend_->disconnect_network().result == WiFiResult::NOT_INITIALIZED);

    const auto status = backend_->get_status();
    REQUIRE_FALSE(status.connected);

    backend_->register_event_callback("SCAN_COMPLETE", [](const std::string&) {});

    // Capability answers.
    REQUIRE(backend_->is_network_manager() == false);
    REQUIRE(backend_->is_radio_enabled());
    REQUIRE_FALSE(backend_->resolved_interface().has_value());

    const auto radio = backend_->set_radio_enabled(false);
    REQUIRE(radio.result == WiFiResult::BACKEND_ERROR);
    REQUIRE(radio.user_msg == "WiFi radio is managed by the printer's network daemon");

    const auto forget = backend_->forget_network("SomeNet");
    REQUIRE(forget.result == WiFiResult::BACKEND_ERROR);

    // Lifecycle via the base pointer.
    REQUIRE(backend_->start().success());
    REQUIRE(backend_->is_running());
    backend_->stop();
    REQUIRE_FALSE(backend_->is_running());

    // supports_5ghz is checked last so the earlier assertions keep their
    // evidence value when it fails.
    REQUIRE_FALSE(backend_->supports_5ghz());
}

// ============================================================================
// 2. Handshake: start() connects and speaks SUBSCRIBE then GET, byte-exact.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd start sends SUBSCRIBE then GET", "[netd][wifi]") {
    REQUIRE(backend_->start().success());
    REQUIRE(backend_->is_running());

    REQUIRE(wait_until([&] { return server_->recorded_line_count() >= 2; }));
    const auto lines = server_->recorded_lines();
    REQUIRE(lines.size() >= 2);
    REQUIRE(lines[0] == "SUBSCRIBE");
    REQUIRE(lines[1] == "GET");
}

// ============================================================================
// 2b. Factory selection: when the daemon's socket is present (the fixture's
//     HELIX_NETD_SOCKET), WifiBackend::create() returns the netd backend —
//     proven behaviorally by the SUBSCRIBE handshake, not by type inspection
//     (the build is -fno-rtti). Reordering the factory branches so the probe
//     is skipped would leave nothing speaking to the fake daemon.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd factory selects the netd backend", "[netd][wifi]") {
    std::unique_ptr<WifiBackend> selected = WifiBackend::create(false);
    REQUIRE(selected != nullptr);

    REQUIRE(selected->start().success());
    REQUIRE(wait_until([&] { return server_->recorded_line_count() >= 1; }));
    const auto lines = server_->recorded_lines();
    REQUIRE_FALSE(lines.empty());
    REQUIRE(lines[0] == "SUBSCRIBE");

    selected->stop();
}

// ============================================================================
// 3. Snapshot pushes surface through get_status(), and a status read never
//    writes to the socket.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd snapshot pushes drive get_status", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());
    REQUIRE(wait_until([&] { return server_->recorded_line_count() >= 2; }));

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("SSID=" + b64("Studio 5G"));
    server_->push_line("SIGNAL=-52");
    server_->push_line("IP=10.0.0.4");

    // Wait on the FULL merged status, not just connected: the five lines can
    // land in the backend across several reads, and connected becomes true
    // as soon as MODE+STATE merge — before SSID does.
    REQUIRE(wait_until([&] {
        const auto s = backend_->get_status();
        return s.connected && s.ssid == "Studio 5G" && s.ip_address == "10.0.0.4" &&
               s.signal_strength == 63;
    }));

    const auto status = backend_->get_status();
    REQUIRE(status.connected);
    REQUIRE(status.ssid == "Studio 5G");
    REQUIRE(status.ip_address == "10.0.0.4");
    REQUIRE(status.signal_strength == 63); // wifi_signal_percent_from_dbm(-52)
    REQUIRE(status.bssid.empty());
    REQUIRE(status.frequency_mhz == 0);

    // Reads must not mutate the wire: no further lines for any get_status().
    const size_t before = server_->recorded_line_count();
    for (int i = 0; i < 3; ++i) {
        const auto again = backend_->get_status();
        REQUIRE(again.connected);
    }
    REQUIRE(server_->recorded_line_count() == before);
}

// ============================================================================
// 4. State-diff events: one CONNECTED on the offline->connected transition,
//    one DISCONNECTED on the drop. Non-state fields and connected->connected
//    transitions must not re-fire.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd state diff fires CONNECTED and DISCONNECTED once",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=OFFLINE");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // A non-state field and a connected->connected hop must be silent.
    server_->push_line("SIGNAL=-50");
    server_->push_line("STATE=ONLINE");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("CONNECTED") == 1);
    REQUIRE(event_count("DISCONNECTED") == 0);

    server_->push_line("STATE=DISCONNECTED");
    REQUIRE(wait_for_event("DISCONNECTED", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("CONNECTED") == 1);
    REQUIRE(event_count("DISCONNECTED") == 1);
}

// ============================================================================
// 4b. A drop that passes through an intermediate state must still surface:
//     CONNECTED -> RETRYING (beacon loss) -> DISCONNECTED is ONE disconnect.
//     The old terminal-state gate consumed was_connected_ on the RETRYING
//     push and swallowed the drop entirely.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd drop via RETRYING still fires DISCONNECTED",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // Beacon loss: the daemon passes through RETRYING before the terminal
    // state. The DISCONNECTED must fire here (the link IS down), not be
    // deferred past a gate that will never open again.
    server_->push_line("STATE=RETRYING");
    REQUIRE(wait_for_event("DISCONNECTED", 1));

    server_->push_line("STATE=DISCONNECTED");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("DISCONNECTED") == 1);
    REQUIRE(event_count("CONNECTED") == 1);
}

// ============================================================================
// 4c. Ack attribution: a late ERR for an accepted-but-unacked SCAN must never
//     read as a verdict on a join sent behind it — the join stays accepted,
//     the scan completes, and no join-failure event fires.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd late scan error does not fail a parked join",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    // Scan accepted by timeout (the daemon says nothing).
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());

    // A join goes out behind the outstanding scan.
    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));

    // The daemon finally answers — with the SCAN's failure.
    server_->push_line("ERR SCAN_FAILED");
    join_caller.join();

    REQUIRE(join.success());                     // the join was never rejected
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1)); // the scan still completed
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("AUTH_FAILED") == 0);
    REQUIRE(event_count("DISCONNECTED") == 0);
}

// ============================================================================
// 5. Join wire format: byte-exact CONNECT_WIFI lines, psk token omitted
//    entirely for open networks.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd connect_network sends exact wire lines",
                 "[netd][wifi]") {
    REQUIRE(backend_->start().success());

    WiFiError secured{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { secured = backend_->connect_network("Cafe 5G", "pw"); });
    const std::string want_secured = "CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw");
    REQUIRE(wait_until([&] { return line_recorded(want_secured); }));
    caller.join();
    // Fire-and-forget: the send succeeded, the daemon owns the join.
    REQUIRE(secured.success());

    WiFiError open{WiFiResult::UNKNOWN_ERROR};
    std::thread caller2([&] { open = backend_->connect_network("OpenNet", ""); });
    const std::string want_open = "CONNECT_WIFI ssid=" + b64("OpenNet");
    REQUIRE(wait_until([&] { return line_recorded(want_open); }));
    caller2.join();
    REQUIRE(open.success());

    // Open join carries no psk token anywhere on the wire.
    const auto lines = server_->recorded_lines();
    for (const auto& line : lines) {
        if (line.rfind("CONNECT_WIFI", 0) == 0 && line != want_secured) {
            REQUIRE(line == want_open);
            REQUIRE(line.find("psk=") == std::string::npos);
        }
    }
}

// ============================================================================
// 6. Rejected join: connect_network returns immediately (fire-and-forget —
//    the daemon owns the join), and the terminal ERR WRONG_KEY arrives as
//    ONE AUTH_FAILED event, not multiplied by RETRYING pushes or repeats.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd rejected join maps to AUTH_FAILED once",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError result{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { result = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    caller.join();
    // Sent, not waited on: the verdict comes as an event.
    REQUIRE(result.success());

    // The daemon narrates its retries, then reports the terminal failure.
    server_->push_line("STATE=CONNECTING");
    server_->push_line("STATE=RETRYING");
    server_->push_line("REASON=WRONG_KEY");
    server_->push_line("ERR WRONG_KEY");
    REQUIRE(wait_for_event("AUTH_FAILED", 1));

    // More retry narration and a repeated error must not multiply the event.
    server_->push_line("STATE=RETRYING");
    server_->push_line("REASON=WRONG_KEY");
    server_->push_line("ERR WRONG_KEY");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("AUTH_FAILED") == 1);
}

// ============================================================================
// 7. Scan: rows then OK => exactly one SCAN_COMPLETE; per-BSS rows merge
//    into one SSID row with the union of bands and the stronger signal.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd scan rows merge and complete once", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError result{WiFiResult::SUCCESS};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));

    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=WPA2-PSK NETWORK=" + b64("Studio 5G"));
    server_->push_line("FREQUENCY=5180 SIGNAL=-61 SECURITY=WPA2-PSK NETWORK=" + b64("Studio 5G"));
    server_->push_line("OK");

    caller.join();
    REQUIRE(result.success());
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // Past the watchdog bound: the completion must not fire twice.
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * kWatchdogMs));
    REQUIRE(event_count("SCAN_COMPLETE") == 1);

    std::vector<WiFiNetwork> networks;
    REQUIRE(backend_->get_scan_results(networks).success());
    REQUIRE(networks.size() == 1);
    REQUIRE(networks[0].ssid == "Studio 5G");
    REQUIRE(networks[0].signal_strength == 63);
    REQUIRE(networks[0].is_secured);
    REQUIRE(networks[0].security_type == "PSK");
    REQUIRE(networks[0].frequency_mhz == 2437);
    REQUIRE(networks[0].band_mask == (WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ));

    // A 5 GHz BSS in the results is evidence the radio sees the band.
    REQUIRE(backend_->supports_5ghz());
}

// ============================================================================
// 8. A refused scan: fire-and-forget means the ERR arrives as a daemon line
//    and completes the scan through the outstanding-scan attribution — one
//    SCAN_COMPLETE, an empty cache (nothing was found), and the caller's
//    return is success (the obligation is discharged by the event).
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd refused scan completes empty", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError result{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    caller.join();
    REQUIRE(result.success()); // never parked: the send itself succeeded

    server_->push_line("ERR SCAN_FAILED");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    std::this_thread::sleep_for(std::chrono::milliseconds(kWatchdogMs));
    REQUIRE(event_count("SCAN_COMPLETE") == 1);

    std::vector<WiFiNetwork> networks;
    REQUIRE(backend_->get_scan_results(networks).success());
    REQUIRE(networks.empty());
}

// ============================================================================
// 9. Accepted scan against a daemon that goes silent: the ack window times
//    out (the scan reads as accepted), nothing ever arrives, and the
//    watchdog alone completes it (the scan scheduler must never latch).
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd scan watchdog completes a silent scan", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError result{WiFiResult::SUCCESS};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    // Deliberately NO reply: the ack window expires, trigger_scan returns
    // success, and the only possible completion is the watchdog.

    caller.join();
    REQUIRE(result.success());
    // The watchdog (kWatchdogMs, plus scheduling slack) is the only thing
    // that can complete this scan: nothing else ever fires.
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1, 10 * kWatchdogMs));
}

// ============================================================================
// 10. Daemon death: the backend survives, keeps serving the last snapshot,
//     reconnects within the window, and NEVER sends CANCEL.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd daemon death survives and reconnects", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("SSID=" + b64("Studio 5G"));
    server_->push_line("SIGNAL=-52");
    server_->push_line("IP=10.0.0.4");
    // Full-status wait for the same reason as the snapshot case: the lines
    // can merge across several reads.
    REQUIRE(wait_until([&] {
        const auto s = backend_->get_status();
        return s.connected && s.ssid == "Studio 5G";
    }));

    server_->close_clients();

    // Survives: still running, still serving the last-known snapshot.
    REQUIRE(backend_->is_running());
    REQUIRE(backend_->get_status().connected);
    REQUIRE(backend_->get_status().ssid == "Studio 5G");

    // Reconnects within the injected window and re-subscribes.
    REQUIRE(
        wait_until([&] { return line_recorded("SUBSCRIBE") && server_->connection_count() >= 1; },
                   10 * kReconnectMs));
    const auto lines = server_->recorded_lines();
    size_t subscribes = 0;
    for (const auto& line : lines) {
        REQUIRE(line != "CANCEL");
        REQUIRE(line.rfind("CANCEL", 0) != 0);
        if (line == "SUBSCRIBE")
            ++subscribes;
    }
    REQUIRE(subscribes >= 2); // first connection + the reconnect

    // The snapshot survived the socket loss and the reconnect.
    REQUIRE(backend_->get_status().connected);
}

// ============================================================================
// 11. stop() closes the connection without CANCEL, and start() works again
//     on the still-running event loop.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd stop closes silently and start reuses the loop",
                 "[netd][wifi]") {
    REQUIRE(backend_->start().success());
    REQUIRE(wait_until([&] { return server_->connection_count() == 1; }));

    backend_->stop();
    REQUIRE_FALSE(backend_->is_running());

    // The daemon side sees the connection close.
    REQUIRE(wait_until([&] { return server_->connection_count() == 0; }));

    for (const auto& line : server_->recorded_lines()) {
        REQUIRE(line != "CANCEL");
        REQUIRE(line.rfind("CANCEL", 0) != 0);
    }

    // A stopped backend refuses work.
    REQUIRE(backend_->trigger_scan().result == WiFiResult::NOT_INITIALIZED);

    // start() again on the same loop reconnects and re-subscribes.
    REQUIRE(backend_->start().success());
    REQUIRE(backend_->is_running());
    REQUIRE(wait_until([&] { return server_->connection_count() == 1; }));
    REQUIRE(wait_until([&] {
        size_t subscribes = 0;
        for (const auto& line : server_->recorded_lines())
            if (line == "SUBSCRIBE")
                ++subscribes;
        return subscribes >= 2;
    }));
}

// ============================================================================
// 12. The shared dBm -> percent mapping (wifi_signal_percent_from_dbm).
// ============================================================================
TEST_CASE("wifi signal percent from dbm clamps and scales", "[netd][wifi]") {
    REQUIRE(wifi_signal_percent_from_dbm(-30) == 100);
    REQUIRE(wifi_signal_percent_from_dbm(-52) == 63);
    REQUIRE(wifi_signal_percent_from_dbm(-90) == 0);
    REQUIRE(wifi_signal_percent_from_dbm(-100) == 0);
    REQUIRE(wifi_signal_percent_from_dbm(-60) == 50);
}

#endif // !__APPLE__ && !__ANDROID__
