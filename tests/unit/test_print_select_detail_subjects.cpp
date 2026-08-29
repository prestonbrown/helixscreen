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
#include "ui_pre_print_options_renderer.h"
#include "ui_print_select_detail_view.h"
#include "ui_subject_registry.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "pre_print_option.h"
#include "preflight_validator.h"
#include "tools_used_cache.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Pre-print Option Subject Default Tests
// ============================================================================

namespace {

/// A two-option set in the shape the print-detail panel renders: one "skip"
/// option that ships enabled, one "add-on" that ships disabled. Built directly
/// rather than pulled from printer_database.json so the reset contract under
/// test does not move when the shipped DB does.
PrePrintOptionSet make_skip_and_addon_set() {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption skip;
    skip.id = "bed_mesh";
    skip.category = PrePrintCategory::Mechanical;
    skip.order = 10;
    skip.default_enabled = true; // "don't skip, do what the file says"
    skip.strategy_kind = PrePrintStrategyKind::MacroParam;
    // 4-arg form, as test_pre_print_options_renderer.cpp uses: adaptive_value keeps
    // its "1" default member initializer.
    skip.strategy = PrePrintStrategyMacroParam{"SKIP_BED_MESH", "0", "1", "0"};

    PrePrintOption addon;
    addon.id = "timelapse";
    addon.category = PrePrintCategory::Monitoring;
    addon.order = 10;
    addon.default_enabled = false; // "don't add extras by default"
    addon.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    addon.strategy = PrePrintStrategyPreStartGcode{"TIMELAPSE_RENDER"};

    set.options = {skip, addon};
    return set;
}

} // namespace

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
    AmsBackendMock* backend = nullptr;

    explicit ScopedAmsBackend(int slot_count) {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<AmsBackendMock>(slot_count);
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
        auto& ams = AmsState::instance();
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

    // set_slot_info deliberately drops SlotStatus (no real backend accepts a
    // user-written status), so the empty case has to go through
    // force_slot_status - see ams_backend_mock.cpp:999.
    auto load_lane = [&ams](uint32_t rgb, const char* material, SlotStatus status) {
        auto slot = ams.backend->get_slot_info(0);
        slot.color_rgb = rgb;
        slot.material = material;
        ams.backend->set_slot_info(0, slot, /*persist=*/false);
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
        load_lane(0x0000FF, "PLA", SlotStatus::LOADED); // right polymer, nowhere near red
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::ColorMismatch);
        CHECK(lv_subject_get_int(triangle) == 0);
        CHECK(lv_subject_get_int(empty_warning) == 0);
    }

    SECTION("a material mismatch lights it") {
        load_lane(0xFF0000, "PETG", SlotStatus::LOADED); // exactly the file's colour
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::MaterialMismatch);
        CHECK(lv_subject_get_int(triangle) == 1);
        CHECK(lv_subject_get_int(empty_warning) == 0);
    }

    SECTION("an empty lane is its own signal, not the triangle") {
        load_lane(0xFF0000, "PLA", SlotStatus::EMPTY);
        view.show("one_tool.gcode", "sub", "PLA", colors, materials, kSize, kMtime);
        view.recompute_preflight();

        only_severity(helix::ToolCheck::Severity::EmptySlot);
        CHECK(lv_subject_get_int(triangle) == 0);
        CHECK(lv_subject_get_int(empty_warning) == 1); // the block, published separately
    }

    SECTION("a lane that satisfies the file lights nothing") {
        // The complement. Without it a subject wired to a constant 0 would pass
        // the two dark cases above and the empty case's triangle assertion too.
        load_lane(0xFF0000, "PLA", SlotStatus::LOADED);
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
    CHECK(lv_obj_find_by_name(card, "empty_tools_warning_icon") != nullptr);
    CHECK(lv_obj_find_by_name(card, "filament_mismatch_icon") != nullptr);
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

    lv_subject_t* const visible = lv_xml_get_subject(nullptr, "filament_mapping_visible");
    lv_subject_t* const remappable = lv_xml_get_subject(nullptr, "color_card_remappable");
    REQUIRE(visible != nullptr);
    REQUIRE(remappable != nullptr);

    lv_obj_t* const card = lv_obj_find_by_name(root, "filament_mapping_card");
    REQUIRE(card != nullptr);

    SECTION("bypass engaged: no chip, no card, no tap affordance") {
        REQUIRE(ams.backend->enable_bypass().success());
        REQUIRE(ams.backend->is_bypass_active());
        REQUIRE(AmsState::instance().any_bypass_active());

        view.show("bypass_one.gcode", "sub", "PLA", {"#FF0000"}, {"PLA"}, kSize, kMtime);

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
        REQUIRE_FALSE(AmsState::instance().any_bypass_active());

        view.show("bypass_one.gcode", "sub", "PLA", {"#FF0000"}, {"PLA"}, kSize, kMtime);

        CHECK(lv_subject_get_int(visible) == 1);
        CHECK_FALSE(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_find_by_name(root, "top_band") != nullptr);
    }
}

// The chevron and the card's clickable flag both read color_card_remappable,
// and it is now published from the same call that publishes the card's
// visibility. This walks the whole `card_visible && color_card_opens_remap()`
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
        }
    } closer{view};

    constexpr size_t kSize = 2468;
    constexpr time_t kMtime = 1357;
    helix::ToolsUsedCache warmer;
    warmer.store("sub/two_tools.gcode", kSize, kMtime, {0, 1});
    warmer.store("sub/no_palette.gcode", kSize, kMtime, {0});

    lv_subject_t* const visible = lv_xml_get_subject(nullptr, "filament_mapping_visible");
    lv_subject_t* const remappable = lv_xml_get_subject(nullptr, "color_card_remappable");
    REQUIRE(visible != nullptr);
    REQUIRE(remappable != nullptr);

    const std::vector<std::string> two_colors{"#FF0000", "#00FF00"};
    const std::vector<std::string> two_materials{"PLA", "PETG"};

    SECTION("card shown on a backend with a picker: chevron lit") {
        // Snapmaker U1 shape - mapping not editable inline, but the backend has
        // a native remap picker, so color_card_opens_remap() is true. The known
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
        // The other half. The plain mock reports RemapStrategy::None, so the
        // card is worth showing - the chips still say which lane each tool
        // resolves to - but a tap opens nothing and must not be advertised.
        REQUIRE_FALSE(helix::ui::PrintSelectDetailView::color_card_opens_remap());
        view.show("two_tools.gcode", "sub", "PLA", two_colors, two_materials, kSize, kMtime);

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
    // reach the opener at all (color_card_opens_remap() gates on the backend's
    // remap strategy).
    ams.backend->set_snapmaker_mode(true);

    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
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
