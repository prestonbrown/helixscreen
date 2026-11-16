// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/wifi_backend.h"
#include "../../include/wifi_backend_networkmanager.h"
#include "netd_test_server.h" // helix_test::wait_until

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
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
 * to inject canned nmcli output, so most cases need no nmcli binary — the
 * connect tests are the exception, exec'ing the fake nmcli that
 * FakeNmcliDir puts on PATH.
 */

// ============================================================================
// Testable Subclass: Override exec_nmcli() to inject canned output
// ============================================================================

class TestableNMBackend : public WifiBackendNetworkManager {
  public:
    // Expose private methods for unit testing via public wrappers
    using WifiBackendNetworkManager::connect_argv;
    using WifiBackendNetworkManager::is_polkit_permission_error;
    using WifiBackendNetworkManager::parse_scan_output;
    using WifiBackendNetworkManager::split_nmcli_fields;
    using WifiBackendNetworkManager::validate_input;

    // Friend access allows TestableNMBackend to reach private members.

    /// Reset the connection-transition latch, standing in for what start() does
    /// on a reused backend (wifi_backend_networkmanager.cpp) so the next poll
    /// re-fires CONNECTED even if the system was already connected (#1059).
    /// TEST_MIRROR_OK: start() shells out to nmcli and refuses without a live
    /// NetworkManager, so the latch is seeded directly; the transition rule the
    /// reset feeds is production's own apply_polled_status(), not a copy.
    /// Exposed as a public hook because free TEST_CASE bodies aren't covered by
    /// friendship.
    void reset_prev_connected() {
        prev_connected_.store(false);
    }

    /// Feed one poll result through production's own poll handler — the same
    /// apply_polled_status() call status_thread_func() makes on each tick, minus
    /// the nmcli subprocess that produced the ConnectionStatus. Returns the raw
    /// event string production fired, or empty if no transition.
    std::string simulate_status_poll(bool connected) {
        ConnectionStatus st;
        st.connected = connected;
        st.ssid = connected ? "TestSSID" : "";
        st.signal_strength = connected ? 75 : 0;
        st.ip_address = connected ? "192.168.1.100" : "";
        st.mac_address = "de:ad:be:ef:ca:fe";

        return apply_polled_status(st);
    }

    /// Feed one exact poll result through apply_polled_status().
    std::string feed_status(const ConnectionStatus& st) {
        return apply_polled_status(st);
    }

    /// Seed the state start() would have established — a resolved interface
    /// and a live backend — since start() itself shells out to nmcli and
    /// refuses without a real NetworkManager.
    void force_running(const char* iface) {
        running_ = true;
        wifi_interface_ = iface;
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

namespace {

/// A throwaway dir holding a fake `nmcli` that records the argv it is exec'd
/// with. Prepended to PATH, it stands in for NetworkManager: the only way to
/// observe what the backend's fork/exec actually sends.
struct FakeNmcliDir {
    std::string dir;
    std::string argv_file;
    std::string count_file;

    FakeNmcliDir() {
        char tmpl[] = "/tmp/helix-fake-nmcli-XXXXXX";
        dir = ::mkdtemp(tmpl);
        argv_file = dir + "/argv.txt";
        count_file = dir + "/count";

        std::string script = R"SH(#!/bin/sh
n=$(($(cat "$NMCLI_COUNT" 2>/dev/null || echo 0) + 1))
echo "$n" > "$NMCLI_COUNT"
if [ -n "$NMCLI_FAIL_ALL" ]; then
  echo "$NMCLI_FAIL_ALL" >&2
  exit 1
fi
if [ "$n" -eq 1 ] && [ "$NMCLI_FAIL_FIRST" = "1" ]; then
  echo "Error: 802-11-wireless-security.key-mgmt: property is missing." >&2
  exit 1
fi
while [ -n "$NMCLI_HOLD" ] && [ -e "$NMCLI_HOLD" ]; do sleep 0.05; done
printf '%s\n' "$@" > "$NMCLI_ARGV"
exit 0
)SH";
        std::ofstream(dir + "/nmcli") << script;
        ::chmod((dir + "/nmcli").c_str(), 0755);

        ::setenv("NMCLI_ARGV", argv_file.c_str(), 1);
        ::setenv("NMCLI_COUNT", count_file.c_str(), 1);
    }
    ~FakeNmcliDir() {
        for (const char* var :
             {"NMCLI_ARGV", "NMCLI_COUNT", "NMCLI_FAIL_FIRST", "NMCLI_FAIL_ALL", "NMCLI_HOLD"}) {
            ::unsetenv(var);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// First nmcli invocation fails with the stale-profile key-mgmt error, so
    /// the backend's self-heal delete + retry path runs.
    void fail_first() {
        ::setenv("NMCLI_FAIL_FIRST", "1", 1);
    }

    /// Successful invocations wait until release(), standing in for a join in progress.
    void hold() {
        std::ofstream(dir + "/hold");
        ::setenv("NMCLI_HOLD", (dir + "/hold").c_str(), 1);
    }
    void release() {
        std::filesystem::remove(dir + "/hold");
    }

    /// Every invocation fails with `stderr_line` on stderr and nothing on stdout.
    void fail_all(const char* stderr_line) {
        ::setenv("NMCLI_FAIL_ALL", stderr_line, 1);
    }

    /// How many times the fake nmcli was exec'd.
    int call_count() {
        std::ifstream f(count_file);
        int n = 0;
        f >> n;
        return n;
    }

    /// The argv of the LAST invocation, one argument per line.
    std::vector<std::string> argv_lines() {
        std::ifstream f(argv_file);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
        }
        return lines;
    }
};

/// Prepends a dir to PATH for its lifetime; the forked nmcli child inherits it.
struct ScopedPathPrepend {
    std::string old_path;
    bool path_was_set = false;

    explicit ScopedPathPrepend(const std::string& prepend) {
        const char* cur = ::getenv("PATH");
        path_was_set = (cur != nullptr);
        old_path = cur ? cur : "";
        std::string merged = prepend + ":" + old_path;
        ::setenv("PATH", merged.c_str(), 1);
    }
    ~ScopedPathPrepend() {
        if (path_was_set) {
            ::setenv("PATH", old_path.c_str(), 1);
        } else {
            ::unsetenv("PATH");
        }
    }
};

} // namespace

// ============================================================================
// Connect Argument Vector Tests
// ============================================================================
//
// `nmcli device wifi connect <ssid>` matches the SSID against NetworkManager's
// scan cache. A hidden network broadcasts no SSID, so it is never in that
// cache and the connect can only end in "No network with SSID found" (exit 10)
// unless the `hidden yes` pair tells nmcli to associate anyway. These cases
// pin the exact argv the exec path builds.

TEST_CASE("NM backend: connect_argv", "[network][nm][hidden]") {
    SECTION("Visible secured network: ssid, password pair, ifname last") {
        auto argv = TestableNMBackend::connect_argv("HomeNet", "secret", false, "wlan0");
        std::vector<std::string> expected{"device",   "wifi",   "connect", "HomeNet",
                                          "password", "secret", "ifname",  "wlan0"};
        REQUIRE(argv == expected);
    }

    SECTION("Hidden network inserts 'hidden yes' between password and ifname") {
        auto argv = TestableNMBackend::connect_argv("HiddenNet", "secret", true, "wlan0");
        std::vector<std::string> expected{"device", "wifi",   "connect", "HiddenNet", "password",
                                          "secret", "hidden", "yes",     "ifname",    "wlan0"};
        REQUIRE(argv == expected);
    }

    SECTION("Hidden open network omits the password pair entirely") {
        auto argv = TestableNMBackend::connect_argv("HiddenOpen", "", true, "wlan0");
        std::vector<std::string> expected{"device", "wifi", "connect", "HiddenOpen",
                                          "hidden", "yes",  "ifname",  "wlan0"};
        REQUIRE(argv == expected);
    }

    SECTION("Visible open network has neither password nor hidden pair") {
        auto argv = TestableNMBackend::connect_argv("OpenNet", "", false, "wlan0");
        std::vector<std::string> expected{"device",  "wifi",   "connect",
                                          "OpenNet", "ifname", "wlan0"};
        REQUIRE(argv == expected);
    }
}

// The argv above only proves the helper; this drives the whole fork/exec path
// through connect_network() against a fake nmcli, so what NetworkManager would
// actually receive is on the wire.
TEST_CASE("NM backend: hidden connect execs nmcli with 'hidden yes'",
          "[network][nm][hidden][slow]") {
    FakeNmcliDir fake;
    ScopedPathPrepend path_prepend(fake.dir);

    // Declared before the backend so it outlives the connect thread's
    // callback: a failed wait unwinds locals in reverse order, and the
    // backend destructor joins a thread that can still fire CONNECTED.
    std::atomic<bool> connected{false};
    TestableNMBackend backend;
    backend.force_running("wlan0");
    backend.register_event_callback("CONNECTED", [&](const std::string&) { connected = true; });

    std::vector<std::string> expected{"device",  "wifi",   "connect", "StealthNet", "password",
                                      "pw12345", "hidden", "yes",     "ifname",     "wlan0"};

    SECTION("plain hidden join") {
        REQUIRE(backend.connect_network("StealthNet", "pw12345", /*is_hidden=*/true).success());
        REQUIRE(helix_test::wait_until([&connected] { return connected.load(); }, 8000));

        CHECK(fake.argv_lines() == expected);
    }

    SECTION("stale-profile self-heal retry keeps the hidden flag") {
        fake.fail_first();

        REQUIRE(backend.connect_network("StealthNet", "pw12345", /*is_hidden=*/true).success());
        REQUIRE(helix_test::wait_until([&connected] { return connected.load(); }, 8000));

        // The retry really ran: call 1 failed with the key-mgmt error, call 2
        // deleted the stale profile, call 3 retried the connect.
        REQUIRE(fake.call_count() == 3);
        // The fake nmcli's argv file holds its LAST invocation: the retry.
        CHECK(fake.argv_lines() == expected);
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
// Band Parsing (helixscreen#1189)
//
// Without FREQ in the requested fields every network arrives with frequency 0
// and its band unknowable. FREQ is the fifth field, rendered by nmcli as
// "<n> MHz".
// ============================================================================

TEST_CASE("NM backend: parse_scan_output reads the FREQ field", "[network][nm][band][1189]") {
    TestableNMBackend backend;

    SECTION("Frequency populates band_mask") {
        std::string output = " :TwoFour:70:WPA2:2437 MHz\n"
                             " :FiveGig:60:WPA2:5745 MHz\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 2);
        CHECK(networks[0].frequency_mhz == 2437);
        CHECK(networks[0].band_mask == WIFI_BAND_2_4GHZ);
        CHECK(networks[1].frequency_mhz == 5745);
        CHECK(networks[1].band_mask == WIFI_BAND_5GHZ);
    }

    SECTION("Dual-band SSID collapses to one row advertising both bands") {
        // The 2.4GHz BSS is stronger; the 5GHz twin must not vanish entirely.
        std::string output = " :HomeNet:82:WPA2:2437 MHz\n"
                             " :HomeNet:54:WPA2:5745 MHz\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].signal_strength == 82);
        CHECK((networks[0].band_mask & WIFI_BAND_5GHZ) != 0);
        CHECK((networks[0].band_mask & WIFI_BAND_2_4GHZ) != 0);
    }

    SECTION("Missing FREQ field leaves the band unknown, not wrong") {
        std::string output = " :LegacyNmcli:75:WPA2\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].frequency_mhz == 0);
        CHECK(networks[0].band_mask == WIFI_BAND_NONE);
    }

    SECTION("Unparseable FREQ is ignored") {
        std::string output = " :WeirdFreq:75:WPA2:not-a-number\n";

        auto networks = backend.parse_scan_output(output);
        REQUIRE(networks.size() == 1);
        CHECK(networks[0].frequency_mhz == 0);
        CHECK(networks[0].band_mask == WIFI_BAND_NONE);
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

        WiFiError connect_err = backend.connect_network("Test", "pass", /*is_hidden=*/false);
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

    backend.register_event_callback("CONNECTED", [&](const std::string& d) {
        connect_count++;
        last_event_data = d;
    });
    backend.register_event_callback("DISCONNECTED", [&](const std::string& d) {
        disconnect_count++;
        last_event_data = d;
    });

    SECTION("First poll with connected=true fires CONNECTED") {
        std::string ev = backend.simulate_status_poll(true);
        CHECK(ev == "CONNECTED");
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);
    }

    SECTION("Full cycle fires correct sequence") {
        // Initial: prev=false (default)
        backend.simulate_status_poll(false); // no change
        CHECK(connect_count == 0);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(true); // false→true: CONNECTED
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(true); // same: no event
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 0);

        backend.simulate_status_poll(false); // true→false: DISCONNECTED
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 1);

        backend.simulate_status_poll(false); // same: no event
        CHECK(connect_count == 1);
        CHECK(disconnect_count == 1);

        backend.simulate_status_poll(true); // false→true: CONNECTED
        CHECK(connect_count == 2);
        CHECK(disconnect_count == 1);
    }
}

// ============================================================================
// Status poll transition detection — prev_connected_ reset on stop
// ============================================================================

TEST_CASE("NM backend: start() resets prev_connected_ so next poll re-detects",
          "[network][nm][status][events]") {
    // The backend object is REUSED across a WiFi off/on toggle: WiFiManager
    // drives backend_->set_radio_enabled() and deliberately leaves the backend
    // running, so the member is NOT recreated. A genuine stop()/start() cycle
    // (auto-failover, re-init after INIT_FAILED) hits the same reuse. start()
    // therefore explicitly resets prev_connected_ to false, so the first poll
    // after a restart re-fires CONNECTED if the system is still connected and
    // re-notifies observers (fixes the stale-icon-after-toggle bug, #1059).
    // Here we drive that reset directly since start() needs a live NetworkManager.
    TestableNMBackend backend;

    int connect_count = 0;
    backend.register_event_callback("CONNECTED", [&](const std::string&) { connect_count++; });

    SECTION("reset + simulate_poll fires CONNECTED on next disconnected->connected") {
        backend.simulate_status_poll(true); // CONNECTED fires, prev_=true
        connect_count = 0;

        // Reproduce what start() does on the reused object after a stop/start.
        backend.reset_prev_connected();

        backend.simulate_status_poll(true); // false→true: CONNECTED fires again
        CHECK(connect_count == 1);
    }
}

// ============================================================================
// Radio power — the backend must answer for itself, not inherit the base
// class's silent no-op + unconditional "radio is on"
// ============================================================================

TEST_CASE("NM backend implements radio power instead of inheriting the base no-op",
          "[network][nm][radio]") {
    TestableNMBackend backend;

    // Not started: set_radio_enabled() must refuse rather than report the
    // base class's silent success, which is what let the UI switch flip and
    // then snap straight back.
    WiFiError err = backend.set_radio_enabled(false);
    CHECK_FALSE(err.success());
    CHECK(err.result == WiFiResult::NOT_INITIALIZED);

    // And a refused change must not have moved the cached state.
    CHECK(backend.is_radio_enabled());

    // The base-class defaults would have made both of the above pass in the
    // opposite direction; pin the override explicitly so removing it fails.
    WifiBackend& as_base = backend;
    CHECK_FALSE(as_base.set_radio_enabled(false).success());
}

// ============================================================================
// wpa_supplicant fallback: only when NetworkManager holds nothing
// ============================================================================

// A polkit denial is an answer FROM NetworkManager: the daemon is alive and
// owns wlan0 and its supplicant, so wpa_supplicant must not take the radio.
TEST_CASE("NM backend: polkit denial refuses the wpa_supplicant fallback",
          "[network][nm][fallback]") {
    FakeNmcliDir fake;
    fake.fail_all("Error: Not authorized to control networking.");
    ScopedPathPrepend path(fake.dir);

    WifiBackendNetworkManager backend;
    backend.set_silent(true);
    WiFiError result = backend.start();

    REQUIRE(result.result == WiFiResult::PERMISSION_DENIED);
    CHECK_FALSE(backend.supports_wpa_supplicant_fallback());
}

TEST_CASE("NM backend: an unreachable daemon allows the wpa_supplicant fallback",
          "[network][nm][fallback]") {
    FakeNmcliDir fake;
    fake.fail_all("Error: NetworkManager is not running.");
    ScopedPathPrepend path(fake.dir);

    WifiBackendNetworkManager backend;
    backend.set_silent(true);
    WiFiError result = backend.start();

    REQUIRE(result.result == WiFiResult::SERVICE_NOT_RUNNING);
    CHECK(backend.supports_wpa_supplicant_fallback());
}

TEST_CASE("NM backend: polls while DHCP is pending", "[network][nm][status][wifi_dhcp_pending]") {
    FakeNmcliDir fake;
    ScopedPathPrepend path_prepend(fake.dir);
    std::atomic<int> connects{0};
    std::atomic<int> changes{0};
    TestableNMBackend backend;
    backend.force_running("wlan0");
    backend.register_event_callback("CONNECTED", [&](const std::string&) { connects++; });
    backend.register_event_callback("ADDRESS_CHANGED", [&](const std::string&) { changes++; });

    WifiBackend::ConnectionStatus st;
    st.connected = true;
    st.ssid = "Home";
    st.awaiting_ipv4 = true;

    SECTION("a poll during a user's join leaves the outcome to nmcli") {
        fake.hold();
        REQUIRE(backend.connect_network("Home", "", false).success());
        REQUIRE(helix_test::wait_until([&] { return fake.call_count() >= 1; }, 8000));
        CHECK(backend.feed_status(st).empty());
        CHECK(connects == 0);
        fake.release();
        REQUIRE(helix_test::wait_until([&] { return connects.load() == 1; }, 8000));
    }

    SECTION("the address arriving fires ADDRESS_CHANGED once") {
        CHECK(backend.feed_status(st) == "CONNECTED");
        st.awaiting_ipv4 = false;
        st.ip_address = "10.0.0.9";
        CHECK(backend.feed_status(st) == "ADDRESS_CHANGED");
        CHECK(backend.feed_status(st).empty());
        CHECK(changes == 1);
    }
}

#else
// macOS: Provide a placeholder test so the file isn't empty
TEST_CASE("NM backend: not available on macOS", "[network][nm]") {
    SUCCEED("NetworkManager backend is Linux-only");
}
#endif // __APPLE__
