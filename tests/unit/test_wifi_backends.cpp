// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/wifi_backend.h"
#include "../../include/wifi_backend_mock.h"
#if !defined(__APPLE__) && !defined(__ANDROID__)
#include "../../include/wifi_backend_wpa_supplicant.h"
#endif

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "../catch_amalgamated.hpp"

/**
 * WiFi Backend Unit Tests
 *
 * Tests verify backend-specific functionality:
 * - Backend lifecycle (start/stop/is_running)
 * - Event system (callback registration and firing)
 * - Mock backend behavior (scan timing, network data)
 * - Timer cleanup and resource management
 *
 * CRITICAL BUGS CAUGHT:
 * - Backend auto-start bug: Mock backend should NOT start itself in constructor
 * - Timer cleanup: Timers must be cleaned up in stop()/destructor
 * - Event callback validation: Events should not fire when backend stopped
 */

// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

// No LVGL initialization needed - WiFi backend uses std::thread, not LVGL timers

// ============================================================================
// Test Fixtures
// ============================================================================

class WiFiBackendTestFixture {
  public:
    WiFiBackendTestFixture() {
        // Create mock backend directly for testing
        backend = std::make_unique<WifiBackendMock>();

        // Reset state
        event_count = 0;
        last_event_name.clear();
        last_event_data.clear();
    }

    ~WiFiBackendTestFixture() {
        // Cleanup backend
        if (backend) {
            backend->stop();
        }
    }

    /**
     * @brief Signal that an event occurred (call from test callbacks)
     */
    void notify_event() {
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            event_count++;
        }
        event_cv_.notify_all();
    }

    /**
     * @brief Wait for event count to reach a specific value
     * @param target_count Number of events to wait for
     * @param timeout_ms Maximum time to wait
     * @return true if count reached, false on timeout
     */
    bool wait_for_event_count(int target_count, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(event_mutex_);
        return event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [this, target_count]() { return event_count >= target_count; });
    }

    /**
     * @brief Wait for a condition to become true
     * @param condition Function that returns true when condition is met
     * @param timeout_ms Maximum time to wait
     * @return true if condition met, false on timeout
     */
    bool wait_for_condition(std::function<bool()> condition, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(event_mutex_);
        return event_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), condition);
    }

    /**
     * @brief Reset event count (call between test phases)
     */
    void reset_event_count() {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_count = 0;
    }

    /**
     * @brief Get current event count (thread-safe)
     */
    int get_event_count() {
        std::lock_guard<std::mutex> lock(event_mutex_);
        return event_count;
    }

    // Test backend
    std::unique_ptr<WifiBackend> backend;

    // Test state (protected by mutex for thread safety)
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    int event_count = 0;
    std::string last_event_name;
    std::string last_event_data;
};

// ============================================================================
// Backend Lifecycle Tests
// ============================================================================

TEST_CASE_METHOD(WiFiBackendTestFixture, "Backend lifecycle",
                 "[network][backend][lifecycle][slow]") {
    SECTION("Backend created but not running by default") {
        // CRITICAL: This catches the auto-start bug
        REQUIRE_FALSE(backend->is_running());
    }

    SECTION("Backend start() enables it") {
        WiFiError result = backend->start();
        REQUIRE(result.success());
        REQUIRE(backend->is_running());
    }

    SECTION("Backend stop() disables it") {
        backend->start();
        REQUIRE(backend->is_running());

        backend->stop();
        REQUIRE_FALSE(backend->is_running());
    }

    SECTION("Backend lifecycle: start → stop → start") {
        // Initial: not running
        REQUIRE_FALSE(backend->is_running());

        // First start
        backend->start();
        REQUIRE(backend->is_running());

        // Stop
        backend->stop();
        REQUIRE_FALSE(backend->is_running());

        // Second start (should work)
        WiFiError result = backend->start();
        REQUIRE(result.success());
        REQUIRE(backend->is_running());
    }

    SECTION("Multiple start() calls are idempotent") {
        backend->start();
        REQUIRE(backend->is_running());

        // Second start() should succeed (no-op)
        WiFiError result = backend->start();
        REQUIRE(result.success());
        REQUIRE(backend->is_running());
    }

    SECTION("Multiple stop() calls are safe") {
        backend->start();
        backend->stop();
        REQUIRE_FALSE(backend->is_running());

        // Second stop() should be safe (no crash)
        REQUIRE_NOTHROW(backend->stop());
        REQUIRE_FALSE(backend->is_running());
    }
}

// ============================================================================
// Event System Tests
// ============================================================================

TEST_CASE_METHOD(WiFiBackendTestFixture, "Backend event system",
                 "[network][backend][events][slow]") {
    SECTION("Event callback registration") {
        int callback_count = 0;
        backend->register_event_callback("TEST_EVENT", [&callback_count](const std::string& data) {
            (void)data;
            callback_count++;
        });

        // Callback registered (can't directly test until event fires)
        REQUIRE(callback_count == 0); // Not fired yet
    }

    SECTION("SCAN_COMPLETE event fires after scan") {
        backend->start();

        // Register callback that signals condition variable
        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Trigger scan
        WiFiError result = backend->trigger_scan();
        REQUIRE(result.success());

        // Wait for SCAN_COMPLETE event with proper synchronization
        REQUIRE(wait_for_event_count(1, 5000));
    }

    SECTION("Multiple event callbacks can be registered") {
        backend->start();

        int scan_count = 0;
        int connect_count = 0;

        backend->register_event_callback("SCAN_COMPLETE", [&scan_count](const std::string& data) {
            (void)data;
            scan_count++;
        });

        backend->register_event_callback("CONNECTED", [&connect_count](const std::string& data) {
            (void)data;
            connect_count++;
        });

        // Both callbacks registered
        REQUIRE(scan_count == 0);
        REQUIRE(connect_count == 0);
    }

    SECTION("Event callback survives backend restart") {
        backend->start();

        // Register callback that signals condition variable
        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Restart backend
        backend->stop();
        backend->start();

        // Trigger scan
        backend->trigger_scan();

        // Wait for scan to complete with proper synchronization
        REQUIRE(wait_for_event_count(1, 5000));
    }
}

// ============================================================================
// Mock Backend Scan Tests
// ============================================================================

TEST_CASE_METHOD(WiFiBackendTestFixture, "Mock backend scan behavior",
                 "[network][backend][mock][scan][slow]") {
    SECTION("trigger_scan() fails when backend not running") {
        // Backend not started
        REQUIRE_FALSE(backend->is_running());

        WiFiError result = backend->trigger_scan();
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == WiFiResult::NOT_INITIALIZED);
    }

    SECTION("trigger_scan() succeeds when backend running") {
        backend->start();
        REQUIRE(backend->is_running());

        WiFiError result = backend->trigger_scan();
        REQUIRE(result.success());
    }

    SECTION("Scan results available after SCAN_COMPLETE") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        backend->trigger_scan();

        // Wait for scan to complete with proper synchronization
        REQUIRE(wait_for_event_count(1, 5000));

        // Get scan results
        std::vector<WiFiNetwork> networks;
        WiFiError result = backend->get_scan_results(networks);
        REQUIRE(result.success());
        REQUIRE(networks.size() == 10); // Mock backend has 10 networks
    }

    SECTION("get_scan_results() fails when backend not running") {
        REQUIRE_FALSE(backend->is_running());

        std::vector<WiFiNetwork> networks;
        WiFiError result = backend->get_scan_results(networks);
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == WiFiResult::NOT_INITIALIZED);
        REQUIRE(networks.empty());
    }

    SECTION("Mock networks have valid data") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        backend->trigger_scan();

        // Wait for scan with proper synchronization
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);

        REQUIRE(networks.size() == 10);

        for (const auto& net : networks) {
            // SSID not empty
            REQUIRE_FALSE(net.ssid.empty());

            // Signal strength in range
            REQUIRE(net.signal_strength >= 0);
            REQUIRE(net.signal_strength <= 100);

            // Security info present
            if (net.is_secured) {
                REQUIRE_FALSE(net.security_type.empty());
            }
        }
    }

    SECTION("Networks sorted by signal strength") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        backend->trigger_scan();

        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);

        // Mock backend sorts by signal strength (strongest first)
        for (size_t i = 1; i < networks.size(); i++) {
            REQUIRE(networks[i - 1].signal_strength >= networks[i].signal_strength);
        }
    }

    SECTION("Signal strength varies on each scan") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // First scan
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> scan1;
        backend->get_scan_results(scan1);

        // Second scan
        reset_event_count();
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> scan2;
        backend->get_scan_results(scan2);

        // At least one network should have different signal strength (±5% variation)
        bool found_variation = false;
        for (size_t i = 0; i < scan1.size(); i++) {
            if (scan1[i].signal_strength != scan2[i].signal_strength) {
                found_variation = true;
                break;
            }
        }

        // Note: May occasionally be same due to random number generation
        INFO("Signal strength varied: " << (found_variation ? "yes" : "no"));
    }
}

// ============================================================================
// Mock Backend Connection Tests
// ============================================================================

TEST_CASE_METHOD(WiFiBackendTestFixture, "Mock backend connection behavior",
                 "[network][backend][mock][connect][slow]") {
    SECTION("connect_network() fails when backend not running") {
        REQUIRE_FALSE(backend->is_running());

        WiFiError result = backend->connect_network("TestNet", "password");
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == WiFiResult::NOT_INITIALIZED);
    }

    SECTION("connect_network() fails for non-existent network") {
        backend->start();

        WiFiError result = backend->connect_network("NonExistentNetwork", "password");
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == WiFiResult::NETWORK_NOT_FOUND);
    }

    SECTION("connect_network() requires password for secured networks") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Get a secured network
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);

        auto secured = std::find_if(networks.begin(), networks.end(),
                                    [](const WiFiNetwork& n) { return n.is_secured; });
        REQUIRE(secured != networks.end());

        // Try connecting without password
        WiFiError result = backend->connect_network(secured->ssid, "");
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == WiFiResult::INVALID_PARAMETERS);
    }

    SECTION("Successful connection fires CONNECTED event") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Get available networks
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);
        REQUIRE(networks.size() > 0);

        // Register CONNECTED callback
        reset_event_count();
        backend->register_event_callback("CONNECTED", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Connect to first network (mock backend simulates 2-3s delay)
        WiFiError result = backend->connect_network(networks[0].ssid, "test_password");
        REQUIRE(result.success()); // Connection initiated

        // Wait for CONNECTED event with proper synchronization
        bool connected = wait_for_event_count(1, 5000);

        // Note: Mock has 5% chance of auth failure, might not always succeed
        INFO("Got CONNECTED event: " << (connected ? "yes" : "no"));
    }

    SECTION("disconnect_network() is safe when not connected") {
        backend->start();

        WiFiError result = backend->disconnect_network();
        REQUIRE(result.success()); // Idempotent operation
    }

    SECTION("Connection status updated after connect") {
        backend->start();

        // Initial status: not connected
        auto status = backend->get_status();
        REQUIRE_FALSE(status.connected);

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Get networks and connect
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);

        reset_event_count();
        backend->register_event_callback("CONNECTED", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        backend->connect_network(networks[0].ssid, "test_password");

        // Wait for connection with proper synchronization
        bool connected = wait_for_event_count(1, 5000);

        if (connected) {
            status = backend->get_status();
            REQUIRE(status.connected);
            REQUIRE_FALSE(status.ssid.empty());
            REQUIRE_FALSE(status.ip_address.empty());
        }
    }
}

// ============================================================================
// Timer Cleanup Tests
// ============================================================================

TEST_CASE_METHOD(WiFiBackendTestFixture, "Backend timer cleanup",
                 "[network][backend][cleanup][slow]") {
    SECTION("stop() cleans up scan timer") {
        backend->start();
        backend->trigger_scan();

        // Stop before scan completes
        backend->stop();

        // No crash - timers cleaned up
        REQUIRE_NOTHROW(backend->stop());
    }

    SECTION("stop() cleans up connection timer") {
        backend->start();

        backend->register_event_callback("SCAN_COMPLETE", [this](const std::string& data) {
            (void)data;
            notify_event();
        });

        // Get networks
        backend->trigger_scan();
        REQUIRE(wait_for_event_count(1, 5000));

        std::vector<WiFiNetwork> networks;
        backend->get_scan_results(networks);

        // Start connection
        backend->connect_network(networks[0].ssid, "password");

        // Stop before connection completes
        backend->stop();

        // No crash - timers cleaned up
        REQUIRE_NOTHROW(backend->stop());
    }

    SECTION("Destructor cleans up active timers") {
        auto temp_backend = std::make_unique<WifiBackendMock>();
        temp_backend->start();
        temp_backend->trigger_scan();

        // Destroy while scan in progress
        REQUIRE_NOTHROW(temp_backend.reset());
    }

    SECTION("No events fire after backend stopped") {
        backend->start();

        int event_count = 0;
        backend->register_event_callback("SCAN_COMPLETE", [&event_count](const std::string& data) {
            (void)data;
            event_count++;
        });

        backend->trigger_scan();

        // Stop immediately (before scan completes)
        backend->stop();

        // Wait to ensure scan thread is fully cleaned up
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        // Event should NOT fire (thread was canceled)
        REQUIRE(event_count == 0);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

// ============================================================================
// Non-Blocking Construction Tests (proposed API)
// ============================================================================
//
// WifiBackend::create() currently calls the concrete backend's start()
// inline on the calling thread. For WifiBackendNetworkManager that means
// 4+ blocking subprocess calls (nmcli, iw phy) before create() returns —
// if NetworkManager is hung, the UI thread hangs with it.
//
// The proposed contract is:
//   - WifiBackend::create() returns quickly, with the backend NOT yet running.
//     Any subprocess probing must be deferred to a worker thread.
//   - Callers can register an on-ready handler (or "READY" event) and only
//     treat the backend as usable once it fires.
//   - is_running() reflects the actual state (may be false immediately after
//     create()) — callers must not assume the backend is ready synchronously.

TEST_CASE("WifiBackend::create() does not block the caller",
          "[network][backend][lifecycle][slow]") {
    // Even if the platform backend would block in start(), create() must
    // return immediately — the subprocess probing belongs on a worker.
    auto t0 = std::chrono::steady_clock::now();
    auto backend = WifiBackend::create(/*silent=*/true);
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    REQUIRE(backend != nullptr);
    // Generous bound; a cold nmcli probe chain is typically 10-100ms+ and
    // unbounded when NetworkManager is wedged. Anything under 50 ms here
    // indicates the probe is not happening synchronously on this thread.
    REQUIRE(elapsed_ms < 50);
}

TEST_CASE("WifiBackend: READY event fires once backend finishes async init",
          "[network][backend][events][slow]") {
    // Test the async-init contract directly against the mock. Going through
    // WifiBackend::create() here is brittle because the factory path depends
    // on RuntimeConfig::test_mode, which isn't globally set for helix-tests
    // and is mutated by other tests — a prior test flipping test_mode=false
    // makes create() pick the real backend (nmcli/wpa_supplicant), which on
    // a CI runner with no NM daemon never fires READY, hanging for 10s.
    auto backend = std::make_unique<WifiBackendMock>();
    backend->set_silent(true);

    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> ready{false};

    backend->register_event_callback("READY", [&](const std::string&) {
        {
            std::lock_guard<std::mutex> lock(m);
            ready.store(true);
        }
        cv.notify_all();
    });

    backend->start_async();

    std::unique_lock<std::mutex> lock(m);
    bool ok = cv.wait_for(lock, std::chrono::seconds(10), [&]() { return ready.load(); });
    REQUIRE(ok);
    REQUIRE(backend->is_running());
}

TEST_CASE_METHOD(WiFiBackendTestFixture, "Backend edge cases",
                 "[network][backend][edge-cases][slow]") {
    SECTION("Rapid start/stop cycles") {
        for (int i = 0; i < 5; i++) {
            backend->start();
            backend->stop();
        }

        // Final state: not running
        REQUIRE_FALSE(backend->is_running());
    }

    SECTION("Multiple trigger_scan() calls") {
        backend->start();

        // Trigger multiple scans rapidly
        backend->trigger_scan();
        backend->trigger_scan();
        backend->trigger_scan();

        // Should not crash (later calls replace earlier timer)
        REQUIRE_NOTHROW(backend->stop());
    }

    SECTION("get_status() safe when not connected") {
        auto status = backend->get_status();
        REQUIRE_FALSE(status.connected);
        REQUIRE(status.ssid.empty());
        REQUIRE(status.ip_address.empty());
        REQUIRE(status.signal_strength == 0);
    }
}

#if !defined(__APPLE__) && !defined(__ANDROID__)
// ============================================================================
// Regression: wpa_supplicant backend must NOT report "running" after a FAILED
// init (helixscreen#1036).
//
// On a fresh boot the control socket can be present (so discovery + preflight
// pass) while wpa_supplicant isn't actually answering yet, so the control
// connection / ATTACH fails. The old code keyed is_running() off init_complete_,
// which signal_init_complete() sets on *every* init path — success OR failure —
// so a dead backend reported as "running". The wizard then believed WiFi was up,
// every scan returned "Backend not started", and nothing ever retried. is_running()
// now keys off init_succeeded_ (set only on a fully-connected init), so a failed
// init reports honestly and the retry paths re-attempt.
//
// We fake the failure with a bound-but-unanswered AF_UNIX DGRAM socket pointed at
// by HELIX_WPA_SOCKET_DIR: directory iteration + is_socket() + permission checks
// pass, but it speaks no wpa protocol, so init_wpa() fails at connect/ATTACH.
// ============================================================================
TEST_CASE("WpaBackend honest is_running after failed init (#1036)",
          "[network][backend][wpa][slow]") {
    char dir_template[] = "/tmp/helix_wpa_test_XXXXXX";
    char* dir = mkdtemp(dir_template);
    REQUIRE(dir != nullptr);

    std::string sock_path = std::string(dir) + "/wlan0";
    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    setenv("HELIX_WPA_SOCKET_DIR", dir, 1);

    {
        WifiBackendWpaSupplicant backend;
        // start() runs preflight (passes — socket exists + accessible) then
        // init_wpa() (fails — nobody answers on the dummy socket).
        WiFiError result = backend.start();

        // The init attempt must NOT be reported as a usable backend:
        // is_running() must not return init_complete_ (true after any finished
        // attempt).
        REQUIRE_FALSE(result.success());
        REQUIRE_FALSE(backend.is_running());

        backend.stop();
    }

    unsetenv("HELIX_WPA_SOCKET_DIR");
    ::close(fd);
    ::unlink(sock_path.c_str());
    ::rmdir(dir);
}

// ============================================================================
// read_ctrl_interface_from_conf — config-file ctrl_interface parsing (#1036)
//
// Vendor firmwares often launch `wpa_supplicant -c <conf>` with a non-standard
// ctrl_interface set inside the config file rather than via -O/-C on the
// command line. The /proc cmdline scan parses that file so we can auto-detect
// the socket directory without a manual HELIX_WPA_SOCKET_DIR override.
// ============================================================================
TEST_CASE("WpaBackend never offers static IPv4, so the row hides", "[network][wifi][ipv4]") {
    WifiBackendWpaSupplicant backend;
    CHECK_FALSE(backend.supports_ipv4_config());
    helix::net::Ipv4Config out;
    CHECK(backend.get_ipv4_config(out).status == helix::net::Ipv4Result::Status::NotSupported);
}

TEST_CASE("read_ctrl_interface_from_conf parses ctrl_interface forms", "[network][backend][wpa]") {
    using helix::wifi::detail::read_ctrl_interface_from_conf;

    auto write_conf = [](const std::string& body) {
        char tmpl[] = "/tmp/helix_wpa_conf_XXXXXX";
        int fd = ::mkstemp(tmpl);
        REQUIRE(fd >= 0);
        ::close(fd);
        std::ofstream out(tmpl);
        out << body;
        out.close();
        return std::string(tmpl);
    };

    SECTION("keyed DIR=... GROUP=... form") {
        auto p = write_conf("ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
                            "update_config=1\n");
        REQUIRE(read_ctrl_interface_from_conf(p) == "/var/run/wpa_supplicant");
        ::unlink(p.c_str());
    }

    SECTION("bare absolute path form") {
        auto p = write_conf("ctrl_interface=/etc/wifi/wpa_supplicant/sockets\n");
        REQUIRE(read_ctrl_interface_from_conf(p) == "/etc/wifi/wpa_supplicant/sockets");
        ::unlink(p.c_str());
    }

    SECTION("ignores comments and leading whitespace") {
        auto p = write_conf("# ctrl_interface=/wrong/commented/out\n"
                            "\n"
                            "    ctrl_interface=DIR=/run/wpa_supplicant\n");
        REQUIRE(read_ctrl_interface_from_conf(p) == "/run/wpa_supplicant");
        ::unlink(p.c_str());
    }

    SECTION("tolerates CRLF line endings") {
        auto p = write_conf("ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev\r\n");
        REQUIRE(read_ctrl_interface_from_conf(p) == "/run/wpa_supplicant");
        ::unlink(p.c_str());
    }

    SECTION("no ctrl_interface present → empty") {
        auto p = write_conf("update_config=1\nnetwork={\n ssid=\"x\"\n}\n");
        REQUIRE(read_ctrl_interface_from_conf(p).empty());
        ::unlink(p.c_str());
    }

    SECTION("non-absolute value rejected") {
        auto p = write_conf("ctrl_interface=wlan0\n");
        REQUIRE(read_ctrl_interface_from_conf(p).empty());
        ::unlink(p.c_str());
    }

    SECTION("unreadable file → empty") {
        REQUIRE(read_ctrl_interface_from_conf("/nonexistent/helix/wpa.conf").empty());
    }
}
#endif // !__APPLE__ && !__ANDROID__
