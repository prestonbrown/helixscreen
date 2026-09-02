// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "data_root_resolver.h"
#include "wifi_backend_mock.h"
#include "wifi_backend_wpa_supplicant.h"
#include "wifi_saved_config.h"

#include <cstdlib>
#include <filesystem>

#include "../catch_amalgamated.hpp"

/**
 * connect_network() used to ADD_NETWORK unconditionally on every connect. A
 * real user's wpa_supplicant had reached network id 7 for a handful of
 * networks, and duplicate all-enabled entries give wpa_supplicant more
 * candidates to roam between after a reboot — a plausible contributor to that
 * user's printer reassociating to a 5 GHz SSID they had explicitly forgotten.
 *
 * find_network_id() locates an already-saved entry for an SSID in a raw
 * LIST_NETWORKS reply so connect_network() can reuse it instead of stacking a
 * new one. It is declared outside the header's __APPLE__ guard specifically
 * so this parsing logic is unit-tested on every platform, independent of the
 * wpa_ctrl-backed call site that only builds on Linux.
 */

TEST_CASE("find_network_id locates an existing SSID", "[wifi][dedupe]") {
    const std::string reply = "network id / ssid / bssid / flags\n"
                              "0\tOldNet\tany\t[DISABLED]\n"
                              "7\tHomeNet\tany\t[CURRENT]\n";
    CHECK(helix::wifi::detail::find_network_id(reply, "HomeNet") == "7");
    CHECK(helix::wifi::detail::find_network_id(reply, "Missing").empty());
}

TEST_CASE("find_network_id does not prefix-match", "[wifi][dedupe]") {
    const std::string reply = "network id / ssid / bssid / flags\n3\tHome\tany\t[]\n";
    CHECK(helix::wifi::detail::find_network_id(reply, "HomeNet").empty());
}

TEST_CASE("find_network_id handles an SSID containing spaces", "[wifi][dedupe]") {
    // Real reporter SSIDs contain spaces; splitting on whitespace would break.
    const std::string reply = "network id / ssid / bssid / flags\n2\tmy home net\tany\t[]\n";
    CHECK(helix::wifi::detail::find_network_id(reply, "my home net") == "2");
}

TEST_CASE("find_network_id returns empty on an empty reply", "[wifi][dedupe]") {
    // send_command() returns "" when the control connection is down. That
    // must read as "no existing entry", not crash.
    CHECK(helix::wifi::detail::find_network_id("", "HomeNet").empty());
}

TEST_CASE("find_network_id returns empty on a header-only reply", "[wifi][dedupe]") {
    CHECK(helix::wifi::detail::find_network_id("network id / ssid / bssid / flags\n", "HomeNet")
              .empty());
}

TEST_CASE("find_network_id ignores an empty ssid argument", "[wifi][dedupe]") {
    const std::string reply = "network id / ssid / bssid / flags\n0\t\tany\t[DISABLED]\n";
    CHECK(helix::wifi::detail::find_network_id(reply, "").empty());
}

TEST_CASE("find_network_id matches the last line without a trailing newline", "[wifi][dedupe]") {
    const std::string reply = "network id / ssid / bssid / flags\n5\tHomeNet\tany\t[CURRENT]";
    CHECK(helix::wifi::detail::find_network_id(reply, "HomeNet") == "5");
}

/**
 * forget_network(): a real, user-initiated way to remove a saved network.
 * REMOVE_NETWORK by itself was only ever issued as connect-failure cleanup —
 * a user who wanted a 5GHz SSID gone had no in-app path and had to fall back
 * to the vendor screen (the exact path that failed them in the first place).
 *
 * Task 11's helix::wifi::store re-adds any stored network wpa_supplicant does
 * not know about at every backend init (reconcile_saved_networks()). A forget
 * that only touches wpa_supplicant's LIST_NETWORKS would therefore be
 * silently undone the next time the printer boots — forget_network() must
 * also drop the SSID from that store, and the tests below assert the store
 * file itself, not just the return value.
 */

namespace {

/// Point HELIX_CONFIG_DIR at an isolated temp directory for the duration of a
/// test, restoring whatever was there before on scope exit. Mirrors
/// tests/unit/test_wifi_saved_config_store.cpp's guard of the same name —
/// forget_network() touches the store on disk, so every test here needs its
/// own isolated config dir rather than whatever "config" the process cwd
/// resolves to by default.
class ConfigDirGuard {
  public:
    explicit ConfigDirGuard(const std::string& dir) {
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            had_prev_ = true;
            prev_ = prev;
        }
        setenv("HELIX_CONFIG_DIR", dir.c_str(), 1);
    }

    ~ConfigDirGuard() {
        if (had_prev_)
            setenv("HELIX_CONFIG_DIR", prev_.c_str(), 1);
        else
            unsetenv("HELIX_CONFIG_DIR");
    }

    ConfigDirGuard(const ConfigDirGuard&) = delete;
    ConfigDirGuard& operator=(const ConfigDirGuard&) = delete;

  private:
    bool had_prev_ = false;
    std::string prev_;
};

std::string make_temp_dir(const std::string& name) {
    const std::string dir = "/tmp/" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// Minimal concrete WifiBackend — implements only the pure virtuals, so
/// forget_network() falls through to WifiBackend's own base-class default.
/// Exists purely to prove that default is a real failure, not the silent
/// success set_radio_enabled() deliberately uses for an unreachable toggle.
class MinimalStubBackend : public WifiBackend {
  public:
    WiFiError start() override {
        return WiFiErrorHelper::success();
    }
    void stop() override {}
    bool is_running() const override {
        return true;
    }
    void register_event_callback(const std::string&,
                                 std::function<void(const std::string&)>) override {}
    WiFiError trigger_scan() override {
        return WiFiErrorHelper::success();
    }
    WiFiError get_scan_results(std::vector<WiFiNetwork>&) override {
        return WiFiErrorHelper::success();
    }
    WiFiError connect_network(const std::string&, const std::string&) override {
        return WiFiErrorHelper::success();
    }
    WiFiError disconnect_network() override {
        return WiFiErrorHelper::success();
    }
    ConnectionStatus get_status() override {
        return {};
    }
    bool supports_5ghz() const override {
        return false;
    }
};

} // namespace

TEST_CASE("WifiBackend's base forget_network default is a failure, not a silent success",
          "[wifi][forget]") {
    MinimalStubBackend backend;
    WiFiError result = backend.forget_network("AnySSID");

    // A silent success here would let the UI tell the user a network was
    // forgotten when the base class did nothing at all — set_radio_enabled()
    // gets to be a no-op default because an unreachable toggle is harmless;
    // a fake "forgotten" is not.
    //
    // NOT_SUPPORTED rather than BACKEND_ERROR: still a failure to the caller,
    // but a distinguishable one. WiFiManager::forget() branches on it to say
    // "this platform manages saved networks itself" instead of "Failed to
    // forget WiFi network 'X'", which blamed the user for a capability the
    // platform never had.
    CHECK_FALSE(result.success());
    CHECK(result.result == WiFiResult::NOT_SUPPORTED);
}

TEST_CASE("forget_network on the mock backend removes a connected SSID", "[wifi][forget]") {
    ConfigDirGuard guard(make_temp_dir("helix_forget_mock_connected"));

    WifiBackendMock backend;
    REQUIRE(backend.start().success());

    REQUIRE(backend.connect_network("HomeNetwork-5G", "12345678").success());

    WiFiError forgotten = backend.forget_network("HomeNetwork-5G");
    CHECK(forgotten.success());

    // The SSID is gone: forgetting it again finds nothing left to remove.
    WiFiError second = backend.forget_network("HomeNetwork-5G");
    CHECK_FALSE(second.success());
    CHECK(second.result == WiFiResult::NETWORK_NOT_FOUND);
}

TEST_CASE("forget_network on the currently-connected SSID fires DISCONNECTED", "[wifi][forget]") {
    // connect_network() only starts the simulated async connect — it does not
    // by itself put the mock into connected_ == true, so a test that never
    // waits out that delay (as the case above deliberately doesn't) can only
    // prove removal from saved_networks_. set_connected_state() drives the
    // mock into a genuinely connected state directly, so this test actually
    // reaches the DISCONNECTED branch in WifiBackendMock::forget_network()
    // instead of merely exercising the "not connected" path by accident.
    ConfigDirGuard guard(make_temp_dir("helix_forget_mock_disconnect_event"));

    WifiBackendMock backend;
    REQUIRE(backend.start().success());
    REQUIRE(backend.connect_network("HomeNetwork-5G", "12345678").success());
    backend.set_connected_state(true, "HomeNetwork-5G", "192.168.1.50", 80);

    bool disconnected_fired = false;
    std::string disconnect_reason;
    backend.register_event_callback("DISCONNECTED", [&](const std::string& data) {
        disconnected_fired = true;
        disconnect_reason = data;
    });

    WiFiError forgotten = backend.forget_network("HomeNetwork-5G");
    CHECK(forgotten.success());

    CHECK(disconnected_fired);
    CHECK(disconnect_reason == "reason=forgotten");

    WifiBackend::ConnectionStatus status = backend.get_status();
    CHECK_FALSE(status.connected);
}

TEST_CASE("forget_network on an unknown SSID returns NETWORK_NOT_FOUND", "[wifi][forget]") {
    ConfigDirGuard guard(make_temp_dir("helix_forget_mock_unknown"));

    WifiBackendMock backend;
    REQUIRE(backend.start().success());

    WiFiError result = backend.forget_network("NeverConnectedToThis");
    CHECK_FALSE(result.success());
    CHECK(result.result == WiFiResult::NETWORK_NOT_FOUND);
}

TEST_CASE("forget_network removes the SSID from HelixScreen's own credential store",
          "[wifi][forget][store]") {
    // The critical cross-task requirement: Task 11's reconcile_saved_networks()
    // re-adds anything left in helix::wifi::store at every backend init, so a
    // forget that only forgets the backend's own idea of "saved" would be
    // silently resurrected on the next boot. Prove the store file itself
    // loses the entry — not merely that forget_network() reports success.
    ConfigDirGuard guard(make_temp_dir("helix_forget_store"));

    REQUIRE(helix::wifi::store::save({"StoredOnlyNet", "somepassword"}));
    REQUIRE(helix::wifi::store::load().size() == 1);

    WifiBackendMock backend;
    REQUIRE(backend.start().success());

    // Never connected through this backend instance — the store is the ONLY
    // place this credential exists, exactly the Adventurer 5M scenario
    // wifi_saved_config.h documents (SAVE_CONFIG replied OK but never wrote
    // the vendor config).
    WiFiError result = backend.forget_network("StoredOnlyNet");
    CHECK(result.success());

    auto remaining = helix::wifi::store::load();
    CHECK(remaining.empty());
}

TEST_CASE("forget_network on a stopped mock backend reports not-initialized", "[wifi][forget]") {
    ConfigDirGuard guard(make_temp_dir("helix_forget_not_running"));

    WifiBackendMock backend; // never started
    WiFiError result = backend.forget_network("AnySSID");
    CHECK_FALSE(result.success());
    CHECK(result.result == WiFiResult::NOT_INITIALIZED);
}
