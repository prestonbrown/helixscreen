// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

static bool s_widget_registered = false;

/// Fixture for testing PrintStatusWidget idle thumbnail behavior
class PrintStatusIdleThumbFixture : public LVGLTestFixture {
  public:
    PrintStatusIdleThumbFixture() {
        if (!s_widget_registered) {
            PanelWidgetManager::instance().init_widget_subjects();
            s_widget_registered = true;
        }
        // PrintStatusWidget's static-inline subjects are initialized by its
        // constructor, NOT by init_widget_subjects() — the "print_status" row in
        // panel_widget_registry.cpp carries a null init_subjects hook. Tests that
        // touch those subjects before constructing a widget therefore need this
        // explicitly, the same reason ensure_formatter_for_test() calls it.
        // Idempotent (guarded by the *_initialized_ flags inside).
        PrintStatusWidget::init_static_subjects();
    }

    /// Create minimal mock widget tree matching the XML names
    lv_obj_t* create_mock_print_card(lv_obj_t* parent) {
        lv_obj_t* container = lv_obj_create(parent);

        // Idle state container
        lv_obj_t* idle = lv_obj_create(container);
        lv_obj_set_name(idle, "print_card_idle");

        lv_obj_t* thumb = lv_image_create(idle);
        lv_obj_set_name(thumb, "print_card_thumb");

        lv_obj_t* label = lv_label_create(idle);
        lv_obj_set_name(label, "print_card_label");
        lv_label_set_text(label, "Print Files");

        // Printing state container
        lv_obj_t* printing = lv_obj_create(container);
        lv_obj_set_name(printing, "print_card_printing");

        lv_obj_t* layout = lv_obj_create(printing);
        lv_obj_set_name(layout, "print_card_layout");

        lv_obj_t* thumb_wrap = lv_obj_create(layout);
        lv_obj_set_name(thumb_wrap, "print_card_thumb_wrap");

        lv_obj_t* active_thumb = lv_image_create(thumb_wrap);
        lv_obj_set_name(active_thumb, "print_card_active_thumb");

        lv_obj_t* info = lv_obj_create(layout);
        lv_obj_set_name(info, "print_card_info");

        return container;
    }

    /// Get the image source string from the idle thumbnail widget
    std::string get_idle_thumb_src(lv_obj_t* container) {
        auto* thumb = lv_obj_find_by_name(container, "print_card_thumb");
        if (!thumb)
            return "";
        auto* src = lv_image_get_src(thumb);
        if (!src)
            return "";
        return reinterpret_cast<const char*>(src);
    }

    static constexpr const char* BENCHY_PATH = "A:assets/images/benchy_thumbnail_white.png";
};

// =============================================================================
// Tests: Idle thumbnail falls back to benchy when no history
// =============================================================================

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: idle state shows benchy when no history available",
                 "[print_status_widget][idle_thumb]") {
    // get_print_history_manager() returns nullptr in test environment
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    // Should fall back to benchy since no history manager
    auto src = get_idle_thumb_src(container);
    REQUIRE(src == BENCHY_PATH);

    widget.detach();
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: label stays 'Print Files' in idle state",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    auto* label = lv_obj_find_by_name(container, "print_card_label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == "Print Files");

    widget.detach();
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: attach/detach lifecycle with idle thumb is clean",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    // Multiple attach/detach cycles should not crash
    widget.attach(container, test_screen());
    process_lvgl(200);
    widget.detach();

    widget.attach(container, test_screen());
    process_lvgl(200);
    widget.detach();

    // LVGL processing after detach should be safe
    process_lvgl(200);
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: detach invalidates alive guard for async safety",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    // Verify benchy is shown (no history in test env)
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    widget.detach();

    // After detach, LVGL processing should not crash
    // (validates that alive guard prevents stale async callbacks)
    process_lvgl(200);
}

// =============================================================================
// Regression: attach() must defer the initial idle reset (AD5M SY6JLLKJ)
//
// Calling reset_print_card_to_idle() synchronously from attach() cascades:
//   lv_subject_copy_string(idle_thumb_path) → bind_src observer →
//   lv_image_set_src → update_align → lv_obj_update_layout → grid_update
// which crashed in grid_update reading half-built track data when the parent
// page-grid was still mid-construction (populate_page builds widgets one at a
// time). The fix defers the initial reset via lv_async_call. This test pins
// the deferral invariant: idle_thumb_path_subject_ must NOT be touched
// synchronously during attach().
// =============================================================================
TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: attach() defers initial idle reset (no sync subject notify)",
                 "[print_status_widget][idle_thumb][regression]") {
    auto* subj = PrintStatusWidgetTestAccess::idle_thumb_path_subject();
    // `subj` is the address of a static member, so a null check proves nothing —
    // it is non-null whether or not lv_subject_init_string() has ever run. Assert
    // the subject is actually a live string subject instead: an uninitialized
    // lv_subject_t is zeroed, so type would be 0 (not STRING) and the backing
    // buffer pointer would be null, and lv_subject_copy_string() below would
    // silently no-op.
    REQUIRE(subj->type == LV_SUBJECT_TYPE_STRING);
    REQUIRE(subj->value.pointer != nullptr);
    REQUIRE(subj->size > 0);

    // Seed the subject buffer with a sentinel so we can detect whether
    // reset_print_card_to_idle() (which rewrites to benchy) ran before
    // attach() returned, or only after the next LVGL tick.
    static constexpr const char* SENTINEL = "A:assets/images/__test_seed__.png";
    lv_subject_copy_string(subj, SENTINEL);

    auto current_value = [&]() {
        const char* p = static_cast<const char*>(subj->value.pointer);
        return std::string(p ? p : "");
    };
    REQUIRE(current_value() == SENTINEL);

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    std::string after_attach = current_value();

    // Pump the queue + LVGL timers so the deferred async_call can fire.
    process_lvgl(200);
    std::string after_process = current_value();

    // Invariant: attach() must NOT synchronously call reset_print_card_to_idle.
    // If it does, the bind_src observer on idle_thumb cascades through
    // lv_image_set_src → layout → grid_update on the page grid that
    // populate_page is still building sibling widgets into (AD5M SY6JLLKJ).
    REQUIRE(after_attach == SENTINEL);

    // Sanity: after one LVGL tick the deferred reset has run and rewritten
    // the subject back to benchy (no history available in this fixture).
    REQUIRE(after_process == BENCHY_PATH);

    widget.detach();
}

// =============================================================================
// The idle thumbnail fetch: staleness guard + the shared subject
//
// reset_print_card_to_idle() resolves "the last print's thumbnail" and is
// reachable repeatedly — from attach(), from a print-state change and from a
// history change. Two defects lived in that resolve:
//
//  1. The async fetch carried a lifetime token and nothing else, so two loads
//     for different history heads resolved last-write-wins.
//  2. Only the SYNCHRONOUS paths published to idle_thumb_path_subject_. The
//     async completion wrote the two Library-mode image widgets and skipped the
//     subject, so the detailed-idle hero (which reads it through bind_src)
//     showed a different image from the thumbs beside it.
//
// Both need the resolve to actually reach the fetch, which needs a loaded
// history (for a thumbnail key), a non-null API (the widget bails without one)
// and a cache state that misses the pre-scaled .bin. The fixture below builds
// exactly that; a test that skipped it would assert against the benchy
// early-return and pin nothing.
// =============================================================================

namespace {

/// Smallest PNG the cache and the processor will both accept: a 10x10 solid
/// square, 75 bytes. Same bytes as tests/unit/test_thumbnail_cache_request.cpp.
// clang-format off
const std::vector<uint8_t> TINY_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x02, 0x50, 0x58, 0xEA, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x68, 0x70, 0x50, 0xC0,
    0x83, 0x18, 0x46, 0xA5, 0xB1, 0x21, 0x00, 0x24, 0x51, 0x57, 0x81, 0xF7,
    0xEC, 0xA3, 0x23, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};
// clang-format on

/// Drop a raw PNG into the cache's PNG slot for `key` WITHOUT a pre-scaled
/// .bin. That is the one cache state that sends fetch_optimized down its
/// "PNG is cached, queue the pre-scale" branch, which resolves on the processor
/// pool and delivers through the UpdateQueue — i.e. asynchronously, which is
/// the whole point.
void plant_cached_png(const ThumbnailCache& cache, const std::string& key) {
    const std::string path = cache.get_cache_path(key);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(TINY_PNG.data()),
              static_cast<std::streamsize>(TINY_PNG.size()));
    out.close();
    REQUIRE(std::filesystem::exists(path));
}

/// The pre-scaled lookup the widget performs, freshness-blind: source_modified
/// stays 0, so this answers "is the .bin present at all" regardless of mtime.
std::string cached_bin(const ThumbnailCache& cache, const std::string& key,
                       const helix::ThumbnailTarget& target) {
    ThumbnailRequest req;
    req.key = key;
    req.target = target;
    return cache.get_if_cached(req);
}

/// Push a cached artifact's mtime far into the past, so any positive
/// source_modified is newer than it. Takes the LVGL path the cache and the
/// processor hand back ("A:/abs/path") and strips the prefix, the same way
/// ThumbnailCache does before stat'ing.
void backdate(const std::string& lvgl_path) {
    REQUIRE(ThumbnailCache::is_lvgl_path(lvgl_path));
    const std::string fs_path = lvgl_path.substr(2);
    REQUIRE(std::filesystem::exists(fs_path));
    // Expressed in the filesystem clock's own terms — converting between it and
    // system_clock is exactly the fiddly step this test does not need.
    std::filesystem::last_write_time(fs_path, std::filesystem::file_time_type::clock::now() -
                                                  std::chrono::hours(24 * 365 * 20));
}

} // namespace

/// PrintStatusIdleThumbFixture with a loaded print history installed, so
/// get_last_print_thumbnail_path() returns a real key instead of "".
///
/// The API is deliberately left null at construction: attach() defers an idle
/// reset, and with no API that reset stops right after painting benchy. Each
/// test installs the API itself at the moment it wants a fetch to happen, so
/// the only loads in flight are the ones the test started.
class PrintStatusIdleThumbHistoryFixture : public PrintStatusIdleThumbFixture {
  public:
    PrintStatusIdleThumbHistoryFixture()
        : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, get_printer_state());
        history_ = std::make_unique<PrintHistoryManager>(api_.get(), &client_);
        history_->fetch();
        wait_until([this]() { return history_->is_loaded(); }, 3000);
        set_print_history_manager(history_.get());
        set_moonraker_api(nullptr);
    }

    ~PrintStatusIdleThumbHistoryFixture() {
        set_print_history_manager(nullptr);
        set_moonraker_api(nullptr);
        // Join the pool and drain whatever it produced while the manager is
        // still alive; a callback left queued would run against dead state.
        for (int i = 0; i < 20; ++i) {
            helix::ThumbnailProcessor::instance().wait_for_completion();
            helix::ui::UpdateQueue::instance().drain();
        }
        history_.reset();
        api_.reset();
        client_.disconnect();
    }

    /// The cache key the widget will resolve to for the most recent job.
    std::string head_thumbnail_key() {
        const auto& jobs = history_->get_jobs();
        REQUIRE_FALSE(jobs.empty());
        const auto& job = jobs.front();
        // One entry means the widget's size-based "smallest adequate, else
        // largest" pick is unambiguous, so the test key is the widget's key.
        REQUIRE(job.thumbnails.size() == 1);
        REQUIRE_FALSE(job.thumbnails.front().relative_path.empty());
        return job.thumbnails.front().relative_path;
    }

    /// The source mtime the widget must validate the cached render against —
    /// read off the same history head head_thumbnail_key() resolves from.
    double head_job_modified() {
        const auto& jobs = history_->get_jobs();
        REQUIRE_FALSE(jobs.empty());
        return jobs.front().modified;
    }

    /// Resize the idle thumb and report the pre-scale target the widget will
    /// derive from it. Computed with the production function off the MEASURED
    /// size, not hardcoded, so it cannot drift from what the widget picks.
    helix::ThumbnailTarget size_thumb(lv_obj_t* container, int w, int h) {
        auto* thumb = lv_obj_find_by_name(container, "print_card_thumb");
        REQUIRE(thumb != nullptr);
        lv_obj_set_size(thumb, w, h);
        lv_obj_update_layout(thumb);
        return helix::ThumbnailProcessor::get_target_for_resolution(
            lv_obj_get_width(thumb), lv_obj_get_height(thumb), helix::ThumbnailSize::Detail);
    }

    std::string subject_value() {
        auto* subj = PrintStatusWidgetTestAccess::idle_thumb_path_subject();
        const char* p = static_cast<const char*>(subj->value.pointer);
        return std::string(p ? p : "");
    }

    /// Join the pool and drain until `done`, or give up.
    void settle_thumb(const std::function<bool()>& done) {
        for (int i = 0; i < 40 && !done(); ++i) {
            helix::ThumbnailProcessor::instance().wait_for_completion();
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Wait until the in-flight pre-scale has finished AND parked its result in
    /// the UpdateQueue, without draining it. That parked callback is the stale
    /// completion the guard has to drop.
    bool park_pool_result() {
        auto& q = helix::ui::UpdateQueue::instance();
        for (int i = 0; i < 400; ++i) {
            helix::ThumbnailProcessor::instance().wait_for_completion();
            if (!helix::ui::UpdateQueueTestAccess::queue_empty(q)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

  protected:
    MoonrakerClientMock client_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintHistoryManager> history_;
};

// Defect 2: the async completion published to the two Library-mode widgets but
// not to idle_thumb_path_subject_, so the detailed-idle hero kept showing the
// benchy placeholder the synchronous path had left there.
TEST_CASE_METHOD(PrintStatusIdleThumbHistoryFixture,
                 "PrintStatusWidget: an async idle thumbnail load publishes to the shared subject",
                 "[print_status_widget][idle_thumb][thumbnail]") {
    auto& cache = get_thumbnail_cache();
    const std::string key = head_thumbnail_key();
    cache.invalidate(key);

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());
    widget.attach(container, test_screen());
    process_lvgl(200); // deferred reset #0 — no API installed yet, so inert
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    const helix::ThumbnailTarget target = size_thumb(container, 100, 100);
    plant_cached_png(cache, key);
    // No .bin, so the resolve cannot short-circuit on the synchronous cache hit
    // — this is what forces it onto the async path under test.
    REQUIRE(cached_bin(cache, key, target).empty());

    set_moonraker_api(api_.get());
    PrintStatusWidgetTestAccess::reset_to_idle(widget);

    // Benchy is painted as a placeholder while the fetch runs. Pinned so the
    // final assertion cannot pass by the subject simply never having moved.
    REQUIRE(subject_value() == BENCHY_PATH);

    settle_thumb([this]() { return subject_value() != BENCHY_PATH; });

    const std::string produced = cached_bin(cache, key, target);
    REQUIRE_FALSE(produced.empty()); // the pre-scale really ran
    CHECK(get_idle_thumb_src(container) == produced);
    CHECK(subject_value() == produced);

    widget.detach();
    cache.invalidate(key);
}

// Defect 1: the fetch carried no generation guard, so a load for the previous
// history head could land after a newer resolve had already painted and
// overwrite it. Load A resolves through the pool and is parked mid-flight;
// load B then supersedes it and resolves synchronously from cache. Draining A
// afterwards must change nothing.
TEST_CASE_METHOD(PrintStatusIdleThumbHistoryFixture,
                 "PrintStatusWidget: a superseded idle thumbnail load cannot overwrite a newer one",
                 "[print_status_widget][idle_thumb][thumbnail]") {
    auto& cache = get_thumbnail_cache();
    const std::string key = head_thumbnail_key();
    cache.invalidate(key);

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());
    widget.attach(container, test_screen());
    process_lvgl(200);
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    // Load A — 100px thumb, so a 200x200 detail target, PNG only.
    const helix::ThumbnailTarget target_a = size_thumb(container, 100, 100);
    plant_cached_png(cache, key);
    REQUIRE(cached_bin(cache, key, target_a).empty());
    set_moonraker_api(api_.get());

    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    REQUIRE(helix::ui::UpdateQueueTestAccess::queue_empty(queue));

    PrintStatusWidgetTestAccess::reset_to_idle(widget);
    REQUIRE(subject_value() == BENCHY_PATH);

    REQUIRE(park_pool_result());
    const std::string path_a = cached_bin(cache, key, target_a);
    REQUIRE_FALSE(path_a.empty());

    // Load B — 900px thumb, so a 400x400 target, pre-scaled up front so this
    // resolve hits the synchronous cache branch and publishes immediately.
    const helix::ThumbnailTarget target_b = size_thumb(container, 900, 900);
    REQUIRE(target_b.width != target_a.width);
    const auto planted_b =
        helix::ThumbnailProcessor::instance().process_sync(TINY_PNG, key, target_b);
    REQUIRE(planted_b.success);
    const std::string path_b = planted_b.output_path;
    REQUIRE(path_b != path_a);

    PrintStatusWidgetTestAccess::reset_to_idle(widget);
    REQUIRE(subject_value() == path_b);
    REQUIRE(get_idle_thumb_src(container) == path_b);

    // A's parked completion lands now. Two drains: the fetch callback itself,
    // then the tok.defer hop it schedules.
    queue.drain();
    queue.drain();

    CHECK(subject_value() == path_b);
    CHECK(get_idle_thumb_src(container) == path_b);

    widget.detach();
    cache.invalidate(key);
}

// The SYNCHRONOUS probe at the top of the idle resolve asked the cache
// get_if_optimized(key, target) with no source_modified, so mtime validation
// never ran on it — the same defect the detail-view fetch had. Re-slice a model
// under the same filename and the idle card serves the previous slice's render
// forever, because the probe answers before the guarded async fetch is ever
// reached.
//
// The two halves are a pair. The first plants a FRESH .bin and pins that the
// resolve reaches the probe and publishes what it finds; without it, "benchy is
// shown" in the second half would hold for the trivial reason that the resolve
// bailed out early. The second backdates that same .bin behind the history
// entry's source mtime and requires it to be refused.
TEST_CASE_METHOD(PrintStatusIdleThumbHistoryFixture,
                 "PrintStatusWidget: the idle probe refuses a cache entry older than its source",
                 "[print_status_widget][idle_thumb][thumbnail]") {
    auto& cache = get_thumbnail_cache();
    const std::string key = head_thumbnail_key();
    cache.invalidate(key);

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());
    widget.attach(container, test_screen());
    process_lvgl(200); // deferred reset #0 — no API installed, so inert
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    // The history head carries the source mtime the probe has to compare
    // against. Without it there is nothing to validate and this test is inert.
    const double source_modified = head_job_modified();
    REQUIRE(source_modified > 0.0);

    const helix::ThumbnailTarget target = size_thumb(container, 100, 100);
    const auto planted = helix::ThumbnailProcessor::instance().process_sync(TINY_PNG, key, target);
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    // --- Fresh: written now, so newer than the history entry's source. ---
    PrintStatusWidgetTestAccess::reset_to_idle(widget);
    REQUIRE(subject_value() == planted.output_path);
    REQUIRE(get_idle_thumb_src(container) == planted.output_path);

    // --- Stale: same entry, now older than the source it was rendered from. ---
    backdate(planted.output_path);

    // Pinned with a freshness-blind lookup: the entry is still there and still
    // servable, so a refusal below is about the mtime and not an empty cache.
    // (Deliberately not the source_modified-carrying lookup — that one deletes
    // the entry it rejects, which would hand the widget an empty cache and let
    // the assertion pass against unfixed code.)
    REQUIRE(cached_bin(cache, key, target) == planted.output_path);

    PrintStatusWidgetTestAccess::reset_to_idle(widget);

    // No API is installed, so a probe that correctly misses can only fall back
    // to benchy — it cannot quietly resolve the same path through a fetch.
    CHECK(subject_value() != planted.output_path);
    CHECK(get_idle_thumb_src(container) != planted.output_path);
    CHECK(subject_value() == BENCHY_PATH);
    CHECK(get_idle_thumb_src(container) == BENCHY_PATH);

    widget.detach();
    cache.invalidate(key);
}

// =============================================================================
// The idle thumbnail must describe a file that still exists
//
// The cache key is the job's thumbnail path and the freshness stamp is the
// job's `modified`, neither of which changes when the gcode is deleted. Nothing
// 404s, so the cached render of a deleted file is served forever unless the
// resolve skips jobs whose `exists` flag is false.
// =============================================================================

namespace {

PrintHistoryJob thumb_job(const char* filename, bool exists, const char* thumb, double modified) {
    PrintHistoryJob job;
    job.filename = filename;
    job.exists = exists;
    job.thumbnail_path = thumb;
    job.modified = modified;
    return job;
}

/// Installs a hand-built history as the process-wide one for the test's scope.
struct ScopedIdleThumbHistory {
    explicit ScopedIdleThumbHistory(std::vector<PrintHistoryJob> jobs) : manager(nullptr, nullptr) {
        helix::PrintHistoryManagerTestAccess::set_loaded_jobs(manager, std::move(jobs));
        set_print_history_manager(&manager);
    }
    ~ScopedIdleThumbHistory() {
        set_print_history_manager(nullptr);
    }
    PrintHistoryManager manager;
};

} // namespace

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: idle thumbnail resolves past a deleted newest job",
                 "[print_status_widget][idle_thumb][exists]") {
    ScopedIdleThumbHistory history(
        {thumb_job("deleted.gcode", false, ".thumbs/deleted.png", 900.0),
         thumb_job("survivor.gcode", true, ".thumbs/survivor.png", 500.0)});

    PrintStatusWidget widget;

    // Unattached: no measured thumb widget, so the resolve takes the
    // pre-selected largest thumbnail off whichever job it picked.
    REQUIRE(PrintStatusWidgetTestAccess::last_print_thumbnail_path(widget) ==
            ".thumbs/survivor.png");
    // The freshness stamp has to come off the same job as the key - otherwise
    // the cache validates one file's render against another file's mtime.
    REQUIRE(PrintStatusWidgetTestAccess::last_print_source_modified(widget) ==
            static_cast<time_t>(500.0));
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: idle thumbnail falls back to benchy when nothing survives",
                 "[print_status_widget][idle_thumb][exists]") {
    ScopedIdleThumbHistory history({thumb_job("deleted_a.gcode", false, ".thumbs/a.png", 900.0),
                                    thumb_job("deleted_b.gcode", false, ".thumbs/b.png", 500.0)});

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    // No key at all - reset_print_card_to_idle() takes its "no history" exit
    // and paints the placeholder.
    REQUIRE(PrintStatusWidgetTestAccess::last_print_thumbnail_path(widget).empty());

    widget.attach(container, test_screen());
    process_lvgl(200);
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    widget.detach();
}
