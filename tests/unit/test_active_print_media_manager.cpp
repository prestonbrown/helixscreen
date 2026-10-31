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

#include "ui_filename_utils.h"

#include "../test_helpers/active_print_media_manager_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/thumbnail_test_png.h"
#include "../ui_test_utils.h"
#include "active_print_media_manager.h"
#include "async_lifetime_guard.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"
#include "printer_state.h"
#include "three_mf_reader.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
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
static const std::string kNoThumb = helix::ActivePrintMediaManager::no_thumbnail_placeholder();

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
        return lv_subject_get_string(state_.print_state().get_print_filename_subject());
    }

    // Get current print_display_filename (processed for UI)
    std::string get_display_filename() {
        return lv_subject_get_string(state_.print_state().get_print_display_filename_subject());
    }

    // Get current print_thumbnail_path
    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.print_state().get_print_thumbnail_path_subject());
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
                 "ActivePrintMediaManager: the display name is the file's base name, the "
                 "original's for a HelixScreen temp file",
                 "[ActivePrintMediaManager]") {
    struct Row {
        std::vector<std::string> filenames; // set in order; the last one is shown
        std::string display;                // empty: any non-empty name (it may be truncated)
    };
    const Row rows[] = {
        {{"benchy.gcode"}, "benchy"},
        {{"my_models/benchy.gcode"}, "benchy"},
        {{"projects/2025/january/test_models/benchy_0.2mm_PLA.gcode"}, "benchy_0.2mm_PLA"},
        // A modified print runs from .helix_temp/modified_<stamp>_<original>.
        {{".helix_temp/modified_1234567890_Body1.gcode"}, "Body1"},
        {{".helix_temp/modified_9876543210_My_Cool_Print.gcode"}, "My_Cool_Print"},
        {{"My Model (v2) - Final.gcode"}, "My Model (v2) - Final"},
        {{"Model.GCODE"}, "Model"},
        {{"Model.GCode"}, "Model"},
        {{std::string(100, 'x') + ".gcode"}, ""},
        // Rapid changes leave the last; a repeat of the same name changes nothing.
        {{"print1.gcode", "print2.gcode", "print3.gcode"}, "print3"},
        {{"same_file.gcode", "same_file.gcode"}, "same_file"},
    };
    for (const auto& row : rows) {
        CAPTURE(row.filenames.back());
        for (const auto& f : row.filenames) {
            set_print_filename(f);
        }
        CHECK(get_print_filename() == row.filenames.back());
        if (row.display.empty()) {
            CHECK_FALSE(get_display_filename().empty());
        } else {
            CHECK(get_display_filename() == row.display);
        }
    }
}

// ============================================================================
// Empty Filename Handling Tests
// ============================================================================
// Design: Empty filename PRESERVES display info (for abort→firmware_restart UX).
// Clearing happens naturally when a NEW print starts with a different filename.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: an empty filename keeps the display name and thumbnail",
                 "[ActivePrintMediaManager]") {
    // Users still see what was printing after cancel -> firmware_restart.
    set_print_filename("test.gcode");
    REQUIRE(get_display_filename() == "test");
    state().print_state().set_print_thumbnail("test.gcode", "A:/tmp/thumbnail_abc123.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");

    set_print_filename("");
    REQUIRE(get_print_filename() == "");
    CHECK(get_display_filename() == "test");
    CHECK(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new filename replaces old display info",
                 "[ActivePrintMediaManager]") {
    // Set initial filename
    set_print_filename("first_print.gcode");
    REQUIRE(get_display_filename() == "first_print");

    // Manually set thumbnail (simulating loaded thumbnail)
    state().print_state().set_print_thumbnail("first_print.gcode", "A:/tmp/first_thumb.bin");
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

// ============================================================================
// Integration with PrinterState Subjects
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: observer fires on display_filename change",
                 "[ActivePrintMediaManager]") {
    int observer_count = 0;
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer = lv_subject_add_observer(
        state().print_state().get_print_display_filename_subject(), observer_cb, &observer_count);

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
                 "ActivePrintMediaManager: no API means no thumbnail load",
                 "[ActivePrintMediaManager]") {
    // Without set_api() being called, thumbnail loading should be skipped gracefully
    set_print_filename("model.gcode");

    // Display name should still work
    REQUIRE(get_display_filename() == "model");

    // No API to load from, so the file has no thumbnail of its own.
    REQUIRE(get_thumbnail_path() == kNoThumb);
}

// ============================================================================
// Direct Thumbnail Path Tests (Pre-extracted from USB/G-code)
// ============================================================================

// A print started from a pre-extracted thumbnail (a USB drive, embedded G-code)
// sets the path directly, without the Moonraker thumbnail API.
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
        // A pre-set thumbnail skips only the download, never the metadata fetch
        // that loads layer_count and estimated_time (#526).

        // Set thumbnail path first (simulating PrintStartController pre-extraction)
        manager().set_thumbnail_path("model_with_layers.gcode", "/tmp/helix/preextracted.png");
        REQUIRE(get_thumbnail_path() == "/tmp/helix/preextracted.png");

        // Verify that having a pre-set thumbnail doesn't prevent layer_total
        // from being set. Without API, metadata can't be fetched, but the code
        // path that checks skip_thumbnail should not early-return before the
        // API check. Verify the subject is still at 0 (no API = no metadata).
        REQUIRE(lv_subject_get_int(state().print_state().get_print_layer_total_subject()) == 0);

        // The key assertion: load_thumbnail_for_file should NOT early-return
        // when thumbnail is set. It should proceed to the API check and only
        // skip after that (because no API is configured in this test).
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

        REQUIRE(get_thumbnail_path() == kNoThumb);
    }
}

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
    lv_observer_t* obs = lv_subject_add_observer(
        state().print_state().get_print_thumbnail_path_subject(), observer_cb, &data);

    // Observer fires on registration with the initial (placeholder) value
    REQUIRE(observer_fire_count == 1);
    REQUIRE(last_observed_path == kNoThumb);

    // Setting a thumbnail path should fire the observer
    state().print_state().set_print_thumbnail("model.gcode", "A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);
    REQUIRE(last_observed_path == "A:/cache/thumb.bin");

    // Setting same path should NOT fire (de-duplication in set_print_thumbnail)
    state().print_state().set_print_thumbnail("model.gcode", "A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);

    // Clearing path should fire
    state().print_state().set_print_thumbnail("model.gcode", "");
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

    lv_observer_t* obs = lv_subject_add_observer(
        state().print_state().get_print_thumbnail_path_subject(), observer_cb, &observed_values);

    // Initial fire
    REQUIRE(observed_values.size() == 1);
    REQUIRE(observed_values[0] == kNoThumb);

    // Rapid updates - observer should see each distinct value
    state().print_state().set_print_thumbnail("first.gcode", "A:/cache/first.bin");
    state().print_state().set_print_thumbnail("second.gcode", "A:/cache/second.bin");
    state().print_state().set_print_thumbnail("third.gcode", "A:/cache/third.bin");

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
        state().print_state().set_print_thumbnail("print_a.gcode", "A:/cache/print_a_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print A ends - Moonraker sends empty filename
        set_print_filename("");
        // Thumbnail preserved (intentional for post-cancel UX)
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print B starts from Mainsail - stale thumbnail must be cleared
        set_print_filename("print_b.gcode");
        REQUIRE(get_display_filename() == "print_b");
        // The old thumbnail path should be cleared so the new one can be fetched
        REQUIRE(get_thumbnail_path() == kNoThumb);
    }

    SECTION("direct switch between prints clears old thumbnail") {
        // Print A with thumbnail
        set_print_filename("first.gcode");
        state().print_state().set_print_thumbnail("first.gcode", "A:/cache/first_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/first_thumb.bin");

        // Print B starts immediately (no empty filename in between)
        set_print_filename("second.gcode");
        REQUIRE(get_display_filename() == "second");
        REQUIRE(get_thumbnail_path() == kNoThumb);
    }

    SECTION("same filename reprint preserves thumbnail") {
        // Print A with thumbnail
        set_print_filename("benchy.gcode");
        state().print_state().set_print_thumbnail("benchy.gcode", "A:/cache/benchy_thumb.bin");
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
    state().print_state().set_print_thumbnail("previous.gcode", "A:/cache/previous.bin");
    REQUIRE(get_thumbnail_path() == "A:/cache/previous.bin");

    set_print_filename("brand_new.gcode");

    REQUIRE(get_display_filename() == "brand_new");
    // The leftover must be dropped, and the identity must describe the file we
    // are now loading for — not the finished print.
    CHECK(get_thumbnail_path() == kNoThumb);
    CHECK(state().print_state().get_print_thumbnail_file() != "previous.gcode");
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
        last_thumbnail_path_ = thumbnail_path;
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

    /// Holds every gcode header request open instead of sending it, so no
    /// real HTTP request outlives the test and a test can fire its error late.
    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error,
                               CancelFlag = nullptr) override {
        (void)root;
        (void)path;
        (void)max_bytes;
        (void)on_success;
        partial_errors_.push_back(std::move(on_error));
    }
    [[nodiscard]] size_t partial_count() const {
        return partial_errors_.size();
    }

    /// Serve @p bytes as the file @p path to the ranged and tail reads a .3mf
    /// archive is read with, synchronously; offsets read are recorded.
    void serve_archive(const std::string& path, std::string bytes) {
        archives_[path] = std::move(bytes);
    }
    void download_file_tail(const std::string& root, const std::string& path, size_t max_bytes,
                            StringCallback on_success, ErrorCallback on_error) override {
        (void)root;
        auto it = archives_.find(path);
        if (it == archives_.end()) {
            on_error(MoonrakerError::file_not_found("download_file_tail", path));
            return;
        }
        const std::string& f = it->second;
        on_success(f.substr(f.size() - std::min(max_bytes, f.size())));
    }
    void download_file_range(const std::string& root, const std::string& path, uint64_t offset,
                             size_t len, StringCallback on_success, ErrorCallback on_error,
                             CancelFlag = nullptr) override {
        (void)root;
        auto it = archives_.find(path);
        if (it == archives_.end()) {
            on_error(MoonrakerError::file_not_found("download_file_range", path));
            return;
        }
        range_offsets_.push_back(offset);
        const std::string& f = it->second;
        on_success(offset < f.size() ? f.substr(static_cast<size_t>(offset), len) : "");
    }
    [[nodiscard]] const std::vector<uint64_t>& range_offsets() const {
        return range_offsets_;
    }
    void fire_partial_error_last(const MoonrakerError& err) {
        REQUIRE(!partial_errors_.empty());
        auto cb = partial_errors_.back();
        REQUIRE(cb);
        cb(err);
    }

    void set_fail_downloads(bool fail) {
        fail_downloads_ = fail;
    }
    [[nodiscard]] int download_count() const {
        return download_count_;
    }
    /// Moonraker path of the most recent thumbnail download.
    [[nodiscard]] const std::string& last_thumbnail_path() const {
        return last_thumbnail_path_;
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
    std::string last_thumbnail_path_;
    std::vector<Captured> captured_;
    std::vector<ErrorCallback> partial_errors_;
    std::map<std::string, std::string> archives_;
    std::vector<uint64_t> range_offsets_;
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
        return lv_subject_get_int(state_.print_state().get_print_layer_total_subject());
    }

    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.print_state().get_print_thumbnail_path_subject());
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
                 "ActivePrintMediaManager: gcode header error after manager destroy is skipped",
                 "[ActivePrintMediaManager][async][lifetime]") {
    set_print_filename_no_drain("doomed_header.gcode");
    drain();
    REQUIRE(files().has_pending());

    // A metadata error self-serves from the gcode header; the stub holds that
    // download open.
    MoonrakerError metadata_err;
    metadata_err.message = "Metadata not available";
    files().fire_error_last(metadata_err);
    drain();
    REQUIRE(transfers_stub().partial_count() == 1);

    (void)helix::async_lifetime::take_snapshot();
    manager_ptr().reset();

    // The download fails after the manager is gone (soft restart). Its error
    // body touches members, so it must be dropped rather than run.
    MoonrakerError download_err;
    download_err.message = "connection reset";
    transfers_stub().fire_partial_error_last(download_err);
    drain();

    auto snapshot = helix::async_lifetime::take_snapshot();
    bool skipped = false;
    for (const auto& entry : snapshot.entries) {
        if (entry.tag == "ActivePrintMediaManager::on_extract_error") {
            skipped = entry.count > 0;
        }
    }
    REQUIRE(skipped);
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
    // It must not clobber B's value: the apply side re-checks the generation.
    files().fire_index(0, make_metadata(/*layer_count=*/111));
    drain();

    REQUIRE(get_layer_total() == 222);
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
    REQUIRE(get_thumbnail_path() == kNoThumb);

    // Print B starts. process_filename() runs synchronously off the filename
    // subject write, so the load generation is bumped here — before A's
    // in-flight download completes. B's own metadata stays pending.
    transfers_stub().set_capture_downloads(false);
    set_print_filename_no_drain("print_b.gcode");
    drain();
    REQUIRE(files().pending_count() == 2);
    REQUIRE(get_thumbnail_path() == kNoThumb);

    // A's download finally lands and its PNG is pre-scaled. The result belongs
    // to a superseded load: publishing it would put print A's image on print B.
    transfers_stub().fire_captured_download(0);
    drain();

    CHECK(get_thumbnail_path() == kNoThumb);
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
    const auto planted =
        helix::ThumbnailProcessor::instance().process_sync(kTinyPng, key, active_print_target());
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
    const auto planted =
        helix::ThumbnailProcessor::instance().process_sync(kTinyPng, key, active_print_target());
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

    REQUIRE(get_thumbnail_path() != kNoThumb);
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

    REQUIRE(get_thumbnail_path() == kNoThumb);
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Recovery: downloads start working, retry fires, thumbnail loads.
    transfers_stub().set_fail_downloads(false);
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    files().fire_last(make_metadata_with_thumb(3, unique_thumb_path("dl_recover")));
    drain();

    REQUIRE(get_thumbnail_path() != kNoThumb);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: subfolder print downloads its thumbnail from that "
                 "subfolder",
                 "[ActivePrintMediaManager][thumbnail]") {
    set_print_filename_no_drain("sub/dir/subfolder_thumb.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Moonraker gives a thumbnail's relative_path relative to the gcode's own
    // directory, so the download has to prepend that directory.
    const std::string rel = unique_thumb_path("subfolder_thumb");
    files().fire_last(make_metadata_with_thumb(4, rel));
    drain();

    REQUIRE(transfers_stub().download_count() == 1);
    REQUIRE(transfers_stub().last_thumbnail_path() == "sub/dir/" + rel);
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
    REQUIRE(get_thumbnail_path() != kNoThumb);
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed for another file, or with null fields, "
                 "does not reload",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    SECTION("an unrelated file") {
        json msg = {{"jsonrpc", "2.0"},
                    {"method", "notify_filelist_changed"},
                    {"params",
                     json::array({{{"action", "create_file"},
                                   {"item", {{"path", "unrelated.gcode"}, {"root", "gcodes"}}}}})}};
        fire_notification("notify_filelist_changed", msg);
        drain();
        // The scheduled backoff retry is untouched.
        REQUIRE(TestAccess::has_pending_retry(manager()));
        REQUIRE(TestAccess::retry_count(manager()) == 1);
    }
    SECTION("null or missing fields") {
        // Moonraker notifications can carry null/missing fields — must not throw.
        json null_item = {{"method", "notify_filelist_changed"},
                          {"params", json::array({{{"action", nullptr}, {"item", nullptr}}})}};
        REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", null_item));
        json no_params = {{"method", "notify_filelist_changed"}};
        REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", no_params));
        drain();
    }
    REQUIRE(files().pending_count() == 1); // no reload
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

    REQUIRE(get_thumbnail_path() != kNoThumb);
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
    REQUIRE(get_thumbnail_path() != kNoThumb);
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

    state().print_state().begin_preparing(helix::PrintJobRef{"next.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    REQUIRE(get_display_filename() == "next");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A superseded preparing job releases its media claim",
                 "[active_print_media][preparing][1339]") {
    // Somebody started a different print while ours was preparing. Our override
    // must go, including thumbnail_origin_ - a stale PreSet skips the thumbnail
    // fetch, which is the mechanism behind #526.
    state().print_state().begin_preparing(helix::PrintJobRef{"mine.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(get_display_filename() == "mine");

    state().print_state().retire_preparing(helix::PreparingExit::Superseded);
    set_print_filename("someone_elses.gcode");

    REQUIRE(get_display_filename() == "someone_elses");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A confirmed preparing job keeps its media claim",
                 "[active_print_media][preparing][1339]") {
    // Confirmed means the printer took OUR job. The override must survive,
    // because the printer may report a rewritten temp file standing in for the
    // file the user actually chose.
    state().print_state().begin_preparing(helix::PrintJobRef{"mine.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
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
    state().print_state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    REQUIRE(files().pending_count() == 1); // commit issued the first attempt

    MoonrakerError err;
    err.message = "file not found";
    files().fire_error_last(err);
    drain();
    REQUIRE(get_layer_total() == 0);

    // The printer takes the job. The effective filename has not changed, so
    // nothing in the filename path will ever ask again.
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
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
    state().print_state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(17, unique_thumb_path("confirm_no_refetch")));
    drain();
    REQUIRE(get_layer_total() == 17);

    const size_t before = files().pending_count();
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
    drain();

    REQUIRE(files().pending_count() == before);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "A print that never reached the printer is not re-armed",
                 "[active_print_media][preparing][metadata]") {
    // Superseded/Cancelled/Failed release the identity instead. Re-arming there
    // would fetch metadata for a job that is not going to run.
    state().print_state().begin_preparing(helix::PrintJobRef{"job.gcode", "", ""});
    drain();
    MoonrakerError err;
    err.message = "file not found";
    files().fire_error_last(err);
    drain();

    const size_t before = files().pending_count();
    state().print_state().retire_preparing(helix::PreparingExit::Cancelled);
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
    state().print_state().begin_preparing(
        helix::PrintJobRef{".helix_temp/modified_1748_orig.gcode", "", ""});
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
    state().print_state().begin_preparing(helix::PrintJobRef{"printA.gcode", "", ""});
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
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
    REQUIRE(state().print_state().get_print_thumbnail_file() == "printB.gcode");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Back-to-back external prints keep republishing the thumbnail",
                 "[active_print_media][preparing][1339]") {
    // The same defect without any in-app start: once ANY override is recorded
    // it must not survive into a file it does not describe.
    state().print_state().begin_preparing(helix::PrintJobRef{"first.gcode", "", ""});
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename("first.gcode");

    set_print_filename("second.gcode");
    REQUIRE(state().print_state().get_print_thumbnail_file() == "second.gcode");

    set_print_filename("third.gcode");
    REQUIRE(state().print_state().get_print_thumbnail_file() == "third.gcode");
    REQUIRE(get_display_filename() == "third");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "Reprinting the same file keeps the media claim it was started with",
                 "[active_print_media][preparing][1339]") {
    // Retiring a stale override must not fire on a REPRINT, where the override
    // still describes exactly what is printing. Losing it here would discard a
    // USB / pre-extracted thumbnail the print is entitled to keep.
    state().print_state().begin_preparing(helix::PrintJobRef{"repeat.gcode", "", ""});
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
    set_print_filename("repeat.gcode");
    REQUIRE(get_display_filename() == "repeat");

    set_print_filename("");
    set_print_filename("repeat.gcode");

    REQUIRE(get_display_filename() == "repeat");
    REQUIRE(state().print_state().get_print_thumbnail_file() == "repeat.gcode");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "A rewritten temp path never retires the override that explains it",
                 "[active_print_media][preparing][1339]") {
    // The override exists precisely to map a rewritten temp copy back to the
    // name the user chose. Only this app writes those paths, so one arriving
    // always belongs to the print whose epoch set the override.
    state().print_state().begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
    state().print_state().retire_preparing(helix::PreparingExit::Confirmed);
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

    state().print_state().begin_preparing(helix::PrintJobRef{"committed.gcode", "", ""});
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // Moonraker is still reporting the finished print while ours prepares.
    set_print_filename("previous.gcode");

    REQUIRE(get_display_filename() == "committed");
}

namespace {

std::string three_mf_fixture(const char* name) {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/three_mf/"
                                   : "tests/fixtures/three_mf/";
    std::ifstream in(dir + name, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    return bytes;
}

/// Where @p entry_name's local header sits in @p archive.
uint64_t entry_offset(const std::string& archive, const std::string& entry_name) {
    std::vector<helix::zip::ZipEntry> entries;
    helix::zip::ZipError err = helix::zip::ZipError::None;
    REQUIRE(helix::zip::parse_central_directory(reinterpret_cast<const uint8_t*>(archive.data()),
                                                archive.size(), archive.size(), entries, err));
    const auto* e = helix::three_mf::find_archive_entry(entries, entry_name);
    REQUIRE(e != nullptr);
    return e->lh_offset;
}

} // namespace

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: a .3mf print's thumbnail comes out of its archive",
                 "[ActivePrintMediaManager][async][three_mf]") {
    const std::string bytes = three_mf_fixture("multi_plate.gcode.3mf");
    const std::string archive = "Set_" + std::to_string(::getpid()) + ".gcode.3mf";
    transfers_stub().serve_archive(archive, bytes);

    std::string expect_entry;
    SECTION("the plate a staged extract names") {
        set_print_filename_no_drain(helix::gcode::make_extracted_plate_path(archive, 5));
        expect_entry = "Metadata/plate_5.png";
    }
    SECTION("a .3mf the printer runs itself: its first plate") {
        set_print_filename_no_drain(archive);
        expect_entry = "Metadata/plate_2.png";
    }
    drain();
    REQUIRE(files().has_pending());
    MoonrakerError metadata_err;
    metadata_err.message = "Metadata not available";
    files().fire_error_last(metadata_err);
    drain();

    // No G-code header read: the render is an entry of the archive.
    CHECK(transfers_stub().partial_count() == 0);
    const auto& offsets = transfers_stub().range_offsets();
    CHECK(std::find(offsets.begin(), offsets.end(), entry_offset(bytes, expect_entry)) !=
          offsets.end());
    const std::string thumb = get_thumbnail_path();
    INFO(thumb);
    CHECK(thumb.find(".bin") != std::string::npos);
}

TEST_CASE_METHOD(
    ActivePrintMediaAsyncFixture,
    "ActivePrintMediaManager: the job's materials come from its metadata and reset per file",
    "[ActivePrintMediaManager][active_print_media][door]") {
    set_print_filename_no_drain("abs_part.gcode");
    drain();
    REQUIRE(files().has_pending());

    FileMetadata abs = make_metadata(/*layer_count=*/10);
    abs.filament_type = "ABS";
    files().fire_last(abs);
    drain();
    CHECK(state().print_state().print_materials() == std::vector<std::string>{"ABS"});

    // A different file: its materials are unknown until its own metadata says.
    set_print_filename_no_drain("next_job.gcode");
    drain();
    REQUIRE(files().has_pending());
    CHECK(state().print_state().print_materials().empty());
}
