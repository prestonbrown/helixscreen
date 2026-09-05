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
#include "../../include/moonraker_history_api.h"
#include "../../include/json_utils.h"
#include "../../include/print_history_data.h"
#include "../../include/print_history_manager.h"
#include "../../include/print_history_parse.h"
#include "../../include/printer_state.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../test_helpers/history_call_counting_api.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
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
        api_ = std::make_unique<helix::HistoryCallCountingMoonrakerAPI>(client_, printer_state_);
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

    /// Install `jobs` as a cache that is live and not stale.
    ///
    /// Drains first: the connection-staleness observer registered when the
    /// manager was constructed is queued with the socket down, and it stales
    /// any cache it finds when it runs. Then brings the socket up, so nothing
    /// stales the cache out from under the test.
    void install_live_cache(std::vector<PrintHistoryJob> jobs,
                            HistoryScope scope = HistoryScope::COMPLETE,
                            int requested = PrintHistoryManager::kCompleteJobLimit) {
        pump(3);
        set_connected();
        pump(3);
        PrintHistoryManagerTestAccess::set_loaded_jobs(*manager_, std::move(jobs), scope,
                                                       requested);
    }

    /// Put the printer state on a live socket. The staleness watcher stales
    /// the cache whenever the socket is down, which is not the state a printer
    /// announcing a finished job is in.
    void set_connected() {
        lv_subject_set_int(printer_state_.get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::CONNECTED));
    }

    /// Let a debounced refetch come due: advances LVGL's clock past the quiet
    /// period, runs the timer, and drains what it posts.
    void pump_debounce() {
        // wait_ms advances LVGL's tick in slices bounded by real time, so one
        // call lands short of its nominal duration. Drive the tick past the
        // quiet period instead of trusting a single wait.
        const uint32_t deadline =
            lv_tick_get() + PrintHistoryManager::kInvalidationDebounceMs + 20;
        while (lv_tick_get() < deadline) {
            UITest::wait_ms(50);
        }
        pump();
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

            if (manager_->is_loaded(HistoryScope::RECENT)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<helix::HistoryCallCountingMoonrakerAPI> api_;
    std::unique_ptr<PrintHistoryManager> manager_;
};
bool HistoryManagerTestFixture::queue_initialized = false;

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager starts unloaded",
                 "[history_manager]") {
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE(manager_->get_jobs().empty());
    REQUIRE(manager_->get_filename_stats().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager fetches history from API",
                 "[history_manager]") {
    // When: fetch is called
    manager_->fetch(HistoryScope::COMPLETE);

    // Then: wait for async completion
    REQUIRE(wait_for_loaded());

    // And: jobs are populated
    REQUIRE_FALSE(manager_->get_jobs().empty());
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager builds filename stats map",
                 "[history_manager]") {
    // When: fetch completes
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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

    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());

    REQUIRE(victim_count.load() == 0);
}

// ============================================================================
// Cache Invalidation Tests
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager invalidate clears loaded state",
                 "[history_manager]") {
    // Given: manager has loaded data
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));

    // When: invalidate is called
    manager_->invalidate();

    // Then: loaded state is cleared
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager can re-fetch after invalidate",
                 "[history_manager]") {
    // Given: manager was loaded then invalidated
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());
    manager_->invalidate();
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    // When: fetch is called again
    manager_->fetch(HistoryScope::COMPLETE);

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

    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());
    const int after_initial = notified.load();
    REQUIRE(after_initial >= 1);

    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "old_print.gcode"));
    pump_debounce();

    // A refetch ran: the cache was invalidated and repopulated, so observers
    // were notified again.
    REQUIRE(notified.load() > after_initial);
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager refetches on a move_file filelist notification",
                 "[history_manager][filelist]") {
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());
    const int after_initial = notified.load();

    // A move makes the old path stop existing just as a delete does.
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("move_file", "sub/moved.gcode"));
    pump_debounce();

    REQUIRE(notified.load() > after_initial);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager ignores filelist actions that cannot orphan a job",
                 "[history_manager][filelist]") {
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->fetch(HistoryScope::COMPLETE);
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
    pump_debounce();

    REQUIRE(notified.load() == after_initial);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager re-fetches when a delete lands mid-flight",
                 "[history_manager][filelist]") {
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());

    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    // Deleting several files in a row: the first delete's refetch is still in
    // flight (real RTT) when the second delete's notification arrives.
    PrintHistoryManagerTestAccess::set_fetching(*manager_, true);
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "second.gcode"));
    pump_debounce();
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
// Lazy load vs invalidation
// ============================================================================
//
// fetch() and ensure_loaded() are two intents behind one request. fetch() means
// "the cached list is wrong"; ensure_loaded() means "I need it populated, by
// whoever". Collapsing them cost a real double fetch: three panels ask for
// history during startup, all of them landed while the first request was still
// out, and every one was read as an invalidation. On an AD5X the 500-job list
// took 10.4s and ~800 KB, then went straight out again for nothing (bundles
// MG34LYR4 / VXYB9JPQ, v0.99.116).

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager does not re-fetch for lazy loads that land mid-flight",
                 "[history_manager]") {
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());
    manager_->invalidate();

    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    // Startup: the first request is still out (real RTT) when the other panels
    // activate and each asks for history.
    PrintHistoryManagerTestAccess::set_fetching(*manager_, true);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    pump();
    REQUIRE(notified.load() == 0);

    // The in-flight response lands. It serves all three callers, so nothing
    // further is owed.
    PrintHistoryManagerTestAccess::complete_fetch(*manager_, {});
    REQUIRE(notified.load() == 1);
    REQUIRE(manager_->get_jobs().empty());

    pump();

    // No second request went out. Through fetch() this was 2 notifications and
    // a repopulated cache, i.e. the whole list pulled twice.
    REQUIRE(notified.load() == 1);
    REQUIRE(manager_->get_jobs().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager joins a load whose response is already delivered",
                 "[history_manager]") {
    // A response arrives on the WebSocket thread and its handler is posted to
    // the update queue; only the main thread draining that queue populates the
    // cache. In between, is_fetching_ is already released and is_loaded_ is
    // still false, and on a 2-core board painting the home panel that gap runs
    // to hundreds of milliseconds - long enough for the rest of the startup
    // burst to land in it. The mock client answers inline, so ensure_loaded()
    // returns with the manager sitting in exactly that state.
    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == 1);
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    // Every other panel activating asks for history while that response waits.
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == 1);

    pump();

    // One request, one parse, one stats build.
    REQUIRE(api_->history_list_calls() == 1);
    REQUIRE(notified.load() == 1);
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager still re-fetches for an invalidation mid-delivery",
                 "[history_manager]") {
    // Joining belongs to callers that only want the cache populated. fetch()
    // means the cache is wrong, and a response already downloaded describes the
    // printer before whatever said so - it cannot satisfy that caller.
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == 1);
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == 2);

    pump();
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager ensure_loaded fetches when nothing is in flight",
                 "[history_manager]") {
    // The other half of the guard: suppressing the redundant re-issue must not
    // suppress the first request too, or history never loads at all.
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    manager_->ensure_loaded(HistoryScope::COMPLETE);

    REQUIRE(wait_for_loaded());
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager ensure_loaded is a no-op once loaded", "[history_manager]") {
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());

    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    // Every panel activation calls this. A loaded cache must not re-request.
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    pump();

    REQUIRE(notified.load() == 0);
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
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));
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
    manager_->fetch(HistoryScope::COMPLETE);
    manager_->fetch(HistoryScope::COMPLETE);
    manager_->fetch(HistoryScope::COMPLETE);

    REQUIRE(wait_for_loaded());

    // Then: at least one fetch completes — never zero, never stranded.
    REQUIRE(callback_count.load() >= 1);
    REQUIRE(callback_count.load() <= 3);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "PrintHistoryManager handles empty history",
                 "[history_manager]") {
    // Note: Mock returns 20 jobs by default, so this test verifies
    // that the manager handles the case gracefully
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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
    manager_->fetch(HistoryScope::COMPLETE);
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

// ============================================================================
// Staleness across a dropped connection
// ============================================================================
//
// notify_history_changed and notify_filelist_changed only reach a live socket.
// Anything added or deleted while the WebSocket is down is therefore never
// announced, and is_loaded_ keeps claiming the cache is good, so the idle tile
// goes on offering a "Reprint Last" for a job that may no longer exist. The
// manager owns the cache, so it is the manager's job to notice it went deaf.
//
// Marking stale is all that is owed here: invalidate() leaves cached_jobs_
// in place, so nothing blanks mid-outage, and the consumers that already call
// ensure_loaded() on the CONNECTED transition pull a fresh list on the way
// back up.

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager marks the cache stale when the connection drops",
                 "[history_manager]") {
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());

    lv_subject_t* conn = printer_state_.get_printer_connection_state_subject();
    REQUIRE(conn != nullptr);
    lv_subject_set_int(conn, static_cast<int>(ConnectionState::CONNECTED));
    pump();
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));

    lv_subject_set_int(conn, static_cast<int>(ConnectionState::DISCONNECTED));
    pump();

    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));
    // invalidate() marks stale without dropping data, so a mid-outage reader
    // still renders the last known list instead of an empty one.
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager keeps a loaded cache while the connection holds",
                 "[history_manager]") {
    // The other half of the gate. The client's connected fan-out also fires on
    // every Klippy-ready transition, so a FIRMWARE_RESTART re-announces CONNECTED
    // without the socket ever dropping. Nothing was missed, and a 500-job refetch
    // there is exactly the cost this cache exists to avoid.
    manager_->fetch(HistoryScope::COMPLETE);
    REQUIRE(wait_for_loaded());

    lv_subject_t* conn = printer_state_.get_printer_connection_state_subject();
    REQUIRE(conn != nullptr);
    for (int i = 0; i < 3; ++i) {
        lv_subject_set_int(conn, static_cast<int>(ConnectionState::CONNECTED));
        pump();
    }

    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "PrintHistoryManager reloads through ensure_loaded after an invalidate",
                 "[history_manager]") {
    // The delivery flag is what lets ensure_loaded() ride a response whose bytes
    // have landed but whose handler has not run. Once that handler applies the
    // response, is_loaded_ is what answers callers and the flag has to be back
    // down: left raised, the next invalidate leaves ensure_loaded() joining a
    // delivery that is never coming, and the cache never repopulates.
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    pump();
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE(api_->history_list_calls() == 1);

    manager_->invalidate();
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == 2);

    pump();
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE_FALSE(manager_->get_jobs().empty());
}

// ============================================================================
// Fidelity
// ============================================================================
// The cache is populated at two fidelities: a small slice at startup, the whole
// list once a consumer that aggregates over all of history asks. A consumer
// that cannot tell the two apart renders a boot-time slice as the complete
// record, so every query names the scope it is asking about.

TEST_CASE_METHOD(HistoryManagerTestFixture, "A RECENT load asks for far fewer jobs than the list",
                 "[history_manager][fidelity]") {
    manager_->ensure_loaded(HistoryScope::RECENT);
    pump();

    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kRecentJobLimit);
    REQUIRE(PrintHistoryManager::kRecentJobLimit < PrintHistoryManager::kCompleteJobLimit);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A COMPLETE load asks for the whole list",
                 "[history_manager][fidelity]") {
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    pump();

    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "A filled RECENT page does not answer COMPLETE, and escalates",
                 "[history_manager][fidelity]") {
    // A response that filled its page leaves an unknown number of older jobs
    // behind it, which is exactly the state a whole-list consumer must not read
    // as the complete record.
    std::vector<PrintHistoryJob> page;
    for (int i = 0; i < PrintHistoryManager::kRecentJobLimit; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(i);
        job.filename = "a.gcode";
        job.status = PrintJobStatus::COMPLETED;
        job.start_time = 10000.0 - i;
        page.push_back(job);
    }
    install_live_cache(page, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);

    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::COMPLETE));

    const int before = api_->history_list_calls();
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == before + 1);
    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "A short RECENT response holds every job, so it answers COMPLETE",
                 "[history_manager][fidelity]") {
    // Moonraker fills a page up to the limit and stops, so a response shorter
    // than its limit is the whole history. A printer with a short history must
    // not pay a second request to learn that.
    std::vector<PrintHistoryJob> few(3);
    for (size_t i = 0; i < few.size(); ++i) {
        few[i].job_id = "job" + std::to_string(i);
        few[i].filename = "a.gcode";
        few[i].start_time = 10000.0 - static_cast<double>(i);
    }
    install_live_cache(few, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);

    REQUIRE(manager_->is_loaded(HistoryScope::COMPLETE));

    const int before = api_->history_list_calls();
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    REQUIRE(api_->history_list_calls() == before);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A COMPLETE caller does not join a narrower load",
                 "[history_manager][fidelity]") {
    // A RECENT response cannot serve a whole-list caller, so joining it would
    // leave that caller reading a slice and never asking again.
    PrintHistoryManagerTestAccess::set_fetching(*manager_, true, HistoryScope::RECENT);
    const int during = api_->history_list_calls();

    manager_->ensure_loaded(HistoryScope::COMPLETE);
    // Blocked by the in-flight guard, so it lands as a queued re-issue rather
    // than a concurrent request.
    REQUIRE(api_->history_list_calls() == during);

    PrintHistoryManagerTestAccess::complete_fetch(*manager_, {}, HistoryScope::RECENT,
                                                  PrintHistoryManager::kRecentJobLimit);
    pump();
    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A queued re-issue keeps the widest scope asked for",
                 "[history_manager][fidelity]") {
    // One slot, but it carries a scope: a narrow invalidation arriving behind a
    // whole-list request must not be what the single re-issue asks for.
    PrintHistoryManagerTestAccess::set_fetching(*manager_, true);
    manager_->fetch(HistoryScope::COMPLETE);
    manager_->fetch(HistoryScope::RECENT);

    PrintHistoryManagerTestAccess::complete_fetch(*manager_, {}, HistoryScope::RECENT,
                                                  PrintHistoryManager::kRecentJobLimit);
    pump();

    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}

// ============================================================================
// Weekly coverage
// ============================================================================
// PrintStatsWidget aggregates a seven-day window on every update, so a startup
// slice that stops short of it undercounts on a busy printer.

TEST_CASE_METHOD(HistoryManagerTestFixture, "A slice reaching past the window covers it",
                 "[history_manager][coverage]") {
    const double now = 1'000'000.0;
    const double week_ago = now - 7 * 24 * 3600;

    std::vector<PrintHistoryJob> page;
    for (int i = 0; i < PrintHistoryManager::kRecentJobLimit; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(i);
        job.filename = "a.gcode";
        // Spread over a month, so the oldest cached job predates the window.
        job.start_time = now - static_cast<double>(i) * 12 * 3600;
        page.push_back(job);
    }
    install_live_cache(page, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);

    REQUIRE(manager_->covers_since(week_ago));

    const int before = api_->history_list_calls();
    manager_->ensure_covers_since(week_ago);
    REQUIRE(api_->history_list_calls() == before);
}

TEST_CASE_METHOD(HistoryManagerTestFixture,
                 "A busy printer's slice stops short of the window and escalates",
                 "[history_manager][coverage]") {
    const double now = 1'000'000.0;
    const double week_ago = now - 7 * 24 * 3600;

    // Every cached job is from the last three days, so jobs from days 4-7 exist
    // on the printer and are not in the cache.
    std::vector<PrintHistoryJob> page;
    for (int i = 0; i < PrintHistoryManager::kRecentJobLimit; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(i);
        job.filename = "a.gcode";
        job.start_time = now - static_cast<double>(i) * 3600;
        page.push_back(job);
    }
    install_live_cache(page, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);

    REQUIRE_FALSE(manager_->covers_since(week_ago));

    const int before = api_->history_list_calls();
    manager_->ensure_covers_since(week_ago);
    REQUIRE(api_->history_list_calls() == before + 1);
    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A cold cache is covered by loading the slice",
                 "[history_manager][coverage]") {
    REQUIRE_FALSE(manager_->covers_since(0.0));

    manager_->ensure_covers_since(1'000'000.0);
    REQUIRE(api_->history_last_limit() == PrintHistoryManager::kRecentJobLimit);
}

// ============================================================================
// Notification payload
// ============================================================================
// notify_history_changed carries the complete job it announces, including the
// `exists` flag Moonraker recomputes against the file manager. Re-pulling the
// list to learn what the notification already said costs ~714KB of JSON and
// ~3.9MB of DOM per event.

namespace {

/// A notify_history_changed frame shaped like Moonraker's.
nlohmann::json history_msg(const char* action, const nlohmann::json& job) {
    return nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notify_history_changed"},
                          {"params", nlohmann::json::array({nlohmann::json{{"action", action},
                                                                           {"job", job}}})}};
}

nlohmann::json job_payload(const char* job_id, const char* filename, const char* status,
                           double start_time, bool exists) {
    return nlohmann::json{{"job_id", job_id},   {"filename", filename},
                          {"status", status},   {"start_time", start_time},
                          {"end_time", start_time + 60}, {"print_duration", 60.0},
                          {"total_duration", 60.0},      {"filament_used", 1234.0},
                          {"exists", exists}};
}

} // namespace

TEST_CASE_METHOD(HistoryManagerTestFixture, "A finished notification patches the job, no refetch",
                 "[history_manager][patch]") {
    PrintHistoryJob cached;
    cached.job_id = "000001";
    cached.filename = "part.gcode";
    cached.status = PrintJobStatus::IN_PROGRESS;
    cached.start_time = 5000.0;
    cached.exists = true;
    install_live_cache({cached});

    std::atomic<int> notified{0};
    HistoryChangedCallback callback = [&notified]() { notified++; };
    manager_->add_observer(&callback);

    const int before = api_->history_list_calls();
    client_.dispatch_method_callback(
        "notify_history_changed",
        history_msg("finished", job_payload("000001", "part.gcode", "completed", 5000.0, true)));
    pump();

    // The list never went out again.
    REQUIRE(api_->history_list_calls() == before);
    // The cached entry was amended in place rather than duplicated.
    REQUIRE(manager_->get_jobs().size() == 1);
    REQUIRE(manager_->get_jobs()[0].status == PrintJobStatus::COMPLETED);
    // Consumers still hear about it.
    REQUIRE(notified.load() >= 1);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "An added notification inserts the job, no refetch",
                 "[history_manager][patch]") {
    PrintHistoryJob cached;
    cached.job_id = "000001";
    cached.filename = "old.gcode";
    cached.status = PrintJobStatus::COMPLETED;
    cached.start_time = 5000.0;
    cached.exists = true;
    install_live_cache({cached});

    const int before = api_->history_list_calls();
    client_.dispatch_method_callback(
        "notify_history_changed",
        history_msg("added", job_payload("000002", "new.gcode", "in_progress", 9000.0, true)));
    pump();

    REQUIRE(api_->history_list_calls() == before);
    REQUIRE(manager_->get_jobs().size() == 2);
    // Newest first, which is the order get_newest_existing_job() relies on.
    REQUIRE(manager_->get_jobs()[0].job_id == "000002");
    REQUIRE(manager_->get_jobs()[1].job_id == "000001");
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A patched job carries the payload's exists flag",
                 "[history_manager][patch][exists]") {
    // The `exists` flag is recomputed per request, and it is what the idle tile
    // reads to decide whether "Reprint Last" can be offered at all.
    install_live_cache({});

    client_.dispatch_method_callback(
        "notify_history_changed",
        history_msg("finished", job_payload("000009", "gone.gcode", "completed", 7000.0, false)));
    pump();

    REQUIRE(manager_->get_jobs().size() == 1);
    REQUIRE_FALSE(manager_->get_jobs()[0].exists);
    REQUIRE(manager_->get_newest_existing_job() == nullptr);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A patch leaves the cache's fidelity alone",
                 "[history_manager][patch][fidelity]") {
    // One more job neither completes a truncated cache nor truncates a
    // complete one, so a whole-list consumer must not start reading a slice
    // as complete just because a print finished.
    std::vector<PrintHistoryJob> page;
    for (int i = 0; i < PrintHistoryManager::kRecentJobLimit; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(i);
        job.filename = "a.gcode";
        job.start_time = 10000.0 - i;
        page.push_back(job);
    }
    install_live_cache(page, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::COMPLETE));

    client_.dispatch_method_callback(
        "notify_history_changed",
        history_msg("finished", job_payload("000099", "new.gcode", "completed", 99999.0, true)));
    pump();

    REQUIRE(manager_->get_jobs().size() == page.size() + 1);
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::COMPLETE));
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A history notification with no job payload refetches",
                 "[history_manager][patch]") {
    // A payload we cannot apply is the case the full pull still exists for.
    install_live_cache({});
    const int before = api_->history_list_calls();

    client_.dispatch_method_callback(
        "notify_history_changed",
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"method", "notify_history_changed"},
                       {"params", nlohmann::json::array({nlohmann::json{{"action", "finished"}}})}});
    pump_debounce();

    REQUIRE(api_->history_list_calls() > before);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "A history notification on a cold cache fetches",
                 "[history_manager][patch]") {
    // A patch cannot establish which jobs precede the one being announced.
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));
    const int before = api_->history_list_calls();

    client_.dispatch_method_callback(
        "notify_history_changed",
        history_msg("finished", job_payload("000001", "a.gcode", "completed", 5000.0, true)));
    pump_debounce();

    REQUIRE(api_->history_list_calls() > before);
}

// ============================================================================
// Invalidation coalescing
// ============================================================================

TEST_CASE_METHOD(HistoryManagerTestFixture, "A burst of invalidations costs one request",
                 "[history_manager][filelist][debounce]") {
    // A klippy restart fires the config-backup move_file and the restart's own
    // history event together, and deleting several files walks the same path
    // once per file.
    manager_->ensure_loaded(HistoryScope::COMPLETE);
    pump();
    const int before = api_->history_list_calls();

    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "a.gcode"));
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "b.gcode"));
    client_.dispatch_method_callback("notify_filelist_changed", filelist_msg("move_file", "c.gcode"));

    // The cache is stale from the moment the change is known, even though the
    // request that repairs it waits out the quiet period.
    pump(2);
    REQUIRE_FALSE(manager_->is_loaded(HistoryScope::RECENT));

    pump_debounce();
    REQUIRE(api_->history_list_calls() == before + 1);
}

TEST_CASE_METHOD(HistoryManagerTestFixture, "An invalidation refetches at the loaded scope",
                 "[history_manager][filelist][fidelity]") {
    // A refetch that dropped back to the startup slice would silently truncate
    // a cache a panel had escalated to the whole list.
    std::vector<PrintHistoryJob> page;
    for (int i = 0; i < PrintHistoryManager::kRecentJobLimit; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(i);
        job.filename = "a.gcode";
        job.start_time = 10000.0 - i;
        page.push_back(job);
    }

    install_live_cache(page, HistoryScope::RECENT, PrintHistoryManager::kRecentJobLimit);
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "a.gcode"));
    pump_debounce();
    CHECK(api_->history_last_limit() == PrintHistoryManager::kRecentJobLimit);

    install_live_cache(page, HistoryScope::COMPLETE, PrintHistoryManager::kCompleteJobLimit);
    client_.dispatch_method_callback("notify_filelist_changed",
                                     filelist_msg("delete_file", "b.gcode"));
    pump_debounce();
    CHECK(api_->history_last_limit() == PrintHistoryManager::kCompleteJobLimit);
}
