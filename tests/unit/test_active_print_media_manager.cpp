// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_media_manager.cpp
 * @brief Unit tests for ActivePrintMediaManager class
 *
 * Tests the media manager that:
 * - Observes print_filename_ subject from PrinterState
 * - Processes raw filename to display name
 * - Loads thumbnails via MoonrakerAPI
 * - Updates print_display_filename_ and print_thumbnail_path_ subjects
 * - Uses generation counter for stale callback detection
 */

#include "../test_helpers/active_print_media_manager_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "active_print_media_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"
#include "printer_state.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using namespace helix::ui;

// What the shared subject carries when the current file has no thumbnail. It is
// NEVER the empty string: ActivePrintMediaManager publishes an explicit
// placeholder so no consumer has to carry its own empty-path branch (a "" src is
// classified LV_IMAGE_SRC_VARIABLE by LVGL and dereferenced as a descriptor).
//
// This namespace-scope initializer is also LOAD-BEARING for the whole binary:
// no_thumbnail_placeholder() caches its resolution in a function-local static
// on first call, and this runs before any test body — so tests that temporarily
// set_asset_root() (test_asset_root.cpp) can never be the first caller and pin
// a non-default root into the cache. Do not inline it into the fixture.
static const std::string NO_THUMB = helix::ActivePrintMediaManager::no_thumbnail_placeholder();

// ============================================================================
// Test Fixture for ActivePrintMediaManager tests
// ============================================================================

class ActivePrintMediaManagerTestFixture {
  public:
    ActivePrintMediaManagerTestFixture() {
        // Default spdlog output is suppressed process-wide by the
        // IsolationListener at testRunStarting (single-threaded, before the
        // HttpExecutor workers start) — swapping the default logger per test
        // here raced those workers' logging (spdlog's default-logger read is
        // unlocked).

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        // Reset PrinterState for test isolation
        PrinterStateTestAccess::reset(state_);

        // Initialize subjects (without XML registration in tests)
        state_.init_subjects(false);

        // Create ActivePrintMediaManager for this test
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
    }

    ~ActivePrintMediaManagerTestFixture() {
        // Destroy manager first (it observes state_)
        manager_.reset();

        // Drain any pending updates before shutdown to ensure clean state
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Shutdown update queue - also clears any remaining pending callbacks
        helix::ui::update_queue_shutdown();
        queue_initialized = false; // Reset static flag for next test

        // Destroy display to prevent cross-shard leaks
        if (display_created_ && display_) {
            lv_display_delete(display_);
            display_ = nullptr;
            display_created_ = false;
        }

        // Reset after each test
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    helix::ActivePrintMediaManager& manager() {
        return *manager_;
    }

    // Helper to update print filename via status JSON (simulates Moonraker notification)
    void set_print_filename(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        // Drain all queued UI updates, including nested queue_update calls from
        // deferred observers (observer → handler → display filename setter)
        UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    // Get current print_filename (raw)
    std::string get_print_filename() {
        return lv_subject_get_string(state_.get_print_filename_subject());
    }

    // Get current print_display_filename (processed for UI)
    std::string get_display_filename() {
        return lv_subject_get_string(state_.get_print_display_filename_subject());
    }

    // Get current print_thumbnail_path
    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

  private:
    PrinterState state_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
    static lv_display_t* display_;
    static bool display_created_;
    static bool queue_initialized;
};

lv_display_t* ActivePrintMediaManagerTestFixture::display_ = nullptr;
bool ActivePrintMediaManagerTestFixture::display_created_ = false;
bool ActivePrintMediaManagerTestFixture::queue_initialized = false;

// ============================================================================
// Display Name Formatting Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: simple filename produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("benchy.gcode");

    REQUIRE(get_print_filename() == "benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: filename with path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("my_models/benchy.gcode");

    REQUIRE(get_print_filename() == "my_models/benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: helix_temp filename resolves to original",
                 "[ActivePrintMediaManager]") {
    // When HelixScreen modifies G-code, it creates temp files like:
    // .helix_temp/modified_1234567890_Original_Model.gcode
    // The display name should show "Original_Model", not the temp filename
    set_print_filename(".helix_temp/modified_1234567890_Body1.gcode");

    REQUIRE(get_print_filename() == ".helix_temp/modified_1234567890_Body1.gcode");
    REQUIRE(get_display_filename() == "Body1");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: complex helix_temp path resolves correctly",
                 "[ActivePrintMediaManager]") {
    set_print_filename(".helix_temp/modified_9876543210_My_Cool_Print.gcode");

    REQUIRE(get_display_filename() == "My_Cool_Print");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: deeply nested path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");

    REQUIRE(get_print_filename() == "projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");
    REQUIRE(get_display_filename() == "benchy_0.2mm_PLA");
}

// ============================================================================
// Empty Filename Handling Tests
// ============================================================================
// Design: Empty filename PRESERVES display info (for abort→firmware_restart UX).
// Clearing happens naturally when a NEW print starts with a different filename.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves display name",
                 "[ActivePrintMediaManager]") {
    // First set a filename
    set_print_filename("test.gcode");
    REQUIRE(get_print_filename() == "test.gcode");
    REQUIRE(get_display_filename() == "test");

    // When printer goes to standby (empty filename), display name is preserved
    // so users can see what was printing after cancel→firmware_restart
    set_print_filename("");
    REQUIRE(get_print_filename() == "");

    // Display filename should be PRESERVED (not cleared)
    REQUIRE(get_display_filename() == "test");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves thumbnail path",
                 "[ActivePrintMediaManager]") {
    // Set a filename first (to trigger the manager to process)
    set_print_filename("test.gcode");

    // Manually set a thumbnail path (simulating a loaded thumbnail)
    state().set_print_thumbnail("test.gcode", "A:/tmp/thumbnail_abc123.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");

    // When filename is cleared, thumbnail is PRESERVED (not cleared)
    // This allows users to see the print info after abort→firmware_restart
    set_print_filename("");

    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new filename replaces old display info",
                 "[ActivePrintMediaManager]") {
    // Set initial filename
    set_print_filename("first_print.gcode");
    REQUIRE(get_display_filename() == "first_print");

    // Manually set thumbnail (simulating loaded thumbnail)
    state().set_print_thumbnail("first_print.gcode", "A:/tmp/first_thumb.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/first_thumb.bin");

    // Start a NEW print - this should replace display name
    set_print_filename("second_print.gcode");
    REQUIRE(get_display_filename() == "second_print");

    // Thumbnail path is cleared when new print starts (will be reloaded via API)
    // Note: Without API set, thumbnail loading is skipped, so path remains
    // until explicitly cleared or new thumbnail is loaded
}

// ============================================================================
// Thumbnail Source Override Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: manual thumbnail source takes precedence",
                 "[ActivePrintMediaManager]") {
    // When PrintPreparationManager starts a modified print, it knows the original filename
    // and can provide it via set_thumbnail_source() for proper resolution

    // Set the thumbnail source BEFORE the filename arrives
    manager().set_thumbnail_source("original_model.gcode");

    // Now when a temp filename arrives, the source override should be used
    set_print_filename(".helix_temp/modified_12345_original_model.gcode");

    // Display name should use the source override
    REQUIRE(get_display_filename() == "original_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: clear_thumbnail_source resets state",
                 "[ActivePrintMediaManager]") {
    // Set up initial state
    set_print_filename("first.gcode");
    REQUIRE(get_display_filename() == "first");

    // Set an override
    manager().set_thumbnail_source("override.gcode");

    // Clear the override
    manager().clear_thumbnail_source();

    // Next filename should be processed normally (no override)
    set_print_filename("second.gcode");
    REQUIRE(get_display_filename() == "second");
}

// ============================================================================
// Generation Counter / Stale Callback Detection Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: rapid filename changes use latest generation",
                 "[ActivePrintMediaManager]") {
    // When filename changes rapidly (user quickly switches prints),
    // only the last one should be reflected

    set_print_filename("print1.gcode");
    set_print_filename("print2.gcode");
    set_print_filename("print3.gcode");

    // Only print3 should be reflected in the display name
    REQUIRE(get_print_filename() == "print3.gcode");
    REQUIRE(get_display_filename() == "print3");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: idempotent on repeated same filename",
                 "[ActivePrintMediaManager]") {
    // Setting the same filename multiple times should not trigger redundant processing
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");

    // Set again - should be idempotent
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");
}

// ============================================================================
// Integration with PrinterState Subjects
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: updates print_display_filename subject",
                 "[ActivePrintMediaManager]") {
    set_print_filename("test_model.gcode");

    REQUIRE(get_display_filename() == "test_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: observer fires on display_filename change",
                 "[ActivePrintMediaManager]") {
    int observer_count = 0;
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer = lv_subject_add_observer(state().get_print_display_filename_subject(),
                                                      observer_cb, &observer_count);

    // Initial observer registration fires once
    REQUIRE(observer_count == 1);

    // Change filename - should fire observer after processing
    set_print_filename("new_model.gcode");

    // Observer should have fired again
    REQUIRE(observer_count == 2);

    lv_observer_remove(observer);
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles filename with special characters",
                 "[ActivePrintMediaManager]") {
    set_print_filename("My Model (v2) - Final.gcode");

    REQUIRE(get_print_filename() == "My Model (v2) - Final.gcode");
    REQUIRE(get_display_filename() == "My Model (v2) - Final");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles very long filename",
                 "[ActivePrintMediaManager]") {
    // Test handling of very long filenames (within buffer limits)
    std::string long_name(100, 'x');
    long_name += ".gcode";

    set_print_filename(long_name);

    // Should handle gracefully (may be truncated to buffer size)
    REQUIRE_FALSE(get_display_filename().empty());
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: no API means no thumbnail load",
                 "[ActivePrintMediaManager]") {
    // Without set_api() being called, thumbnail loading should be skipped gracefully
    set_print_filename("model.gcode");

    // Display name should still work
    REQUIRE(get_display_filename() == "model");

    // No API to load from, so the file has no thumbnail of its own.
    REQUIRE(get_thumbnail_path() == NO_THUMB);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: uppercase extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCODE");

    REQUIRE(get_display_filename() == "Model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: mixed case extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCode");

    REQUIRE(get_display_filename() == "Model");
}

// ============================================================================
// Direct Thumbnail Path Tests (Pre-extracted from USB/G-code)
// ============================================================================

// NOTE: This test intentionally fails to compile because set_thumbnail_path()
// doesn't exist yet. This is TDD-style - implement the method to make it compile.
//
// TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
//                  "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
//                  "[media][thumbnail][direct]")
//
// When PrintStartController starts a print with a pre-extracted thumbnail
// (e.g., from USB drive or embedded G-code), it should be able to set the
// thumbnail path directly without going through Moonraker thumbnail API.
//
// Required new method signature:
//   void set_thumbnail_path(const std::string& path);
//
// Test cases that need to pass once implemented:
// 1. Direct path sets thumbnail_path subject
// 2. Direct path works alongside filename
// 3. Direct path not overwritten by filename change if already set
// 4. Empty path clears thumbnail
//
// Uncomment below and add set_thumbnail_path() to ActivePrintMediaManager

#if 1 // Enable when set_thumbnail_path() is implemented
TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
                 "[ActivePrintMediaManager]") {
    SECTION("direct path sets thumbnail_path subject") {
        // Pre-extracted thumbnail from USB or G-code
        std::string extracted_path = "/tmp/helix/thumbnails/extracted_12345.png";

        // Set the thumbnail path directly via new method
        manager().set_thumbnail_path("usb_extracted.gcode", extracted_path);

        // Thumbnail path subject should have the value
        REQUIRE(get_thumbnail_path() == extracted_path);
    }

    SECTION("direct path works alongside filename") {
        // Set a filename for the print
        set_print_filename("usb_print.gcode");
        REQUIRE(get_display_filename() == "usb_print");

        // Set thumbnail path directly (from pre-extracted USB thumbnail)
        std::string usb_thumbnail = "/media/usb/thumbnails/usb_print.png";
        manager().set_thumbnail_path("usb_print.gcode", usb_thumbnail);

        // Both should be set correctly
        REQUIRE(get_display_filename() == "usb_print");
        REQUIRE(get_thumbnail_path() == usb_thumbnail);
    }

    SECTION("direct path not overwritten by filename change if set") {
        // Set thumbnail path first (from PrintStartController)
        std::string preextracted = "/tmp/helix/embedded_thumbnail.png";
        manager().set_thumbnail_path("some_file.gcode", preextracted);
        REQUIRE(get_thumbnail_path() == preextracted);

        // When filename arrives from Moonraker, the pre-set thumbnail should persist
        // (because we already have a valid thumbnail, no need to fetch)
        set_print_filename("some_file.gcode");

        // The pre-extracted thumbnail should still be there
        REQUIRE(get_thumbnail_path() == preextracted);
    }

    SECTION("pre-set thumbnail does not block layer count from metadata") {
        // Regression: #526 - when thumbnail was pre-set, the metadata fetch was
        // skipped entirely, so layer_count and estimated_time were never loaded.
        // The fix ensures metadata is always fetched; only thumbnail download is skipped.

        // Set thumbnail path first (simulating PrintStartController pre-extraction)
        manager().set_thumbnail_path("model_with_layers.gcode", "/tmp/helix/preextracted.png");
        REQUIRE(get_thumbnail_path() == "/tmp/helix/preextracted.png");

        // Verify that having a pre-set thumbnail doesn't prevent layer_total
        // from being set. Without API, metadata can't be fetched, but the code
        // path that checks skip_thumbnail should not early-return before the
        // API check. Verify the subject is still at 0 (no API = no metadata).
        REQUIRE(lv_subject_get_int(state().get_print_layer_total_subject()) == 0);

        // The key assertion: load_thumbnail_for_file should NOT early-return
        // when thumbnail is set. It should proceed to the API check and only
        // skip after that (because no API is configured in this test).
        // This is implicitly tested: if the old early-return was still there,
        // the "Thumbnail already set, skipping API lookup" log would fire,
        // and we'd never reach the API check at all.
        set_print_filename("model_with_layers.gcode");

        // Display name should still work (metadata fetch path is entered)
        REQUIRE(get_display_filename() == "model_with_layers");
    }

    SECTION("empty path clears thumbnail") {
        // Set a thumbnail first
        manager().set_thumbnail_path("cleared.gcode", "/tmp/some_thumbnail.png");
        REQUIRE(get_thumbnail_path() == "/tmp/some_thumbnail.png");

        // Clear it — "no pre-extracted thumbnail" publishes the placeholder,
        // never "".
        manager().set_thumbnail_path("cleared.gcode", "");

        REQUIRE(get_thumbnail_path() == NO_THUMB);
    }
}
#endif

// ============================================================================
// Stale Thumbnail Invalidation Tests
// ============================================================================
// Regression: Starting a new print via Mainsail showed the PREVIOUS print's
// thumbnail because print_thumbnail_path_ was never cleared between prints.

// ============================================================================
// Thumbnail Path Subject Observer Integration Tests
// ============================================================================
// These tests verify that the print_thumbnail_path subject correctly notifies
// observers, which is the mechanism PrintStatusWidget uses to update its image.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: thumbnail path subject fires observer on change",
                 "[ActivePrintMediaManager]") {
    std::string last_observed_path;
    int observer_fire_count = 0;

    // Set up an observer on the thumbnail path subject (mimics what the widget does)
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* data =
            static_cast<std::pair<std::string*, int*>*>(lv_observer_get_user_data(observer));
        *data->first = lv_subject_get_string(subj);
        (*data->second)++;
    };

    auto data = std::make_pair(&last_observed_path, &observer_fire_count);
    lv_observer_t* obs =
        lv_subject_add_observer(state().get_print_thumbnail_path_subject(), observer_cb, &data);

    // Observer fires on registration with the initial (placeholder) value
    REQUIRE(observer_fire_count == 1);
    REQUIRE(last_observed_path == NO_THUMB);

    // Setting a thumbnail path should fire the observer
    state().set_print_thumbnail("model.gcode", "A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);
    REQUIRE(last_observed_path == "A:/cache/thumb.bin");

    // Setting same path should NOT fire (de-duplication in set_print_thumbnail)
    state().set_print_thumbnail("model.gcode", "A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);

    // Clearing path should fire
    state().set_print_thumbnail("model.gcode", "");
    REQUIRE(observer_fire_count == 3);
    REQUIRE(last_observed_path.empty());

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(
    ActivePrintMediaManagerTestFixture,
    "ActivePrintMediaManager: thumbnail path observer receives correct value during rapid updates",
    "[ActivePrintMediaManager]") {
    // This tests the scenario where the thumbnail path changes rapidly.
    // An immediate observer should receive each value in sequence.
    std::vector<std::string> observed_values;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* values = static_cast<std::vector<std::string>*>(lv_observer_get_user_data(observer));
        values->push_back(lv_subject_get_string(subj));
    };

    lv_observer_t* obs = lv_subject_add_observer(state().get_print_thumbnail_path_subject(),
                                                 observer_cb, &observed_values);

    // Initial fire
    REQUIRE(observed_values.size() == 1);
    REQUIRE(observed_values[0] == NO_THUMB);

    // Rapid updates - observer should see each distinct value
    state().set_print_thumbnail("first.gcode", "A:/cache/first.bin");
    state().set_print_thumbnail("second.gcode", "A:/cache/second.bin");
    state().set_print_thumbnail("third.gcode", "A:/cache/third.bin");

    REQUIRE(observed_values.size() == 4); // initial + 3 changes
    REQUIRE(observed_values[1] == "A:/cache/first.bin");
    REQUIRE(observed_values[2] == "A:/cache/second.bin");
    REQUIRE(observed_values[3] == "A:/cache/third.bin");

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new print clears stale thumbnail path",
                 "[ActivePrintMediaManager]") {
    SECTION("different file after idle clears old thumbnail") {
        // Print A starts and gets a thumbnail
        set_print_filename("print_a.gcode");
        state().set_print_thumbnail("print_a.gcode", "A:/cache/print_a_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print A ends - Moonraker sends empty filename
        set_print_filename("");
        // Thumbnail preserved (intentional for post-cancel UX)
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print B starts from Mainsail - stale thumbnail must be cleared
        set_print_filename("print_b.gcode");
        REQUIRE(get_display_filename() == "print_b");
        // The old thumbnail path should be cleared so the new one can be fetched
        REQUIRE(get_thumbnail_path() == NO_THUMB);
    }

    SECTION("direct switch between prints clears old thumbnail") {
        // Print A with thumbnail
        set_print_filename("first.gcode");
        state().set_print_thumbnail("first.gcode", "A:/cache/first_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/first_thumb.bin");

        // Print B starts immediately (no empty filename in between)
        set_print_filename("second.gcode");
        REQUIRE(get_display_filename() == "second");
        REQUIRE(get_thumbnail_path() == NO_THUMB);
    }

    SECTION("same filename reprint preserves thumbnail") {
        // Print A with thumbnail
        set_print_filename("benchy.gcode");
        state().set_print_thumbnail("benchy.gcode", "A:/cache/benchy_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");

        // Same file reprinted - idempotent guard means no change, which is correct
        // (the cached thumbnail is still valid for the same file)
        set_print_filename("benchy.gcode");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");
    }
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: first print clears a leftover thumbnail path",
                 "[ActivePrintMediaManager][thumbnail]") {
    // The reported field bug (Discord, jeremytodd1): the app reconnects or
    // restarts mid-print, so THIS manager never processed the previous print —
    // last_loaded_thumbnail_filename_ is empty. The shared subject, however,
    // still carries the previous print's path. The first filename this manager
    // ever sees must clear that leftover, not adopt it.
    state().set_print_thumbnail("previous.gcode", "A:/cache/previous.bin");
    REQUIRE(get_thumbnail_path() == "A:/cache/previous.bin");

    set_print_filename("brand_new.gcode");

    REQUIRE(get_display_filename() == "brand_new");
    // The leftover must be dropped, and the identity must describe the file we
    // are now loading for — not the finished print.
    CHECK(get_thumbnail_path() == NO_THUMB);
    CHECK(state().get_print_thumbnail_file() != "previous.gcode");
}

// ============================================================================
// Async Lifetime / Stale-Callback Safety (background-thread UAF regression)
// ============================================================================
//
// The metadata fetch in load_thumbnail_for_file() registers a callback that, on
// a real printer, fires on the Moonraker WebSocket background thread. Before the
// fix, that callback applied its result (layer_total / estimated_time) to the
// PrinterState subjects WITHOUT consulting the manager's AsyncLifetimeGuard, and
// guarded only by a non-atomic generation counter that was re-checked on the bg
// thread. If the manager was destroyed (soft restart / reconnect) while a fetch
// was in flight, the deferred subject write still ran against the freed object's
// captured state -> heap-corruption UAF.
//
// These tests capture the metadata callback (instead of letting the mock fire it
// synchronously) so they can fire it AFTER a superseding event:
//   1. a newer load bumps the generation, or
//   2. the manager is destroyed (lifetime invalidated).
// The post-event callback MUST NOT apply its (now stale / dangling) result.

namespace {

/// File API that captures the metadata request instead of answering it, so the
/// test controls exactly when (and after which lifecycle event) the success
/// callback fires. get_file_metadata is virtual on MoonrakerFileAPI (parity with
/// the virtual HTTP transfer methods the transfer mock overrides).
class CapturingFileAPI : public MoonrakerFileAPI {
  public:
    using MoonrakerFileAPI::MoonrakerFileAPI;

    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error, bool silent = false) override {
        (void)silent;
        last_filename_ = filename;
        pending_.push_back(std::move(on_success));
        pending_errors_.push_back(std::move(on_error));
    }

    /// Number of captured (not-yet-fired) metadata callbacks.
    [[nodiscard]] size_t pending_count() const {
        return pending_.size();
    }
    [[nodiscard]] bool has_pending() const {
        return !pending_.empty();
    }

    /// Fire the Nth captured callback (0-based) with the given metadata, WITHOUT
    /// consuming it from the list (so callers can fire callbacks in any order).
    void fire_index(size_t index, const FileMetadata& metadata) {
        REQUIRE(index < pending_.size());
        auto cb = pending_[index];
        cb(metadata);
    }

    /// Fire the most recently captured callback.
    void fire_last(const FileMetadata& metadata) {
        REQUIRE(!pending_.empty());
        fire_index(pending_.size() - 1, metadata);
    }

    /// Fire the Nth captured ERROR callback (0-based) with the given error.
    void fire_error_index(size_t index, const MoonrakerError& err) {
        REQUIRE(index < pending_errors_.size());
        auto cb = pending_errors_[index];
        REQUIRE(cb);
        cb(err);
    }

    /// Fire the most recently captured error callback.
    void fire_error_last(const MoonrakerError& err) {
        REQUIRE(!pending_errors_.empty());
        fire_error_index(pending_errors_.size() - 1, err);
    }

    /// Filename of the most recent metadata request.
    [[nodiscard]] const std::string& last_filename() const {
        return last_filename_;
    }

  private:
    std::vector<FileMetadataCallback> pending_;
    std::vector<ErrorCallback> pending_errors_;
    std::string last_filename_;
};

/// Transfer API whose download_thumbnail synchronously "succeeds" without
/// writing a file. ThumbnailCache::process_and_callback then hands the missing
/// path to ThumbnailProcessor, whose worker reports the read failure and falls
/// back to invoking on_success with the PNG path. That fallback crosses two
/// async hops (pool task, then UpdateQueue), so tests must settle both — see
/// ActivePrintMediaAsyncFixture::drain().
class StubTransferAPI : public MoonrakerFileTransferAPI {
  public:
    explicit StubTransferAPI(helix::MoonrakerClient& client)
        : MoonrakerFileTransferAPI(client, base_url_storage()) {}

    void download_thumbnail(const std::string& thumbnail_path, const std::string& cache_path,
                            StringCallback on_success, ErrorCallback on_error) override {
        (void)thumbnail_path;
        download_count_++;
        if (capture_downloads_) {
            captured_.push_back(Captured{cache_path, std::move(on_success)});
            return;
        }
        if (fail_downloads_) {
            MoonrakerError err;
            err.message = "stub download failure";
            if (on_error) {
                on_error(err);
            }
            return;
        }
        if (on_success) {
            on_success(cache_path);
        }
    }

    void set_fail_downloads(bool fail) {
        fail_downloads_ = fail;
    }
    [[nodiscard]] int download_count() const {
        return download_count_;
    }

    /// Hold subsequent downloads open instead of completing them inline, so a
    /// test can let a newer print supersede the load that started them.
    void set_capture_downloads(bool capture) {
        capture_downloads_ = capture;
    }
    [[nodiscard]] size_t captured_downloads() const {
        return captured_.size();
    }
    /// Complete a held download as if its PNG had just finished transferring.
    void fire_captured_download(size_t index) {
        REQUIRE(index < captured_.size());
        auto cb = captured_[index].on_success;
        REQUIRE(cb);
        cb(captured_[index].cache_path);
    }

  private:
    struct Captured {
        std::string cache_path;
        StringCallback on_success;
    };

    // Ctor stores a reference to the URL string; keep storage with static duration.
    static const std::string& base_url_storage() {
        static const std::string url = "http://stub.invalid";
        return url;
    }

    bool fail_downloads_ = false;
    bool capture_downloads_ = false;
    int download_count_ = 0;
    std::vector<Captured> captured_;
};

/// MoonrakerAPI that installs the CapturingFileAPI in place of the real file API
/// and a StubTransferAPI in place of the real HTTP transfer API.
class CapturingMoonrakerAPI : public MoonrakerAPI {
  public:
    CapturingMoonrakerAPI(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        // file_api_ / file_transfer_api_ are protected; swap in test implementations.
        file_api_ = std::make_unique<CapturingFileAPI>(client);
        file_transfer_api_ = std::make_unique<StubTransferAPI>(client);
    }

    CapturingFileAPI& capturing_files() {
        return static_cast<CapturingFileAPI&>(files());
    }

    StubTransferAPI& stub_transfers() {
        return static_cast<StubTransferAPI&>(transfers());
    }
};

/// Fixture that wires the manager to a CapturingMoonrakerAPI so metadata
/// callbacks can be fired on demand, after a lifecycle event.
class ActivePrintMediaAsyncFixture {
  public:
    ActivePrintMediaAsyncFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // Default spdlog output is suppressed process-wide by the
        // IsolationListener at testRunStarting — see the sync fixture above.

        lv_init_safe();

        if (!queue_initialized_) {
            helix::ui::update_queue_init();
            queue_initialized_ = true;
        }

        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);

        api_ = std::make_unique<CapturingMoonrakerAPI>(mock_client_, state_);
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
        manager_->set_api(api_.get());
    }

    ~ActivePrintMediaAsyncFixture() {
        manager_.reset();
        api_.reset();
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        helix::ui::update_queue_shutdown();
        queue_initialized_ = false;
        if (display_created_ && display_) {
            lv_display_delete(display_);
            display_ = nullptr;
            display_created_ = false;
        }
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }
    helix::ActivePrintMediaManager& manager() {
        return *manager_;
    }
    CapturingFileAPI& files() {
        return api_->capturing_files();
    }

    StubTransferAPI& transfers_stub() {
        return api_->stub_transfers();
    }

    MoonrakerClientMock& client() {
        return mock_client_;
    }

    /// Set the print filename WITHOUT draining the queue, so deferred applies
    /// stay pending until the test decides to drain.
    void set_print_filename_no_drain(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
    }

    /// Settle every hop a thumbnail load takes before asserting on its result.
    ///
    /// ThumbnailCache::process_and_callback() opens the PNG inside a
    /// ThumbnailProcessor pool task, and that task reports back through
    /// UpdateQueue. Draining the queue alone races the worker: the callback that
    /// publishes print_thumbnail_path_ has usually not been queued yet, so the
    /// subject still reads empty. Join the pool first, then drain — and repeat,
    /// since a drained callback can commit further pool work.
    ///
    /// Test the settled condition BEFORE draining, never after. It is
    /// drain_all() that commits the pool task (the metadata callback it runs
    /// reaches process_file_async), and pending_tasks() is HThreadPool::taskNum()
    /// — queue depth only. A worker that has already dequeued that task reports
    /// zero while still running it, so a post-drain check reads "settled" and
    /// returns before the result exists. wait_for_completion() is the accurate
    /// join: it waits for in-flight tasks too.
    void drain() {
        auto& processor = helix::ThumbnailProcessor::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        for (int pass = 0; pass < 4; ++pass) {
            processor.wait_for_completion();
            if (processor.pending_tasks() == 0 && UpdateQueueTestAccess::queue_empty(queue)) {
                break;
            }
            UpdateQueueTestAccess::drain_all(queue);
        }
    }

    int get_layer_total() {
        return lv_subject_get_int(state_.get_print_layer_total_subject());
    }

    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

    static FileMetadata make_metadata(uint32_t layer_count) {
        FileMetadata m;
        m.layer_count = layer_count;
        m.estimated_time = 0; // no estimate -> exercises layer path cleanly
        return m;
    }

    /// Metadata that includes a thumbnail entry. Pass a unique relative path
    /// per test so ThumbnailCache disk-cache hits from previous runs can't
    /// short-circuit the download we want to observe.
    static FileMetadata make_metadata_with_thumb(uint32_t layer_count,
                                                 const std::string& thumb_path) {
        FileMetadata m = make_metadata(layer_count);
        m.thumbnails.push_back(ThumbnailInfo{thumb_path, 300, 300});
        return m;
    }

    /// Unique thumbnail relative path (avoids cross-run ThumbnailCache hits).
    static std::string unique_thumb_path(const char* stem) {
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::string(".thumbs/") + stem + "_" + std::to_string(now) + "_" +
               std::to_string(counter.fetch_add(1)) + ".png";
    }

    /// Simulate a Moonraker notification arriving (fires the persistent method
    /// callbacks the manager registered on the client).
    void fire_notification(const std::string& method, const json& msg) {
        MoonrakerClientTestAccess::fire_method_callbacks(mock_client_, method, msg);
    }

    std::unique_ptr<helix::ActivePrintMediaManager>& manager_ptr() {
        return manager_;
    }

  private:
    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<CapturingMoonrakerAPI> api_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
    static lv_display_t* display_;
    static bool display_created_;
    static bool queue_initialized_;
};

lv_display_t* ActivePrintMediaAsyncFixture::display_ = nullptr;
bool ActivePrintMediaAsyncFixture::display_created_ = false;
bool ActivePrintMediaAsyncFixture::queue_initialized_ = false;

} // namespace

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata callback after manager destroy does not apply",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Start a load; the capturing API holds the metadata callback.
    set_print_filename_no_drain("doomed.gcode");
    drain(); // flush display-name update so only the layer apply is in flight later
    REQUIRE(files().has_pending());

    // The metadata arrives (bg thread) and the manager applies layer_total via
    // the lifetime-guarded defer path.
    files().fire_last(make_metadata(/*layer_count=*/137));

    // Owner is destroyed BEFORE the deferred apply runs (soft restart / reconnect).
    // AsyncLifetimeGuard destructor invalidates all outstanding tokens.
    manager_ptr().reset();

    // Now the deferred apply runs. It must be skipped — applying it would be a
    // use-after-free against the destroyed manager's captured state.
    drain();

    REQUIRE(get_layer_total() == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: stale metadata callback (superseded gen) does not apply",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Load A starts; capture its metadata callback (index 0).
    set_print_filename_no_drain("print_a.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Load B supersedes A (bumps generation) and captures a second callback (index 1).
    set_print_filename_no_drain("print_b.gcode");
    REQUIRE(files().pending_count() == 2);

    // B's metadata arrives FIRST and is applied (layer=222).
    files().fire_index(1, make_metadata(/*layer_count=*/222));
    drain();
    REQUIRE(get_layer_total() == 222);

    // A's (stale) metadata arrives LATE — its generation was superseded by B.
    // It MUST NOT clobber B's value. On the buggy code (no generation re-check on
    // the apply side / non-atomic gen race) this would overwrite with 111.
    files().fire_index(0, make_metadata(/*layer_count=*/111));
    drain();

    REQUIRE(get_layer_total() == 222);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: live metadata callback applies layer total",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Positive control: with no superseding event, the layer total is applied.
    set_print_filename_no_drain("live.gcode");
    drain();
    REQUIRE(files().has_pending());

    files().fire_last(make_metadata(/*layer_count=*/99));
    drain();

    REQUIRE(get_layer_total() == 99);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: a superseded load cannot publish its thumbnail",
                 "[ActivePrintMediaManager][async][thumbnail]") {
    // The metadata callback's generation check is not enough on its own: a load
    // can clear metadata while it is still current and only lose the race later,
    // during the thumbnail download. Hold the download open so print B starts in
    // that window, then let A's image arrive.
    transfers_stub().set_capture_downloads(true);

    set_print_filename_no_drain("print_a.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // A's metadata resolves while A is still the current print, so the fetch
    // starts. The download is held, so nothing is published yet.
    files().fire_last(make_metadata_with_thumb(10, unique_thumb_path("superseded_publish")));
    drain();
    REQUIRE(transfers_stub().captured_downloads() == 1);
    REQUIRE(get_thumbnail_path() == NO_THUMB);

    // Print B starts. process_filename() runs synchronously off the filename
    // subject write, so the load generation is bumped here — before A's
    // in-flight download completes. B's own metadata stays pending.
    transfers_stub().set_capture_downloads(false);
    set_print_filename_no_drain("print_b.gcode");
    drain();
    REQUIRE(files().pending_count() == 2);
    REQUIRE(get_thumbnail_path() == NO_THUMB);

    // A's download finally lands and its PNG is pre-scaled. The result belongs
    // to a superseded load: publishing it would put print A's image on print B.
    transfers_stub().fire_captured_download(0);
    drain();

    CHECK(get_thumbnail_path() == NO_THUMB);
    // Publishing also disarms recovery, so a leaked publish would additionally
    // stop B's own thumbnail from ever being retried.
    CHECK_FALSE(helix::ActivePrintMediaManagerTestAccess::thumbnail_loaded(manager()));
}

// ============================================================================
// Thumbnail cache freshness (re-slice under the same filename)
// ============================================================================
// The active print was the last consumer still building its ThumbnailRequest
// without source_modified, so ThumbnailCache never got to validate the cached
// artifact's mtime for it. Re-slice a model, reprint under the same name, and
// the print-status panel showed the PREVIOUS render for the whole job.
//
// The metadata callback already receives FileMetadata::modified, which is the
// timestamp Moonraker reports for the gcode file itself — exactly what the
// print-select cards feed their own requests.

namespace {

/// Smallest PNG both the cache and the processor accept: a 10x10 solid square.
/// Same bytes as tests/unit/test_thumbnail_cache_request.cpp.
// clang-format off
const std::vector<uint8_t> FRESHNESS_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x02, 0x50, 0x58, 0xEA, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x68, 0x70, 0x50, 0xC0,
    0x83, 0x18, 0x46, 0xA5, 0xB1, 0x21, 0x00, 0x24, 0x51, 0x57, 0x81, 0xF7,
    0xEC, 0xA3, 0x23, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};
// clang-format on

/// The target ActivePrintMediaManager builds its request from. Planting under
/// any other target would produce a .bin the manager never looks for, and the
/// "not served" assertion below would then hold for the wrong reason.
helix::ThumbnailTarget active_print_target() {
    return helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Detail);
}

/// Metadata carrying both a thumbnail and the source mtime Moonraker reports.
FileMetadata metadata_with_thumb_modified(uint32_t layer_count, const std::string& thumb_path,
                                          double modified) {
    FileMetadata m;
    m.layer_count = layer_count;
    m.estimated_time = 0;
    m.modified = modified;
    m.thumbnails.push_back(ThumbnailInfo{thumb_path, 300, 300});
    return m;
}

} // namespace

/// Positive control for the test below. It pins that a pre-scaled entry planted
/// under this key and target really is servable to the active-print load, so
/// the "stale entry is not served" assertion cannot pass just because the cache
/// was empty or the key/target never matched.
TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: a cached thumbnail older than the source is served",
                 "[ActivePrintMediaManager][async][thumbnail]") {
    const std::string key = unique_thumb_path("freshness_control");
    const auto planted = helix::ThumbnailProcessor::instance().process_sync(FRESHNESS_PNG, key,
                                                                            active_print_target());
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    set_print_filename_no_drain("reprint_control.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Source last modified in 2001 — long before the .bin that was just written.
    files().fire_last(metadata_with_thumb_modified(5, key, 1000000000.0));
    drain();

    CHECK(get_thumbnail_path() == planted.output_path);
    // A fresh cache hit resolves synchronously; nothing is downloaded.
    CHECK(transfers_stub().download_count() == 0);

    get_thumbnail_cache().invalidate(key);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: a source newer than the cached thumbnail is not served "
                 "from cache",
                 "[ActivePrintMediaManager][async][thumbnail]") {
    const std::string key = unique_thumb_path("freshness_stale");
    const auto planted = helix::ThumbnailProcessor::instance().process_sync(FRESHNESS_PNG, key,
                                                                            active_print_target());
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    set_print_filename_no_drain("reprint_stale.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // The re-slice: Moonraker reports the gcode as modified in 2100, newer than
    // anything on disk. The cached render describes the OLD model.
    files().fire_last(metadata_with_thumb_modified(5, key, 4102444800.0));
    drain();

    // The stale artifact must be invalidated and the image re-fetched, not
    // handed straight back.
    CHECK(transfers_stub().download_count() == 1);
    CHECK(get_thumbnail_path() != planted.output_path);

    get_thumbnail_cache().invalidate(key);
}

// ============================================================================
// Thumbnail Retry Tests (bounded backoff + Moonraker notification re-triggers)
// ============================================================================
// Field bug (recurring, v0.99.75): the print-status thumbnail stays blank for
// an entire print because load_thumbnail_for_file() was strictly one-shot —
// if Moonraker hadn't finished scanning a just-uploaded file (OrcaSlicer
// upload-and-print), the metadata fetch failed once and nothing ever
// re-queried. These tests cover the three retry legs:
//   1. bounded lv_timer backoff retry on each failure path
//   2. notify_filelist_changed re-trigger when the current file's metadata
//      becomes available
//   3. notify_klippy_ready re-trigger (WebSocket reconnect mid-print)

using TestAccess = helix::ActivePrintMediaManagerTestAccess;

namespace {
MoonrakerError make_error(const std::string& message) {
    MoonrakerError err;
    err.code = -32601;
    err.message = message;
    return err;
}
} // namespace

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: retry backoff schedule is 2s/5s/10s/20s/30s",
                 "[ActivePrintMediaManager][retry]") {
    REQUIRE(TestAccess::retry_delay_ms(1) == 2000);
    REQUIRE(TestAccess::retry_delay_ms(2) == 5000);
    REQUIRE(TestAccess::retry_delay_ms(3) == 10000);
    REQUIRE(TestAccess::retry_delay_ms(4) == 20000);
    REQUIRE(TestAccess::retry_delay_ms(5) == 30000);
    REQUIRE(TestAccess::retry_delay_ms(9) == 30000);
    REQUIRE(TestAccess::max_attempts() == 10);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata error schedules retry, retry success sets "
                 "thumbnail path",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("just_uploaded.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Attempt 1 fails: Moonraker hasn't scanned the file yet (bg thread error,
    // marshalled to main on drain).
    files().fire_error_last(make_error("Metadata not available for <just_uploaded.gcode>"));
    drain();

    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
    REQUIRE(TestAccess::retry_filename(manager()) == "just_uploaded.gcode");
    REQUIRE_FALSE(TestAccess::thumbnail_loaded(manager()));

    // Retry timer fires -> a NEW metadata request goes out.
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // This time the metadata is ready and has a thumbnail; the stub transfer
    // API "downloads" it synchronously and the subject gets set.
    files().fire_last(make_metadata_with_thumb(42, unique_thumb_path("retry_success")));
    drain();

    REQUIRE(get_thumbnail_path() != NO_THUMB);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(get_layer_total() == 42);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata without thumbnails schedules retry",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("scanning.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Metadata record exists but thumbnails aren't extracted yet.
    files().fire_last(make_metadata(/*layer_count=*/7));
    drain();

    REQUIRE(get_layer_total() == 7); // metadata fields still applied
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
    REQUIRE_FALSE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: thumbnail download failure schedules retry",
                 "[ActivePrintMediaManager][retry]") {
    transfers_stub().set_fail_downloads(true);

    set_print_filename_no_drain("dl_fail.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(3, unique_thumb_path("dl_fail")));
    drain();

    REQUIRE(get_thumbnail_path() == NO_THUMB);
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Recovery: downloads start working, retry fires, thumbnail loads.
    transfers_stub().set_fail_downloads(false);
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    files().fire_last(make_metadata_with_thumb(3, unique_thumb_path("dl_recover")));
    drain();

    REQUIRE(get_thumbnail_path() != NO_THUMB);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed for current file triggers immediate "
                 "reload",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Moonraker finishes scanning and emits notify_filelist_changed for the
    // file we're printing (fires on the WS thread in production; the handler
    // marshals to main, hence the drain).
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "notify_filelist_changed"},
        {"params", json::array({{{"action", "create_file"},
                                 {"item", {{"path", "benchy.gcode"}, {"root", "gcodes"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    // Immediate reload: pending retry cancelled, counter reset, new request out.
    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);

    files().fire_last(make_metadata_with_thumb(11, unique_thumb_path("filelist_reload")));
    drain();
    REQUIRE(get_thumbnail_path() != NO_THUMB);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed for unrelated file does not reload",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "notify_filelist_changed"},
        {"params", json::array({{{"action", "create_file"},
                                 {"item", {{"path", "unrelated.gcode"}, {"root", "gcodes"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    // No new request; the scheduled backoff retry is untouched.
    REQUIRE(files().pending_count() == 1);
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed with null/missing fields is ignored "
                 "safely",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Moonraker notifications can carry null/missing fields — must not throw.
    json null_item = {{"method", "notify_filelist_changed"},
                      {"params", json::array({{{"action", nullptr}, {"item", nullptr}}})}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", null_item));
    json no_params = {{"method", "notify_filelist_changed"}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", no_params));
    drain();

    REQUIRE(files().pending_count() == 1); // no reload triggered
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: klippy_ready re-triggers load while thumbnail missing",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("mid_print.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("connection reset"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Klippy comes back (WebSocket reconnect mid-print) — filename subject
    // never changes, so this is the only signal we get.
    fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}});
    drain();

    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: klippy_ready does NOT reload when thumbnail loaded",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("loaded.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(5, unique_thumb_path("klippy_loaded")));
    drain();
    REQUIRE(TestAccess::thumbnail_loaded(manager()));

    fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}});
    drain();

    REQUIRE(files().pending_count() == 1); // no redundant reload
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: a pre-set thumbnail still allows retry re-triggers",
                 "[ActivePrintMediaManager][thumbnail][retry]") {
    // PrintStartController pre-sets a USB / embedded-gcode thumbnail for the
    // file that is about to print. That is a legitimate reason to skip the
    // thumbnail FETCH — it is never a reason to disarm RECOVERY. Conflating the
    // two is what makes a wrong path permanent instead of transient.
    manager().set_thumbnail_path("usb_model.gcode", "A:/cache/usb_preset.bin");
    set_print_filename_no_drain("usb_model.gcode");
    drain();

    // The pre-set path belongs to this print, so it survives and no thumbnail
    // fetch is issued — but metadata is still requested.
    CHECK(get_thumbnail_path() == "A:/cache/usb_preset.bin");
    CHECK(TestAccess::thumbnail_origin(manager()) == helix::ThumbnailOrigin::PreSet);
    CHECK_FALSE(TestAccess::thumbnail_loaded(manager()));
    REQUIRE(files().pending_count() == 1);

    // Leg 1: the backoff ladder. A metadata failure schedules a retry, and the
    // retry body must actually re-issue the request.
    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    CHECK(files().pending_count() == 2);

    // Leg 2: notify_klippy_ready (WebSocket reconnect mid-print).
    fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}});
    drain();
    CHECK(files().pending_count() == 3);

    // Leg 3: notify_filelist_changed for the file being printed.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "modify_file"},
                                         {"item", {{"path", "usb_model.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();
    CHECK(files().pending_count() == 4);

    // Recovery stayed armed without ever clobbering the pre-set image.
    CHECK(get_thumbnail_path() == "A:/cache/usb_preset.bin");
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filename change cancels pending retry",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("first.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // A different print starts: the pending retry for first.gcode must be
    // cancelled and retry state reset for the new filename.
    set_print_filename_no_drain("second.gcode");
    drain();

    REQUIRE(files().pending_count() == 2); // fresh load for second.gcode
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);

    // No stale load for first.gcode fires later: the (deleted) timer is gone,
    // and even a stale-scheduled retry would no-op on the filename check.
    REQUIRE(TestAccess::retry_filename(manager()).empty());
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: retries stop after max attempts",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("never_scans.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    const int max_attempts = TestAccess::max_attempts();

    // Fail every attempt; pump the retry timer manually each round.
    int requests = 1;
    for (int i = 0; i < max_attempts + 3; i++) {
        files().fire_error_last(make_error("Metadata not available"));
        drain();
        if (!TestAccess::has_pending_retry(manager())) {
            break;
        }
        REQUIRE(TestAccess::fire_pending_retry(manager()));
        requests++;
        REQUIRE(static_cast<int>(files().pending_count()) == requests);
    }

    // Exactly max_attempts total requests went out, then it gave up.
    REQUIRE(requests == max_attempts);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Further failures don't resurrect the timer.
    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(static_cast<int>(files().pending_count()) == max_attempts);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: successful first load creates no retry timer",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("works_first_try.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(21, unique_thumb_path("first_try")));
    drain();

    REQUIRE(get_thumbnail_path() != NO_THUMB);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: empty-thumbnail metadata retries cap at 2",
                 "[ActivePrintMediaManager][retry]") {
    // A metadata record with no thumbnails is usually a PERMANENT condition
    // (file sliced without thumbnails), so this leg uses a 2-retry cap instead
    // of the full 10-attempt ladder reserved for transport failures.
    set_print_filename_no_drain("no_thumbs_sliced.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Attempt 1: success, no thumbnails -> retry 1.
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Attempt 2: still no thumbnails -> retry 2.
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 2);

    // Attempt 3: still no thumbnails -> gives up (cap is 2 retries).
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 3);
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(files().pending_count() == 3);

    // But a filelist_changed for the file still re-triggers (covers the
    // genuinely-late-scan case beyond the cheap cap).
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "modify_file"},
                                         {"item", {{"path", "no_thumbs_sliced.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();
    REQUIRE(files().pending_count() == 4);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: delete_file for current file does not re-trigger",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("doomed_delete.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // The printing file gets deleted — re-querying it is guaranteed to fail,
    // so the notification must NOT kick off a fresh retry ladder.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "delete_file"},
                                         {"item", {{"path", "doomed_delete.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 1);            // no immediate reload
    REQUIRE(TestAccess::retry_count(manager()) == 1); // ladder not reset
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: move_file of current file reloads from destination",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // The printing file is renamed: source_item is the old (current) name,
    // item is the destination. Metadata now lives under the destination.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "move_file"},
                                         {"source_item", {{"path", "benchy.gcode"}}},
                                         {"item", {{"path", "archive/benchy_v2.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 2);
    REQUIRE(files().last_filename() == "archive/benchy_v2.gcode"); // dest, not stale name
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Destination metadata resolves -> thumbnail loads.
    files().fire_last(make_metadata_with_thumb(8, unique_thumb_path("moved_dest")));
    drain();
    REQUIRE(get_thumbnail_path() != NO_THUMB);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: move_file without destination path is skipped",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Malformed/partial move notification: source matches but no dest item.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "move_file"},
                                         {"source_item", {{"path", "benchy.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 1);             // no reload from a missing dest
    REQUIRE(TestAccess::has_pending_retry(manager())); // backoff ladder untouched
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: notification after manager destroy is safe (expired "
                 "token no-ops)",
                 "[ActivePrintMediaManager][retry][lifetime]") {
    // Regression guard for the registration-lifetime story: unregistration is
    // skipped during teardown (get_moonraker_manager() is null by then), so the
    // method callbacks stay registered on the client. Safety relies on the
    // lifetime token captured at registration: bg-side parsing touches locals
    // only, and the token.defer() apply must no-op once the manager is gone.
    set_print_filename_no_drain("teardown.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    manager_ptr().reset(); // destroy the manager (lifetime invalidated)

    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "create_file"},
                                         {"item", {{"path", "teardown.gcode"}}}}})}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", msg));
    REQUIRE_NOTHROW(
        fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}}));
    drain(); // expired-token defers must no-op, not UAF

    REQUIRE(files().pending_count() == 1); // no reload fired against the dead manager
}

// ============================================================================
// Preparing-job identity
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Media adopts the preparing job's identity at commit",
                 "[active_print_media][preparing][1339]") {
    // The thumbnail source used to be set from the Moonraker-confirmed callback,
    // which on a printer with a host-side pre-start block lands minutes after the
    // user pressed Print. In between, print_stats still names the PREVIOUS job,
    // so the panel resolved and loaded the wrong file's preview.
    set_print_filename("previous.gcode");
    REQUIRE(get_display_filename() == "previous");

    state().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(get_display_filename() == "next");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A superseded preparing job releases its media claim",
                 "[active_print_media][preparing][1339]") {
    // Somebody started a different print while ours was preparing. Our override
    // must go, including thumbnail_origin_ - a stale PreSet skips the thumbnail
    // fetch, which is the mechanism behind #526.
    state().begin_preparing(helix::PrintJobRef{"mine.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(get_display_filename() == "mine");

    state().retire_preparing(helix::PreparingExit::Superseded);
    set_print_filename("someone_elses.gcode");

    REQUIRE(get_display_filename() == "someone_elses");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A confirmed preparing job keeps its media claim",
                 "[active_print_media][preparing][1339]") {
    // Confirmed means the printer took OUR job. The override must survive,
    // because the printer may report a rewritten temp file standing in for the
    // file the user actually chose.
    state().begin_preparing(helix::PrintJobRef{"mine.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename(".helix_temp/modified_mine.gcode");

    REQUIRE(get_display_filename() == "mine");
}

// ============================================================================
// Commit-time media loads must not strand the job without metadata
//
// Adopting identity at commit moves the metadata fetch to a moment when the
// file may not be uploaded or scanned yet. Its failures consume a bounded retry
// ladder (~217s), and process_filename() early-returns forever after on the
// unchanged effective filename - so without an explicit re-arm at print start,
// a pre-start block longer than the ladder leaves layers 0/0 for the whole job.
// That is #526's symptom reached by a new route.
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "A confirmed print re-arms media that failed to load while preparing",
                 "[active_print_media][preparing][metadata]") {
    state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    REQUIRE(files().pending_count() == 1); // commit issued the first attempt

    MoonrakerError err;
    err.message = "file not found";
    files().fire_error_last(err);
    drain();
    REQUIRE(get_layer_total() == 0);

    // The printer takes the job. The effective filename has not changed, so
    // nothing in the filename path will ever ask again.
    state().retire_preparing(helix::PreparingExit::Confirmed);
    drain();

    REQUIRE(files().pending_count() == 2); // re-armed

    files().fire_last(make_metadata(42));
    drain();
    REQUIRE(get_layer_total() == 42);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "A confirmed print does not re-fetch media it already has",
                 "[active_print_media][preparing][metadata]") {
    // The re-arm is for recovery, not a second unconditional fetch. Firing it
    // when the data is already present would waste an RPC on every print start.
    state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(17, unique_thumb_path("confirm_no_refetch")));
    drain();
    REQUIRE(get_layer_total() == 17);

    const size_t before = files().pending_count();
    state().retire_preparing(helix::PreparingExit::Confirmed);
    drain();

    REQUIRE(files().pending_count() == before);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "A print that never reached the printer is not re-armed",
                 "[active_print_media][preparing][metadata]") {
    // Superseded/Cancelled/Failed release the identity instead. Re-arming there
    // would fetch metadata for a job that is not going to run.
    state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    MoonrakerError err;
    err.message = "file not found";
    files().fire_error_last(err);
    drain();

    const size_t before = files().pending_count();
    state().retire_preparing(helix::PreparingExit::Cancelled);
    drain();

    REQUIRE(files().pending_count() == before);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Reprint of a modified file displays the original name, not the temp one",
                 "[active_print_media][preparing][1339]") {
    // Reprint replays whatever print_stats last reported, which for a modified
    // print is the rewritten temp path. Recording that raw as the thumbnail
    // source also suppressed process_filename()'s auto-resolve, which is guarded
    // on the source being empty - so the panel showed `modified_1748_orig`.
    state().begin_preparing(helix::PrintJobRef{".helix_temp/modified_1748_orig.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(get_display_filename() == "orig");
}

// ============================================================================
// Cross-print override staleness (#1339)
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A print started outside the app clears the previous print's thumbnail source",
                 "[active_print_media][preparing][1339]") {
    // #1339: print A is started FROM HelixScreen, so the preparing epoch records
    // its identity as the thumbnail source. Print B is then started from
    // Mainsail/Fluidd or the printer's own screen, so no preparing epoch fires
    // and nothing re-points the override. process_filename() computes the
    // effective name from the stale override, matches last_effective_filename_,
    // and early-returns - so the thumbnail subject is never republished and the
    // preview keeps rendering print A's image for the whole of print B.
    state().begin_preparing(helix::PrintJobRef{"printA.gcode", "", ""});
    state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename("printA.gcode");
    REQUIRE(get_display_filename() == "printA");

    // Print A ends. Moonraker reports an empty filename; the manager
    // deliberately preserves the display so the finished print stays readable.
    set_print_filename("");

    // Print B arrives with no preparing epoch of its own - external start.
    set_print_filename("printB.gcode");

    REQUIRE(get_display_filename() == "printB");
    // The thumbnail subject must now describe print B. If it still names
    // print A, the preview is showing the previous print's image.
    REQUIRE(state().get_print_thumbnail_file() == "printB.gcode");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Back-to-back external prints keep republishing the thumbnail",
                 "[active_print_media][preparing][1339]") {
    // The same defect without any in-app start: once ANY override is recorded
    // it must not survive into a file it does not describe.
    state().begin_preparing(helix::PrintJobRef{"first.gcode", "", ""});
    state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename("first.gcode");

    set_print_filename("second.gcode");
    REQUIRE(state().get_print_thumbnail_file() == "second.gcode");

    set_print_filename("third.gcode");
    REQUIRE(state().get_print_thumbnail_file() == "third.gcode");
    REQUIRE(get_display_filename() == "third");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Reprinting the same file keeps the media claim it was started with",
                 "[active_print_media][preparing][1339]") {
    // Retiring a stale override must not fire on a REPRINT, where the override
    // still describes exactly what is printing. Losing it here would discard a
    // USB / pre-extracted thumbnail the print is entitled to keep.
    state().begin_preparing(helix::PrintJobRef{"repeat.gcode", "", ""});
    state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename("repeat.gcode");
    REQUIRE(get_display_filename() == "repeat");

    set_print_filename("");
    set_print_filename("repeat.gcode");

    REQUIRE(get_display_filename() == "repeat");
    REQUIRE(state().get_print_thumbnail_file() == "repeat.gcode");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A rewritten temp path never retires the override that explains it",
                 "[active_print_media][preparing][1339]") {
    // The override exists precisely to map a rewritten temp copy back to the
    // name the user chose. Only this app writes those paths, so one arriving
    // always belongs to the print whose epoch set the override.
    state().begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
    state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename(".helix_temp/modified_1748_chosen.gcode");

    REQUIRE(get_display_filename() == "chosen");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A preparing job's claim survives the previous print still being reported",
                 "[active_print_media][preparing][1339]") {
    // The retirement rule must not fire while a job is preparing. print_stats
    // keeps naming the PREVIOUS job for the whole pre-start block - that lag is
    // the reason identity is recorded at commit - so a mismatch there is
    // expected, not stale. Retiring on it would discard the identity the epoch
    // just adopted and send the new print's media lookup back to the old file.
    set_print_filename("previous.gcode");

    state().begin_preparing(helix::PrintJobRef{"committed.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // Moonraker is still reporting the finished print while ours prepares.
    set_print_filename("previous.gcode");

    REQUIRE(get_display_filename() == "committed");
}
