// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/process_async_timers.h"
#include "../test_helpers/typed_mock_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "lvgl/lvgl_private.h"
#include "lvgl_image_writer.h"
#include "misc/lv_timer_private.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "prerendered_images.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "src/ui/panel_widgets/printer_image_widget.h"
#include "wizard_config_paths.h"

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Call the first pending one-shot timer's callback. Returns whether one fired.
/// Pumping them one at a time lets a test observe the state between two chained
/// deferrals; neither this nor process_async_timers() drains the UpdateQueue, which
/// runs off a repeating timer.
bool fire_one_async_call() {
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
        if (t->repeat_count > 0 && t->timer_cb) {
            t->repeat_count--;
            t->timer_cb(t);
            if (timer_is_live(t) && t->repeat_count == 0) {
                lv_timer_delete(t);
            }
            return true;
        }
    }
    return false;
}

/// Fire the one-shot timers that lv_timer_handler() would run at the current
/// tick, without advancing it: what lands before the display's next refresh in
/// the same timer pass.
void fire_due_one_shots() {
    for (int safety = 0; safety < 100; ++safety) {
        bool fired = false;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            if (t->repeat_count > 0 && t->timer_cb && !t->paused &&
                lv_tick_elaps(t->last_run) >= t->period) {
                t->repeat_count--;
                t->last_run = lv_tick_get();
                t->timer_cb(t);
                if (timer_is_live(t) && t->repeat_count == 0) {
                    lv_timer_delete(t);
                }
                fired = true;
                break; // the callback may have changed the list
            }
        }
        if (!fired)
            return;
    }
}

} // namespace

TEST_CASE("classify_printer_image_change: a refresh redoes only what changed",
          "[panel_widget][printer_image]") {
    using helix::ui::classify_printer_image_change;
    using helix::ui::PrinterImageChange;
    using helix::ui::PrinterImageStamp;
    const PrinterImageStamp shown{"A:/img/voron-v2.bin", "1700000000-478824", 220, 195};

    SECTION("same source, same file, same area: nothing to redo") {
        CHECK(classify_printer_image_change(shown, shown) == PrinterImageChange::None);
    }
    SECTION("another area width or height: relayout only") {
        PrinterImageStamp next = shown;
        next.area_w = 300;
        CHECK(classify_printer_image_change(shown, next) == PrinterImageChange::Resized);
        next = shown;
        next.area_h = 120;
        CHECK(classify_printer_image_change(shown, next) == PrinterImageChange::Resized);
    }
    SECTION("another source: new pixels") {
        PrinterImageStamp next = shown;
        next.source = "A:/img/voron-trident.bin";
        CHECK(classify_printer_image_change(shown, next) == PrinterImageChange::NewPixels);
    }
    SECTION("the same path rewritten: new pixels") {
        PrinterImageStamp next = shown;
        next.fingerprint = "1700000999-478824";
        CHECK(classify_printer_image_change(shown, next) == PrinterImageChange::NewPixels);
        next.area_w = 300; // a rewrite outranks a resize
        CHECK(classify_printer_image_change(shown, next) == PrinterImageChange::NewPixels);
    }
    SECTION("nothing shown yet: new pixels") {
        CHECK(classify_printer_image_change(PrinterImageStamp{}, shown) ==
              PrinterImageChange::NewPixels);
    }
}

// A move or removal in grid edit mode rebuilds the page from XML, and the
// recycled widget gets a fresh tree. Its picture must be on that tree, at the
// exact-size copy, before the first frame draws it, or the tile visibly
// re-lays itself out though nothing about it changed.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget shows its exact-size picture on a rebuilt tree's first frame",
                 "[panel_widget][printer_image][cache]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // The XML component's shape: a fixed tile, the image filling it.
    auto build_tree = [&](lv_obj_t*& widget_obj, lv_obj_t*& img) {
        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, 200, 200);
        widget_obj = lv_obj_create(container);
        lv_obj_set_size(widget_obj, 180, 160);
        img = lv_image_create(widget_obj);
        lv_obj_set_name(img, "printer_image");
        lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    };
    auto src_of = [](lv_obj_t* img) {
        const auto* s = static_cast<const char*>(lv_image_get_src(img));
        return std::string(s ? s : "");
    };

    lv_obj_t* widget_obj = nullptr;
    lv_obj_t* img = nullptr;
    build_tree(widget_obj, img);
    process_lvgl(5);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    process_async_timers();
    const std::string source = src_of(img);
    REQUIRE_FALSE(source.empty());
    const int32_t img_w = lv_obj_get_width(img);
    const int32_t img_h = lv_obj_get_height(img);
    REQUIRE(img_w > 0);
    REQUIRE(img_h > 0);
    const std::string cache_src = "A:" + helix::get_cached_printer_image_path(source, img_w, img_h);
    if (!wait_until([&]() { return src_of(img) == cache_src; })) {
        w.detach();
        SKIP("no cacheable printer image in this tree (source '" + source + "')");
    }

    // The rebuild: the old tree goes, a new one is built and attached, and no
    // layout pass has run on it yet.
    w.detach();
    lv_obj_delete(lv_obj_get_parent(widget_obj));
    build_tree(widget_obj, img);
    w.attach(widget_obj, test_screen());
    REQUIRE(src_of(img).empty()); // still deferred out of attach() (#1025)

    fire_due_one_shots();
    INFO("the picture must land in the timer pass that follows the rebuild, ahead of "
         "the display refresh, and at its exact-size copy rather than the scaled source");
    CHECK(src_of(img) == cache_src);

    // A tree deleted under the widget, with no detach first: the next tree
    // has shown nothing, so the exact-size copy must be set on it again.
    lv_obj_delete(lv_obj_get_parent(widget_obj));
    build_tree(widget_obj, img);
    w.attach(widget_obj, test_screen());
    fire_due_one_shots();
    INFO("a tree deleted without a detach must not leave the next one blank");
    CHECK(src_of(img) == cache_src);

    w.detach();
}

// A custom image can be rewritten under its own path while the home grid is
// being rebuilt. The recycled widget's fresh tree has shown nothing yet, so the
// decoded copy it must drop is the one named by what it showed last.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget drops a rewritten source's decoded copy across a rebuild",
                 "[panel_widget][printer_image][cache]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // The header cache is the decoded state a read can observe; it ships at 0.
    struct ScopedHeaderCache {
        ScopedHeaderCache() {
            lv_image_header_cache_resize(8, false);
        }
        ~ScopedHeaderCache() {
            lv_image_header_cache_drop(nullptr);
            lv_image_header_cache_resize(0, true);
        }
    } header_cache;

    auto& pim = helix::PrinterImageManager::instance();
    const std::string saved_custom_dir = pim.get_custom_dir();
    const std::filesystem::path config_dir =
        std::filesystem::temp_directory_path() /
        ("helix_printer_image_rewrite_" + std::to_string(::getpid()));
    std::filesystem::remove_all(config_dir);
    pim.init(config_dir.string());

    auto write_custom = [&](int w, int h) {
        const std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, 0xFF);
        for (const char* size : {"150", "300"}) {
            REQUIRE(helix::write_lvgl_bin(pim.get_custom_dir() + "rewritten-" + size + ".bin", w, h,
                                          LV_COLOR_FORMAT_ARGB8888, px.data(), px.size()));
        }
    };
    write_custom(40, 30);
    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::PRINTER_IMAGE, "custom:rewritten");

    auto build_tree = [&](lv_obj_t*& widget_obj, lv_obj_t*& img) {
        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, 200, 200);
        widget_obj = lv_obj_create(container);
        lv_obj_set_size(widget_obj, 180, 160);
        img = lv_image_create(widget_obj);
        lv_obj_set_name(img, "printer_image");
        lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
        process_lvgl(2);
    };
    auto decoded_width = [](const std::string& src) {
        lv_image_header_t hdr{};
        REQUIRE(lv_image_decoder_get_info(src.c_str(), &hdr) == LV_RESULT_OK);
        return static_cast<int>(hdr.w);
    };

    lv_obj_t* widget_obj = nullptr;
    lv_obj_t* img = nullptr;
    build_tree(widget_obj, img);
    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    process_async_timers();
    const std::string source = pim.get_displayed_image_path(printer_images::current_screen_width());
    REQUIRE(source.find("rewritten-") != std::string::npos);
    REQUIRE(decoded_width(source) == 40);

    // The rebuild: the old tree goes, the file is rewritten with another size,
    // and the new tree is attached.
    w.detach();
    lv_obj_delete(lv_obj_get_parent(widget_obj));
    write_custom(20, 50);
    build_tree(widget_obj, img);
    w.attach(widget_obj, test_screen());
    process_async_timers();

    INFO("a source rewritten across a rebuild must not keep its stale decoded copy");
    CHECK(decoded_width(source) == 20);

    w.detach();
    process_async_timers();
    cfg->set<std::string>(cfg->df() + helix::PRINTER_IMAGE, "");
    if (!saved_custom_dir.empty())
        pim.init(std::filesystem::path(saved_custom_dir).parent_path().parent_path().string());
    std::error_code ec;
    std::filesystem::remove_all(config_dir, ec);
}

// Regression test for #1025 (grid walk-off crash family, #983). PrinterImageWidget
// forced a SYNCHRONOUS LVGL layout from inside attach() -> reload_from_config() ->
// refresh_printer_image() -> lv_image_set_inner_align()/update_align ->
// lv_obj_update_layout, which cascades into the parent grid's grid_update. When
// that runs against a mid-rebuild grid, count_tracks walks the freed descriptor off
// the heap end (SIGSEGV on v0.99.75). The v0.99.76 LV_LAYOUT_NONE window in
// panel_widget_manager.cpp prevents the crash, but the widget forcing synchronous
// layout from the rebuild path is the underlying fragility. PrintStatusWidget
// already established the safe idiom: defer image/layout work to a later tick.
//
// Invariant: the image src must NOT be set synchronously inside attach(); the
// refresh is deferred to a one-shot timer. This FAILS pre-fix (src set
// synchronously in attach) and PASSES post-fix (src still null right after
// attach, set only after a tick). Attached into a PLAIN lv_obj (not a grid) so
// the pre-fix synchronous path fails on the assertion, not a SIGSEGV.
TEST_CASE_METHOD(XMLTestFixture, "PrinterImageWidget defers image refresh out of attach() (#1025)",
                 "[panel_widget][printer_image][regression]") {
    // PrinterImageWidget's subjects (printer_type_text, printer_host_text,
    // printer_info_visible) must exist before reload_from_config touches them.
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // Build the widget tree by hand. attach() looks up the child image by name
    // ("printer_image"), so no XML component is needed. A plain lv_obj container
    // (NOT a grid) means the pre-fix synchronous layout cannot SIGSEGV — it just
    // sets the src early, tripping the assertion below.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 200, 200);
    lv_obj_t* widget_obj = lv_obj_create(container);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    process_lvgl(2);

    // Baseline: no source before attach.
    REQUIRE(lv_image_get_src(img) == nullptr);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());

    // KEY REGRESSION ASSERTION: the refresh must be DEFERRED. Immediately after
    // attach() the image source is still null — refresh_printer_image() (which
    // calls lv_image_set_src + lv_image_set_inner_align, forcing a synchronous
    // layout) has been scheduled on a one-shot timer, not run inline. Pre-fix this
    // FAILS (src set synchronously inside attach); post-fix it PASSES.
    INFO("attach() must not set the image src synchronously — refresh_printer_image() "
         "forces lv_obj_update_layout and must be deferred off the rebuild path (#1025/#983)");
    REQUIRE(lv_image_get_src(img) == nullptr);

    // Fire the deferred refresh timer and let layout settle.
    process_async_timers();
    process_lvgl(5);

    // After a tick the deferred refresh ran and applied a source image.
    REQUIRE(lv_image_get_src(img) != nullptr);

    SECTION("on_activate() also defers the refresh") {
        // After the first refresh resolved, on_activate() re-runs reload_from_config().
        // It must not crash and must (re)apply the source via the deferred timer.
        w.on_activate();
        // Drain any pending deferred refresh + cache timers from on_activate().
        process_async_timers();
        process_lvgl(5);
        REQUIRE(lv_image_get_src(img) != nullptr);
    }

    w.detach();
}

// Generating a scaled cache entry is a decode plus a resize — around half a second
// per entry on a two-core MIPS board, and the panel schedules a cache check on
// every activation. Run on the UI thread that is a freeze of exactly that length
// each time the user returns home, so the miss goes to a worker and only the
// finished result comes back. The hit stays synchronous: it is a stat and a
// pointer swap, and deferring it would cost a visible frame on the source image.
TEST_CASE_METHOD(XMLTestFixture, "PrinterImageWidget generates its image cache off the UI thread",
                 "[panel_widget][printer_image][cache]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 200, 200);
    lv_obj_t* widget_obj = lv_obj_create(container);
    lv_obj_set_size(widget_obj, 180, 180);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    // A fixed size: the cache check needs a laid-out image, and the dimensions are
    // part of the entry's name.
    lv_obj_set_size(img, 120, 90);
    process_lvgl(5);

    helix::PrinterImageWidget widget;
    widget.attach(widget_obj, test_screen());

    // Pump one deferral at a time and stop as soon as the refresh has applied a
    // source. The cache check is the next timer in line, and the source it will be
    // handed has to be read before it runs.
    bool refreshed = false;
    for (int i = 0; i < 10 && !refreshed; ++i) {
        REQUIRE(fire_one_async_call());
        refreshed = lv_image_get_src(img) != nullptr;
    }
    REQUIRE(refreshed);
    const std::string source = static_cast<const char*>(lv_image_get_src(img));

    const int32_t width_px = lv_obj_get_width(img);
    const int32_t height_px = lv_obj_get_height(img);
    REQUIRE(width_px > 0);
    REQUIRE(height_px > 0);

    const std::string cache_path =
        helix::get_cached_printer_image_path(source, width_px, height_px);
    const std::string cache_src = "A:" + cache_path;
    std::error_code ec;
    std::filesystem::remove(cache_path, ec); // force a miss even on a warm cache

    // MISS: firing the cache check must not decode and resize on this thread. The
    // widget stays on the CONTAIN-scaled source until a worker reports back — the
    // same state a generation failure leaves behind, so nothing blanks meanwhile.
    process_async_timers();
    INFO("a cache miss must hand the generation to a worker, not run it inline");
    CHECK(std::string(static_cast<const char*>(lv_image_get_src(img))) == source);

    // The worker finishes and its deferred continuation swaps in the entry.
    const bool applied = wait_until([&]() {
        const auto* now = static_cast<const char*>(lv_image_get_src(img));
        return now != nullptr && std::string(now) == cache_src;
    });
    if (!applied) {
        widget.detach();
        SKIP("no cacheable printer image in this tree (source '" + source + "')");
    }
    CHECK(std::filesystem::exists(cache_path));

    // HIT: with the entry present the next activation resolves it on the UI thread.
    // Only one-shot timers are pumped below, so the UpdateQueue is never drained and
    // no worker result can be what lands.
    process_lvgl(20); // settle any straggler from the generation above
    widget.on_activate();

    // With the entry on disk the widget must go straight to it and never show the
    // tier image again: decoding that and rescaling it costs the whole saving on
    // every return to the panel.
    bool re_refreshed = false;
    for (int i = 0; i < 10 && !re_refreshed; ++i) {
        REQUIRE(fire_one_async_call());
        const auto* now = static_cast<const char*>(lv_image_get_src(img));
        REQUIRE(now != nullptr);
        INFO("re-activation must not fall back to the tier image");
        CHECK(std::string(now) != source);
        re_refreshed = std::string(now) == cache_src;
    }
    REQUIRE(re_refreshed);

    widget.detach();
    std::filesystem::remove(cache_path, ec);
}

// A widget's image can still be LV_SIZE_CONTENT when the cache check fires.
// The raw special coordinate is large and positive, so it must read as "no size
// yet": a cache job sized off it can never complete, so it stays in flight
// and blocks the generation for the real size that follows.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget skips the cache while its image is content-sized",
                 "[panel_widget][printer_image][cache]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 200, 200);
    lv_obj_t* widget_obj = lv_obj_create(container);
    lv_obj_set_size(widget_obj, 180, 180);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    // No lv_obj_set_size(img, ...): stays at LVGL's default LV_SIZE_CONTENT on
    // both axes, the state attach() can see before a layout pass ever runs.
    process_lvgl(5);

    helix::PrinterImageWidget widget;
    widget.attach(widget_obj, test_screen());

    bool refreshed = false;
    for (int i = 0; i < 10 && !refreshed; ++i) {
        REQUIRE(fire_one_async_call());
        refreshed = lv_image_get_src(img) != nullptr;
    }
    REQUIRE(refreshed);
    const std::string source = static_cast<const char*>(lv_image_get_src(img));

    // Content-sized cache check: must skip rather than submit a job sized off
    // the LV_SIZE_CONTENT sentinel. Nothing to assert directly here — the proof
    // is that a real size assigned right after still gets its cache entry.
    process_async_timers();

    lv_obj_set_size(img, 120, 90);
    process_lvgl(5);
    widget.on_activate();

    const std::string cache_path = helix::get_cached_printer_image_path(source, 120, 90);
    const std::string cache_src = "A:" + cache_path;
    std::error_code ec;
    std::filesystem::remove(cache_path, ec); // force a miss even on a warm cache

    process_async_timers();
    INFO("a content-sized cache check must not wedge cache_job_inflight_ and block "
         "the next real-size generation");
    REQUIRE(wait_until([&]() {
        const auto* now = static_cast<const char*>(lv_image_get_src(img));
        return now != nullptr && std::string(now) == cache_src;
    }));
    CHECK(std::filesystem::exists(cache_path));

    widget.detach();
    std::filesystem::remove(cache_path, ec);
}

// The printer-type subject publishes every change to the resolved type, and the
// setter's early return (same type + same z-offset strategy) must not re-notify.
// Consumers like PrinterImageWidget re-resolve on this subject, so a duplicate
// notification is a wasted config read + layout, not just noise.
TEST_CASE_METHOD(XMLTestFixture, "PrinterState type subject fires only when the type changes",
                 "[1552][printer_state][panel_widget]") {
    int fires = 0;
    lv_observer_t* obs = lv_subject_add_observer(
        state().profile_state().get_printer_type_subject(),
        [](lv_observer_t* o, lv_subject_t*) { ++*static_cast<int*>(lv_observer_get_user_data(o)); },
        &fires);
    REQUIRE(obs != nullptr);
    REQUIRE(fires == 1); // observers run once on attach with the current value

    state().set_printer_type_sync("Voron 2.4");
    REQUIRE(fires == 2);
    REQUIRE(std::string(lv_subject_get_string(
                state().profile_state().get_printer_type_subject())) == "Voron 2.4");

    // Same type: the early-return path must not re-notify.
    state().set_printer_type_sync("Voron 2.4");
    REQUIRE(fires == 2);

    state().set_printer_type_sync("Creality K1C");
    REQUIRE(fires == 3);
    REQUIRE(std::string(lv_subject_get_string(
                state().profile_state().get_printer_type_subject())) == "Creality K1C");

    lv_observer_remove(obs);
}

// On a fresh install auto-detection settles AFTER the home panel is built, so
// attach()'s one-shot resolve shows the generic silhouette and nothing re-runs
// it for the rest of the session. The widget observes the printer-type subject
// and re-resolves when the detected type lands.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget re-resolves its image when the type settles after attach",
                 "[1552][panel_widget][printer_image][regression]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // The widget observes the process-wide PrinterState, whose subjects this
    // fixture does not initialize (its own per-instance state owns the XML
    // scope). Without this the observer silently fails to attach and the
    // detection below changes nothing.
    get_printer_state().init_subjects(false);

    auto build_tree = [&](lv_obj_t*& widget_obj, lv_obj_t*& img) {
        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, 200, 200);
        widget_obj = lv_obj_create(container);
        img = lv_image_create(widget_obj);
        lv_obj_set_name(img, "printer_image");
        process_lvgl(2);
    };

    lv_obj_t* widget_obj = nullptr;
    lv_obj_t* img = nullptr;
    build_tree(widget_obj, img);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();

    const char* first_raw = static_cast<const char*>(lv_image_get_src(img));
    REQUIRE(first_raw != nullptr);
    const std::string first_src(first_raw);

    // Detection settles: config gains the type and PrinterState publishes it.
    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "Voron 2.4");
    get_printer_state().set_printer_type_sync("Voron 2.4");

    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();

    const std::string after_src(static_cast<const char*>(lv_image_get_src(img)));
    INFO("the widget must re-resolve once the detected type lands in config");
    REQUIRE(after_src != first_src);
    REQUIRE(after_src.find("voron") != std::string::npos);

    // A rebuilt panel recycles the widget instance and attaches it to a new
    // tree; the re-armed observer must re-resolve from attach()'s reload.
    w.detach();
    build_tree(widget_obj, img);
    w.attach(widget_obj, test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();

    const std::string recycled_src(static_cast<const char*>(lv_image_get_src(img)));
    INFO("a recycled instance must re-resolve the settled type from attach()");
    REQUIRE(recycled_src.find("voron") != std::string::npos);

    w.detach();

    // Restore the config type: the fixture resets the singleton between
    // cases, but a same-value leftover inside this case's lifetime would
    // have made the mid-session transition above a no-op for the next
    // SECTION leaf.
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");
}

// A printer added by address has no name or type until it connects; Home names it by its host.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget labels a printer not yet identified by its host",
                 "[panel_widget][printer_image][multi-printer]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    helix::Config* cfg = helix::Config::get_instance();
    const nlohmann::json saved = helix::ConfigTestAccess::data(*cfg);
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    nlohmann::json data = saved;
    data["printers"]["printer-2"] = {{"moonraker_host", "10.0.0.9"}};
    helix::ConfigTestAccess::data(*cfg) = data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = "printer-2";

    lv_obj_t* widget_obj = lv_obj_create(test_screen());
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());

    lv_subject_t* label = lv_xml_get_subject(nullptr, "printer_type_text");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_subject_get_string(label)) == "10.0.0.9");

    w.detach();
    process_async_timers();
    helix::ConfigTestAccess::data(*cfg) = saved;
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
}

// A filament system registers after connect, after the home panel is built. A
// database entry's image_variants can pick another image for it, so the widget
// re-resolves when the detected filament systems change, and back when they go.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget switches to an image variant when a filament system registers",
                 "[panel_widget][printer_image][printer_image_variants]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    get_printer_state().init_subjects(false);
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.init_subjects(false);

    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "Creality K2");

    lv_obj_t* widget_obj = lv_obj_create(test_screen());
    lv_obj_set_size(widget_obj, 200, 200);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    process_lvgl(2);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    auto source = [&]() { return std::string(static_cast<const char*>(lv_image_get_src(img))); };
    const std::string plain = source();
    REQUIRE(plain.find("creality-k2") != std::string::npos);
    REQUIRE(plain.find("creality-k2-cfs") == std::string::npos);

    ams.add_backend(std::make_unique<helix::test::TypedMockBackend>(AmsType::CFS));
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source().find("creality-k2-cfs") != std::string::npos);

    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source() == plain);

    w.detach();
    process_async_timers();
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");
}

// The top can be fitted or lifted off while connected, so the widget follows the
// enclosure-top subject as well.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget switches to the with-top image when the top is fitted",
                 "[panel_widget][printer_image][printer_image_variants][enclosure_top]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    get_printer_state().init_subjects(false);
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.init_subjects(false);
    auto& caps = get_printer_state().capabilities_state();
    caps.set_enclosure_top_fitted(false);
    caps.set_has_chamber_heater(false);

    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "Snapmaker U1");

    lv_obj_t* widget_obj = lv_obj_create(test_screen());
    lv_obj_set_size(widget_obj, 200, 200);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    process_lvgl(2);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    auto source = [&]() { return std::string(static_cast<const char*>(lv_image_get_src(img))); };
    const std::string plain = source();
    REQUIRE(plain.find("snapmaker-u1") != std::string::npos);
    REQUIRE(plain.find("snapmaker-u1-tophat") == std::string::npos);

    caps.set_enclosure_top_fitted(true);
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source().find("snapmaker-u1-tophat") != std::string::npos);

    caps.set_enclosure_top_fitted(false);
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source() == plain);

    caps.set_has_chamber_heater(true);
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source().find("snapmaker-u1-tophat") != std::string::npos);

    caps.set_has_chamber_heater(false);
    helix::ui::UpdateQueue::instance().drain();
    process_async_timers();
    CHECK(source() == plain);

    w.detach();
    process_async_timers();
    cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");
}

// A chamber with only a sensor still has a reading worth showing, so the home
// callout follows HasChamber (sensor or heater), as the print status tile does.
TEST_CASE_METHOD(XMLTestFixture, "PrinterImageWidget shows a sensor-only chamber's callout",
                 "[panel_widget][printer_image]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    get_printer_state().init_subjects(false);
    auto& caps = get_printer_state().capabilities_state();
    caps.set_has_chamber_heater(false);
    caps.set_has_chamber_sensor(true);
    // Hot enough to show with no target.
    lv_subject_set_int(get_printer_state().temperature_state().get_chamber_temp_subject(), 600);

    lv_obj_t* widget_obj = lv_obj_create(test_screen());
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* shown = lv_xml_get_subject(nullptr, "callout_chamber_shown");
    REQUIRE(shown != nullptr);
    CHECK(lv_subject_get_int(shown) != 0);

    caps.set_has_chamber_sensor(false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(shown) == 0);

    w.detach();
    process_async_timers();
    lv_subject_set_int(get_printer_state().temperature_state().get_chamber_temp_subject(), 0);
}
