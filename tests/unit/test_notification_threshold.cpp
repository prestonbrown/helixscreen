// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_notification_threshold.cpp
 * @brief Minimum toast severity threshold (prestonbrown/helixscreen#1213).
 *
 * Every NOTIFY_* call becomes a toast today. A user setting now gates which
 * severities interrupt: the rest still go to history (nothing is lost), only
 * the toast is suppressed. The ladder is three rungs:
 *
 *   index 0  "All"               threshold INFO    everything shows
 *   index 1  "Warnings & errors" threshold WARNING INFO and SUCCESS suppressed
 *   index 2  "Errors only"       threshold ERROR   only ERROR shows
 *
 * SUCCESS is treated as info-tier: it sits between INFO and WARNING in the enum
 * (INFO=0, SUCCESS=1, WARNING=2, ERROR=3), so the "Warnings" rung suppresses it
 * too. This is deliberate - the SUCCESS toasts people complain about ("Klipper
 * is ready" after they flip the power switch) are exactly the interruption this
 * setting exists to quiet, and print-completion already has its own setting
 * (`completion_alert`).
 *
 * These tests cover the pure mapping. The wiring (atomic cache, toast-site
 * gates, settings subject) lives in ui_notification.cpp / SafetySettingsManager.
 */

#include "ui_toast_manager.h"

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "core/notification_threshold.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// Severity ints mirror the ToastSeverity enum ordering
// (INFO=0, SUCCESS=1, WARNING=2, ERROR=3) - see ui_toast_manager.h.
namespace {
constexpr int INFO = 0;
constexpr int SUCCESS = 1;
constexpr int WARNING = 2;
constexpr int kErrorSev = 3;
} // namespace

TEST_CASE("setting index maps to a minimum severity threshold", "[notifications][1213]") {
    using helix::ui::notifications::min_toast_severity_for_index;
    CHECK(min_toast_severity_for_index(0) == INFO);      // "All"
    CHECK(min_toast_severity_for_index(1) == WARNING);   // "Warnings & errors"
    CHECK(min_toast_severity_for_index(2) == kErrorSev); // "Errors only"
}

TEST_CASE("an out-of-range setting index never silently suppresses errors",
          "[notifications][1213]") {
    using helix::ui::notifications::min_toast_severity_for_index;
    // Defensive: a corrupt or unknown value falls back to "All" (INFO), since
    // silently swallowing an error toast is worse than showing one too many.
    CHECK(min_toast_severity_for_index(-1) == INFO);
    CHECK(min_toast_severity_for_index(3) == INFO);
    CHECK(min_toast_severity_for_index(99) == INFO);
}

TEST_CASE("'All' shows every severity", "[notifications][1213]") {
    using helix::ui::notifications::severity_meets_threshold;
    for (int sev : {INFO, SUCCESS, WARNING, kErrorSev}) {
        CHECK(severity_meets_threshold(sev, 0));
    }
}

TEST_CASE("'Warnings & errors' suppresses INFO and SUCCESS (info-tier)", "[notifications][1213]") {
    using helix::ui::notifications::severity_meets_threshold;
    CHECK_FALSE(severity_meets_threshold(INFO, 1));
    CHECK_FALSE(severity_meets_threshold(SUCCESS, 1)); // info-tier, suppressed
    CHECK(severity_meets_threshold(WARNING, 1));
    CHECK(severity_meets_threshold(kErrorSev, 1));
}

TEST_CASE("'Errors only' shows only ERROR", "[notifications][1213]") {
    using helix::ui::notifications::severity_meets_threshold;
    CHECK_FALSE(severity_meets_threshold(INFO, 2));
    CHECK_FALSE(severity_meets_threshold(SUCCESS, 2));
    CHECK_FALSE(severity_meets_threshold(WARNING, 2));
    CHECK(severity_meets_threshold(kErrorSev, 2));
}

TEST_CASE_METHOD(HelixTestFixture, "transient success toast honours init and the minimum severity",
                 "[notifications][1213]") {
    using helix::ui::notifications::set_min_toast_severity_cache;
    using helix::ui::notifications::show_transient_success;

    std::vector<std::pair<ToastSeverity, std::string>> shown;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity sev, const std::string& msg, uint32_t) { shown.push_back({sev, msg}); });
    auto& toasts = ToastManager::instance();
    const bool was_initialized = toasts.is_initialized();

    SECTION("All shows it as a success toast") {
        toasts.init();
        set_min_toast_severity_cache(0);
        show_transient_success("Connection restored", 3000);
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].first == ToastSeverity::SUCCESS);
        CHECK(shown[0].second == "Connection restored");
    }
    SECTION("Warnings and up suppresses it") {
        toasts.init();
        for (int index : {1, 2}) {
            set_min_toast_severity_cache(index);
            show_transient_success("Connection restored", 3000);
        }
        CHECK(shown.empty());
    }
    SECTION("Before ToastManager init it does nothing") {
        toasts.deinit_subjects();
        set_min_toast_severity_cache(0);
        show_transient_success("Connection restored", 3000);
        CHECK(shown.empty());
    }

    set_min_toast_severity_cache(0);
    if (was_initialized) {
        toasts.init();
    } else {
        toasts.deinit_subjects();
    }
    helix::ui::set_test_toast_hook(nullptr);
}
