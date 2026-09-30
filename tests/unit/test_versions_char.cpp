// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_versions_char.cpp
 * @brief Characterization tests for PrinterState version subjects
 *
 * These tests capture the CURRENT behavior of version-related subjects in PrinterState.
 *
 * Version subjects (2 subjects):
 * - klipper_version_ (string, 64-byte buffer) - Klipper firmware version
 * - moonraker_version_ (string, 64-byte buffer) - Moonraker service version
 *
 * Default values:
 * - klipper_version_: "—" (em dash)
 * - moonraker_version_: "—" (em dash)
 *
 * XML registration names:
 * - "klipper_version"
 * - "moonraker_version"
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
// ============================================================================
// Initialization Tests - Document default initialization behavior
// ============================================================================

TEST_CASE("Versions characterization: klipper_version initializes to em dash",
          "[characterization][versions][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false); // Skip XML registration

    const char* version = lv_subject_get_string(state.get_klipper_version_subject());
    REQUIRE(version != nullptr);
    REQUIRE(std::string(version) == "—");
}

TEST_CASE("Versions characterization: moonraker_version initializes to em dash",
          "[characterization][versions][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    const char* version = lv_subject_get_string(state.get_moonraker_version_subject());
    REQUIRE(version != nullptr);
    REQUIRE(std::string(version) == "—");
}

// ============================================================================
// Version Subject Update Tests - Verify subject updates work correctly
// ============================================================================

TEST_CASE("Versions characterization: klipper_version subject accepts string updates",
          "[characterization][versions][setter]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* subject = state.get_klipper_version_subject();

    SECTION("typical version string") {
        lv_subject_copy_string(subject, "v0.12.0-108-g2c7a9d58");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "v0.12.0-108-g2c7a9d58");
    }

    SECTION("simple version string") {
        lv_subject_copy_string(subject, "v0.12.0");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "v0.12.0");
    }

    SECTION("empty version string") {
        lv_subject_copy_string(subject, "");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "");
    }
}

TEST_CASE("Versions characterization: moonraker_version subject accepts string updates",
          "[characterization][versions][setter]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* subject = state.get_moonraker_version_subject();

    SECTION("typical version string") {
        lv_subject_copy_string(subject, "v0.8.0-143-g2c7a9d58");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "v0.8.0-143-g2c7a9d58");
    }

    SECTION("simple version string") {
        lv_subject_copy_string(subject, "v0.8.0");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "v0.8.0");
    }

    SECTION("empty version string") {
        lv_subject_copy_string(subject, "");

        const char* version = lv_subject_get_string(subject);
        REQUIRE(std::string(version) == "");
    }
}

// ============================================================================
// Version Subject Independence Tests
// ============================================================================

TEST_CASE("Versions characterization: version subjects are independent",
          "[characterization][versions][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* klipper = state.get_klipper_version_subject();
    lv_subject_t* moonraker = state.get_moonraker_version_subject();

    SECTION("changing klipper_version does not affect moonraker_version") {
        lv_subject_copy_string(moonraker, "v0.8.0");
        lv_subject_copy_string(klipper, "v0.12.0");

        REQUIRE(std::string(lv_subject_get_string(moonraker)) == "v0.8.0");
        REQUIRE(std::string(lv_subject_get_string(klipper)) == "v0.12.0");
    }

    SECTION("changing moonraker_version does not affect klipper_version") {
        lv_subject_copy_string(klipper, "v0.12.0");
        lv_subject_copy_string(moonraker, "v0.8.0");

        REQUIRE(std::string(lv_subject_get_string(klipper)) == "v0.12.0");
        REQUIRE(std::string(lv_subject_get_string(moonraker)) == "v0.8.0");
    }
}

// ============================================================================
// Reset Cycle Tests - Verify subjects reset to default values
// ============================================================================

TEST_CASE("Versions characterization: versions reset to em dash after reset cycle",
          "[characterization][versions][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* klipper = state.get_klipper_version_subject();
    lv_subject_t* moonraker = state.get_moonraker_version_subject();

    // Set version values
    lv_subject_copy_string(klipper, "v0.12.0-108-g2c7a9d58");
    lv_subject_copy_string(moonraker, "v0.8.0-143-g2c7a9d58");

    // Verify values were set
    REQUIRE(std::string(lv_subject_get_string(klipper)) == "v0.12.0-108-g2c7a9d58");
    REQUIRE(std::string(lv_subject_get_string(moonraker)) == "v0.8.0-143-g2c7a9d58");

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // After reset, values should be back to default em dash
    REQUIRE(std::string(lv_subject_get_string(state.get_klipper_version_subject())) == "—");
    REQUIRE(std::string(lv_subject_get_string(state.get_moonraker_version_subject())) == "—");
}

TEST_CASE("Versions characterization: subjects are functional after reset cycle",
          "[characterization][versions][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* klipper = state.get_klipper_version_subject();
    lv_subject_t* moonraker = state.get_moonraker_version_subject();

    // Set initial values
    lv_subject_copy_string(klipper, "v0.11.0");
    lv_subject_copy_string(moonraker, "v0.7.0");

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Get new subject pointers after reset
    klipper = state.get_klipper_version_subject();
    moonraker = state.get_moonraker_version_subject();

    // Set new values - should work
    lv_subject_copy_string(klipper, "v0.12.0");
    lv_subject_copy_string(moonraker, "v0.8.0");

    REQUIRE(std::string(lv_subject_get_string(klipper)) == "v0.12.0");
    REQUIRE(std::string(lv_subject_get_string(moonraker)) == "v0.8.0");
}

// ============================================================================
// Placeholder Normalization Tests
//
// Some Moonraker forks (QIDI's on the Q2 / Max 4) report a literal "?" instead
// of a real version, and the discovery parse layer substitutes "unknown" for a
// missing key. Neither is meaningful to display, so the public setters collapse
// them (and empty/whitespace) to a translated "Unknown". A real version passes
// through untouched. Regression coverage for prestonbrown/helixscreen#1068.
// ============================================================================

TEST_CASE("Versions: placeholder version strings normalize to Unknown",
          "[versions][setter][normalization]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto klipper = [&] {
        return std::string(lv_subject_get_string(state.get_klipper_version_subject()));
    };
    auto moonraker = [&] {
        return std::string(lv_subject_get_string(state.get_moonraker_version_subject()));
    };

    SECTION("QIDI '?' sentinel becomes Unknown") {
        state.set_klipper_version("?");
        state.set_moonraker_version("?");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "Unknown");
        REQUIRE(moonraker() == "Unknown");
    }

    SECTION("parse-layer 'unknown' fallback becomes Unknown (case-insensitive)") {
        state.set_klipper_version("unknown");
        state.set_moonraker_version("UNKNOWN");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "Unknown");
        REQUIRE(moonraker() == "Unknown");
    }

    SECTION("empty and whitespace-only become Unknown") {
        state.set_klipper_version("");
        state.set_moonraker_version("   ");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "Unknown");
        REQUIRE(moonraker() == "Unknown");
    }

    SECTION("a real version passes through untouched") {
        state.set_klipper_version("v0.12.0-108-g2c7a9d58");
        state.set_moonraker_version("v0.8.0");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "v0.12.0-108-g2c7a9d58");
        REQUIRE(moonraker() == "v0.8.0");
    }

    SECTION("surrounding whitespace is trimmed from a real version") {
        state.set_klipper_version("  v0.12.0  ");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "v0.12.0");
    }

    // The raw string is what the debug bundle ships: it must show the exact
    // sentinel the host sent, not the localized label the subject collapses
    // it to, so a "?" fork is distinguishable from a missing field.
    SECTION("raw accessor retains what the host sent, sentinels included") {
        REQUIRE(state.get_klipper_version_raw().empty()); // nothing set yet this section

        state.set_klipper_version("?");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(klipper() == "Unknown");
        REQUIRE(state.get_klipper_version_raw() == "?");

        state.set_klipper_version("  v0.12.0  ");
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        REQUIRE(state.get_klipper_version_raw() == "  v0.12.0  ");
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on state changes
// ============================================================================

TEST_CASE("Versions characterization: observer fires when klipper_version changes",
          "[characterization][versions][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* subject = state.get_klipper_version_subject();

    int callback_count = 0;
    std::string last_value;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        auto* data =
            static_cast<std::pair<int*, std::string*>*>(lv_observer_get_user_data(observer));
        (*data->first)++;
        *data->second = lv_subject_get_string(subject);
    };

    std::pair<int*, std::string*> user_data = {&callback_count, &last_value};

    lv_observer_t* observer = lv_subject_add_observer(subject, observer_cb, &user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(callback_count == 1);
    REQUIRE(last_value == "—");

    // Update version
    lv_subject_copy_string(subject, "v0.12.0");

    REQUIRE(callback_count == 2);
    REQUIRE(last_value == "v0.12.0");

    lv_observer_remove(observer);
}

TEST_CASE("Versions characterization: observer fires when moonraker_version changes",
          "[characterization][versions][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* subject = state.get_moonraker_version_subject();

    int callback_count = 0;
    std::string last_value;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        auto* data =
            static_cast<std::pair<int*, std::string*>*>(lv_observer_get_user_data(observer));
        (*data->first)++;
        *data->second = lv_subject_get_string(subject);
    };

    std::pair<int*, std::string*> user_data = {&callback_count, &last_value};

    lv_observer_t* observer = lv_subject_add_observer(subject, observer_cb, &user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(callback_count == 1);
    REQUIRE(last_value == "—");

    // Update version
    lv_subject_copy_string(subject, "v0.8.0");

    REQUIRE(callback_count == 2);
    REQUIRE(last_value == "v0.8.0");

    lv_observer_remove(observer);
}

TEST_CASE("Versions characterization: observers on different version subjects are independent",
          "[characterization][versions][observer][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    lv_subject_t* klipper = state.get_klipper_version_subject();
    lv_subject_t* moonraker = state.get_moonraker_version_subject();

    int klipper_count = 0;
    int moonraker_count = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* klipper_observer = lv_subject_add_observer(klipper, observer_cb, &klipper_count);
    lv_observer_t* moonraker_observer =
        lv_subject_add_observer(moonraker, observer_cb, &moonraker_count);

    // Both observers fire on initial add
    REQUIRE(klipper_count == 1);
    REQUIRE(moonraker_count == 1);

    // Update only klipper version
    lv_subject_copy_string(klipper, "v0.12.0");

    // Only klipper observer should fire
    REQUIRE(klipper_count == 2);
    REQUIRE(moonraker_count == 1);

    // Update only moonraker version
    lv_subject_copy_string(moonraker, "v0.8.0");

    // Only moonraker observer should fire
    REQUIRE(klipper_count == 2);
    REQUIRE(moonraker_count == 2);

    lv_observer_remove(klipper_observer);
    lv_observer_remove(moonraker_observer);
}
