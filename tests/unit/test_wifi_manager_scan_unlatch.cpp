// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_manager_scan_unlatch.cpp
 * @brief Regression test for helixscreen#1405 — a backend swap mid-scan must
 *        unlatch the scan scheduler.
 *
 * WiFiManager's NM->wpa_supplicant fallback swaps the backend object when the
 * NetworkManager backend reports INIT_FAILED. A scan that was outstanding on
 * the old backend can never receive its SCAN_COMPLETE after the swap, and
 * ScanScheduler latches on "a scan is outstanding", so should_trigger() stays
 * false for the rest of the scan session — periodic scanning silently dies on
 * the network settings page.
 *
 * The manager owns the latch, so the manager resolves it when it stops or
 * swaps the backend; a backend that merely re-establishes its own connection
 * mid-scan resolves the obligation itself (the contract stated on
 * WifiBackend::trigger_scan, include/wifi_backend.h).
 */

#include "ui_update_queue.h"

#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wifi_manager_test_access.h"
#include "../ui_test_utils.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// The NM->wpa fallback only exists on the Linux desktop path; other platforms
// compile the swap out entirely (wifi_manager.cpp guards it the same way).
#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)

namespace {

// Test mode + headless LVGL + UpdateQueue for the duration of the test — same
// shape as test_wifi_manager_auth_debounce.cpp's WifiDebounceFixture.
struct ScanUnlatchFixture {
    ScopedRuntimeConfig scoped_config;

    ScanUnlatchFixture() {
        auto* rc = get_runtime_config();
        rc->test_mode = true;      // should_mock_wifi() -> true
        rc->use_real_wifi = false; // pick the idle mock backend
        lv_init_safe();
        ensure_headless_display();
        helix::ui::UpdateQueue::instance().init();
    }

    ~ScanUnlatchFixture() {
        helix::ui::UpdateQueue::instance().drain();
    }
};

// A backend that ACCEPTS a scan whose SCAN_COMPLETE never arrives — exactly
// the state a mid-scan backend swap strands. trigger_scan() is overridden
// rather than reusing the mock's simulated 2s scan, which would complete on
// its own and unlatch before the swap lands. The nm flag decides whether
// INIT_FAILED takes the fallback path (only the NetworkManager backend has
// that fallback).
class StalledScanBackend : public WifiBackendMock {
  public:
    explicit StalledScanBackend(bool nm) : nm_(nm) {}

    bool is_network_manager() const override {
        return nm_;
    }

    WiFiError trigger_scan() override {
        return WiFiErrorHelper::success(); // accepted; completion never comes
    }

  private:
    bool nm_;
};

} // namespace

TEST_CASE("NM->wpa fallback swap unlatches an outstanding scan", "[wifi][1405]") {
    ScanUnlatchFixture fx;
    auto wm = std::make_shared<WiFiManager>(std::make_unique<StalledScanBackend>(/*nm=*/true),
                                            /*silent=*/true);
    wm->init_self_reference(wm);

    // Scan session live with one scan outstanding on the NM backend.
    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE_FALSE(WiFiManagerTestAccess::scan_should_trigger(*wm)); // the latch is set

    // The NM daemon turns out to be dead: INIT_FAILED schedules the deferred
    // backend swap.
    WiFiManagerTestAccess::fire_init_failed(*wm, /*silent=*/true, "nmcli present but NM masked");
    lv_timer_handler_safe(); // drain queue -> swap runs (stop old, wpa, start)

    // The swapped-out backend can never deliver the SCAN_COMPLETE it owed, so
    // the manager must have resolved the latch — otherwise periodic scanning
    // is dead for the rest of this session (prestonbrown/helixscreen#1405).
    REQUIRE(WiFiManagerTestAccess::scan_should_trigger(*wm));

    wm->stop_scan();
}

TEST_CASE("INIT_FAILED without a backend swap leaves the outstanding scan alone", "[wifi][1405]") {
    ScanUnlatchFixture fx;
    // Not the NM backend: INIT_FAILED is terminal (a log line in silent mode),
    // not a swap. The backend object stays alive and still owes the completion.
    auto wm = std::make_shared<WiFiManager>(std::make_unique<StalledScanBackend>(/*nm=*/false),
                                            /*silent=*/true);
    wm->init_self_reference(wm);

    wm->start_scan([](const std::vector<WiFiNetwork>&) {});
    REQUIRE_FALSE(WiFiManagerTestAccess::scan_should_trigger(*wm)); // the latch is set

    WiFiManagerTestAccess::fire_init_failed(*wm, /*silent=*/true, "init failed");
    lv_timer_handler_safe();

    // No backend was stopped, so the manager must NOT resolve the latch on
    // INIT_FAILED alone — the obligation still belongs to a live backend.
    REQUIRE_FALSE(WiFiManagerTestAccess::scan_should_trigger(*wm));

    wm->stop_scan();
}

#endif // !__APPLE__ && !__ANDROID__ && !ESP_PLATFORM
