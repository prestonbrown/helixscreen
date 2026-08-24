// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_os_link_fallback.cpp
 * @brief Tests for the OS-level wireless link probe and its use as a fallback
 *        when the managed backend can't reach its control socket
 *        (helixscreen#1059, Qidi Q2 boots already connected).
 *
 * Two surfaces are covered:
 *   1. probe_os_wifi_link() parsing against a fixture sysfs/proc tree — link up,
 *      link down, no wireless iface, eth0-only.
 *   2. WiFiManager behavior: when the OS link is up but the managed backend
 *      fails, the scan-failed path must NOT emit a user-facing warning.
 */

#include "ui_update_queue.h"

#include "../ui_test_utils.h"
#include "runtime_config.h"
#include "wifi_manager.h"
#include "wifi_ui_utils.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace fs = std::filesystem;

namespace helix {
// Friend accessor — injects a stub OS-link probe and drives start_scan()
// without standing up a threaded backend, so the suppression decision is
// deterministic.
class WiFiManagerTestAccess {
  public:
    static void set_os_link_probe(std::function<bool()> probe) {
        WiFiManager::os_link_probe_ = std::move(probe);
    }
    static void reset_os_link_probe() {
        WiFiManager::os_link_probe_ = []() {
            return helix::ui::wifi::probe_os_wifi_link().has_link;
        };
    }
    static bool os_link_up() {
        return WiFiManager::os_link_up();
    }
    // Stops the backend directly, bypassing set_enabled()'s radio-only
    // semantics (helixscreen wifi-interface-identity task 6: set_enabled(false)
    // now disables the radio and keeps the backend/control-connection alive,
    // so it no longer forces trigger_scan() into a NOT_INITIALIZED failure).
    // Tests that need that specific failure path reach in here instead.
    static void stop_backend(WiFiManager& wm) {
        if (wm.backend_) {
            wm.backend_->stop();
        }
    }
    // Backdate the association stamp past ASSOCIATION_GRACE so the expiry side
    // of the suppression is testable without a 5-second sleep.
    static void expire_association_grace(WiFiManager& wm) {
        wm.last_association_change_ = std::chrono::steady_clock::now() -
                                      WiFiManager::ASSOCIATION_GRACE - std::chrono::seconds(1);
    }
    static bool in_association_grace(const WiFiManager& wm) {
        return wm.in_association_grace();
    }
};
} // namespace helix

namespace {

// Builds a throwaway sysfs/proc fixture tree under a unique temp dir and cleans
// it up on destruction. Mirrors the layout probe_os_wifi_link() reads.
struct SysfsFixture {
    fs::path root;

    SysfsFixture() {
        root = fs::temp_directory_path() /
               fs::path("helix_wifi_probe_" + std::to_string(::getpid()) + "_" +
                        std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(sys() / "class" / "net");
        fs::create_directories(proc() / "net");
    }
    ~SysfsFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path sys() const {
        return root / "sys";
    }
    fs::path proc() const {
        return root / "proc";
    }

    fs::path iface_dir(const std::string& iface) const {
        return sys() / "class" / "net" / iface;
    }

    void write_file(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << contents;
    }

    // Create a wireless iface (with a wireless/ subdir) and its link state.
    void make_wireless(const std::string& iface, const std::string& operstate,
                       const std::string& carrier) {
        fs::create_directories(iface_dir(iface) / "wireless");
        write_file(iface_dir(iface) / "operstate", operstate);
        write_file(iface_dir(iface) / "carrier", carrier);
    }

    // Create a wired iface (no wireless/ or phy80211 subdir).
    void make_wired(const std::string& iface, const std::string& operstate) {
        fs::create_directories(iface_dir(iface));
        write_file(iface_dir(iface) / "operstate", operstate);
        write_file(iface_dir(iface) / "carrier", operstate == "up" ? "1" : "0");
    }

    helix::ui::wifi::OsWifiLink probe() {
        return helix::ui::wifi::probe_os_wifi_link(sys().string(), proc().string());
    }
};

} // namespace

// ============================================================================
// Probe parser tests
// ============================================================================

TEST_CASE("probe_os_wifi_link: wireless iface with operstate up reports has_link",
          "[wifi][unit][1059]") {
    SysfsFixture fx;
    fx.make_wireless("wlan0", "up", "1");

    auto link = fx.probe();
    REQUIRE(link.has_link);
    REQUIRE(link.iface == "wlan0");
}

TEST_CASE("probe_os_wifi_link: carrier 1 alone (operstate unknown) reports has_link",
          "[wifi][unit][1059]") {
    SysfsFixture fx;
    fx.make_wireless("wlan0", "unknown", "1");

    auto link = fx.probe();
    REQUIRE(link.has_link);
    REQUIRE(link.iface == "wlan0");
}

TEST_CASE("probe_os_wifi_link: wireless iface down reports no link", "[wifi][unit][1059]") {
    SysfsFixture fx;
    fx.make_wireless("wlan0", "down", "0");

    auto link = fx.probe();
    REQUIRE_FALSE(link.has_link);
    // The iface is still discovered as wireless even though it's down.
    REQUIRE(link.iface == "wlan0");
}

TEST_CASE("probe_os_wifi_link: no wireless iface present reports no link", "[wifi][unit][1059]") {
    SysfsFixture fx;
    // Only loopback-like dir, nothing wireless.
    fx.write_file(fx.iface_dir("lo") / "operstate", "unknown");

    auto link = fx.probe();
    REQUIRE_FALSE(link.has_link);
    REQUIRE(link.iface.empty());
}

TEST_CASE("probe_os_wifi_link: eth0-only is not detected as wifi even when up",
          "[wifi][unit][1059]") {
    SysfsFixture fx;
    fx.make_wired("eth0", "up");

    auto link = fx.probe();
    REQUIRE_FALSE(link.has_link);
    REQUIRE(link.iface.empty());
}

TEST_CASE("probe_os_wifi_link: phy80211 subdir marks an iface wireless", "[wifi][unit][1059]") {
    SysfsFixture fx;
    fs::create_directories(fx.iface_dir("wlp2s0") / "phy80211");
    fx.write_file(fx.iface_dir("wlp2s0") / "operstate", "up");
    fx.write_file(fx.iface_dir("wlp2s0") / "carrier", "1");

    auto link = fx.probe();
    REQUIRE(link.has_link);
    REQUIRE(link.iface == "wlp2s0");
}

TEST_CASE(
    "probe_os_wifi_link: /proc/net/wireless name marks iface wireless when sysfs lacks subdir",
    "[wifi][unit][1059]") {
    SysfsFixture fx;
    // sysfs node without wireless/ or phy80211 — but listed in /proc/net/wireless.
    fx.write_file(fx.iface_dir("wlan0") / "operstate", "up");
    fx.write_file(fx.iface_dir("wlan0") / "carrier", "1");
    fx.write_file(fx.proc() / "net" / "wireless",
                  "Inter-| sta-|   Quality        |   Discarded packets\n"
                  " face | tus | link level noise |  nwid  crypt   frag\n"
                  " wlan0: 0000   54.  -60.  -256        0      0      0\n");

    auto link = fx.probe();
    REQUIRE(link.has_link);
    REQUIRE(link.iface == "wlan0");
}

// ============================================================================
// Behavior test: suppression of false warnings when the OS link is up
// ============================================================================

namespace {

// Test-mode mock WiFi backend + headless display, mirroring the auth-debounce
// fixture so start_scan() exercises the real WiFiManager code path.
struct OsLinkBehaviorFixture {
    RuntimeConfig* rc;
    bool prev_test_mode;
    bool prev_use_real_wifi;

    OsLinkBehaviorFixture()
        : rc(get_runtime_config()), prev_test_mode(rc->test_mode),
          prev_use_real_wifi(rc->use_real_wifi) {
        rc->test_mode = true;
        rc->use_real_wifi = false;
        lv_init_safe();
        ensure_display();
        helix::ui::UpdateQueue::instance().init();
    }

    ~OsLinkBehaviorFixture() {
        // A WiFiManager built by make_manager() fires READY synchronously
        // (mock backend), which now always queues a
        // WiFiManager::reassert_stored_radio_state closure via
        // async_lifetime_.defer(). Drain here — before UpdateQueue's state
        // outlives this fixture — so it can't leak into the next test.
        // See scripts/check_update_queue_leaks.py.
        helix::ui::UpdateQueue::instance().drain();
        rc->test_mode = prev_test_mode;
        rc->use_real_wifi = prev_use_real_wifi;
        helix::WiFiManagerTestAccess::reset_os_link_probe();
    }

    static void ensure_display() {
        static bool created = false;
        if (created) {
            return;
        }
        auto* disp = lv_display_create(480, 320);
        alignas(64) static lv_color_t buf[480 * 10];
        lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(
            disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
        created = true;
    }

    std::shared_ptr<WiFiManager> make_manager() {
        auto wm = std::make_shared<WiFiManager>(/*silent=*/true);
        wm->init_self_reference(wm);
        return wm;
    }
};

} // namespace

TEST_CASE("WiFiManager::os_link_up reflects injected probe", "[wifi][unit][1059]") {
    OsLinkBehaviorFixture fx;

    helix::WiFiManagerTestAccess::set_os_link_probe([]() { return true; });
    REQUIRE(helix::WiFiManagerTestAccess::os_link_up());

    helix::WiFiManagerTestAccess::set_os_link_probe([]() { return false; });
    REQUIRE_FALSE(helix::WiFiManagerTestAccess::os_link_up());
}

TEST_CASE("start_scan with OS link up suppresses the scan-failed user warning",
          "[wifi][unit][1059]") {
    OsLinkBehaviorFixture fx;
    auto wm = fx.make_manager();

    // Stop the backend so trigger_scan() fails synchronously with
    // NOT_INITIALIZED — exactly the boot-race path that emits "WiFi scan
    // failed. Try again." (the constructor's start_async() leaves the mock
    // running, which would otherwise let the scan succeed). set_enabled(false)
    // no longer does this itself — it now only toggles the radio and leaves
    // the backend/control-connection running (helixscreen wifi-interface-identity
    // task 6) — so reach past it directly for this specific failure path.
    helix::WiFiManagerTestAccess::stop_backend(*wm);
    REQUIRE_FALSE(wm->is_enabled());

    // With the OS link reported up, the scan-failed warning MUST be suppressed.
    int warnings = 0;
    helix::ui::set_test_notification_warning_hook([&](const std::string&) { warnings++; });

    helix::WiFiManagerTestAccess::set_os_link_probe([]() { return true; });
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(warnings == 0);

    // Sanity: with the OS link DOWN, the same failing path DOES warn. This is
    // what makes the test fail if the suppression feature is removed.
    warnings = 0;
    helix::WiFiManagerTestAccess::set_os_link_probe([]() { return false; });
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(warnings >= 1);

    helix::ui::set_test_notification_warning_hook(nullptr);
    wm->stop_scan();
}

TEST_CASE("A scan that fails right after our own forget does not warn the user",
          "[wifi][unit][ad5x]") {
    // AD5X bundle TAU4PW4H: the user taps Forget on the CONNECTED network, that
    // disassociates, the overlay restarts scanning immediately, the trigger
    // fails while the link is mid-teardown — and because the link genuinely is
    // down at that instant, the os_link_up() suppression above does not apply.
    // The user got "WiFi scan failed. Try again." 48 ms after their own tap, for
    // something the app did.
    OsLinkBehaviorFixture fx;
    auto wm = fx.make_manager();
    helix::WiFiManagerTestAccess::stop_backend(*wm);
    helix::WiFiManagerTestAccess::set_os_link_probe([]() { return false; });

    int warnings = 0;
    helix::ui::set_test_notification_warning_hook([&](const std::string&) { warnings++; });

    // Go through the real entry point, so the test covers the stamp being taken
    // and not just the predicate.
    wm->forget("SomeNetwork", nullptr);
    REQUIRE(helix::WiFiManagerTestAccess::in_association_grace(*wm));

    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(warnings == 0);

    // Sanity / mutation guard: once the grace has passed, the same failing scan
    // with the same down link DOES warn. Without this the test would also pass
    // if suppression became unconditional.
    warnings = 0;
    helix::WiFiManagerTestAccess::expire_association_grace(*wm);
    REQUIRE_FALSE(helix::WiFiManagerTestAccess::in_association_grace(*wm));
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE(warnings >= 1);

    helix::ui::set_test_notification_warning_hook(nullptr);
    wm->stop_scan();
    // forget()/start_scan() on a stopped backend queue notification work through
    // UpdateQueue; drain before the manager goes out of scope so nothing leaks
    // into the next test (scripts/check_update_queue_leaks.py).
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("The association grace is not armed before any association change",
          "[wifi][unit][ad5x]") {
    // A fresh manager must not start life suppressing scan failures — the boot
    // -race warning path (#1059's other half) still has to reach the user.
    OsLinkBehaviorFixture fx;
    auto wm = fx.make_manager();
    REQUIRE_FALSE(helix::WiFiManagerTestAccess::in_association_grace(*wm));
}
