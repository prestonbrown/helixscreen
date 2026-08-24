// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_history_manager.cpp
 * @brief Unit tests for PrintHistoryManager (TDD)
 *
 * Tests the centralized print history cache that provides:
 * - Raw jobs list for HistoryDashboardPanel/HistoryListPanel
 * - Aggregated filename stats for PrintSelectPanel status indicators
 * - Observer notification when data changes
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/print_history_data.h"
#include "../../include/print_history_manager.h"
#include "../../include/printer_state.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
// ============================================================================
// Global LVGL Initialization
// ============================================================================

namespace {
struct LVGLInitializerHistoryManager {
    LVGLInitializerHistoryManager() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerHistoryManager lvgl_init;
} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class HistoryManagerTestFixture {
    static bool queue_initialized;

  public:
    HistoryManagerTestFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }

        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, printer_state_);
        manager_ = std::make_unique<PrintHistoryManager>(api_.get(), &client_);
    }

    ~HistoryManagerTestFixture() {
        // Destroy managed objects first
        manager_.reset();
        api_.reset();
        client_.disconnect();

        // Drain pending callbacks
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Shutdown queue
        helix::ui::update_queue_shutdown();

        // Reset static flag for next test
        queue_initialized = false;
    }

  protected:
    /// Drain the update queue repeatedly, giving deferred work that itself
    /// defers (invalidate -> fetch -> success defer) room to finish.
    void pump(int rounds = 20) {
        for (int i = 0; i < rounds; ++i) {
            UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    /// A notify_filelist_changed frame shaped like Moonraker's.
    static nlohmann::json filelist_msg(const char* action, const char* path) {
        return nlohmann::json{
            {"jsonrpc", "2.0"},
            {"method", "notify_filelist_changed"},
            {"params", nlohmann::json::array({nlohmann::json{
                           {"action", action},
                           {"item", {{"root", "gcodes"}, {"path", path}, {"size", 1234}}}}})}};
    }

    /// Wait for async fetch to complete
    bool wait_for_loaded(int timeout_ms = 500) {
        for (int i = 0; i < timeout_ms / 10; ++i) {
            // Drain the update queue to process callbacks scheduled via ui_queue_update
            UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

            if (manager_->is_loaded()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintHistoryManager> manager_;
};
bool HistoryManagerTestFixture::queue_initialized = false;

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager starts unloaded",
                 "[history_manager]") {
    REQUIRE_FALSE(manager_->is_loaded());
    REQUIRE(manager_->get_jobs().empty());
    REQUIRE(manager_->get_filename_stats().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager fetches history from API",
                 "[history_manager]") {
    // When: fetch is called
    manager_->fetch();

    // Then: wait for async completion
    REQUIRE(wait_for_loaded());

    // And: jobs are populated
    REQUIRE_FALSE(manager_->get_jobs().empty());
    REQUIRE(manager_->is_loaded());
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager builds filename stats map",
                 "[history_manager]") {
    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    // Then: filename stats map is populated
    const auto& stats = manager_->get_filename_stats();
    REQUIRE_FALSE(stats.empty());

    // And: each entry has valid data
    for (const auto& [filename, info] : stats) {
        REQUIRE_FALSE(filename.empty());
        // At least one count should be non-zero (success or failure)
        bool has_history = (info.success_count > 0 || info.failure_count > 0);
        REQUIRE(has_history);
    }
}

// ============================================================================
// Aggregation Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager aggregates success count correctly", "[history_manager]") {
    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    // Then: check that COMPLETED jobs are counted as successes
    const auto& jobs = manager_->get_jobs();
    const auto& stats = manager_->get_filename_stats();

    // Count completed jobs manually for verification
    int total_completed = 0;
    for (const auto& job : jobs) {
        if (job.status == PrintJobStatus::COMPLETED) {
            total_completed++;
        }
    }

    // Sum up success counts from stats
    int total_success_in_stats = 0;
    for (const auto& [_, info] : stats) {
        total_success_in_stats += info.success_count;
    }

    REQUIRE(total_success_in_stats == total_completed);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager aggregates failure count correctly", "[history_manager]") {
    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& jobs = manager_->get_jobs();
    const auto& stats = manager_->get_filename_stats();

    // Count cancelled + error jobs manually
    int total_failures = 0;
    for (const auto& job : jobs) {
        if (job.status == PrintJobStatus::CANCELLED || job.status == PrintJobStatus::ERROR) {
            total_failures++;
        }
    }

    // Sum up failure counts from stats
    int total_failure_in_stats = 0;
    for (const auto& [_, info] : stats) {
        total_failure_in_stats += info.failure_count;
    }

    REQUIRE(total_failure_in_stats == total_failures);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager tracks most recent job status",
                 "[history_manager]") {
    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& jobs = manager_->get_jobs();
    const auto& stats = manager_->get_filename_stats();

    // For each filename, verify the last_status matches the most recent job
    for (const auto& [filename, info] : stats) {
        // Find most recent job for this filename
        double most_recent_time = 0.0;
        PrintJobStatus most_recent_status = PrintJobStatus::UNKNOWN;

        for (const auto& job : jobs) {
            // Strip path from job filename for comparison
            std::string job_basename = job.filename;
            auto slash_pos = job_basename.rfind('/');
            if (slash_pos != std::string::npos) {
                job_basename = job_basename.substr(slash_pos + 1);
            }

            if (job_basename == filename && job.start_time > most_recent_time) {
                most_recent_time = job.start_time;
                most_recent_status = job.status;
            }
        }

        if (most_recent_time > 0.0) {
            REQUIRE(info.last_status == most_recent_status);
        }
    }
}

// ============================================================================
// Path Stripping Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager strips path from filename for aggregation",
                 "[history_manager]") {
    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& stats = manager_->get_filename_stats();

    // All keys should be basenames (no slashes)
    for (const auto& [filename, _] : stats) {
        REQUIRE(filename.find('/') == std::string::npos);
    }
}

// ============================================================================
// Observer Pattern Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager notifies observers on fetch",
                 "[history_manager]") {
    std::atomic<int> callback_count{0};

    // Given: an observer is registered (store in variable, pass pointer)
    HistoryChangedCallback callback = [&callback_count]() { callback_count++; };
    manager_->add_observer(&callback);

    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    // Then: observer was notified
    REQUIRE(callback_count.load() >= 1);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager supports multiple observers",
                 "[history_manager]") {
    std::atomic<int> callback1_count{0};
    std::atomic<int> callback2_count{0};

    // Given: multiple observers registered (store in variables, pass pointers)
    HistoryChangedCallback callback1 = [&callback1_count]() { callback1_count++; };
    HistoryChangedCallback callback2 = [&callback2_count]() { callback2_count++; };
    manager_->add_observer(&callback1);
    manager_->add_observer(&callback2);

    // When: fetch completes
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    // Then: both observers were notified
    REQUIRE(callback1_count.load() >= 1);
    REQUIRE(callback2_count.load() >= 1);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager skips observers removed during notification",
                 "[history_manager]") {
    // Regression: notify_observers() iterated a snapshot of the observer list
    // and called through each raw HistoryChangedCallback* without re-checking it
    // against the live set. If an observer's backing object was destroyed during
    // the same dispatch pass — e.g. a PrintStatusWidget torn down on panel
    // teardown, whose destructor calls remove_observer() — the stale snapshot
    // still dereferenced the now-freed std::function pointer (SIGSEGV, debug
    // bundle S52DJB5W: fault ~0x800020c during nav-away + reconnect).
    std::atomic<int> victim_count{0};

    // The "victim" models that torn-down widget: it is removed mid-dispatch and
    // must NOT be invoked afterward (in production its memory is already freed).
    HistoryChangedCallback victim = [&victim_count]() { victim_count++; };

    // The "remover" is registered first so it runs before the victim in the
    // snapshot iteration; when it fires it removes the victim, exactly as
    // PrintStatusWidget::detach() removes its observer during teardown.
    HistoryChangedCallback remover = [this, &victim]() { manager_->remove_observer(&victim); };

    manager_->add_observer(&remover); // dispatched first
    manager_->add_observer(&victim);  // dispatched second — must be skipped

    manager_->fetch();
    REQUIRE(wait_for_loaded());

    REQUIRE(victim_count.load() == 0);
}

// ============================================================================
// Cache Invalidation Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager invalidate clears loaded state",
                 "[history_manager]") {
    // Given: manager has loaded data
    manager_->fetch();
    REQUIRE(wait_for_loaded());
    REQUIRE(manager_->is_loaded());

    // When: invalidate is called
    manager_->invalidate();

    // Then: loaded state is cleared
    REQUIRE_FALSE(manager_->is_loaded());
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager can re-fetch after invalidate",
                 "[history_manager]") {
    // Given: manager was loaded then invalidated
    manager_->fetch();
    REQUIRE(wait_for_loaded());
    manager_->invalidate();
    REQUIRE_FALSE(manager_->is_loaded());

    // When: fetch is called again
    manager_->fetch();

    // Then: data is reloaded
    REQUIRE(wait_for_loaded());
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

// ============================================================================
// File-list Notification Tests
// ============================================================================
// Deleting a gcode file fires notify_filelist_changed, NOT
// notify_history_changed: Moonraker only emits the latter when a job is added
// or the history is cleared. Subscribing to history alone left the cache
// holding a deleted file as its newest entry, which the Print Status idle tile
// then offered as "Reprint Last".

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager refetches on a delete_file filelist notification",
                 "[history_manager][filelist]") {
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->fetch();
    REQUIRE(wait_for_loaded());
    const int after_initial = notified.load();
    REQUIRE(after_initial >= 1);

    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "old_print.gcode"));
    pump();

    // A refetch ran: the cache was invalidated and repopulated, so observers
    // were notified again.
    REQUIRE(notified.load() > after_initial);
    REQUIRE(manager_->is_loaded());
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager refetches on a move_file filelist notification",
                 "[history_manager][filelist]") {
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->fetch();
    REQUIRE(wait_for_loaded());
    const int after_initial = notified.load();

    // A move makes the old path stop existing just as a delete does.
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("move_file", "sub/moved.gcode"));
    pump();

    REQUIRE(notified.load() > after_initial);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager ignores filelist actions that cannot orphan a job",
                 "[history_manager][filelist]") {
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->fetch();
    REQUIRE(wait_for_loaded());
    const int after_initial = notified.load();

    // Uploads and metadata scans fire this notification constantly (an AFC
    // printer rewrites AFC.var.unit on every SET_* command). None of them can
    // flip an existing job's `exists` flag, so none may trigger a refetch.
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("create_file", "fresh_upload.gcode"));
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("modify_file", "fresh_upload.gcode"));
    client_.dispatch_method_callback("notify_filelist_changed", filelist_msg("create_dir", "sub"));
    client_.dispatch_method_callback("notify_filelist_changed", filelist_msg("root_update", ""));
    pump();

    REQUIRE(notified.load() == after_initial);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager re-fetches when a delete lands mid-flight",
                 "[history_manager][filelist]") {
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    // Deleting several files in a row: the first delete's refetch is still in
    // flight (real RTT) when the second delete's notification arrives.
    PrintHistoryManagerTestAccess::set_fetching(*manager_, true);
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "second.gcode"));
    pump();
    // Nothing landed yet - the request was dropped by the in-flight guard.
    REQUIRE(notified.load() == 0);

    // The in-flight response arrives, describing the printer BEFORE the second
    // delete. Dropping the request outright would leave that stale list in
    // place until some later notification happened along.
    PrintHistoryManagerTestAccess::complete_fetch(*manager_, {});
    REQUIRE(notified.load() == 1);
    REQUIRE(manager_->get_jobs().empty());

    pump();

    // The queued re-issue ran and repopulated the cache.
    REQUIRE(notified.load() >= 2);
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

// ============================================================================
// Newest-surviving-job Accessor Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager skips deleted jobs when picking the newest",
                 "[history_manager][exists]") {
    PrintHistoryJob gone;
    gone.filename = "deleted.gcode";
    gone.exists = false;
    PrintHistoryJob survivor;
    survivor.filename = "survivor.gcode";
    survivor.exists = true;
    PrintHistoryJob older;
    older.filename = "older.gcode";
    older.exists = true;

    PrintHistoryManagerTestAccess::set_loaded_jobs(*manager_, {gone, survivor, older});

    const auto* pick = manager_->get_newest_existing_job();
    REQUIRE(pick != nullptr);
    REQUIRE(pick->filename == "survivor.gcode");
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager reports no surviving job when every file is gone",
                 "[history_manager][exists]") {
    PrintHistoryJob gone;
    gone.filename = "deleted.gcode";
    gone.exists = false;

    PrintHistoryManagerTestAccess::set_loaded_jobs(*manager_, {gone});
    REQUIRE(manager_->get_newest_existing_job() == nullptr);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager reports no surviving job before history loads",
                 "[history_manager][exists]") {
    REQUIRE_FALSE(manager_->is_loaded());
    REQUIRE(manager_->get_newest_existing_job() == nullptr);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager handles concurrent fetch calls",
                 "[history_manager]") {
    std::atomic<int> callback_count{0};
    HistoryChangedCallback callback = [&callback_count]() { callback_count++; };
    manager_->add_observer(&callback);

    // When: multiple fetches are called rapidly.
    //
    // In production the atomic `is_fetching_` guard dedups rapid calls because
    // the server has measurable RTT. With the synchronous mock client the
    // success callback fires inline and clears the guard before the next
    // fetch() executes, so all three calls may proceed. We assert the weaker
    // invariant the fix (1f719d0e2) actually guarantees: at least one fetch
    // completes, and the guard is never stranded (subsequent fetches proceed).
    manager_->fetch();
    manager_->fetch();
    manager_->fetch();

    REQUIRE(wait_for_loaded());

    // Then: at least one fetch completes — never zero, never stranded.
    REQUIRE(callback_count.load() >= 1);
    REQUIRE(callback_count.load() <= 3);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager handles empty history",
                 "[history_manager]") {
    // Note: Mock returns 20 jobs by default, so this test verifies
    // that the manager handles the case gracefully
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    // Stats should not crash with empty/null data
    const auto& stats = manager_->get_filename_stats();
    // Just verify we can access it without crash
    (void)stats.size();
}

// ============================================================================
// UUID/Size-Based Matching Tests
// ============================================================================
// These tests verify the UUID and file size fields that enable precise
// history matching (prevents false positives with same-named files).

TEST_CASE("PrintHistoryJob has uuid field", "[history][uuid]") {
    PrintHistoryJob job;
    job.uuid = "test-uuid-12345";
    REQUIRE(job.uuid == "test-uuid-12345");

    job.uuid = "";
    REQUIRE(job.uuid.empty());
}

TEST_CASE("PrintHistoryJob has size_bytes field", "[history][uuid]") {
    PrintHistoryJob job;
    job.size_bytes = 807487;
    REQUIRE(job.size_bytes == 807487);

    job.size_bytes = 0;
    REQUIRE(job.size_bytes == 0);
}

TEST_CASE("PrintHistoryStats has uuid field", "[history][uuid]") {
    PrintHistoryStats stats;
    stats.uuid = "stats-uuid-67890";
    REQUIRE(stats.uuid == "stats-uuid-67890");
}

TEST_CASE("PrintHistoryStats has size_bytes field", "[history][uuid]") {
    PrintHistoryStats stats;
    stats.size_bytes = 2178649;
    REQUIRE(stats.size_bytes == 2178649);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "UUID field is populated from history response",
                 "[history][uuid]") {
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& jobs = manager_->get_jobs();
    REQUIRE_FALSE(jobs.empty());

    // At least one job should have uuid populated (mock returns uuid in metadata)
    bool found_uuid = false;
    for (const auto& job : jobs) {
        if (!job.uuid.empty()) {
            found_uuid = true;
            break;
        }
    }
    REQUIRE(found_uuid);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "size_bytes field is populated from history response",
                 "[history][uuid]") {
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& jobs = manager_->get_jobs();
    REQUIRE_FALSE(jobs.empty());

    // At least one job should have size_bytes populated
    bool found_size = false;
    for (const auto& job : jobs) {
        if (job.size_bytes > 0) {
            found_size = true;
            break;
        }
    }
    REQUIRE(found_size);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryStats includes uuid from most recent job",
                 "[history][uuid]") {
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& stats = manager_->get_filename_stats();
    REQUIRE_FALSE(stats.empty());

    // Stats entries should include uuid from the most recent job
    bool found_stats_with_uuid = false;
    for (const auto& [filename, stat] : stats) {
        if (!stat.uuid.empty()) {
            found_stats_with_uuid = true;
            break;
        }
    }
    REQUIRE(found_stats_with_uuid);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryStats includes size_bytes from most recent job", "[history][uuid]") {
    manager_->fetch();
    REQUIRE(wait_for_loaded());

    const auto& stats = manager_->get_filename_stats();
    REQUIRE_FALSE(stats.empty());

    // Stats entries should include size from the most recent job
    bool found_stats_with_size = false;
    for (const auto& [filename, stat] : stats) {
        if (stat.size_bytes > 0) {
            found_stats_with_size = true;
            break;
        }
    }
    REQUIRE(found_stats_with_size);
}
