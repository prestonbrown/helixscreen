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

    // 15s, not 5s: a full-suite shard run on a heavily loaded runner can starve
    // the backend dispatch thread past 5s (observed once at load ~16); the cost
    // of the extra headroom is paid only on already-failing runs.
    bool wait_for_event(const std::string& name, int target, int timeout_ms = 15000) {
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

    /// How many exact @p line occurrences the server has recorded so far.
    int line_count(const std::string& line) {
        const auto lines = server_->recorded_lines();
        return static_cast<int>(std::count(lines.begin(), lines.end(), line));
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

    // A join goes out behind the outstanding scan: single-flight resolves the
    // scan FIRST (the join outranks it), so the join is the only outstanding
    // op by the time its bytes leave.
    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    join_caller.join();
    REQUIRE(join.success());

    // The scan was resolved by the join's dispatch, not by its own timer —
    // no scan ack can exist anymore to be misread against the join.
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // A late ERR is now unambiguously the JOIN's verdict.
    server_->push_line("ERR TIMEOUT");
    REQUIRE(wait_for_event("DISCONNECTED", 1));
    REQUIRE(event_count("AUTH_FAILED") == 0);
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

// ============================================================================
// 13. MAC retry (#1399): a box booted in ETHERNET mode has no wlan0 when
//     init runs, so the init-time read legitimately finds nothing. The
//     CONNECTED transition must retry the read and the result must surface
//     through get_status(), or the MAC stays blank for the whole session.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd MAC read retries on the CONNECTED transition",
                 "[netd][wifi][1399]") {
    // wlan0 appearing late: the first read (init) finds no interface, every
    // later one sees the station address.
    std::atomic<int> reads{0};
    const std::string injected_mac = "02:41:1C:9A:7B:05";
    auto mac_reader = [&reads, injected_mac]() -> std::string {
        return reads.fetch_add(1) == 0 ? std::string() : injected_mac;
    };
    // Swap in a backend built with the injected reader (the fixture's own was
    // never started, so its teardown here is a no-op).
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, kWatchdogMs, mac_reader);
    register_standard_events();
    REQUIRE(backend_->start().success());

    // start() waits for init, and init reads the MAC before signaling
    // completion — so the empty first read has already happened here.
    REQUIRE(backend_->get_status().mac_address.empty());

    // The transport flips: MODE=WIFI plus the rising edge to CONNECTED is the
    // transition handle_snapshot_diff retries the read on (the fixture starts
    // from a disconnected snapshot, so this is the first edge).
    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // The retry runs before the CONNECTED dispatch on the same loop thread,
    // so the event landing means the address is already committed.
    REQUIRE(backend_->get_status().mac_address == injected_mac);
}

// ============================================================================
// 14. Ack attribution with BOTH a join and a scan outstanding (#1398): the
//     join's auth-failure ERR must resolve the JOIN (AUTH_FAILED), not be
//     consumed as the pending scan's completion. The reachable shape is a
//     scan FIRST, then a join landing while the scan is still pending (the
//     reverse is refused synchronously — see the gate case below): netd
//     retries a wrong-password join for tens of seconds, so the collision is
//     routine. The scan loses the tie and its watchdog owns it.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd auth ERR with a scan pending resolves the join first",
                 "[netd][wifi][1398]") {
    // Long watchdog: within this case the scan can only be completed by the
    // misattribution (the bug) — never by its own timer.
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, 10000);
    register_standard_events();
    REQUIRE(backend_->start().success());

    // A scan goes out first and the daemon holds its ack; then the join
    // piles up behind it.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());

    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    join_caller.join();
    REQUIRE(join.success());

    // Single-flight: the join's dispatch resolved the outstanding scan
    // BEFORE its own bytes left, so by verdict time only the join is
    // outstanding and the auth ERR can only mean the join.
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1)); // the resolved scan

    server_->push_line("ERR WRONG_KEY");
    REQUIRE(wait_for_event("AUTH_FAILED", 1));
    REQUIRE(event_count("DISCONNECTED") == 0);
    REQUIRE(event_count("SCAN_COMPLETE") == 1); // no second, misread completion
}

// ============================================================================
// 14b. The daemon runs one op at a time: a scan requested while a join is
//      outstanding is DEFERRED, not refused. A synchronous refusal would
//      toast "scan failed" for an action the UI itself initiated (the
//      manager's association grace is shorter than a netd join's retry
//      ladder) and latch the scanning spinner. The deferred scan goes out
//      the moment the join resolves, and its completion satisfies the
//      trigger_scan obligation.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd scan during a join defers until the join resolves",
                 "[netd][wifi][1398]") {
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, 10000);
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    join_caller.join();
    REQUIRE(join.success());

    // Requested mid-join: accepted, nothing on the wire yet, nothing done.
    const WiFiError scan = backend_->trigger_scan();
    REQUIRE(scan.success());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE_FALSE(line_recorded("SCAN"));
    REQUIRE(event_count("SCAN_COMPLETE") == 0);

    // The join's verdict releases the deferred scan.
    server_->push_line("ERR WRONG_KEY");
    REQUIRE(wait_for_event("AUTH_FAILED", 1));
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));

    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Post"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
    {
        std::vector<WiFiNetwork> rows;
        REQUIRE(backend_->get_scan_results(rows).success());
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0].ssid == "Post");
    }
}

// ============================================================================
// 14c. ERR BUSY reaching a backend with a join in flight is the JOIN's
//      rejection (the daemon denies CONNECT_WIFI while an op flag is
//      active). Single-flight guarantees no scan can be co-pending, so the
//      verdict resolves the join as a disconnect.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd ERR BUSY during a join resolves the join not the scan",
                 "[netd][wifi][1398]") {
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, 10000);
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    join_caller.join();
    REQUIRE(join.success());

    server_->push_line("ERR BUSY");
    REQUIRE(wait_for_event("DISCONNECTED", 1));
    REQUIRE(event_count("AUTH_FAILED") == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(event_count("SCAN_COMPLETE") == 0);
}

// ============================================================================
// 15. Reconnect handshake vs a pending scan (#1398): the liveness give-up
//     closes the connection DELIBERATELY (no drop-detection path runs), so a
//     scan pending at that moment survives into the new connection — and the
//     handshake's seed ack would be consumed as an instant empty-scan
//     completion. The new connection must never answer the old one's scan.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd reconnect handshake does not complete a pending scan",
                 "[netd][wifi][1398]") {
    // Shrunk liveness so the deliberate close lands inside the case; a long
    // watchdog so nothing but the handshake (or the bug) could complete the
    // scan here.
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, 3000, WifiBackendNetd::MacReader{},
                                                 120, 150);
    register_standard_events();
    REQUIRE(backend_->start().success());
    REQUIRE(wait_until([&] { return server_->recorded_line_count() >= 2; })); // SUBSCRIBE+GET

    // A scan goes out and the daemon stays silent: the probe fires, its GET
    // draws no answer either, and the give-up forces the reconnect with the
    // scan still pending.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());

    // The probe GET went out and the reconnect's handshake (SUBSCRIBE+GET)
    // landed on a second connection.
    REQUIRE(wait_until([&] { return line_count("GET") >= 2 && line_count("SUBSCRIBE") >= 2; },
                       10 * (120 + 150 + kReconnectMs)));

    // The orphaned scan was COMPLETED by the reconnect itself (trigger_scan
    // owes the manager exactly one SCAN_COMPLETE; a bare flag clear latches
    // the scheduler), from whatever rows existed before the drop — none here.
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // The handshake's own traffic stays the handshake's: the new connection's
    // GET is answered with a snapshot plus its ack, and nothing about that
    // produces a SECOND completion.
    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("OK");
    REQUIRE(wait_for_event("CONNECTED", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(event_count("SCAN_COMPLETE") == 1);

    // And the new connection scans normally afterwards.
    WiFiError again{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { again = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") >= 2; }));
    caller.join();
    REQUIRE(again.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Fresh"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 2));
}

// ============================================================================
// 16. Liveness probes are owned and single (#1398): one probe chain covers
//     every outstanding op (not one GET per command against a quiet daemon),
//     and stop() retires the chain — a stopped backend stays silent, and the
//     chain cannot follow the backend into a restart.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd liveness probe is single and stops with the backend",
                 "[netd][wifi][1398]") {
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, kWatchdogMs,
                                                 WifiBackendNetd::MacReader{}, 150, 400);
    register_standard_events();
    REQUIRE(backend_->start().success());
    REQUIRE(wait_until([&] { return server_->recorded_line_count() >= 2; }));

    // Two commands go out while the daemon stays silent.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());

    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Quiet", "pw"); });
    REQUIRE(wait_until(
        [&] { return line_recorded("CONNECT_WIFI ssid=" + b64("Quiet") + " psk=" + b64("pw")); }));
    join_caller.join();
    REQUIRE(join.success());

    // Past both commands' probe windows: exactly one probe GET joined the
    // handshake's — never one per command.
    REQUIRE(wait_until([&] { return line_count("GET") >= 2; }, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    REQUIRE(line_count("GET") == 2);

    // stop() retires the chain: nothing the backend owes the daemon survives
    // the stop, so no GET and no reconnect churn can follow it.
    backend_->stop();
    REQUIRE(wait_until([&] { return server_->connection_count() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(line_count("GET") == 2);
    REQUIRE(line_count("SUBSCRIBE") == 1);

    // A restart re-speaks only its own handshake; no stale chain follows it.
    REQUIRE(backend_->start().success());
    REQUIRE(wait_until([&] { return line_count("SUBSCRIBE") >= 2 && line_count("GET") >= 3; }));
    const int gets_after_restart = line_count("GET");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(line_count("GET") == gets_after_restart);
}

// ============================================================================
// 17. Joining the network we are already on (#1398): the daemon ignores a
//     redundant CONNECT_WIFI, so the request must resolve from the snapshot —
//     a prompt synthetic CONNECTED and nothing on the wire. A different
//     network still takes the wire path.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd joining the current network resolves from the snapshot",
                 "[netd][wifi][1398]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("SSID=" + b64("Already"));
    REQUIRE(wait_for_event("CONNECTED", 1));
    // The setup reached the branch this case exercises.
    REQUIRE(backend_->get_status().connected);
    REQUIRE(backend_->get_status().ssid == "Already");

    // Credential-free reselect of the connected network: resolved from the
    // snapshot, nothing on the wire.
    WiFiError result{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { result = backend_->connect_network("Already", ""); });
    caller.join();
    REQUIRE(result.success());
    REQUIRE(wait_for_event("CONNECTED", 2));
    REQUIRE(event_count("DISCONNECTED") == 0);
    for (const auto& line : server_->recorded_lines())
        REQUIRE(line.rfind("CONNECT_WIFI", 0) != 0);

    // A TYPED password is newer information than the snapshot (rotated PSK):
    // it always takes the wire, never a silent snapshot resolution.
    WiFiError rekey{WiFiResult::UNKNOWN_ERROR};
    std::thread caller1([&] { rekey = backend_->connect_network("Already", "newpw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Already") + " psk=" + b64("newpw"));
    }));
    caller1.join();
    REQUIRE(rekey.success());

    // A different network still goes out on the wire.
    WiFiError other{WiFiResult::UNKNOWN_ERROR};
    std::thread caller2([&] { other = backend_->connect_network("Other", "psk"); });
    REQUIRE(wait_until(
        [&] { return line_recorded("CONNECT_WIFI ssid=" + b64("Other") + " psk=" + b64("psk")); }));
    caller2.join();
    REQUIRE(other.success());
}

// ============================================================================
// A refused scan keeps the previous list on screen (#1398): the daemon
// answers a mid-join scan with ERR BUSY, and a scan that never ran must not
// blank the rows the UI is showing. The cache turns over only when the new
// scan's first row arrives (rows precede the completing OK).
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd refused scan keeps the previous rows",
                 "[netd][wifi][1398]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    // Seed the cache with one successful scan.
    WiFiError first{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { first = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    caller.join();
    REQUIRE(first.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Seed"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
    {
        std::vector<WiFiNetwork> rows;
        REQUIRE(backend_->get_scan_results(rows).success());
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0].ssid == "Seed");
    }

    // The next scan is refused by the daemon (no join needed for this shape:
    // BUSY is BUSY). No rows arrive, no OK — just the ERR.
    WiFiError second{WiFiResult::UNKNOWN_ERROR};
    std::thread caller2([&] { second = backend_->trigger_scan(); });
    REQUIRE(
        wait_until([&] { return server_->recorded_line_count() > 0 && line_count("SCAN") == 2; }));
    caller2.join();
    REQUIRE(second.success());
    server_->push_line("ERR BUSY");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 2));
    {
        std::vector<WiFiNetwork> rows;
        REQUIRE(backend_->get_scan_results(rows).success());
        REQUIRE(rows.size() == 1); // the seed row survived the refusal
        REQUIRE(rows[0].ssid == "Seed");
    }
}

// ============================================================================
// stop() with a scan outstanding honors the trigger_scan contract as divided
// by #1405: the manager owns the scheduler latch at the stop boundary, so the
// backend stays SILENT — it clears its internal single-flight flag (a
// stop-then-start reuse begins clean) and keeps serving the last completed
// scan's rows. The #1398-era interim had cleanup_netd() dispatch a synthetic
// SCAN_COMPLETE here; with WiFiManager unlatching on the swap itself
// (test_wifi_manager_scan_unlatch.cpp) that would double-resolve.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd stop with a pending scan stays silent and reusable",
                 "[netd][wifi][1398][1405]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    // Seed one completed scan so the surviving cache holds a row.
    WiFiError seed{WiFiResult::UNKNOWN_ERROR};
    std::thread seeder([&] { seed = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    seeder.join();
    REQUIRE(seed.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Kept"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // A second scan goes out and the daemon holds its ack through the stop.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") == 2; }));
    caller.join();
    REQUIRE(scan.success());

    backend_->stop();
    // No completion for the abandoned scan — the owner resolves the scheduler
    // when IT stops the backend. Past the watchdog bound so the absence is
    // meaningful rather than a race.
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * kWatchdogMs));
    REQUIRE(event_count("SCAN_COMPLETE") == 1); // only the seed's

    // The rows survive the stop: a consumer fetching after the swap answers
    // from the cache, not NOT_INITIALIZED plus a blanked list.
    std::vector<WiFiNetwork> rows;
    REQUIRE(backend_->get_scan_results(rows).success());
    REQUIRE_FALSE(rows.empty());

    // The internal single-flight flag was cleared: after start() reuse, a new
    // scan is accepted (a phantom in-flight scan would refuse it).
    REQUIRE(backend_->start().success());
    REQUIRE(backend_->is_running());
    WiFiError again{WiFiResult::UNKNOWN_ERROR};
    std::thread retry([&] { again = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") >= 3; }));
    retry.join();
    REQUIRE(again.success());
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 2));
}

// ============================================================================
// A scan that RAN and found nothing clears the list honestly: AP turned off,
// out of range - the daemon answers OK with no rows, and serving the
// previous SSIDs would offer ghost networks a user can tap into a full join
// timeout. (An ERR'd scan keeps them: it never ran.)
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd completed-empty scan clears ghost rows",
                 "[netd][wifi][1398]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    // Seed the cache with one network.
    WiFiError first{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { first = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    caller.join();
    REQUIRE(first.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Ghost"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
    {
        std::vector<WiFiNetwork> rows;
        REQUIRE(backend_->get_scan_results(rows).success());
        REQUIRE(rows.size() == 1);
    }

    // The world went empty: OK with no rows.
    WiFiError second{WiFiResult::UNKNOWN_ERROR};
    std::thread caller2([&] { second = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") == 2; }));
    caller2.join();
    REQUIRE(second.success());
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 2));
    {
        std::vector<WiFiNetwork> rows;
        REQUIRE(backend_->get_scan_results(rows).success());
        REQUIRE(rows.empty()); // the ghost is gone
    }
}

// ============================================================================
// An unknown ERR reason still completes a pending scan immediately (no
// closed reason table): a future daemon's new vocabulary must not hang the
// scan to the watchdog. With single-flight, no join can be co-pending, so
// the scan owns whatever ack arrives.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd unknown ERR completes the pending scan",
                 "[netd][wifi][1398]") {
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, 10000);
    register_standard_events();
    REQUIRE(backend_->start().success());

    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    caller.join();
    REQUIRE(scan.success());

    // A reason no client has ever heard of.
    server_->push_line("ERR SOME_FUTURE_REASON");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
    REQUIRE(event_count("DISCONNECTED") == 0);
    REQUIRE(event_count("AUTH_FAILED") == 0);
}

// ============================================================================
// A join that ends via the DISCONNECTED state edge (no ERR ack, no CONNECTED)
// must clear the in-flight latch: a stuck flag blocks every later scan - the
// single-flight gate turns it from inert bookkeeping into a scan killer.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd drop-edge join end reopens scanning",
                 "[netd][wifi][1398]") {
    register_standard_events();
    REQUIRE(backend_->start().success());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // A join goes out (a different network), then the link drops via state,
    // not via any ack.
    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Other", "pw"); });
    REQUIRE(wait_until(
        [&] { return line_recorded("CONNECT_WIFI ssid=" + b64("Other") + " psk=" + b64("pw")); }));
    join_caller.join();
    REQUIRE(join.success());

    server_->push_line("STATE=DISCONNECTED");
    REQUIRE(wait_for_event("DISCONNECTED", 1));

    // Scanning must work again immediately.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
}

#endif // !__APPLE__ && !__ANDROID__
