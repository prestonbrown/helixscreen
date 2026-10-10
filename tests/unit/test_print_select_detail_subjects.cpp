// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_detail_subjects.cpp
 * @brief Unit tests for print select detail view subject initialization
 *
 * Pre-print toggle state lives in PrePrintOptionsRenderer's per-option heap
 * subjects — the six fixed preprint_* subjects this file used to name were
 * retired in Phase 3.5 (ui_print_select_detail_view.h:618). What is pinned here
 * is the re-show reset: populate() re-initializes every option from
 * default_enabled, so opening a second file cannot inherit the first file's
 * toggles.
 *
 * Bug context for the defaults themselves: switches once defaulted to OFF in
 * XML, so is_option_disabled() returned true even when the user hadn't touched
 * them, triggering false modification warnings when printing without the plugin.
 *
 * Also covers the detail_mapping_ready skeleton-latch subject: 0 = chips not
 * authoritative (XML skeletons visible), 1 = authoritative chip state rendered.
 * It must track the tools-used cache (instant on re-prints) and the scan /
 * viewer-parse readiness the print-start gate waits on.
 */

#include "ui_callback_helpers.h"
#include "ui_gcode_viewer.h"
#include "ui_pre_print_options_renderer.h"
#include "ui_print_select_detail_view.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/mock_bypass.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_portrait_layout.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "gcode_ops_detector.h"
#include "gcode_parser.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "pre_print_option.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/pre_print_option_sets.h"
#include "theme_manager.h"
#include "tools_used_cache.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::test::make_skip_and_addon_set;

// ============================================================================
// Pre-print Option Subject Default Tests
// ============================================================================

// This used to be three TEST_CASEs that each declared their own local
// lv_subject_t, initialized it, and read the value back — asserting LVGL's own
// getter and nothing of ours. They named `preprint_bed_mesh` / `preprint_qgl` /
// `preprint_timelapse`, the six fixed subjects retired in Phase 3.5
// (ui_print_select_detail_view.h:618); toggle state has lived in
// PrePrintOptionsRenderer's per-option heap subjects ever since, so the cases
// could not have gone red no matter what the panel did.
//
// The default_enabled -> initial state rule is covered against the real renderer
// in test_pre_print_options_renderer.cpp:142. What was NOT covered anywhere, and
// is what these cases claimed to be about, is the reset-on-re-show contract:
// populate() re-initializes every state subject from default_enabled, so opening
// a second file cannot inherit the first file's toggles
// (ui_pre_print_options_renderer.h:105).
TEST_CASE_METHOD(LVGLUITestFixture,
                 "re-showing the detail view resets pre-print toggles to their defaults",
                 "[print_select][detail_view][subjects][pre_print_options]") {
    helix::ui::PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    const auto set = make_skip_and_addon_set();
    renderer.populate(container, set, nullptr, nullptr);

    // Defaults as shipped: skip option ON, add-on OFF.
    REQUIRE(renderer.get_state("bed_mesh") == 1);
    REQUIRE(renderer.get_state("timelapse") == 0);

    // User flips both for this file.
    renderer.set_state("bed_mesh", 0);
    renderer.set_state("timelapse", 1);
    REQUIRE(renderer.get_state("bed_mesh") == 0);
    REQUIRE(renderer.get_state("timelapse") == 1);

    // show() for the next file re-populates. Both must be back at their
    // defaults — a carried-over toggle silently changes what the next print
    // does, in opposite directions for the two kinds of option.
    renderer.populate(container, set, nullptr, nullptr);
    CHECK(renderer.get_state("bed_mesh") == 1);
    CHECK(renderer.get_state("timelapse") == 0);

    renderer.clear();
}

// ============================================================================
// detail_mapping_ready skeleton latch (tools-used cache + scan readiness)
// ============================================================================

namespace {

/// No-op stand-ins for the print_file_detail.xml event callbacks (normally
/// registered by PrintSelectPanel's init_subjects). The XML references them
/// at create() time; the handlers themselves don't matter to this subject.
void detail_noop_cb(lv_event_t* /*e*/) {}

/// Per-test temp cache dir for HELIX_CACHE_DIR — keeps ToolsUsedCache disk
/// state out of the real user cache. Saves/restores the env var so later
/// tests in this binary are unaffected (tests share the process env).
struct CacheDirGuard {
    std::filesystem::path dir;
    std::string prev_env_;
    bool had_prev_ = false;
    CacheDirGuard()
        : dir(std::filesystem::temp_directory_path() /
              ("detail_subjects_test_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir);
        if (const char* old = ::getenv("HELIX_CACHE_DIR")) {
            prev_env_ = old;
            had_prev_ = true;
        }
        ::setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }
    ~CacheDirGuard() {
        if (had_prev_) {
            ::setenv("HELIX_CACHE_DIR", prev_env_.c_str(), 1);
        } else {
            ::unsetenv("HELIX_CACHE_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

// A queued job's saved option states arrive as a seed applied over the
// freshly-populated rows for that ONE render. The merge rules live in the
// renderer's set_state(): ids with no row are dropped, ids the seed does not
// mention keep their defaults — which is why this case can assert all three
// (override, default-kept, unknown-dropped) from one seed. And the seed is
// consumed by the show() that applies it, so the NEXT show starts from
// defaults again: a seed that survived would fight the user's own toggles on
// every later file.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "a queued job's saved option states seed the rows for one show only",
                 "[print_select][detail_view][subjects][pre_print_options][job_queue]") {
    CacheDirGuard guard;

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    PrinterStateTestAccess::set_option_set(get_printer_state(), make_skip_and_addon_set());

    helix::ui::PrintSelectDetailView view;
    view.set_dependencies(nullptr, &get_printer_state());
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    // Saved states for a queued job: bed_mesh flipped off, timelapse flipped
    // on, plus an id no option set has ever defined.
    view.seed_option_states({{"bed_mesh", false}, {"timelapse", true}, {"ghost_option", true}});
    view.show("queued.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();

    const auto states = view.collect_option_states();
    REQUIRE(states.count("bed_mesh") == 1);
    REQUIRE(states.count("timelapse") == 1);
    CHECK(states.at("bed_mesh") == false);
    CHECK(states.at("timelapse") == true);
    CHECK(states.count("ghost_option") == 0);

    // The seed is consumed: the next show of any file starts from defaults.
    view.hide();
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler(); // the close callback runs on the next tick
    view.show("other.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();

    const auto reset_states = view.collect_option_states();
    REQUIRE(reset_states.count("bed_mesh") == 1);
    REQUIRE(reset_states.count("timelapse") == 1);
    CHECK(reset_states.at("bed_mesh") == true);
    CHECK(reset_states.at("timelapse") == false);
}

// A printer with no database options gets its rows from the PRINT_START analysis.
TEST_CASE_METHOD(LVGLUITestFixture, "rows come from the macro analysis when the database has none",
                 "[print_select][detail_view][pre_print_options][macro_rows]") {
    CacheDirGuard guard;

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    PrinterStateTestAccess::set_option_set(get_printer_state(), PrePrintOptionSet{});

    helix::ui::PrintSelectDetailView view;
    view.set_dependencies(nullptr, &get_printer_state());
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    helix::PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    helix::PrintStartOperation qgl;
    qgl.name = "QUAD_GANTRY_LEVEL";
    qgl.category = helix::PrintStartOpCategory::QGL;
    qgl.has_skip_param = true;
    qgl.skip_param_name = "SKIP_QGL";
    analysis.operations.push_back(qgl);
    REQUIRE(view.get_prep_manager() != nullptr);
    view.get_prep_manager()->set_macro_analysis(analysis);

    view.show("wrapped.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();

    const auto states = view.collect_option_states();
    REQUIRE(states.count("qgl") == 1);
    CHECK(states.at("qgl") == true);
}

// A row whose skip rewrites the job hides when no rewrite can run, and the
// transport alone can be the reason.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "a rewrite-gated row hides on a transport that keeps no local copy",
                 "[print_select][detail_view][pre_print_options][macro_rows]") {
    CacheDirGuard guard;

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    const bool local_copies = GENERATE(true, false);
    CAPTURE(local_copies);
    // The view answers for the printer it was handed, not the app's: the
    // app's reads plugin-absent here, the injected one plugin-present.
    MockPrinter device;
    device.api.transfers_mock().mock_no_local_copies(!local_copies);
    PrinterStateTestAccess::set_option_set(device.state, PrePrintOptionSet{});
    device.state.set_helix_plugin_installed(true);
    get_printer_state().set_helix_plugin_installed(false);
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::PrintSelectDetailView view;
    view.set_dependencies(&device.api, &device.state);
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
            v.set_dependencies(nullptr, &get_printer_state());
        }
    } closer{view};

    // A MacroParam skip with no pre-start block: disabling it rewrites the job.
    helix::PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    helix::PrintStartOperation qgl;
    qgl.name = "QUAD_GANTRY_LEVEL";
    qgl.category = helix::PrintStartOpCategory::QGL;
    qgl.has_skip_param = true;
    qgl.skip_param_name = "SKIP_QGL";
    analysis.operations.push_back(qgl);
    REQUIRE(view.get_prep_manager() != nullptr);
    view.get_prep_manager()->set_macro_analysis(analysis);

    view.show("wrapped.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* const row = lv_obj_find_by_name(root, "option_tile_qgl");
    REQUIRE(row != nullptr);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) == !local_copies);
}

// An open view follows the analysis as it lands, adding rows and dropping stale ones.
TEST_CASE_METHOD(LVGLUITestFixture, "an open detail view rebuilds its rows when an analysis lands",
                 "[print_select][detail_view][pre_print_options][macro_rows]") {
    CacheDirGuard guard;

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    PrinterStateTestAccess::set_option_set(get_printer_state(), PrePrintOptionSet{});

    helix::ui::PrintSelectDetailView view;
    view.set_dependencies(nullptr, &get_printer_state());
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);
    int forwarded = 0;
    view.set_on_macro_analysis([&](const helix::PrintStartAnalysis&) { ++forwarded; });

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    view.show("wrapped.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(view.collect_option_states().count("qgl") == 0);

    auto* prep = view.get_prep_manager();
    REQUIRE(prep != nullptr);
    helix::PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    helix::PrintStartOperation qgl;
    qgl.name = "QUAD_GANTRY_LEVEL";
    qgl.category = helix::PrintStartOpCategory::QGL;
    qgl.has_skip_param = true;
    qgl.skip_param_name = "SKIP_QGL";
    analysis.operations.push_back(qgl);
    prep->set_macro_analysis(analysis);
    prep->analyze_print_start_macro(); // delivers the cached analysis

    CHECK(view.collect_option_states().count("qgl") == 1);
    CHECK(forwarded == 1);

    analysis.operations.clear();
    prep->set_macro_analysis(analysis);
    prep->analyze_print_start_macro();

    CHECK(view.collect_option_states().count("qgl") == 0);
    CHECK(forwarded == 2);
}

TEST_CASE_METHOD(LVGLUITestFixture, "detail_mapping_ready tracks cache seed and scan readiness",
                 "[print_select][detail_view][subjects]") {
    CacheDirGuard guard;

    // The fixture doesn't init PrintSelectPanel, so the panel's XML callbacks
    // aren't registered — install no-ops before creating the detail view.
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);

    lv_subject_t* ready = lv_xml_get_subject(nullptr, "detail_mapping_ready");
    REQUIRE(ready != nullptr);
    REQUIRE(lv_subject_get_int(ready) == 0); // fresh view: skeleton armed

    const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    // The (path, size, mtime) triple show() is called with — the cache key.
    constexpr size_t kSize = 1234;
    constexpr time_t kMtime = 5678;

    // Production close flow: go_back is deferred, so drain runs on_deactivate
    // + the destroy-on-close callback before the view goes out of scope.
    auto pop_and_drain = [&view]() {
        view.hide();
        helix::ui::UpdateQueue::instance().drain();
        lv_timer_handler(); // the close callback runs on the next tick
    };

    SECTION("warmed cache: ready=1 and tools_used seeded before activation") {
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime);

        // No drain: show() itself must have published readiness from the
        // cache hit — the deferred push/on_activate hasn't even run yet.
        REQUIRE(lv_subject_get_int(ready) == 1);
        REQUIRE(view.get_tools_used() == std::set<int>{0, 2});

        pop_and_drain();
        REQUIRE(lv_subject_get_int(ready) == 0); // latch re-arms on deactivate
    }

    SECTION("cold cache: skeleton (0) until the scan resolves") {
        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime);
        REQUIRE(lv_subject_get_int(ready) == 0);

        // Drain runs the deferred push → on_activate → scan kick-off. With no
        // API the degrade path marks the scan done immediately — the same
        // readiness flip the real scan-finish helper performs.
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(ready) == 1);

        pop_and_drain();
        REQUIRE(lv_subject_get_int(ready) == 0);
    }

    SECTION("cache hit with NO palette keeps the skeleton until colors settle") {
        // The K2 Plus regression. Moonraker reports filament_type for an
        // OrcaSlicer file and no filament_colors at all, so show() gets an
        // empty palette. The tools-used cache answers the OTHER question
        // instantly, and readiness used to key off that alone — so the chips
        // were published built from neutral stand-ins, rendering a grey dot
        // pointing at the real lane color.
        //
        // Readiness must now wait for the palette question too. "Settled", not
        // "non-empty": with no API nothing can ever read the file, so the
        // degrade path settles it and the latch still opens rather than
        // hanging on the skeleton forever.
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", /*filament_colors=*/{}, {}, kSize, kMtime);

        // Tools are known — but nothing has said what color they print in.
        REQUIRE(view.get_tools_used() == std::set<int>{0, 2});
        REQUIRE(lv_subject_get_int(ready) == 0);

        // Drain runs the deferred push → on_activate → scan kick-off. With no
        // API, the degrade path settles the palette question ("nothing knows")
        // so the latch opens rather than stranding the user on a skeleton.
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(ready) == 1);

        pop_and_drain();
    }

    SECTION("cache hit WITH a metadata palette is ready immediately, as before") {
        // Guards the common path against the change above: metadata that
        // carried colors settles the palette question during show(), so a
        // re-print still renders final chips on the first frame.
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime);
        REQUIRE(lv_subject_get_int(ready) == 1);

        pop_and_drain();
    }

    SECTION("stale cache entry (mtime changed) is a miss") {
        helix::ToolsUsedCache warmer;
        warmer.store("sub/flash.gcode", kSize, kMtime, {0, 2});

        view.show("flash.gcode", "sub", "PLA", colors, {}, kSize, kMtime + 1);
        REQUIRE(lv_subject_get_int(ready) == 0); // re-sliced file → skeleton

        pop_and_drain();
    }
}

// ============================================================================
// print_file_detail.xml structure
// ============================================================================

namespace {
// No fixture builds this root today. LVGLUITestFixture registers every
// production component and initialises subjects first, so this should work;
// if it returns null, the detail view's own subjects are not part of the
// fixture's Phase 4 and this whole XML-structure approach is not viable.
lv_obj_t* make_detail_root(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "print_file_detail", nullptr));
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Sliced colors toggle sits outside the filament card",
                 "[print_select][detail][xml]") {
    // The toggle recolors the 3D preview, not the chips. Task 4 merges the two
    // cards and the header cannot carry the toggle, the chevron and two icons
    // at 480x272 — so the toggle must already live outside the card.
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);

    lv_obj_t* const toggle = lv_obj_find_by_name(root, "sliced_colors_toggle");
    REQUIRE(toggle != nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);

    // Walk up from the toggle: the filament card must not be an ancestor.
    for (lv_obj_t* p = lv_obj_get_parent(toggle); p != nullptr; p = lv_obj_get_parent(p)) {
        CHECK(p != card);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Sliced colors row is shown only while the live viewer is on screen",
                 "[print_select][detail][xml]") {
    // The toggle sets detail_prefer_sliced_colors, which apply_preview_colors()
    // uses to choose which mappings feed the 3D gcode viewer. It reaches nothing
    // else. So there are two ways for it to be inert: viewer mode 0, where the
    // viewer is not the active preview at all (print_file_detail.xml binds
    // viewer_hidden="detail_gcode_viewer_mode eq 0"), and no first frame yet,
    // where the slicer thumbnail is still drawn over it
    // (thumbnail_hidden="detail_viewer_first_frame eq 1"). Offering a control
    // that changes nothing the user can see is the bug; both halves have to hold.
    //
    // Built through the real view rather than make_detail_root(): the two
    // subjects below are registered by PrintSelectDetailView::init_subjects(),
    // so a bare lv_xml_create() finds the row but not the subjects driving it.
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);

    lv_obj_t* const row = lv_obj_find_by_name(root, "sliced_colors_row");
    REQUIRE(row != nullptr);

    lv_subject_t* const mode = lv_xml_get_subject(nullptr, "detail_gcode_viewer_mode");
    lv_subject_t* const first_frame = lv_xml_get_subject(nullptr, "detail_viewer_first_frame");
    REQUIRE(mode != nullptr);
    REQUIRE(first_frame != nullptr);

    auto set_state = [&](int viewer_mode, int frame) {
        lv_subject_set_int(mode, viewer_mode);
        lv_subject_set_int(first_frame, frame);
        process_lvgl(20);
    };

    // Neither half, then each half alone. The show case below matters as much as
    // these three: a binding that is wrong in that direction hides the row
    // forever and would still pass a hidden-only test.
    set_state(0, 0);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    set_state(1, 0); // viewer is the preview, but the thumbnail still covers it
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    set_state(0, 1); // a frame exists, but the viewer is not the active preview
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Both: the live viewer is what is on screen, the toggle recolours something
    // visible, so the row is offered.
    set_state(1, 1);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // And it goes away again when the viewer does - the binding is reactive, not
    // a one-shot evaluated when the tree was built.
    set_state(0, 1);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    view.hide();
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler(); // the close callback runs on the next tick
}

TEST_CASE_METHOD(LVGLUITestFixture, "prep time estimate line appears on the first open",
                 "[print_select][detail][xml]") {
    // The line hides itself through preprint_estimate_visible, the int twin
    // update_prep_time_label() writes beside the estimate string. The writer
    // runs from on_activate(), so a real show() must publish a visible
    // estimate with no option toggled and no direct subject write.
    CacheDirGuard guard;

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);
    // After create(): set_dependencies() forwards the printer state to the
    // prep manager, which create() has just constructed.
    view.set_dependencies(nullptr, &get_printer_state());

    lv_obj_t* const line = lv_obj_find_by_name(root, "prep_time_estimate");
    REQUIRE(line != nullptr);
    lv_subject_t* const visible = lv_xml_get_subject(nullptr, "preprint_estimate_visible");
    REQUIRE(visible != nullptr);

    // Before the view is shown there is no estimate: 0 and hidden.
    CHECK(lv_subject_get_int(visible) == 0);
    CHECK(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));

    // First open. show() defers the push; the drain runs on_activate()
    // -> update_prep_time_label() -> recalculate_estimate().
    view.show("prep.gcode", "", "PLA");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    CHECK(lv_subject_get_int(visible) == 1);
    CHECK_FALSE(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(line)).find("prep time") != std::string::npos);

    // The binding is reactive in both directions, not a one-shot at build.
    lv_subject_set_int(visible, 0);
    process_lvgl(20);
    CHECK(lv_obj_has_flag(line, LV_OBJ_FLAG_HIDDEN));

    view.hide();
    helix::ui::UpdateQueue::instance().drain();
    lv_timer_handler(); // the close callback runs on the next tick
}

TEST_CASE_METHOD(LVGLUITestFixture, "History row lives in the metadata strip",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* const row = lv_obj_find_by_name(root, "history_status_row");
    lv_obj_t* const strip = lv_obj_find_by_name(root, "detail_metadata_overlay");
    REQUIRE(row != nullptr);
    REQUIRE(strip != nullptr);
    bool inside = false;
    for (lv_obj_t* p = lv_obj_get_parent(row); p; p = lv_obj_get_parent(p)) {
        inside = inside || p == strip;
    }
    CHECK(inside);
    CHECK(std::string(lv_obj_get_name(lv_obj_get_parent(row))) == "detail_history_wrap");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Print button sits outside the options scroll area",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* const scroll = lv_obj_find_by_name(root, "detail_options_scroll");
    lv_obj_t* const print = lv_obj_find_by_name(root, "print_button");
    REQUIRE(scroll != nullptr);
    REQUIRE(print != nullptr);
    for (lv_obj_t* p = lv_obj_get_parent(print); p; p = lv_obj_get_parent(p)) {
        CHECK(p != scroll);
    }
    CHECK(lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE));
    // The fade must never take touches from the tiles under it.
    lv_obj_t* const cue = lv_obj_find_by_name(root, "detail_options_cue");
    REQUIRE(cue != nullptr);
    CHECK_FALSE(lv_obj_has_flag(cue, LV_OBJ_FLAG_CLICKABLE));
}

// ============================================================================
// filament_mismatch means MATERIAL, and only material
// ============================================================================
//
// The subject print_file_detail.xml:303 binds the warning triangle's hidden flag
// to. recompute_preflight() used to set it on ANY severity other than Ok, which
// flattened three different problems into one lamp: an empty lane (which already
// has empty_tools_warning), a material mismatch, and a colour mismatch - the
// card-level colour alarm that was explicitly decided against, since the stacked
// chip shows both colours side by side and its own surround says the rest.
//
// PreflightResult already separated them: has_block() is the empty lane,
// has_advisory() is the material mismatch. Only this call site did not ask.

namespace {

/// A started mock backend on AmsState, torn down on scope exit. AmsState is a
/// singleton, so a test that installs a backend and walks away leaves it live for
/// every test that runs after it. Mirrors MappingCardRenderFixture's wiring.
struct ScopedAmsBackend {
    helix::AmsBackendMock* backend = nullptr;

    explicit ScopedAmsBackend(int slot_count) {
        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<helix::AmsBackendMock>(slot_count);
        backend = owned.get();
        backend->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        backend->start();
    }

    ~ScopedAmsBackend() {
        helix::ui::UpdateQueue::instance().drain();
        if (backend) {
            backend->stop();
        }
        auto& ams = helix::AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "filament_mismatch is lit by a material mismatch and by nothing else",
                 "[print_select][detail_view][subjects][preflight][colour_mismatch]") {
    CacheDirGuard guard;
    // ONE lane, so the mapping cannot wander: with no colour match available the
    // positional fallback lands T0 on slot 0 and every SECTION validates the
    // pairing it set up rather than one the seeding chose.
    ScopedAmsBackend ams(1);

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    REQUIRE(view.create(test_screen()) != nullptr);

    // hide() has to run even when a REQUIRE fails. A section that leaves the
    // overlay pushed trips NavigationManager's strict-mode check on the NEXT
    // section's show(), turning one failed assertion into a SIGABRT that buries
    // which case actually broke.
    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    lv_subject_t* const triangle = lv_xml_get_subject(nullptr, "filament_mismatch");
    lv_subject_t* const empty_warning = lv_xml_get_subject(nullptr, "empty_tools_warning");
    REQUIRE(triangle != nullptr);
    REQUIRE(empty_warning != nullptr);

    // A warmed tools-used cache is what makes recompute_preflight() run at all:
    // it is a no-op until either the viewer has parsed or a headless scan landed.
    constexpr size_t kSize = 4321;
    constexpr time_t kMtime = 8765;
    helix::ToolsUsedCache warmer;
    warmer.store("sub/one_tool.gcode", kSize, kMtime, {0});

    // T0 prints red PLA, per the slicer palette show() is handed.
    const std::vector<std::string> colors{"#FF0000"};
    const std::vector<std::string> materials{"PLA"};

    // sync_external_identity deliberately drops SlotStatus (no real backend accepts a
    // user-written status), so the empty case has to go through
    // force_slot_status - see ams_backend_mock.cpp:999.
    auto load_lane = [&ams](uint32_t rgb, const char* material, helix::SlotStatus status) {
        auto slot = ams.backend->get_slot_info(0);
        slot.color_rgb = rgb;
        slot.material = material;
        ams.backend->sync_external_identity(0, slot);
        ams.backend->force_slot_status(0, status);
    };

    // Asserting the severity is load-bearing, not belt-and-braces: a result with
    // NO checks at all leaves the subject at 0 and would pass the colour SECTION
    // under both the old code and the new, proving nothing about either.
    auto only_severity = [&view](helix::ToolCheck::Severity expected) {
        const auto& checks = view.preflight_result().checks;
        REQUIRE(checks.size() == 1);
        REQUIRE(checks[0].severity == expected);
    };

    SECTION("a colour-only mismatch leaves the triangle dark") {
        load_lane(0x0000FF, "PLA", helix::SlotStatus::LOADED); // right polymer, nowhere near red
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::ColorMismatch);
        CHECK(lv_subject_get_int(triangle) == 0);
        CHECK(lv_subject_get_int(empty_warning) == 0);
    }

    SECTION("a material mismatch lights it") {
        load_lane(0xFF0000, "PETG", helix::SlotStatus::LOADED); // exactly the file's colour
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::MaterialMismatch);
        CHECK(lv_subject_get_int(triangle) == 1);
        CHECK(lv_subject_get_int(empty_warning) == 0);
    }

    SECTION("an empty lane is its own signal, not the triangle") {
        load_lane(0xFF0000, "PLA", helix::SlotStatus::EMPTY);
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::EmptySlot);
        CHECK(lv_subject_get_int(triangle) == 0);
        CHECK(lv_subject_get_int(empty_warning) == 1); // the block, published separately
    }

    SECTION("a lane that satisfies the file lights nothing") {
        // The complement. Without it a subject wired to a constant 0 would pass
        // the two dark cases above and the empty case's triangle assertion too.
        load_lane(0xFF0000, "PLA", helix::SlotStatus::LOADED);
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::Ok);
        CHECK(lv_subject_get_int(triangle) == 0);
        CHECK(lv_subject_get_int(empty_warning) == 0);
    }
}

// ============================================================================
// One card, one gate
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "One filament card, titled FILAMENTS, with the tap chevron",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);

    // The second card is gone: one surface for the chips on every backend.
    CHECK(lv_obj_find_by_name(root, "color_requirements_card") == nullptr);
    CHECK(lv_obj_find_by_name(root, "color_swatches_row") == nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);
    // Chrome absorbed from the retired card: the tap cue and the empty-lane
    // warning, plus the mismatch icon this card already had.
    CHECK(lv_obj_find_by_name(card, "color_card_remap_chevron") != nullptr);

    // The two header lamps must not be the same lamp. While they lived in
    // mutually exclusive cards a user could never see both at once; one card
    // puts them adjacent in the same row, and two identical amber triangles
    // report "something is wrong" twice without saying which thing. An empty
    // lane BLOCKS the print, a material mismatch only advises, and the app's
    // canonical severity mapping (ui_preflight_check_modal.cpp severity_visual)
    // already draws them close/danger and alert/warning respectively.
    //
    // The icon widget is an lv_label holding the glyph's codepoint, so this
    // compares what is actually drawn rather than the XML attribute that asked
    // for it - a src that silently failed to resolve would fall back and be
    // caught here too.
    lv_obj_t* const mismatch = lv_obj_find_by_name(card, "filament_mismatch_icon");
    lv_obj_t* const empty_lane = lv_obj_find_by_name(card, "empty_tools_warning_icon");
    REQUIRE(mismatch != nullptr);
    REQUIRE(empty_lane != nullptr);

    const char* const mismatch_glyph = lv_label_get_text(mismatch);
    const char* const empty_glyph = lv_label_get_text(empty_lane);
    REQUIRE(mismatch_glyph != nullptr);
    REQUIRE(empty_glyph != nullptr);
    CHECK(std::string(mismatch_glyph) != std::string(empty_glyph));

    // And they differ in colour too, so the pair stays distinguishable to a
    // reader who cannot tell the two small glyphs apart at 480x272.
    CHECK_FALSE(lv_color_eq(lv_obj_get_style_text_color(mismatch, LV_PART_MAIN),
                            lv_obj_get_style_text_color(empty_lane, LV_PART_MAIN)));
}

// The two surfaces used to disagree about exactly one state.
// FilamentMappingCard::should_show() hides the card for a single-lane print
// with bypass engaged - a K2 Plus user read a chip as "this maps to lane 2",
// tapped it to confirm, and printed off the bypass spool. But
// render_authoritative_chips() computed the SECOND surface as
// `!mapping_visible && swatches_card_visible_for(...)`, and that rule is TRUE
// for one tool on any multi-slot printer. Hiding one surface therefore SHOWED
// the other, and the chips the incident was about stayed on screen anyway.
// One card behind one predicate leaves nothing to disagree with.
TEST_CASE_METHOD(LVGLUITestFixture, "A bypassed single-lane print renders no filament chip at all",
                 "[print_select][detail_view][subjects][bypass]") {
    CacheDirGuard guard;
    // FOUR lanes on purpose. The retired rule asked only "multi-slot printer
    // AND at least one tool referenced", so a multi-slot backend is what made
    // the second surface appear for this one-tool file. A single-slot backend
    // would hide it for an unrelated reason and prove nothing about bypass.
    ScopedAmsBackend ams(4);

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    // A warmed cache is what makes show() render authoritatively in the same
    // call. Without it the used-tool set is unknown, every surface is
    // legitimately blank, and the assertions below would hold no matter what
    // the visibility rules did.
    constexpr size_t kSize = 999;
    constexpr time_t kMtime = 1111;
    helix::ToolsUsedCache warmer;
    warmer.store("sub/bypass_one.gcode", kSize, kMtime, {0});

    // THREE palette entries against ONE used tool, deliberately. The card
    // decides visibility from print_lane_requirement(), which prefers the used
    // set and falls back to the palette count - so a palette that happens to be
    // the same size as the used set makes a rule reading the WRONG one
    // indistinguishable from a rule reading the right one. A 3-colour file that
    // really prints one tool is a re-sliced or multi-variant file, and it is the
    // shape the bypass gate has to survive.
    const std::vector<std::string> palette{"#FF0000", "#00FF00", "#0000FF"};
    const std::vector<std::string> materials{"PLA", "PETG", "ABS"};

    lv_subject_t* const visible = lv_xml_get_subject(nullptr, "filament_mapping_visible");
    lv_subject_t* const remappable = lv_xml_get_subject(nullptr, "color_card_remappable");
    REQUIRE(visible != nullptr);
    REQUIRE(remappable != nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);

    SECTION("bypass engaged: no chip, no card, no tap affordance") {
        REQUIRE(helix::test::unload_for_bypass(*ams.backend));
        REQUIRE(ams.backend->enable_bypass().success());
        REQUIRE(ams.backend->is_bypass_active());
        REQUIRE(helix::AmsState::instance().any_bypass_active());

        view.show("bypass_one.gcode", "sub", "PLA", palette, materials, kSize, kMtime);

        CHECK(lv_subject_get_int(visible) == 0);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
        // The teeth. "top_band" is a child of filament_swatch and of nothing
        // else in the tree, so finding one anywhere under the detail root means
        // a chip was drawn. The retired swatch renderer drew exactly one here.
        CHECK(lv_obj_find_by_name(root, "top_band") == nullptr);
    }

    SECTION("same file WITHOUT bypass: the one card renders its chip") {
        // The known positive. Without it the zeros above could come from a
        // fixture that cannot produce a chip under any conditions.
        REQUIRE_FALSE(helix::AmsState::instance().any_bypass_active());

        view.show("bypass_one.gcode", "sub", "PLA", palette, materials, kSize, kMtime);

        CHECK(lv_subject_get_int(visible) == 1);
        CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_find_by_name(root, "top_band") != nullptr);
    }
}

// The chevron and the card's clickable flag both read color_card_remappable,
// and it is now published from the same call that publishes the card's
// visibility. This walks the whole `card_visible && remap available`
// truth table, because each half fails in its own way: without the first, a
// hidden card lights a chevron pointing at chips nobody can see (the state the
// retired second surface used to draw); without the second, a card on a backend
// with no remap picker cues a tap it would silently drop.
TEST_CASE_METHOD(LVGLUITestFixture, "The tap chevron tracks the card, and the backend",
                 "[print_select][detail_view][subjects][remap]") {
    CacheDirGuard guard;
    // FOUR lanes: enough for the multi-tool half of the card's tool-count rule,
    // so a one-tool file is not what decides visibility in any section below.
    ScopedAmsBackend ams(4);

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    constexpr size_t kSize = 2468;
    constexpr time_t kMtime = 1357;
    helix::ToolsUsedCache warmer;
    warmer.store("sub/two_tools.gcode", kSize, kMtime, {0, 1});
    warmer.store("sub/no_palette.gcode", kSize, kMtime, {0});

    lv_subject_t* const visible = lv_xml_get_subject(nullptr, "filament_mapping_visible");
    lv_subject_t* const remappable = lv_xml_get_subject(nullptr, "color_card_remappable");
    lv_subject_t* const needs_setup = lv_xml_get_subject(nullptr, "color_card_remap_needs_setup");
    lv_subject_t* const help_visible = lv_xml_get_subject(nullptr, "color_card_remap_help_visible");
    REQUIRE(visible != nullptr);
    REQUIRE(remappable != nullptr);
    REQUIRE(needs_setup != nullptr);
    REQUIRE(help_visible != nullptr);

    const std::vector<std::string> two_colors{"#FF0000", "#00FF00"};
    const std::vector<std::string> two_materials{"PLA", "PETG"};

    SECTION("GcodeRewrite with no plugin: chevron dark, card greyed, reason stated") {
        // The case this predicate exists for. A tool changer with no ASSIGN_TOOL
        // rewrites the job file instead, which needs the HelixPrint plugin to
        // keep print history readable. With the plugin term in the opener alone,
        // can_remap() answered true here: chevron lit, tap refused.
        ams.backend->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        get_printer_state().set_helix_plugin_installed(false);
        helix::ui::UpdateQueue::instance().drain();
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        CHECK(view.current_remap_block() == helix::printer::RemapBlock::NeedsPlugin);
        CHECK(lv_subject_get_int(visible) == 1);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_subject_get_int(needs_setup) == 1);
        // A greyed control with no way to ask why reads as a bug.
        CHECK(lv_subject_get_int(help_visible) == 1);

        // The card must NOT carry LV_STATE_DISABLED. LVGL gates PRESSED,
        // PRESSING and CLICKED on !lv_obj_has_state(obj, LV_STATE_DISABLED), so
        // a disabled card takes no pointer events and the explanation becomes
        // unreachable on a real panel - which a ctl-driven click would not
        // catch, because ctl bypasses the input device entirely.
        lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
        lv_obj_t* const help = lv_obj_find_by_name(root, "color_card_remap_help");
        REQUIRE(card != nullptr);
        REQUIRE(help != nullptr);
        CHECK_FALSE(lv_obj_has_state(card, LV_STATE_DISABLED));
        CHECK_FALSE(lv_obj_has_state(help, LV_STATE_DISABLED));
        CHECK(lv_obj_has_flag(help, LV_OBJ_FLAG_CLICKABLE));
        CHECK_FALSE(lv_obj_has_flag(help, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("GcodeRewrite with the plugin: chevron lit, nothing greyed or explained") {
        ams.backend->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        get_printer_state().set_helix_plugin_installed(true);
        helix::ui::UpdateQueue::instance().drain();
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        CHECK(view.current_remap_block() == helix::printer::RemapBlock::None);
        CHECK(lv_subject_get_int(remappable) == 1);
        CHECK(lv_subject_get_int(needs_setup) == 0);
        CHECK(lv_subject_get_int(help_visible) == 0);
    }

    SECTION("GcodeRewrite on a transport without local copies is simply not offered") {
        // The plugin is installed, so nothing here can be fixed by the user:
        // no greyed card, no Set up, no help icon, and the rewrite-gated option
        // rows read the same refusal.
        MockPrinter device;
        device.api.transfers_mock().mock_no_local_copies();
        view.set_analysis_dependencies(&device.api, &get_printer_state());
        ams.backend->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        get_printer_state().set_helix_plugin_installed(true);
        helix::ui::UpdateQueue::instance().drain();
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        CHECK(view.current_remap_block() == helix::printer::RemapBlock::NotOnThisDevice);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_subject_get_int(needs_setup) == 0);
        CHECK(lv_subject_get_int(help_visible) == 0);
        lv_subject_t* const rewrite = lv_xml_get_subject(nullptr, "detail_gcode_rewrite_available");
        REQUIRE(rewrite != nullptr);
        CHECK(lv_subject_get_int(rewrite) == 0);

        device.api.transfers_mock().mock_no_local_copies(false);
        view.set_analysis_dependencies(&device.api, &get_printer_state());
        CHECK(lv_subject_get_int(rewrite) == 1);
        view.hide();
        helix::ui::UpdateQueue::instance().drain();
        view.set_analysis_dependencies(nullptr, &get_printer_state());
    }

    SECTION("an unfinished plugin probe greys nothing") {
        // -1 is what the subject holds until discovery answers, which is after
        // first paint. Treating it as absent would grey the card on every boot
        // and then withdraw it.
        ams.backend->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        CHECK(view.current_remap_block() == helix::printer::RemapBlock::Probing);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_subject_get_int(needs_setup) == 0);
        CHECK(lv_subject_get_int(help_visible) == 0);
    }

    SECTION("a firmware-table remap is never warned that the job file gets rewritten") {
        // The degraded-history advisory only makes sense for a GcodeRewrite
        // remap, which prints a modified copy and needs the plugin to put the
        // original name back. A backend with its own picker routes in firmware
        // and never touches the file, so an old Moonraker costs it nothing - and
        // the modal behind this cue would tell its user the file is rewritten,
        // which on that printer does not happen.
        ams.backend->set_snapmaker_mode(true);
        lv_subject_set_int(
            get_printer_state().versions_state().get_moonraker_history_degraded_subject(), 1);
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        REQUIRE(view.current_remap_block() == helix::printer::RemapBlock::None);
        CHECK(lv_subject_get_int(remappable) == 1);
        CHECK(lv_subject_get_int(help_visible) == 0);

        lv_subject_set_int(
            get_printer_state().versions_state().get_moonraker_history_degraded_subject(), 0);
    }

    SECTION("card shown on a backend with a picker: chevron lit") {
        // Snapmaker U1 shape - mapping not editable inline, but the backend has
        // a native remap picker, so current_remap_block() is None. The known
        // positive: without it the zeros below could be a subject nothing ever
        // sets.
        ams.backend->set_snapmaker_mode(true);
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        CHECK(lv_subject_get_int(visible) == 1);
        CHECK(lv_subject_get_int(remappable) == 1);
        CHECK(lv_obj_find_by_name(root, "top_band") != nullptr);
    }

    SECTION("card hidden on that same backend: chevron dark") {
        // Same backend, same picker - only the card is gone, because an empty
        // palette leaves the card with no tool to map. The chevron must follow
        // the card, not the backend.
        ams.backend->set_snapmaker_mode(true);
        view.show("no_palette.gcode", "sub", "PLA", {}, {}, kSize, kMtime);

        CHECK(lv_subject_get_int(visible) == 0);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_obj_find_by_name(root, "top_band") == nullptr);
    }

    SECTION("card shown on a backend with no picker: chevron dark") {
        // The other half. A backend with no remap route at all - ACE, or a
        // single-extruder printer with no AMS - still makes the card worth
        // showing, because the chips say which lane each tool resolves to. A tap
        // opens nothing and must not be advertised.
        //
        // Declared explicitly. The plain mock used to answer RemapStrategy::None
        // for every non-Snapmaker mode, which is what made this case free - and
        // also what left the picker unreachable under --test for the five
        // backends that really do declare Native.
        ams.backend->set_remap_strategy(helix::AmsBackend::RemapStrategy::None);
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

        // The reason, not just the absence: a dark chevron for the wrong reason
        // is the bug this predicate exists to stop.
        CHECK(view.current_remap_block() == helix::printer::RemapBlock::NoStrategy);
        CHECK(lv_subject_get_int(visible) == 1);
        CHECK(lv_subject_get_int(remappable) == 0);
        CHECK(lv_obj_find_by_name(root, "top_band") != nullptr);
    }
}

// One tap, one picker. The merged card inherits FilamentMappingCard::create()'s
// own CLICKED handler; the retired second card carried its own
// lv_obj_add_event_cb, and moving THAT onto this widget as well would leave two
// handlers on one object. LVGL fires every matching handler, so a single tap
// would call the remap opener twice - which on a live screen is the picker
// opening over itself. Pinned because the obvious way to "move the tap handler
// to the surviving card" is exactly the way that breaks it.
TEST_CASE_METHOD(LVGLUITestFixture, "A tap on the filament card opens the remap picker once",
                 "[print_select][detail_view][remap]") {
    CacheDirGuard guard;
    ScopedAmsBackend ams(4);
    // Snapmaker U1 shape: the only mock configuration where a tap is meant to
    // reach the opener at all (current_remap_block() gates on the backend's
    // remap strategy).
    ams.backend->set_snapmaker_mode(true);

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    int opens = 0;
    view.set_on_remap_requested([&opens]() { ++opens; });

    constexpr size_t kSize = 1122;
    constexpr time_t kMtime = 3344;
    helix::ToolsUsedCache warmer;
    warmer.store("sub/two_tools.gcode", kSize, kMtime, {0, 1});
    view.show("two_tools.gcode", "sub", "PLA", {"#FF0000", "#00FF00"}, {"PLA", "PETG"}, kSize,
              kMtime);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);
    // The card is on screen and says it can be tapped - otherwise a count of 1
    // below could mean "one handler" or "the guard swallowed the second".
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "color_card_remappable")) == 1);

    lv_obj_send_event(card, LV_EVENT_CLICKED, nullptr);
    CHECK(opens == 1);
}

// ============================================================================
// Start-blocked reason (prestonbrown/helixscreen#1395)
// ============================================================================
//
// The Print button's disabled state binds print_select_can_print (owned by
// PrintSelectPanel), and the reason label beside it binds
// print_select_blocked_reason with visibility keyed to the same subject plus
// print_select_button_mode (queue mode keeps the reason on screen as the queue
// hint). Here all three are stubbed so the XML contract is pinned
// independently of the panel: blocked (0) = button disabled AND reason
// visible; printable (1) = button enabled AND no reason on screen.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "print button disables with a visible reason while a print runs",
                 "[print_select][detail][xml][1395]") {
    // Static: the global registry outlives the test, and print_file_detail's
    // bindings resolve these production names in whatever test builds that
    // view next. A stack subject here dangles for that test (SIGSEGV adding
    // an observer to the dead subject).
    static lv_subject_t can_print;
    lv_subject_init_int(&can_print, 0);
    lv_xml_register_subject(nullptr, "print_select_can_print", &can_print);

    static lv_subject_t button_mode;
    lv_subject_init_int(&button_mode, 0);
    lv_xml_register_subject(nullptr, "print_select_button_mode", &button_mode);

    static char reason_buf[96];
    static lv_subject_t reason;
    lv_subject_init_string(&reason, reason_buf, nullptr, sizeof(reason_buf),
                           "Printing: start after this job");
    lv_xml_register_subject(nullptr, "print_select_blocked_reason", &reason);

    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);

    // The tree's observers point at the stack subjects above; scrub them before
    // those go out of scope, or LVGL teardown walks a dead subject.
    struct RootDelete {
        lv_obj_t* r;
        ~RootDelete() {
            if (r && lv_obj_is_valid(r)) {
                lv_obj_delete(r);
            }
        }
    } root_delete{root};

    lv_obj_t* const print_button = lv_obj_find_by_name(root, "print_button");
    REQUIRE(print_button != nullptr);
    lv_obj_t* const reason_label = lv_obj_find_by_name(root, "print_blocked_reason");
    REQUIRE(reason_label != nullptr);

    // Blocked: disabled button, reason on screen carrying the subject's text.
    REQUIRE(lv_obj_has_state(print_button, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_flag(reason_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(reason_label)) == "Printing: start after this job");

    // Printable: enabled button, reason gone.
    lv_subject_set_int(&can_print, 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(print_button, LV_STATE_DISABLED));
    CHECK(lv_obj_has_flag(reason_label, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "More-below subject tracks the options scroll area",
                 "[print_select][detail][xml]") {
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });
    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    struct CloseOnExit {
        helix::ui::PrintSelectDetailView& v;
        ~CloseOnExit() {
            v.hide();
            helix::ui::UpdateQueue::instance().drain();
            lv_timer_handler(); // the close callback runs on the next tick
        }
    } closer{view};

    lv_obj_t* const scroll = lv_obj_find_by_name(root, "detail_options_scroll");
    lv_subject_t* const more = lv_xml_get_subject(nullptr, "detail_options_more_below");
    REQUIRE(scroll != nullptr);
    REQUIRE(more != nullptr);

    // Force an overflow: a tall child in the scroll area.
    lv_obj_t* filler = lv_obj_create(scroll);
    lv_obj_set_size(filler, 10, 4000);
    lv_obj_update_layout(root);
    lv_obj_send_event(scroll, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 1);

    lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 0);

    lv_obj_delete(filler);
    lv_obj_update_layout(root);
    lv_obj_send_event(scroll, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 0);
}

// ============================================================================
// Pre-start object picks
// ============================================================================

class PrintSelectDetailViewTestAccess {
  public:
    static void begin_viewer_load(helix::ui::PrintSelectDetailView& view, const std::string& path) {
        view.begin_viewer_load(path);
    }
    static lv_obj_t* gcode_viewer(const helix::ui::PrintSelectDetailView& view) {
        return view.gcode_viewer_;
    }
};

namespace {

const char* kThreeParts =
    "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4 POLYGON=[[20,-9],[31,-9],[31,1],[20,1]]\n"
    "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
    "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n";

/// A printer with or without [exclude_object] configured.
struct ExcludeObjectHardware {
    explicit ExcludeObjectHardware(bool configured) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(configured ? nlohmann::json{"exclude_object", "extruder"}
                                    : nlohmann::json{"extruder"});
        get_printer_state().set_hardware(hw);
    }
    ~ExcludeObjectHardware() {
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
    }
};

/// A shown detail view whose scan of @p file found @p defines.
struct OpenDetail {
    CacheDirGuard cache;
    helix::ui::PrintSelectDetailView view;

    OpenDetail(lv_obj_t* screen, const std::string& file, const char* defines) {
        register_xml_callbacks({
            {"on_print_select_detail_backdrop", detail_noop_cb},
            {"on_print_select_print_button", detail_noop_cb},
            {"on_print_select_delete_button", detail_noop_cb},
            {"on_print_detail_back_clicked", detail_noop_cb},
            {"on_toggle_sliced_colors", detail_noop_cb},
            {"on_print_select_detail_objects", detail_noop_cb},
        });
        view.set_dependencies(nullptr, &get_printer_state());
        view.init_subjects();
        REQUIRE(view.create(screen) != nullptr);
        helix::gcode::ScanResult scan;
        scan.objects = helix::gcode::collect_exclude_object_defines(defines);
        view.get_prep_manager()->set_cached_scan_result(scan, file);
        view.show(file, "", "PLA");
        settle();
    }
    ~OpenDetail() {
        close();
    }
    void close() {
        view.hide();
        settle();
        lv_timer_handler(); // the close callback runs on the next tick
    }
    static void settle() {
        helix::ui::UpdateQueue::instance().drain();
    }
    static int subject_int(const char* name) {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Details lists the scanned objects in file order",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    const auto& defined = d.view.exclude_objects().get_defined_objects();
    REQUIRE(defined == std::vector<std::string>{"Cone_id_0", "Cube_id_1", "Cylinder_id_2"});
    const auto cone = d.view.exclude_objects().get_object_geometry("Cone_id_0");
    REQUIRE(cone.has_value());
    CHECK(cone->has_bbox);
    CHECK(OpenDetail::subject_int("detail_exclude_available") == 1);
    // The printer's live state is untouched.
    CHECK(get_printer_state().excluded_objects_state().get_defined_objects().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The skip option needs [exclude_object], two objects and G-code",
                 "[print_select][detail_view][pre_start_exclude]") {
    SECTION("no [exclude_object]") {
        ExcludeObjectHardware hw(false);
        OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
        CHECK(OpenDetail::subject_int("detail_exclude_available") == 0);
    }
    SECTION("one object") {
        ExcludeObjectHardware hw(true);
        OpenDetail d(test_screen(), "solo.gcode", "EXCLUDE_OBJECT_DEFINE NAME=Solo CENTER=1,1\n");
        CHECK(OpenDetail::subject_int("detail_exclude_available") == 0);
    }
    // A 3MF is pinned by the pure rule's test: the scan replaces a .3mf's cached
    // result with an empty one, so a view-level case would pass on the count alone.
}

TEST_CASE_METHOD(LVGLUITestFixture, "A pick toggles in the private state, matched upper-case",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    d.view.toggle_exclude_pick("CUBE_ID_1");
    OpenDetail::settle();
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 1);
    CHECK(std::string(lv_subject_get_string(
              lv_xml_get_subject(nullptr, "detail_exclude_pick_count_text"))) == "1");
    CHECK(get_printer_state().excluded_objects_state().get_excluded_objects().empty());

    d.view.toggle_exclude_pick("cube_id_1");
    OpenDetail::settle();
    CHECK(d.view.exclude_picks().empty());
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 0);

    d.view.toggle_exclude_pick("Not_an_object");
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "A name the printer cannot be sent is refused with a toast",
                 "[print_select][detail_view][pre_start_exclude]") {
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&](const std::string& msg) { warnings.push_back(msg); });
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "names.gcode",
                 "EXCLUDE_OBJECT_DEFINE NAME='Part 1' CENTER=1,1\n"
                 "EXCLUDE_OBJECT_DEFINE NAME=Pi\xc3\xa8"
                 "ce CENTER=5,5\n"
                 "EXCLUDE_OBJECT_DEFINE NAME=Plain CENTER=9,9\n");

    d.view.toggle_exclude_pick("Part 1");
    d.view.toggle_exclude_pick("Pi\xc3\xa8"
                               "ce");
    d.view.toggle_exclude_pick("Plain");
    helix::ui::set_test_notification_warning_hook(nullptr);

    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Plain"});
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("Part 1") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Picks clear when the user leaves the file, not on a suspend",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");

    d.view.on_deactivating(DeactivateReason::Suspended);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});

    d.close();
    CHECK(d.view.exclude_picks().empty());

    // Re-opening the same file lists its objects again, with no picks.
    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();
    CHECK(d.view.exclude_objects().get_defined_objects().size() == 3);
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Picks held for a start survive the hide and a same-file re-show",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");

    d.view.hold_picks_for_start(true);
    CHECK(d.view.picks_held_for_start());
    d.close();
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});

    // The start failed: details comes back for the same file with the pick intact.
    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();
    CHECK(d.view.exclude_objects().get_defined_objects().size() == 3);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 1);
    // The hold is spent by that show; leaving the file now clears.
    CHECK_FALSE(d.view.picks_held_for_start());
    d.close();
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "A held pick does not carry over to another file",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");

    d.view.hold_picks_for_start(true);
    d.close();
    d.view.show("other.gcode", "", "PLA");
    OpenDetail::settle();
    CHECK(d.view.exclude_objects().get_excluded_objects().empty());
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 0);
    CHECK_FALSE(d.view.picks_held_for_start());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Every object picked is reported, and dropping picks says so",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    CHECK_FALSE(d.view.drop_exclude_picks());

    d.view.toggle_exclude_pick("Cone_id_0");
    d.view.toggle_exclude_pick("Cube_id_1");
    CHECK_FALSE(d.view.all_objects_picked());
    d.view.toggle_exclude_pick("Cylinder_id_2");
    CHECK(d.view.all_objects_picked());

    CHECK(d.view.drop_exclude_picks());
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Leaving details for shutdown clears picks even while held",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.hold_picks_for_start(true);

    d.view.on_deactivating(DeactivateReason::Shutdown);
    CHECK(d.view.exclude_picks().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The details skip button sits in the preview's top-left corner with a pick count",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    lv_obj_t* root = d.view.get_widget();
    lv_obj_t* btn = lv_obj_find_by_name(root, "btn_detail_objects");
    lv_obj_t* card = lv_obj_find_by_name(root, "detail_card");
    REQUIRE(btn != nullptr);
    REQUIRE(card != nullptr);
    process_lvgl(20);
    CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    // The row around the button leaves taps in its gaps to the preview.
    CHECK_FALSE(
        lv_obj_has_flag(lv_obj_find_by_name(root, "detail_top_left_row"), LV_OBJ_FLAG_CLICKABLE));

    lv_obj_update_layout(root);
    lv_area_t b, c;
    lv_obj_get_coords(btn, &b);
    lv_obj_get_coords(card, &c);
    // Aligned children sit inside the card's border.
    const int32_t inset =
        theme_manager_get_spacing("space_md") + lv_obj_get_style_border_width(card, LV_PART_MAIN);
    CHECK(b.x1 - c.x1 == inset);
    CHECK(b.y1 - c.y1 == inset);

    lv_obj_t* count = lv_obj_find_by_name(btn, "objects_pick_count");
    REQUIRE(count != nullptr);
    CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));

    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_pick("Cone_id_0");
    OpenDetail::settle();
    process_lvgl(20);
    CHECK_FALSE(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(lv_obj_get_child(count, 0))) == "2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "The details skip button hides for a single-object file",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "solo.gcode", "EXCLUDE_OBJECT_DEFINE NAME=Solo CENTER=1,1\n");
    process_lvgl(20);
    lv_obj_t* btn = lv_obj_find_by_name(d.view.get_widget(), "btn_detail_objects");
    REQUIRE(btn != nullptr);
    CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Exclude mode in details toggles a pick on a row tap, with no confirmation",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    process_lvgl(50);
    REQUIRE(d.view.is_exclude_mode_open());
    lv_obj_t* rows = lv_obj_find_by_name(d.view.get_widget(), "rows_container");
    REQUIRE(rows != nullptr);
    REQUIRE(lv_obj_get_child_count(rows) == 3);

    lv_obj_send_event(lv_obj_get_child(rows, 1), LV_EVENT_CLICKED, nullptr);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    OpenDetail::settle();
    process_lvgl(20);
    CHECK(lv_obj_has_flag(lv_obj_get_child(rows, 1), LV_OBJ_FLAG_CLICKABLE));

    lv_obj_send_event(lv_obj_get_child(rows, 1), LV_EVENT_CLICKED, nullptr);
    CHECK(d.view.exclude_picks().empty());

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Opening exclude mode in details keeps the picks and stays inside details",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");
    OpenDetail::settle();

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    process_lvgl(50);
    REQUIRE(d.view.is_exclude_mode_open());
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    // The list is part of details' own tree, not an overlay stacked over it.
    CHECK(lv_obj_find_by_name(d.view.get_widget(), "rows_container") != nullptr);
    CHECK(d.view.is_visible());

    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Leaving details closes exclude mode",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    REQUIRE(d.view.is_exclude_mode_open());

    d.close();
    process_lvgl(50);
    CHECK_FALSE(d.view.is_exclude_mode_open());
}

TEST_CASE_METHOD(LVGLUITestFixture, "A suspend closes exclude mode and keeps the picks",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    REQUIRE(d.view.is_exclude_mode_open());

    d.view.on_deactivating(DeactivateReason::Suspended);
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Clearing the details viewer closes exclude mode",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    lv_obj_t* viewer = lv_obj_find_by_name(d.view.get_widget(), "detail_gcode_viewer");
    REQUIRE(viewer != nullptr);
    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    REQUIRE(d.view.is_exclude_mode_open());

    // What the memory-pressure responder does to every live viewer.
    ui_gcode_viewer_clear_all_active();
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "Showing the open view for another file closes exclude mode",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    REQUIRE(d.view.is_exclude_mode_open());

    d.view.show("other.gcode", "", "PLA");
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());
}

TEST_CASE_METHOD(LVGLUITestFixture, "Picks the skip option no longer offers are never sent",
                 "[print_select][detail_view][pre_start_exclude]") {
    std::optional<ExcludeObjectHardware> hw(std::in_place, true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cone_id_0");
    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_pick("Cylinder_id_2");
    d.view.hold_picks_for_start(true);
    d.close();

    // The start failed, then the user switched to a printer without [exclude_object].
    hw.reset();
    ExcludeObjectHardware none(false);
    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();
    REQUIRE(OpenDetail::subject_int("detail_exclude_available") == 0);
    CHECK(d.view.exclude_picks().empty());
    CHECK_FALSE(d.view.all_objects_picked());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "In portrait the details object list covers the options and not the card",
                 "[print_select][detail_view][pre_start_exclude][portrait]") {
    // Tall enough that the options fill more than half the column.
    ScopedPortraitLayout portrait(480, 1000);
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    lv_obj_t* root = d.view.get_widget();
    process_lvgl(100); // the view settles before the user can tap
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    process_lvgl(600); // the slide-in
    REQUIRE(d.view.is_exclude_mode_open());
    lv_obj_update_layout(root);

    lv_obj_t* rows = lv_obj_find_by_name(root, "rows_container");
    REQUIRE(rows != nullptr);
    lv_area_t list, options, card;
    lv_obj_get_coords(lv_obj_get_parent(rows), &list);
    lv_obj_get_coords(lv_obj_find_by_name(root, "options_section"), &options);
    lv_obj_get_coords(lv_obj_find_by_name(root, "detail_card"), &card);
    INFO("list y1=" << list.y1 << " options y1=" << options.y1 << " card y2=" << card.y2);
    CHECK(list.y1 <= options.y1);
    CHECK(list.y1 > card.y2);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "An open details view follows the printer's [exclude_object] as it changes",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");
    OpenDetail::settle();
    REQUIRE(OpenDetail::subject_int("detail_exclude_available") == 1);

    // A printer switch to a machine without [exclude_object], details left open.
    get_printer_state().set_hardware(helix::PrinterDiscovery{});
    OpenDetail::settle();
    CHECK(OpenDetail::subject_int("detail_exclude_available") == 0);
    CHECK(d.view.exclude_picks().empty());

    // Back on a printer that has it: the same picks are offered again.
    helix::PrinterDiscovery with;
    with.parse_objects(nlohmann::json{"exclude_object", "extruder"});
    get_printer_state().set_hardware(with);
    OpenDetail::settle();
    CHECK(OpenDetail::subject_int("detail_exclude_available") == 1);
    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(LVGLUITestFixture, "The pick count counts only picks the file still defines",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_pick("Cube_id_1");
    d.view.toggle_exclude_pick("Cone_id_0");
    d.view.hold_picks_for_start(true);
    d.close();

    // The same file comes back, now defining only two of its three objects.
    helix::gcode::ScanResult scan;
    scan.objects = helix::gcode::collect_exclude_object_defines(
        "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n");
    d.view.get_prep_manager()->set_cached_scan_result(scan, "parts.gcode");
    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();

    CHECK(d.view.exclude_picks() == std::vector<std::string>{"Cone_id_0"});
    CHECK(OpenDetail::subject_int("detail_exclude_pick_count") == 1);
    CHECK(std::string(lv_subject_get_string(
              lv_xml_get_subject(nullptr, "detail_exclude_pick_count_text"))) == "1");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Re-showing the open view for its file lists the objects again",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();

    CHECK(d.view.exclude_objects().get_defined_objects().size() == 3);
    CHECK(OpenDetail::subject_int("detail_exclude_available") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "An open view shown for another file does not list the previous file's parse",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);

    // The viewer parses parts.gcode and finds an object past the scan window.
    const auto path = std::filesystem::path(d.cache.dir) / "parts_full.gcode";
    {
        std::ofstream out(path);
        out << kThreeParts << "EXCLUDE_OBJECT_DEFINE NAME=Late_part CENTER=10,10\n"
            << "G1 Z0.2 F600\nEXCLUDE_OBJECT_START NAME=Late_part\n"
            << "G1 X10 Y10 E1 F1200\nG1 X20 Y10 E2\nEXCLUDE_OBJECT_END NAME=Late_part\n";
    }
    PrintSelectDetailViewTestAccess::begin_viewer_load(d.view, path.string());
    REQUIRE(wait_until(
        [&] {
            OpenDetail::settle();
            return d.view.exclude_objects().get_defined_objects().size() == 4;
        },
        60000));

    helix::gcode::ScanResult scan;
    scan.objects = helix::gcode::collect_exclude_object_defines(
        "EXCLUDE_OBJECT_DEFINE NAME=Left CENTER=1,1\n"
        "EXCLUDE_OBJECT_DEFINE NAME=Right CENTER=9,9\n");
    d.view.get_prep_manager()->set_cached_scan_result(scan, "other.gcode");
    d.view.show("other.gcode", "", "PLA");
    OpenDetail::settle();

    CHECK(d.view.exclude_objects().get_defined_objects() ==
          std::vector<std::string>{"Left", "Right"});
}

// The viewer outlives the view on teardown (its tree is deleted on a later
// tick), so a parse finishing after cleanup() must not reach the view.
TEST_CASE_METHOD(LVGLUITestFixture, "A parse finishing after cleanup never reaches the view",
                 "[print_select][detail_view][pre_start_exclude]") {
    OpenDetail d(test_screen(), "exclude_object_test.gcode", kThreeParts);
    REQUIRE(d.view.get_widget() != nullptr);
    lv_obj_t* viewer = lv_obj_find_by_name(d.view.get_widget(), "detail_gcode_viewer");
    REQUIRE(viewer != nullptr);
    auto& queue = helix::ui::UpdateQueue::instance();
    helix::ui::UpdateQueueTestAccess::drain_all(queue);

    int loaded = 0;
    PrintSelectDetailViewTestAccess::begin_viewer_load(
        d.view, "assets/test_gcodes/exclude_object_test.gcode");
    d.view.run_when_loaded([&loaded]() { ++loaded; });

    // The parse's result is built and waiting in the queue, not yet delivered.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (helix::ui::UpdateQueueTestAccess::queue_empty(queue) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(helix::ui::UpdateQueueTestAccess::queue_empty(queue));
    REQUIRE(loaded == 0);

    d.view.cleanup();
    helix::ui::UpdateQueueTestAccess::drain_all(queue);
    // What was queued was the parse result: the viewer installed it.
    REQUIRE(ui_gcode_viewer_has_content(viewer));
    CHECK(loaded == 0);
    CHECK_FALSE(d.view.is_gcode_loaded());
}

// The prep manager outlives cleanup(), and a scan it is still running can
// answer after it.
TEST_CASE_METHOD(LVGLUITestFixture, "A scan answering after cleanup never reaches the view",
                 "[print_select][detail_view][pre_start_exclude]") {
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    REQUIRE(d.view.exclude_objects().get_defined_objects().size() == 3);
    d.view.cleanup();
    REQUIRE(d.view.exclude_objects().get_defined_objects().size() == 3);

    // With no printer connection the scan answers at once, with no result.
    d.view.get_prep_manager()->scan_file_for_operations("other.gcode", "", "");
    OpenDetail::settle();
    CHECK(d.view.exclude_objects().get_defined_objects().size() == 3);
}

// A tree deleted by anything but destroy_overlay_ui() must not leave queued
// observers or cleanup() reaching a freed viewer.
TEST_CASE_METHOD(LVGLUITestFixture, "cleanup after the tree is deleted forgets the dead viewer",
                 "[print_select][detail_view]") {
    CacheDirGuard guard;
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });

    helix::ui::PrintSelectDetailView view;
    view.set_dependencies(nullptr, &get_printer_state());
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);
    REQUIRE(PrintSelectDetailViewTestAccess::gcode_viewer(view) != nullptr);

    lv_obj_delete(root);
    CHECK(PrintSelectDetailViewTestAccess::gcode_viewer(view) == nullptr);
    helix::ui::UpdateQueue::instance().drain();

    view.cleanup();
}

// The detail view's own show() is the only path back after a tree deleted
// outside destroy_overlay_ui(): it must build a fresh tree, with exclude mode's
// side list and viewer gone along with the old one.
TEST_CASE_METHOD(LVGLUITestFixture, "show after the tree is deleted externally re-creates it",
                 "[print_select][detail_view][pre_start_exclude]") {
    ExcludeObjectHardware hw(true);
    OpenDetail d(test_screen(), "parts.gcode", kThreeParts);
    d.view.toggle_exclude_mode();
    OpenDetail::settle();
    process_lvgl(50);
    REQUIRE(d.view.is_exclude_mode_open());

    lv_obj_delete(d.view.get_widget());
    OpenDetail::settle();
    CHECK_FALSE(d.view.is_exclude_mode_open());

    d.view.show("parts.gcode", "", "PLA");
    OpenDetail::settle();
    lv_obj_t* const root = d.view.get_widget();
    REQUIRE(root != nullptr);
    CHECK(lv_obj_is_valid(root));
    CHECK(lv_obj_find_by_name(root, "print_button") != nullptr);
    CHECK(PrintSelectDetailViewTestAccess::gcode_viewer(d.view) != nullptr);
}
