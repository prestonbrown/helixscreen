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

    /// Ceiling for every bounded wait in this file. Generous on purpose: each
    /// wait returns the moment its condition holds, so the size is paid only on
    /// a run that was going to fail anyway, and a shard-parallel run on a
    /// saturated host can starve the backend's dispatch thread for seconds.
    static constexpr int kWaitMs = 30000;

    bool wait_for_event(const std::string& name, int target, int timeout_ms = kWaitMs) {
        std::unique_lock<std::mutex> lock(event_mutex_);
        return event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this, &name, target] { return counts_[name] >= target; });
    }

    /// start() plus the one guarantee start() does not give: that a line pushed
    /// next will actually reach the backend.
    ///
    /// A Unix stream connect() completes as soon as the client is in the listen
    /// backlog, so backend_->start() returns while the fake daemon's acceptor
    /// thread may still be inside its poll. push_line() writes to the
    /// connections the server has ACCEPTED — there is no queue in front of that
    /// — so a line pushed in the gap is dropped on the floor, and every wait on
    /// its effect then runs to its ceiling. Waiting for the handshake closes the
    /// gap: those bytes were recorded off an accepted connection.
    ///
    /// Counting from the line total BEFORE the start covers a restart too: the
    /// handshake it is waiting for is the NEW connection's, not one an earlier
    /// connection already recorded.
    bool start_and_settle() {
        const size_t before = server_->recorded_line_count();
        if (!backend_->start().success())
            return false;
        return wait_until([&] { return server_->recorded_line_count() >= before + 2; });
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

    /// Barrier for negative assertions: returns once the backend's loop thread
    /// has FINISHED every line pushed before this call — field merge, snapshot
    /// diff and event dispatch included. An `event_count(x) == 0` placed after
    /// it is a statement about work that has completed, not about how long the
    /// test was willing to wait for it.
    ///
    /// Two probes, not one. The loop thread merges a whole read batch and only
    /// then diffs it, so a probe field becoming visible proves the merge step
    /// of its OWN batch and nothing more. The second probe is pushed after the
    /// first is visible, so it can only arrive in a later read — and a later
    /// read is only serviced once the earlier batch's diff and dispatch have
    /// returned on that same thread.
    ///
    /// SSID is the probe field: it merges into the snapshot, reads back through
    /// get_status(), and moves no connected state, so it fires no event of its
    /// own. Each probe value is unique, so a stale snapshot can never satisfy
    /// the wait.
    bool drain_wire() {
        for (int probe = 0; probe < 2; ++probe) {
            const std::string marker = "wire-drain-" + std::to_string(++drain_seq_);
            server_->push_line("SSID=" + b64(marker));
            if (!wait_until([&] { return backend_->get_status().ssid == marker; }, kWaitMs))
                return false;
        }
        return true;
    }

    helix_test::EnvVarGuard sock_env_{"HELIX_NETD_SOCKET"};
    helix_test::EnvVarGuard bin_env_{"HELIX_NETD_BIN"};
    std::unique_ptr<helix_test::NetdFakeServer> server_;
    std::unique_ptr<WifiBackend> backend_;

  private:
    std::string dir_;
    int drain_seq_ = 0;
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

    // NOT_SUPPORTED, not BACKEND_ERROR: the daemon owns both the radio and the
    // saved-network store, so neither capability is broken — it is absent.
    // WiFiManager branches on exactly this to tell the user "not here" instead
    // of raising a failure they caused (forget) or nothing at all (the radio
    // toggle, whose error path is suppressed while any network path is up).
    REQUIRE_FALSE(backend_->supports_radio_toggle());
    const auto radio = backend_->set_radio_enabled(false);
    REQUIRE(radio.result == WiFiResult::NOT_SUPPORTED);
    REQUIRE(radio.user_msg == "WiFi radio is managed by the printer's network daemon");

    const auto forget = backend_->forget_network("SomeNet");
    REQUIRE(forget.result == WiFiResult::NOT_SUPPORTED);

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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=OFFLINE");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // A non-state field and a connected->connected hop must be silent. The
    // barrier makes that provable: both lines are through the diff by the time
    // the counts are read, so an event either exists or was never going to.
    server_->push_line("SIGNAL=-50");
    server_->push_line("STATE=ONLINE");
    REQUIRE(drain_wire());
    REQUIRE(event_count("CONNECTED") == 1);
    REQUIRE(event_count("DISCONNECTED") == 0);

    server_->push_line("STATE=DISCONNECTED");
    REQUIRE(wait_for_event("DISCONNECTED", 1));
    REQUIRE(drain_wire());
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
    REQUIRE(start_and_settle());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    REQUIRE(wait_for_event("CONNECTED", 1));

    // Beacon loss: the daemon passes through RETRYING before the terminal
    // state. The DISCONNECTED must fire here (the link IS down), not be
    // deferred past a gate that will never open again.
    server_->push_line("STATE=RETRYING");
    REQUIRE(wait_for_event("DISCONNECTED", 1));

    // The terminal state behind the intermediate one adds nothing: the link is
    // already down, so this push must be silent.
    server_->push_line("STATE=DISCONNECTED");
    REQUIRE(drain_wire());
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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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
    REQUIRE(drain_wire());
    REQUIRE(event_count("AUTH_FAILED") == 1);
}

// ============================================================================
// 7. Scan: rows then OK => exactly one SCAN_COMPLETE; per-BSS rows merge
//    into one SSID row with the union of bands and the stronger signal.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd scan rows merge and complete once", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(start_and_settle());

    WiFiError result{WiFiResult::SUCCESS};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    const auto scan_sent_at = std::chrono::steady_clock::now();

    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=WPA2-PSK NETWORK=" + b64("Studio 5G"));
    server_->push_line("FREQUENCY=5180 SIGNAL=-61 SECURITY=WPA2-PSK NETWORK=" + b64("Studio 5G"));
    server_->push_line("OK");

    caller.join();
    REQUIRE(result.success());
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // The watchdog is the only other thing that could complete this scan, and
    // its deadline runs from the SCAN going out — so wait to THAT instant plus
    // a margin, which a loaded host has usually passed already, rather than a
    // fixed span after the completion. The barrier then gives the loop thread a
    // full pass, so a completion merely starved past the deadline is counted
    // here too.
    std::this_thread::sleep_until(scan_sent_at + std::chrono::milliseconds(2 * kWatchdogMs));
    REQUIRE(drain_wire());
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
    REQUIRE(start_and_settle());

    WiFiError result{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    const auto scan_sent_at = std::chrono::steady_clock::now();
    caller.join();
    REQUIRE(result.success()); // never parked: the send itself succeeded

    server_->push_line("ERR SCAN_FAILED");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // Same reasoning as the merge case: the watchdog deadline is anchored on
    // the SCAN, and the barrier gives the loop thread a pass at that point.
    std::this_thread::sleep_until(scan_sent_at + std::chrono::milliseconds(2 * kWatchdogMs));
    REQUIRE(drain_wire());
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
    REQUIRE(start_and_settle());

    WiFiError result{WiFiResult::SUCCESS};
    std::thread caller([&] { result = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    // Deliberately NO reply: the ack window expires, trigger_scan returns
    // success, and the only possible completion is the watchdog.

    caller.join();
    REQUIRE(result.success());
    // The watchdog is the only thing that can complete this scan: nothing else
    // ever fires. What is pinned is that it completes at all, so the ceiling is
    // the file's generous one — a starved loop thread must not read as a latch.
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));
}

// ============================================================================
// 10. Daemon death: the backend survives, keeps serving the last snapshot,
//     reconnects within the window, and NEVER sends CANCEL.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd daemon death survives and reconnects", "[netd][wifi]") {
    register_standard_events();
    REQUIRE(start_and_settle());

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

    // Reconnects on the injected cadence and re-subscribes. The wait is on the
    // SECOND subscribe, which is what the count below asserts: the first
    // connection's is already recorded, and the connection set can still be
    // holding the dropped fd, so neither says anything about the reconnect.
    REQUIRE(wait_until([&] { return line_count("SUBSCRIBE") >= 2; }));
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
    REQUIRE(start_and_settle());
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
    REQUIRE(start_and_settle());
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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

    WiFiError join{WiFiResult::UNKNOWN_ERROR};
    std::thread join_caller([&] { join = backend_->connect_network("Cafe 5G", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Cafe 5G") + " psk=" + b64("pw"));
    }));
    join_caller.join();
    REQUIRE(join.success());

    // Requested mid-join: accepted, nothing on the wire yet, nothing done. The
    // barrier costs the loop thread a full read-and-dispatch pass, so a send
    // the deferral had merely queued onto it would have reached the daemon
    // before the counts below are read. Nothing at all may go out, not just no
    // SCAN — a deferred request writes no bytes of any shape.
    const size_t before_defer = server_->recorded_line_count();
    const WiFiError scan = backend_->trigger_scan();
    REQUIRE(scan.success());
    REQUIRE(drain_wire());
    REQUIRE(server_->recorded_line_count() == before_defer);
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
    REQUIRE(start_and_settle());

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
    REQUIRE(drain_wire());
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
    REQUIRE(start_and_settle());

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
    REQUIRE(wait_until([&] { return line_count("GET") >= 2 && line_count("SUBSCRIBE") >= 2; }));

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
    REQUIRE(drain_wire());
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
    // A give-up far outside the case: what the probe chain does when the daemon
    // stays silent past it is the reconnect case's subject, and leaving it
    // reachable here would drop a reconnect's handshake into these line counts.
    static constexpr int kProbeMs = 150;
    static constexpr int kGiveupMs = 30000;
    backend_ = std::make_unique<WifiBackendNetd>(kReconnectMs, kWatchdogMs,
                                                 WifiBackendNetd::MacReader{}, kProbeMs, kGiveupMs);
    register_standard_events();
    REQUIRE(start_and_settle());

    // Two commands go out while the daemon stays silent. Both are joins: the
    // probe deliberately withholds its GET while a scan is outstanding (the
    // reply's ack would be eaten as that scan's completion), so a scan here
    // would make the count depend on which of the two raced.
    WiFiError first{WiFiResult::UNKNOWN_ERROR};
    std::thread first_caller([&] { first = backend_->connect_network("Quiet", "pw"); });
    REQUIRE(wait_until(
        [&] { return line_recorded("CONNECT_WIFI ssid=" + b64("Quiet") + " psk=" + b64("pw")); }));
    first_caller.join();
    REQUIRE(first.success());

    WiFiError second{WiFiResult::UNKNOWN_ERROR};
    std::thread second_caller([&] { second = backend_->connect_network("Quieter", "pw"); });
    REQUIRE(wait_until([&] {
        return line_recorded("CONNECT_WIFI ssid=" + b64("Quieter") + " psk=" + b64("pw"));
    }));
    second_caller.join();
    REQUIRE(second.success());
    const auto second_sent_at = std::chrono::steady_clock::now();

    // One chain covers both commands: exactly one probe GET joins the
    // handshake's, never one per command. The window is measured from the
    // second command going out — that is what a per-command chain arms its own
    // probe on — so a loaded host cannot shrink it into a vacuous pass.
    REQUIRE(wait_until([&] { return line_count("GET") >= 2; }));
    std::this_thread::sleep_until(second_sent_at + std::chrono::milliseconds(3 * kProbeMs));
    REQUIRE(line_count("GET") == 2);

    // stop() retires the chain: nothing the backend owes the daemon survives
    // the stop, so no GET and no reconnect churn can follow it.
    backend_->stop();
    REQUIRE(wait_until([&] { return server_->connection_count() == 0; }));
    const auto stopped_at = std::chrono::steady_clock::now();
    std::this_thread::sleep_until(stopped_at + std::chrono::milliseconds(3 * kProbeMs));
    REQUIRE(line_count("GET") == 2);
    REQUIRE(line_count("SUBSCRIBE") == 1);

    // A restart re-speaks only its own handshake.
    REQUIRE(start_and_settle());
    REQUIRE(wait_until([&] { return line_count("SUBSCRIBE") == 2 && line_count("GET") == 3; }));

    // And the restarted backend can arm a chain of its own — the positive half
    // of "the chain does not follow the backend into a restart". The slot holds
    // ONE chain, so a timer left over from before the stop would still occupy
    // it and this command's probe GET would never go out.
    const WiFiError rejoin = backend_->connect_network("Quiet", "pw");
    REQUIRE(rejoin.success());
    REQUIRE(wait_until([&] {
        return line_count("CONNECT_WIFI ssid=" + b64("Quiet") + " psk=" + b64("pw")) == 2;
    }));
    REQUIRE(wait_until([&] { return line_count("GET") == 4; }));
    REQUIRE(line_count("SUBSCRIBE") == 2);
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
    REQUIRE(start_and_settle());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("SSID=" + b64("Already"));
    // Wait on the FULL merged status, not just connected: the three lines can
    // land in the backend across several reads, and connected becomes true as
    // soon as MODE+STATE merge — before the SSID this case turns on does.
    REQUIRE(wait_until([&] {
        const auto s = backend_->get_status();
        return s.connected && s.ssid == "Already";
    }));
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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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
    const auto scan_sent_at = std::chrono::steady_clock::now();
    caller.join();
    REQUIRE(scan.success());

    backend_->stop();
    // No completion for the abandoned scan — the owner resolves the scheduler
    // when IT stops the backend. A dispatch out of stop() itself is already
    // counted right here; its watchdog is the only other candidate, so the wait
    // runs to that deadline, measured from the SCAN that armed it.
    REQUIRE(event_count("SCAN_COMPLETE") == 1);
    std::this_thread::sleep_until(scan_sent_at + std::chrono::milliseconds(2 * kWatchdogMs));
    REQUIRE(event_count("SCAN_COMPLETE") == 1); // only the seed's

    // The rows survive the stop: a consumer fetching after the swap answers
    // from the cache, not NOT_INITIALIZED plus a blanked list.
    std::vector<WiFiNetwork> rows;
    REQUIRE(backend_->get_scan_results(rows).success());
    REQUIRE_FALSE(rows.empty());

    // The internal single-flight flag was cleared: after start() reuse, a new
    // scan is accepted (a phantom in-flight scan would refuse it).
    REQUIRE(start_and_settle());
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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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
    REQUIRE(start_and_settle());

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

// ============================================================================
// A dead daemon can never produce a synthetic "Connected". The credential-free
// reselect answers a join from the snapshot instead of going to the wire, and
// is_running() deliberately stays true across socket loss (the reconnect timer
// owns that) — so without a liveness check the backend would report CONNECTED
// with a stale SSID and IP that nothing can confirm.
//
// The drop is made observable without a sleep: on_socket_closed() nulls io_
// and THEN completes the scan it killed, so SCAN_COMPLETE arriving proves the
// socket is already gone. Reconnect and watchdog are pushed past the case so
// neither can re-open the connection or complete the scan first.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd dead socket refuses to fabricate a connect",
                 "[netd][wifi]") {
    backend_ = std::make_unique<WifiBackendNetd>(60000, 60000);
    register_standard_events();
    REQUIRE(start_and_settle());

    server_->push_line("MODE=WIFI");
    server_->push_line("STATE=CONNECTED");
    server_->push_line("SSID=" + b64("Already"));
    server_->push_line("IP=10.0.0.4");
    REQUIRE(wait_until([&] {
        const auto s = backend_->get_status();
        return s.connected && s.ssid == "Already";
    }));
    REQUIRE(wait_for_event("CONNECTED", 1));

    // A scan in flight is the drop detector: it is completed by
    // on_socket_closed(), after io_ has been nulled.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread scan_caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    scan_caller.join();
    REQUIRE(scan.success());

    server_->close_clients();
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1)); // io_ == nullptr from here

    // The stale snapshot still says connected — that is exactly the material
    // a fabricated verdict would be built from.
    REQUIRE(backend_->is_running());
    REQUIRE(backend_->get_status().connected);
    REQUIRE(backend_->get_status().ssid == "Already");

    // Reselecting that same network must report the truth: the request could
    // not be made. No second CONNECTED, no synthetic success.
    WiFiError reselect{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { reselect = backend_->connect_network("Already", ""); });
    caller.join();
    REQUIRE(reselect.result == WiFiResult::CONNECTION_FAILED);
    REQUIRE(event_count("CONNECTED") == 1);
}

// ============================================================================
// Scan rows the daemon broadcasts with no scan of ours outstanding are
// dropped, not staged. netd pushes to every subscriber, so another client's
// scan streams rows at us; staging them grew the cache for the life of the
// process and then published someone else's results as the answer to OUR
// next scan.
//
// Ordering is established without a sleep: a snapshot field pushed after the
// foreign row is only visible through get_status() once the loop thread has
// processed everything ahead of it, the row included.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd rows with no scan outstanding are dropped",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(start_and_settle());

    // Nobody asked for this. The SSID push behind it is the ordering probe.
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Foreign"));
    server_->push_line("SSID=" + b64("probe"));
    REQUIRE(wait_until([&] { return backend_->get_status().ssid == "probe"; }));

    // Our own scan, and only our own rows may come out of it.
    WiFiError scan{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { scan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    caller.join();
    REQUIRE(scan.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-40 SECURITY=PSK NETWORK=" + b64("Ours"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    std::vector<WiFiNetwork> rows;
    REQUIRE(backend_->get_scan_results(rows).success());
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].ssid == "Ours");
}

// ============================================================================
// A 5 GHz row still counts as evidence even when it belongs to nobody's scan:
// the radio demonstrably saw the band. The ownership drop above must not take
// supports_5ghz() down with it.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd unowned 5GHz row still proves band support",
                 "[netd][wifi]") {
    REQUIRE(start_and_settle());
    REQUIRE_FALSE(backend_->supports_5ghz());

    server_->push_line("FREQUENCY=5180 SIGNAL=-61 SECURITY=WPA2-PSK NETWORK=" + b64("Foreign 5G"));
    server_->push_line("SSID=" + b64("probe"));
    REQUIRE(wait_until([&] { return backend_->get_status().ssid == "probe"; }));

    REQUIRE(backend_->supports_5ghz());
}

// ============================================================================
// Rows staged by a scan that was abandoned rather than completed must not
// survive into the next scan's published list. stop() clears the in-flight
// flag WITHOUT completing (the manager owns the scheduler at that boundary,
// #1405), so the staging cache is left holding rows; the start() that follows
// retires the orphaned scan through finish_scan(), and that is where they
// have to be dropped.
// ============================================================================
TEST_CASE_METHOD(NetdBackendFixture, "netd abandoned scan rows do not reach the next scan",
                 "[netd][wifi]") {
    register_standard_events();
    REQUIRE(start_and_settle());

    // Scan 1 completes normally and publishes one row.
    WiFiError seed{WiFiResult::UNKNOWN_ERROR};
    std::thread seeder([&] { seed = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_recorded("SCAN"); }));
    seeder.join();
    REQUIRE(seed.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-52 SECURITY=PSK NETWORK=" + b64("Kept"));
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 1));

    // Scan 2 streams a row and is then abandoned by stop() with no ack. The
    // SSID push behind the row is the ordering probe: once it is visible, the
    // loop thread has staged the row.
    WiFiError orphan{WiFiResult::UNKNOWN_ERROR};
    std::thread caller([&] { orphan = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") == 2; }));
    caller.join();
    REQUIRE(orphan.success());
    server_->push_line("FREQUENCY=2437 SIGNAL=-30 SECURITY=PSK NETWORK=" + b64("Stale"));
    server_->push_line("SSID=" + b64("probe"));
    REQUIRE(wait_until([&] { return backend_->get_status().ssid == "probe"; }));

    backend_->stop();
    REQUIRE(start_and_settle());

    // Scan 3 RAN and found nothing: the honest answer is an empty list. A
    // staged "Stale" surviving the abandonment would be published here
    // instead, as a network the daemon never reported on this scan.
    WiFiError third{WiFiResult::UNKNOWN_ERROR};
    std::thread caller3([&] { third = backend_->trigger_scan(); });
    REQUIRE(wait_until([&] { return line_count("SCAN") >= 3; }));
    caller3.join();
    REQUIRE(third.success());
    server_->push_line("OK");
    REQUIRE(wait_for_event("SCAN_COMPLETE", 2));

    std::vector<WiFiNetwork> rows;
    REQUIRE(backend_->get_scan_results(rows).success());
    REQUIRE(rows.empty());
}

#endif // !__APPLE__ && !__ANDROID__
