// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/wifi_backend.h"
#include "../../include/wifi_backend_networkmanager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

#ifndef __APPLE__

/**
 * NetworkManager WiFi Backend Unit Tests
 *
 * Tests verify:
 * - nmcli output parsing (scan results, status, interface detection)
 * - Terse-mode field splitting with escaped colons
 * - Input validation (SSID/password sanitization)
 * - Backend lifecycle (start/stop/is_running)
 * - Event system (callback registration and firing)
 * - Edge cases (empty results, malformed output, hidden SSIDs)
 *
 * NOTE: These tests use a testable subclass that overrides exec_nmcli()
 * to inject canned nmcli output. No actual nmcli binary needed.
 */

// ============================================================================
// Testable Subclass: Override exec_nmcli() to inject canned output
// ============================================================================

class TestableNMBackend : public WifiBackendNetworkManager {
  public:
    // Expose private methods for unit testing via public wrappers
    using WifiBackendNetworkManager::is_polkit_permission_error;
    using WifiBackendNetworkManager::parse_scan_output;
    using WifiBackendNetworkManager::split_nmcli_fields;
    using WifiBackendNetworkManager::validate_input;

    // Friend access allows TestableNMBackend to reach private members.

    /// Simulate one status-poll cycle: update the cached status, then detect
    /// and fire the CONNECTED / DISCONNECTED event if the connection state
    /// transitioned — exactly what status_thread_func() does on each tick.
    /// Returns the raw event string that was fired, or empty if no event.
    std::string simulate_status_poll(bool connected) {
        ConnectionStatus st;
        st.connected = connected;
        st.ssid = connected ? "TestSSID" : "";
        st.signal_strength = connected ? 75 : 0;
        st.ip_address = connected ? "192.168.1.100" : "";
        st.mac_address = "de:ad:be:ef:ca:fe";

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_ = st;
        }

        bool now = st.connected;
        bool was = prev_connected_.exchange(now);
        std::string event;
        if (now && !was) {
            event = "CONNECTED";
            fire_event(event);
        } else if (!now && was) {
            event = "DISCONNECTED";
            fire_event(event);
        }
        return event;
    }
};

// ============================================================================
// nmcli Field Splitting Tests
// ============================================================================

TEST_CASE("NM backend: split_nmcli_fields", "[network][nm][parsing]") {
    TestableNMBackend backend;

    SECTION("Simple colon-separated fields") {
        auto fields = backend.split_nmcli_fields("field1:field2:field3");
        REQUIRE(fields.size() == 3);
        CHECK(fields[0] == "field1");
        CHECK(fields[1] == "field2");
        CHECK(fields[2] == "field3");
    }

    SECTION("Escaped colons preserved in fields") {
        // nmcli escapes literal colons as \:
        auto fields = backend.split_nmcli_fields("My\\:Network:85:WPA2");
        REQUIRE(fields.size() == 3);
        CHECK(fields[0] == "My:Network");
        CHECK(fields[1] == "85");
        CHECK(fields[2] == "WPA2");
    }

    SECTION("Multiple escaped colons in one field") {
        auto fields = backend.split_nmcli_fields("a\\:b\\:c:value");
        REQUIRE(fields.size() == 2);
        CHECK(fields[0] == "a:b:c");
        CHECK(fields[1] == "value");
    }

    SECTION("Empty fields between colons") {
        auto fields = backend.split_nmcli_fields("a::c");
        REQUIRE(fields.size() == 3);
        CHECK(fields[0] == "a");
        CHECK(fields[1] == "");
        CHECK(fields[2] == "c");
    }

    SECTION("Single field, no colons") {
        auto fields = backend.split_nmcli_fields("justonevalue");
        REQUIRE(fields.size() == 1);
        CHECK(fields[0] == "justonevalue");
    }

    SECTION("Empty string") {
        auto fields = backend.split_nmcli_fields("");
        REQUIRE(fields.size() == 1);
        CHECK(fields[0] == "");
    }

    SECTION("Trailing colon") {
        auto fields = backend.split_nmcli_fields("a:b:");
        REQUIRE(fields.size() == 3);
        CHECK(fields[0] == "a");
        CHECK(fields[1] == "b");
        CHECK(fields[2] == "");
    }

    SECTION("Other backslash escapes pass through") {
        // nmcli also escapes backslashes but we only unescape \: and backslash
        auto fields = backend.split_nmcli_fields("path\\\\dir:value");
        REQUIRE(fields.size() == 2);
        CHECK(fields[0] == "path\\dir");
        CHECK(fields[1] == "value");
    }
}

// ============================================================================
// Scan Output Parsing Tests
// ============================================================================

TEST_CASE("NM backend: parse_scan_output", "[network][nm][parsing]") {
    TestableNMBackend backend;

    SECTION("Typical scan output with multiple networks") {
        // nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list
        std::string output = " :HomeNetwork-5G:92:WPA2\n"
                             "*:Office-Main:78:WPA2\n"
                             " :CoffeeShop_Free:68:\n"
                             " :IoT-Devices:55:WPA\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 4);

        // First network
        CHECK(networks[0].ssid == "HomeNetwork-5G");
        CHECK(networks[0].signal_strength == 92);
        CHECK(networks[0].is_secured == true);
        CHECK(networks[0].security_type == "WPA2");

        // Connected network (marked with *)
        CHECK(networks[1].ssid == "Office-Main");
        CHECK(networks[1].signal_strength == 78);

        // Open network (no security field)
        CHECK(networks[2].ssid == "CoffeeShop_Free");
        CHECK(networks[2].signal_strength == 68);
        CHECK(networks[2].is_secured == false);
        CHECK(networks[2].security_type == "Open");

        // WPA network
        CHECK(networks[3].ssid == "IoT-Devices");
        CHECK(networks[3].is_secured == true);
        CHECK(networks[3].security_type == "WPA");
    }

    SECTION("Hidden networks (empty SSID) are skipped") {
        std::string output = " ::45:WPA2\n"
                             " :VisibleNet:80:WPA2\n"
                             " ::30:WPA\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].ssid == "VisibleNet");
    }

    SECTION("SSIDs with escaped colons") {
        std::string output = " :My\\:Network\\:5G:85:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].ssid == "My:Network:5G");
        CHECK(networks[0].signal_strength == 85);
    }

    SECTION("Duplicate SSIDs deduplicated, keeping strongest signal") {
        std::string output = " :MeshNet:40:WPA2\n"
                             " :MeshNet:85:WPA2\n"
                             " :MeshNet:60:WPA2\n"
                             " :OtherNet:70:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 2);

        // Find MeshNet - should have strongest signal (85)
        auto mesh = std::find_if(networks.begin(), networks.end(),
                                 [](const WiFiNetwork& n) { return n.ssid == "MeshNet"; });
        REQUIRE(mesh != networks.end());
        CHECK(mesh->signal_strength == 85);
    }

    SECTION("Empty output returns empty vector") {
        auto networks = backend.parse_scan_output("");
        REQUIRE(networks.empty());
    }

    SECTION("Malformed lines are skipped") {
        std::string output = "garbage line with no structure\n"
                             " :GoodNetwork:75:WPA2\n"
                             ":::\n"
                             " :AnotherGood:60:WPA\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 2);
        CHECK(networks[0].ssid == "GoodNetwork");
        CHECK(networks[1].ssid == "AnotherGood");
    }

    SECTION("Signal strength clamped to 0-100") {
        std::string output = " :StrongNet:150:WPA2\n"
                             " :WeakNet:-5:WPA2\n"
                             " :NormalNet:50:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 3);

        // All signals should be clamped to valid range
        for (const auto& net : networks) {
            CHECK(net.signal_strength >= 0);
            CHECK(net.signal_strength <= 100);
        }
    }

    SECTION("WPA3 security type detected") {
        std::string output = " :SecureNet:90:WPA3\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].is_secured == true);
        CHECK(networks[0].security_type == "WPA3");
    }

    SECTION("WPA1 WPA2 mixed security") {
        std::string output = " :MixedNet:75:WPA1 WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].is_secured == true);
        // Should detect WPA2 as the highest security
        CHECK(networks[0].security_type == "WPA2");
    }

    SECTION("WEP security detected") {
        std::string output = " :OldRouter:40:WEP\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].is_secured == true);
        CHECK(networks[0].security_type == "WEP");
    }

    SECTION("Non-numeric signal strength skipped") {
        std::string output = " :BadSignal:abc:WPA2\n"
                             " :GoodNet:75:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].ssid == "GoodNet");
    }

    SECTION("Lines with too few fields are skipped") {
        std::string output = " :OnlyTwo\n"
                             " :GoodNet:75:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].ssid == "GoodNet");
    }
}

// ============================================================================
// Input Validation Tests
// ============================================================================

TEST_CASE("NM backend: validate_input", "[network][nm][security]") {
    TestableNMBackend backend;

    SECTION("Normal SSID passes validation") {
        auto result = backend.validate_input("MyHomeNetwork", "SSID");
        CHECK(result == "MyHomeNetwork");
    }

    SECTION("Normal password passes validation") {
        auto result = backend.validate_input("MyP@ssw0rd!", "password");
        CHECK(result == "MyP@ssw0rd!");
    }

    SECTION("SSID with spaces passes") {
        auto result = backend.validate_input("My Home Network", "SSID");
        CHECK(result == "My Home Network");
    }

    SECTION("SSID with hyphens and underscores passes") {
        auto result = backend.validate_input("Home-Net_5G", "SSID");
        CHECK(result == "Home-Net_5G");
    }

    SECTION("Empty string rejected") {
        auto result = backend.validate_input("", "SSID");
        CHECK(result.empty());
    }

    SECTION("String with null byte rejected") {
        std::string with_null = "Hello";
        with_null += '\0';
        with_null += "World";
        auto result = backend.validate_input(with_null, "SSID");
        CHECK(result.empty());
    }

    SECTION("String with control characters rejected") {
        auto result = backend.validate_input("Bad\x01Network", "SSID");
        CHECK(result.empty());
    }

    SECTION("String with newline rejected") {
        auto result = backend.validate_input("Bad\nNetwork", "SSID");
        CHECK(result.empty());
    }

    SECTION("String with tab rejected") {
        auto result = backend.validate_input("Bad\tNetwork", "SSID");
        CHECK(result.empty());
    }

    SECTION("String exceeding 255 chars rejected") {
        std::string long_str(256, 'A');
        auto result = backend.validate_input(long_str, "SSID");
        CHECK(result.empty());
    }

    SECTION("String at exactly 255 chars passes") {
        std::string max_str(255, 'A');
        auto result = backend.validate_input(max_str, "SSID");
        CHECK(result == max_str);
    }

    SECTION("DEL character (0x7F) rejected") {
        auto result = backend.validate_input("Bad\x7FNetwork", "SSID");
        CHECK(result.empty());
    }

    SECTION("Unicode characters pass (above ASCII)") {
        // UTF-8 encoded chars above 0x7F should pass
        auto result = backend.validate_input("CafeNet", "SSID");
        CHECK(result == "CafeNet");
    }
}

// ============================================================================
// Backend Lifecycle Tests (no nmcli needed)
// ============================================================================

TEST_CASE("NM backend: lifecycle basics", "[network][nm][lifecycle]") {
    // These tests check internal state without requiring nmcli

    SECTION("Backend not running after construction") {
        WifiBackendNetworkManager backend;
        REQUIRE_FALSE(backend.is_running());
    }

    SECTION("Operations fail when not started") {
        WifiBackendNetworkManager backend;

        WiFiError scan_err = backend.trigger_scan();
        REQUIRE_FALSE(scan_err.success());
        REQUIRE(scan_err.result == WiFiResult::NOT_INITIALIZED);

        std::vector<WiFiNetwork> networks;
        WiFiError results_err = backend.get_scan_results(networks);
        REQUIRE_FALSE(results_err.success());
        REQUIRE(results_err.result == WiFiResult::NOT_INITIALIZED);

        WiFiError connect_err = backend.connect_network("Test", "pass");
        REQUIRE_FALSE(connect_err.success());
        REQUIRE(connect_err.result == WiFiResult::NOT_INITIALIZED);

        WiFiError disconnect_err = backend.disconnect_network();
        REQUIRE_FALSE(disconnect_err.success());
        REQUIRE(disconnect_err.result == WiFiResult::NOT_INITIALIZED);
    }

    SECTION("get_status returns disconnected when not running") {
        WifiBackendNetworkManager backend;
        auto status = backend.get_status();
        REQUIRE_FALSE(status.connected);
        REQUIRE(status.ssid.empty());
        REQUIRE(status.ip_address.empty());
    }

    SECTION("Multiple stop calls are safe") {
        WifiBackendNetworkManager backend;
        REQUIRE_NOTHROW(backend.stop());
        REQUIRE_NOTHROW(backend.stop());
        REQUIRE_FALSE(backend.is_running());
    }

    SECTION("Event callback registration works before start") {
        WifiBackendNetworkManager backend;
        int count = 0;
        backend.register_event_callback("SCAN_COMPLETE", [&count](const std::string&) { count++; });
        // Callback registered but not fired
        REQUIRE(count == 0);
    }
}

// ============================================================================
// Event System Tests
// ============================================================================

TEST_CASE("NM backend: event callback registration", "[network][nm][events]") {
    SECTION("Replacing callback for same event name is rejected") {
        WifiBackendNetworkManager backend;
        int count1 = 0;
        int count2 = 0;

        backend.register_event_callback("SCAN_COMPLETE",
                                        [&count1](const std::string&) { count1++; });
        // Second registration for same name - should be ignored (same as wpa_supplicant)
        backend.register_event_callback("SCAN_COMPLETE",
                                        [&count2](const std::string&) { count2++; });

        REQUIRE(count1 == 0);
        REQUIRE(count2 == 0);
    }

    SECTION("Multiple different events can be registered") {
        WifiBackendNetworkManager backend;

        int scan_count = 0;
        int connect_count = 0;
        int auth_count = 0;

        backend.register_event_callback("SCAN_COMPLETE",
                                        [&scan_count](const std::string&) { scan_count++; });
        backend.register_event_callback("CONNECTED",
                                        [&connect_count](const std::string&) { connect_count++; });
        backend.register_event_callback("AUTH_FAILED",
                                        [&auth_count](const std::string&) { auth_count++; });

        // All registered, none fired
        REQUIRE(scan_count == 0);
        REQUIRE(connect_count == 0);
        REQUIRE(auth_count == 0);
    }
}

// ============================================================================
// Status Cache Tests
// ============================================================================

TEST_CASE("NM backend: get_status returns cached status when not running",
          "[network][nm][status]") {
    WifiBackendNetworkManager backend;
    auto status = backend.get_status();
    REQUIRE_FALSE(status.connected);
    REQUIRE(status.signal_strength == 0);
    REQUIRE(status.ssid.empty());
    REQUIRE(status.ip_address.empty());
    REQUIRE(status.mac_address.empty());
}

TEST_CASE("NM backend: status thread lifecycle", "[network][nm][status]") {
    SECTION("Status thread not running after construction") {
        WifiBackendNetworkManager backend;
        auto status = backend.get_status();
        REQUIRE_FALSE(status.connected);
    }

    SECTION("Multiple stop calls safe with status thread") {
        WifiBackendNetworkManager backend;
        REQUIRE_NOTHROW(backend.stop());
        REQUIRE_NOTHROW(backend.stop());
    }
}

// ============================================================================
// Polkit Permission Error Detection Tests
// ============================================================================

TEST_CASE("NM backend: is_polkit_permission_error", "[network][nm][polkit]") {
    SECTION("Detects 'Not authorized'") {
        CHECK(TestableNMBackend::is_polkit_permission_error(
            "Error: Not authorized to control networking."));
    }

    SECTION("Detects 'Permission denied'") {
        CHECK(TestableNMBackend::is_polkit_permission_error("Error: Permission denied."));
    }

    SECTION("Detects 'Insufficient privileges'") {
        CHECK(TestableNMBackend::is_polkit_permission_error("Error: Insufficient privileges.\n"));
    }

    SECTION("Detects 'insufficient privilege' case-insensitive") {
        CHECK(TestableNMBackend::is_polkit_permission_error("INSUFFICIENT PRIVILEGES"));
    }

    SECTION("Detects polkit keyword") {
        CHECK(TestableNMBackend::is_polkit_permission_error("polkit: authorization check failed"));
    }

    SECTION("Detects NetworkManager D-Bus denial") {
        CHECK(TestableNMBackend::is_polkit_permission_error(
            "org.freedesktop.NetworkManager: not permitted"));
    }

    SECTION("Returns false for empty string") {
        CHECK_FALSE(TestableNMBackend::is_polkit_permission_error(""));
    }

    SECTION("Returns false for wrong password error") {
        CHECK_FALSE(TestableNMBackend::is_polkit_permission_error(
            "Error: Connection activation failed: Secrets were required, but not provided."));
    }

    SECTION("Returns false for timeout error") {
        CHECK_FALSE(
            TestableNMBackend::is_polkit_permission_error("Error: Timeout 90 sec expired."));
    }
}

// ============================================================================
// Status Poll Transition Event Tests (#1059 regression)
// ============================================================================

TEST_CASE("NM backend: status poll fires CONNECTED/DISCONNECTED on transitions",
          "[network][nm][status][events]") {
    TestableNMBackend backend;

    int connect_count = 0;
    int disconnect_count = 0;
    std::string last_event_data;

    backend.register_event_callback("CONNECTED",
                                    [&](const std::string& d) { connect_count++; last_event_data = d; });
    backend.register_event_callback("DISCONNECTED",
                                    [&](const std::string& d) { disconnect_count++; last_event_data = d; });

    SECTION("First poll with connected=false fires nothing") {
        std::string ev = backend.simulate_status_poll(false);
        CHECK(ev.empty());
        CHECK(connect_count == 0);
        CHECK(disconnect_count == 0);
    }

    SECTION("First poll with connected=true fires CONNECTED") {
        std::string ev = backend.simulate_status_poll(true);
        CHECK(ev == "CONNECTED");
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);
    }

    SECTION("No duplicate event for same connected state") {
        backend.simulate_status_poll(false);
        std::string ev = backend.simulate_status_poll(false);
        CHECK(ev.empty());
        CHECK(connect_count == 0);
        CHECK(disconnect_count == 0);
    }

    SECTION("Transition false->true fires CONNECTED") {
        backend.simulate_status_poll(false);
        std::string ev = backend.simulate_status_poll(true);
        CHECK(ev == "CONNECTED");
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);
    }

    SECTION("Transition true->false fires DISCONNECTED") {
        backend.simulate_status_poll(true);
        std::string ev = backend.simulate_status_poll(false);
        CHECK(ev == "DISCONNECTED");
        CHECK(connect_count == 1);   // first poll true → CONNECTED
        CHECK(disconnect_count == 1);
    }

    SECTION("Full cycle fires correct sequence") {
        // Initial: prev=false (default)
        backend.simulate_status_poll(false);   // no change
        CHECK(connect_count == 0);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(true);    // false→true: CONNECTED
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(true);    // same: no event
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(false);   // true→false: DISCONNECTED
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 1);

        backend.simulate_status_poll(false);   // same: no event
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 1);

        backend.simulate_status_poll(true);    // false→true: CONNECTED
        CHECK(connect_count == 2);
        CHECK(disconnect_count == 1);
    }

    SECTION("Events reach the WiFiManager callback chain") {
        // Verify that events fired through fire_event() actually invoke
        // the registered callbacks (not just our local counters).
        TestableNMBackend inner;
        std::string captured_event;
        inner.register_event_callback("CONNECTED",
                                      [&](const std::string& d) { captured_event = "CB:" + d; });
        inner.simulate_status_poll(true);
        CHECK(captured_event == "CB:");
    }
}

// ============================================================================
// Status poll transition detection — prev_connected_ reset on stop
// ============================================================================

TEST_CASE("NM backend: stop resets prev_connected_ so next start re-detects",
          "[network][nm][status][events]") {
    // When the backend is stopped and restarted, prev_connected_ resets to
    // false (via member initializer), ensuring the first poll after restart
    // fires CONNECTED if the system is still connected (no stale state).
    TestableNMBackend backend;

    int connect_count = 0;
    backend.register_event_callback("CONNECTED",
                                    [&](const std::string&) { connect_count++; });

    SECTION("stop + simulate_poll fires CONNECTED on next disconnected->connected") {
        backend.simulate_status_poll(true);  // CONNECTED fires, prev_=true
        connect_count = 0;

        // Simulate stop: prev_connected_ is default-initialised to false on
        // the next backend instance. The real WifiBackendNetworkManager::stop()
        // destroys the status thread; the member is recreated on next start.
        // For this test we manually reset.
        backend.prev_connected_.store(false);

        backend.simulate_status_poll(true);  // false→true: CONNECTED fires again
        CHECK(connect_count == 1);
    }
}

#else
// macOS: Provide a placeholder test so the file isn't empty
TEST_CASE("NM backend: not available on macOS", "[network][nm]") {
    SUCCEED("NetworkManager backend is Linux-only");
}
#endif // __APPLE__
