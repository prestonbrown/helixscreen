// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_print_status.cpp
 * @brief print_status picks its layout from physical pixels, not
 * colspan/rowspan — the last and most entangled widget in the migration set.
 * `on_size_changed` (print_status_widget.cpp:447-497) derives a three-way
 * width band (compact/normal/wide) from `width_px` against
 * `widget_size::w_normal()`/`w_wide()`, and every one of its five span-reading
 * predicates now reads that band (or `height_px` directly) instead of the
 * `colspan`/`rowspan` arguments, which are ignored.
 *
 * `:450` publishes the band itself as `print_status_width_band` — a renamed,
 * *derived* subject. The XML consumer (`panel_widget_print_status.xml`'s two
 * `library_body_gap_*` `bind_style` entries) used to compare the raw colspan
 * against `ref_value="2"`/`"3"`; both the C++ publisher and the XML
 * `ref_value`s were updated together (now `1`=normal/`2`=wide) so producer
 * and consumer never disagree about what the subject means. The test below
 * asserts the subject transitions directly rather than the resulting
 * `pad_row` pixel value on `library_body`: at this binary's active theme
 * tier, `#space_md` (the wide gap) happens to equal the tier's *other*
 * baseline gap value, so a raw-pixel comparison is a coincidence trap, not a
 * real assertion — the subject is the actual contract `bind_style` reads.
 *
 * Every case below pairs its target pixels with a colspan/rowspan the *old*
 * span-based predicate would resolve to a *different* outcome, so an
 * implementation that still reads spans fails here instead of passing by
 * coincidence — the same shape as the other `[widget_size]` files in this
 * migration (see test_widget_size_job_queue.cpp, test_widget_size_camera.cpp).
 *
 * `print_status_layout_effective` (the `:455` predicate) is migrated and
 * tested here like the other four, but has no XML consumer today —
 * `update_view_subject()` re-derives its own `use_detailed` independently
 * rather than reading this subject back, and `grep -rl
 * print_status_layout_effective ui_xml/` returns nothing. That's a
 * pre-existing condition (not introduced by this migration) matching the
 * `print_stats_show_title` class found in a sibling task; left in place
 * because four existing tests in test_print_status_widget_layout_gate.cpp
 * already assert it and the task brief requires migrating (not removing) it.
 * Flagged here for whoever picks it up next.
 *
 * Every harness instance is scoped in its own block so `~PanelWidgetHarness`
 * (which detaches and destroys the widget) runs BEFORE
 * `destroy_formatter_for_test()` — matching
 * test_print_status_widget_recycle.cpp's teardown order. Calling it first
 * forces the shared DetailedFormatter singleton's refcount to 0 while the
 * widget is still attached and live, and the widget's own destructor then
 * touches formatter-dependent state that's already gone (SIGSEGV, confirmed
 * by hand while writing this file — not a hypothetical).
 *
 * PRE-EXISTING, UNRELATED bug this file works around: test_print_status_
 * fan_section.cpp's `FanPanelFixture` (its own file, not touched here)
 * constructs a real, local `PrintStatusPanel` and lets it fall out of scope
 * at the end of each of its test cases. `~PrintStatusPanel()`
 * (ui_panel_print_status.cpp:321) deinits its subjects but never
 * un-registers them from helix-xml's global subject table
 * (`lv_xml_register_subject(nullptr, "print_progress_text", ...)` and ~15
 * others) — so once that fixture's tests finish, those names dangle for the
 * rest of the process. `panel_widget_print_status.xml:194` shares
 * `print_progress_text` with the full-screen panel, and heap reuse from
 * unrelated allocations elsewhere in the suite (confirmed via lldb: two
 * DetailedFormatter alloc/free cycles in test_print_status_widget_tool_
 * override.cpp, which runs immediately before this file in declaration
 * order) eventually turns the dangling pointer into a real EXC_BAD_ACCESS
 * inside `lv_ll_ins_tail` the next time ANY test binds fresh XML to that
 * name — which every test in this file does, being the first to build the
 * real `panel_widget_print_status` component after that fixture runs.
 * `get_global_print_status_panel()` is production's own lazily-constructed,
 * process-lifetime singleton (see push_overlay(), panel_factory.cpp); poking
 * it once here re-registers every one of its subjects against stable,
 * long-lived storage, healing the dangling entries for the rest of the
 * binary — the same "ensure subjects" idiom used throughout the app
 * (`are_subjects_initialized()` + `init_subjects()`, e.g.
 * panel_factory.cpp:86). This is a workaround, not a fix — the actual bug is
 * FanPanelFixture's/PrintStatusPanel's teardown and belongs to whoever owns
 * that file.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_animations_enabled.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_size.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;
using namespace helix::ui;

namespace {

int width_band() {
    auto* subject = lv_xml_get_subject(nullptr, "print_status_width_band");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

int column_mode() {
    auto* subject = lv_xml_get_subject(nullptr, "print_status_column_mode");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

// See the file comment: heals a pre-existing dangling-subject bug in an
// unrelated fixture (test_print_status_fan_section.cpp's FanPanelFixture)
// that this file's real-XML component builds would otherwise crash into.
void heal_global_print_status_panel_subjects() {
    auto& panel = get_global_print_status_panel();
    if (!panel.are_subjects_initialized()) {
        panel.init_subjects();
    }
}

} // namespace

// --- :450 width band, published for library_body's per-tier gap -----------

TEST_CASE_METHOD(LVGLUITestFixture, "print_status width band follows pixels, not spans",
                 "[widget_size][print_status]") {
    // Reset PrinterState/formatter first — a prior test file in the same
    // [print_status] run may leave the singleton formatter's observers bound
    // to subjects from a PrinterState state it left behind.
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen());

        lv_obj_t* library_body = h.child("library_body");
        REQUIRE(library_body != nullptr);

        // Compact: below the normal floor. Contradicting span (colspan=5 —
        // makes the point that width, not span, decides).
        h.resize(5, 5, w_normal() - 1, 300);
        CHECK(width_band() == 0);

        // Normal band. Contradicting span (colspan=1 — old code's colspan==2
        // default-gap bind would not have applied at colspan=1).
        h.resize(1, 1, w_normal(), 300);
        CHECK(width_band() == 1);

        // Wide band. Contradicting span (colspan=1 — old code's colspan==3
        // wide-gap bind would not have applied at colspan=1).
        h.resize(1, 1, w_wide(), 300);
        CHECK(width_band() == 2);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- :455 layout_effective (user opt-in AND width clears the normal floor) --

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status layout_effective follows pixels, not spans, in detailed mode",
                 "[widget_size][print_status]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});

        // Below the normal floor. Contradicting span (colspan=5 — old
        // predicate colspan>=2 would have activated here).
        h.resize(5, 5, w_normal() - 1, 300);
        CHECK(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 0);

        // At/over the normal floor. Contradicting span (colspan=1 — old
        // predicate colspan>=2 would NOT have activated here).
        h.resize(1, 1, w_normal(), 300);
        CHECK(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 1);

        // Switching back to library must revert effective to 0 regardless of
        // width — re-run on_size_changed at the SAME pixels (set_config
        // alone doesn't recompute this subject; only on_size_changed does).
        h.widget().set_config({{"layout_style", "library"}});
        h.resize(1, 1, w_normal(), 300);
        CHECK(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 0);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- :464 compact mode -> view_subject_ -> card-body sibling visibility ----

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status compact mode follows pixels, not spans, and drives "
                 "which idle card is visible",
                 "[widget_size][print_status]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});

        lv_obj_t* idle_compact = h.child("print_card_idle_compact");
        lv_obj_t* idle_detailed = h.child("print_card_idle_detailed");
        REQUIRE(idle_compact != nullptr);
        REQUIRE(idle_detailed != nullptr);

        // Wide band, not compact. Contradicting span (colspan=1 — old
        // predicate colspan<=1 would have gone compact here).
        h.resize(1, 5, w_wide(), 300);
        process_lvgl(30);
        CHECK(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) == 2); // idle_detailed
        CHECK_FALSE(lv_obj_has_flag(idle_detailed, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(idle_compact, LV_OBJ_FLAG_HIDDEN));

        // Compact band. Contradicting span (colspan=5 — old predicate
        // colspan<=1 would NOT have gone compact here).
        h.resize(5, 5, w_normal() - 1, 300);
        process_lvgl(30);
        CHECK(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
              1); // idle_library_compact
        CHECK_FALSE(lv_obj_has_flag(idle_compact, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(idle_detailed, LV_OBJ_FLAG_HIDDEN));
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- Idle detailed hero: the filename spans the card, not the data column ---
//
// The active card got this in e779c972d; the idle twin kept its filename inside
// idle_data_col, beside a 45%-wide thumbnail, so a long name scrolled through
// roughly half the width it had available. Both cards now hoist it to a
// full-width header above the thumb/data row.

TEST_CASE_METHOD(LVGLUITestFixture, "print_status idle detailed filename spans the whole card",
                 "[widget_size][print_status]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});

        lv_obj_t* filename = h.child("detailed_idle_filename");
        lv_obj_t* main_row = h.child("idle_main_row");
        lv_obj_t* data_col = h.child("idle_data_col");
        REQUIRE(filename != nullptr);
        REQUIRE(main_row != nullptr);
        REQUIRE(data_col != nullptr);

        h.resize(1, 5, w_wide(), 300);
        process_lvgl(30);
        REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
                2); // idle_detailed

        // It is a sibling of the thumb/data row, not a child of the data column:
        // that is what makes the full width available to it.
        CHECK(lv_obj_get_parent(filename) == lv_obj_get_parent(main_row));
        CHECK(lv_obj_get_parent(filename) != data_col);

        // And it actually occupies that width. The data column is the ~55% that
        // was left over beside the 45% thumbnail, so spanning must beat it.
        const int32_t name_w = lv_obj_get_width(filename);
        const int32_t row_w = lv_obj_get_width(main_row);
        const int32_t col_w = lv_obj_get_width(data_col);
        INFO("filename=" << name_w << " row=" << row_w << " data_col=" << col_w);
        CHECK(name_w == row_w);
        CHECK(name_w > col_w);

        // A scrolling long mode is gated on the animations preference, so say so
        // here instead of inheriting whatever value another test left registered.
        // The guard is what puts it back: the preference is a process-global
        // subject that the global PrintStatusPanel watches through
        // observe<int>, and HelixTestFixture restores it AFTER its own drain,
        // so a value left flipped queues an apply callback nothing runs.
        ScopedAnimationsEnabled animations(true);
        REQUIRE(animations.available());
        lv_obj_update_layout(test_screen());

        // Long names must still scroll rather than ellipsize or clip.
        CHECK(lv_label_get_long_mode(filename) == LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- :477 use_column -> print_card_layout_ flex flow + thumb wrap sizing ---
//
// Mutation-checked predicate (per the task brief): forcing use_column to
// always true reddens the row-layout assertions below while the column case
// stays green — verified manually, not encoded as a test (see task report).

TEST_CASE_METHOD(LVGLUITestFixture, "print_status column/row card layout follows pixels, not spans",
                 "[widget_size][print_status][panel_widget]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen());
        // The card layout exists only while a print holds the machine.
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        process_lvgl(30);

        lv_obj_t* layout = h.child("print_card_layout");
        lv_obj_t* thumb_wrap = h.child("print_card_thumb_wrap");
        REQUIRE(layout != nullptr);
        REQUIRE(thumb_wrap != nullptr);

        // Column: normal band + tall enough. Contradicting span (colspan=3 —
        // old predicate required colspan==2 exactly, so 3 would have stayed
        // row).
        h.resize(3, 1, w_normal(), h_tall());
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
        CHECK(column_mode() == 1);
        CHECK(lv_obj_get_style_width(thumb_wrap, LV_PART_MAIN) == LV_PCT(100));

        // Row: wide band. Contradicting span (colspan=2, rowspan=3 — old
        // predicate colspan==2 && rowspan>=2 would have gone column).
        h.resize(2, 3, w_wide(), h_tall());
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
        CHECK(column_mode() == 0);
        CHECK(lv_obj_get_style_width(thumb_wrap, LV_PART_MAIN) == LV_PCT(40));

        // Row: normal band but too short. Contradicting span (colspan=2,
        // rowspan=5 — old predicate colspan==2 && rowspan>=2 would have gone
        // column).
        h.resize(2, 5, w_normal(), h_tall() - 1);
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
        CHECK(column_mode() == 0);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// TEST_MIRROR_OK: "mirror" here names a duplication that lives in PRODUCTION —
// print_status_widget.cpp re-derives the same gate in two places (:461 and
// :1712). The test reimplements neither: it drives the real widget through
// PanelWidgetHarness and asserts on the real show_filament_active subject, so
// it goes red if either production copy drifts.
// --- :461 show_filament_active (wide band AND filament extruded) + the :1712
// mirror (DetailedFormatter::update_filament_text() re-deriving the same gate
// from width_band_subject_ on a used_mm change alone, with no intervening
// on_size_changed call) ------------------------------------------------------

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status filament-active gate follows the wide width band, not span, "
                 "and the mirror re-derives it from used_mm alone",
                 "[widget_size][print_status]") {
    PrintStatusWidget::destroy_formatter_for_test();
    auto& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    lv_subject_set_int(ps.print_state().get_print_filament_used_subject(), 0);

    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});
        // LVGL fires a freshly-subscribed observer immediately on subscribe;
        // drain once before driving anything else so that initial fire
        // doesn't mask a broken predicate later.
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

        // The detailed active view exists only while a print holds the machine.
        helix::test::set_wire_state(ps, PrintJobState::PRINTING);
        process_lvgl(30);
        lv_obj_t* data_col = h.child("detailed_data_col");
        REQUIRE(data_col != nullptr);
        REQUIRE(lv_obj_get_child_count(data_col) == 3); // layer, time, filament
        lv_obj_t* filament_label = h.child("detailed_filament_text");
        REQUIRE(filament_label != nullptr);
        REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
                4); // active_detailed

        // Wide band, no filament yet: hidden. Contradicting span (colspan=1
        // — old predicate colspan>=3 would not have shown it here).
        h.resize(1, 1, w_wide(), 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // Wide band, filament now used: visible. Isolates on_size_changed's
        // half of the gate (:461) — re-run on_size_changed at the SAME
        // pixels so its own recomputation also agrees with the new used_mm.
        lv_subject_set_int(ps.print_state().get_print_filament_used_subject(), 1500);
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        h.resize(1, 1, w_wide(), 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);
        CHECK_FALSE(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // Normal (not wide) band, filament still used: hidden again.
        // Contradicting span (colspan=5 — old predicate colspan>=3 would
        // have kept it shown).
        h.resize(5, 1, w_normal(), 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // --- Mirror (:1712): back to wide band via one resize call...
        h.resize(1, 1, w_wide(), 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);

        // ...then flip used_mm twice with NO further on_size_changed call.
        // Only DetailedFormatter::update_filament_text()'s mirror
        // (:1728-1731) can be responsible for the gate tracking these
        // transitions.
        lv_subject_set_int(ps.print_state().get_print_filament_used_subject(),
                           0); // e.g. print re-sliced
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        lv_subject_set_int(ps.print_state().get_print_filament_used_subject(),
                           2000); // extrusion resumes
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);
        CHECK_FALSE(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- Up next row fits inside the active card in BOTH layout variants ---------

TEST_CASE_METHOD(LVGLUITestFixture,
                 "up next row stays inside the active print card in both layouts",
                 "[widget_size][print_status][up_next]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();
    {
        // The queue subjects must be live before the component parses, or
        // up_next_row's bind_flag_if_eq on job_queue_count is dropped at
        // parse time and the row keeps whatever default the XML gives it.
        JobQueueState jqs(nullptr, nullptr);
        ScopedJobQueueSubjects subject_guard; // destroyed AFTER the harness
        jqs.init_subjects();
        JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
        REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "job_queue_count")) == 3);

        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});
        // Drain attach-time observers first: they fire with STANDBY and would
        // otherwise override the forced active state on the next pump.
        process_lvgl(30);
        auto force_active = [&]() {
            h.widget().on_print_state_changed_for_test(PrintState::Printing);
            process_lvgl(30);
        };

        // Both checks share one contract: the row is VISIBLE (a hidden=false
        // read is not visibility) and its bottom edge lands inside the card's
        // content box, with nothing pushed past the card's bottom into a
        // scroll region the user cannot reach.
        auto check = [&](int expect_view) {
            REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
                    expect_view); // proves WHICH variant is on stage
            lv_obj_t* card = h.child("print_card_printing");
            lv_obj_t* row = h.child("up_next_row");
            REQUIRE(card != nullptr);
            REQUIRE(row != nullptr);
            CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
            lv_obj_update_layout(card);
            const int content_bottom = lv_obj_get_height(card) -
                                       lv_obj_get_style_pad_top(card, LV_PART_MAIN) -
                                       lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
            INFO("row y=" << lv_obj_get_y(row) << " h=" << lv_obj_get_height(row)
                          << " content_bottom=" << content_bottom);
            CHECK(lv_obj_get_y(row) + lv_obj_get_height(row) <= content_bottom);
            CHECK(lv_obj_get_scroll_bottom(card) == 0);
        };

        // Detailed active (view 4) at the granted cell height 800x480 gives
        // this card. The detailed body flex_grows into the space the Up next
        // row leaves, rather than claiming 100% and pushing the row out.
        h.resize(8, 4, w_wide(), 208);
        force_active();
        check(4);

        // Compact width forces the library active view (view 3) even with
        // layout_style=detailed; 480x320 grants the card ~this height.
        h.resize(8, 4, w_normal() - 1, 142);
        force_active();
        check(3);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// --- Tiny landscape stacks the action grid -----------------------------------
//
// The full-screen print_status_panel (not the home widget above) grows its two
// action rows into the grid's leftover column height at ui_breakpoint < 2 and
// stacks each button icon over label. Pinned at 480x320 and 480x272: every visible action
// button's label must be inside the button's content box, and the grid must
// fill the column bottom.

namespace {

/// lv_display_set_resolution + theme_manager_refresh_layout_constants, undone
/// on scope exit: theme_manager writes into a SHARED XML scope, so a stale
/// tier would decide layout for every later test in this binary (same shape as
/// test_chamber_panel_diagnostics.cpp's ScopedGeometry).
class ScopedTinyLandscape {
  public:
    explicit ScopedTinyLandscape(int32_t w, int32_t h)
        : disp_(lv_display_get_default()), w0_(lv_display_get_horizontal_resolution(disp_)),
          h0_(lv_display_get_vertical_resolution(disp_)) {
        lv_display_set_resolution(disp_, w, h);
        theme_manager_refresh_layout_constants(disp_);
    }

    ~ScopedTinyLandscape() {
        lv_display_set_resolution(disp_, w0_, h0_);
        theme_manager_refresh_layout_constants(disp_);
    }

    ScopedTinyLandscape(const ScopedTinyLandscape&) = delete;
    ScopedTinyLandscape& operator=(const ScopedTinyLandscape&) = delete;

  private:
    lv_display_t* disp_;
    int32_t w0_;
    int32_t h0_;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status action grid stacks and fills the column at tiny and micro",
                 "[widget_size][print_status][small_screen]") {
    PrintStatusWidget::destroy_formatter_for_test();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    ToolState::instance().init_subjects(false);
    heal_global_print_status_panel_subjects();

    const struct {
        int32_t w;
        int32_t h;
    } canvases[] = {{480, 320}, {480, 272}};
    for (const auto& c : canvases) {
        CAPTURE(c.w, c.h);
        ScopedTinyLandscape tiny(c.w, c.h);
        // Pause/Resume's icon+label subjects belong to PrintControlButtons; its
        // subjects must be live before the panel parses, or bind_icon is
        // dropped and the button never stacks.
        helix::ui::PrintControlButtons::instance().init_subjects();
        auto panel = std::make_unique<PrintStatusPanel>(get_printer_state(), nullptr);
        panel->init_subjects();
        lv_obj_t* root = panel->create(test_screen());

        // Timelapse is hidden while printer_has_timelapse is 0; an active
        // print (print_outcome 0) keeps Pause and Cancel visible. Either
        // subject may be absent here (they belong to the capabilities and
        // print-state subsystems, not the panel): an absent subject means the
        // bind was dropped at parse, leaving the button's inline default
        // (visible), so the assertions below still see four buttons.
        if (lv_subject_t* timelapse = lv_xml_get_subject(nullptr, "printer_has_timelapse")) {
            lv_subject_set_int(timelapse, 1);
        }
        if (lv_subject_t* outcome = lv_xml_get_subject(nullptr, "print_outcome")) {
            lv_subject_set_int(outcome, 0);
        }
        process_lvgl(30);

        // Speed/Flow gives its height to the buttons below MEDIUM.
        lv_obj_t* speed_flow = lv_obj_find_by_name(root, "speed_flow_row");
        REQUIRE(speed_flow != nullptr);
        CHECK(lv_obj_has_flag(speed_flow, LV_OBJ_FLAG_HIDDEN));

        lv_obj_t* grid = lv_obj_find_by_name(root, "button_grid");
        REQUIRE(grid != nullptr);
        lv_obj_update_layout(grid);

        // Rows grown: the two rows tile the grid's content height exactly.
        REQUIRE(lv_obj_get_child_count(grid) == 2);
        lv_obj_t* row1 = lv_obj_get_child(grid, 0);
        lv_obj_t* row2 = lv_obj_get_child(grid, 1);
        CHECK(lv_obj_get_y(row1) == 0);
        CHECK(lv_obj_get_style_flex_grow(row1, LV_PART_MAIN) == 1);
        CHECK(lv_obj_get_y(row2) + lv_obj_get_height(row2) == lv_obj_get_height(grid));

        // Every visible action button stacks icon over label, and the label
        // fits inside the button's content box.
        const char* action_buttons[] = {"btn_light", "btn_timelapse", "btn_pause", "btn_tune",
                                        "btn_cancel"};
        int visible = 0;
        for (const char* name : action_buttons) {
            lv_obj_t* btn = lv_obj_find_by_name(root, name);
            REQUIRE(btn != nullptr);
            if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            visible++;
            CAPTURE(name);
            lv_obj_update_layout(btn);
            CHECK(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
            CHECK(lv_obj_get_height(btn) == lv_obj_get_height(lv_obj_get_parent(btn)));
            lv_obj_t* label = UITest::button_label(btn);
            REQUIRE(label != nullptr);
            CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
            const int32_t content_h = lv_obj_get_height(btn) -
                                      lv_obj_get_style_pad_top(btn, LV_PART_MAIN) -
                                      lv_obj_get_style_pad_bottom(btn, LV_PART_MAIN);
            CHECK(lv_obj_get_y(label) + lv_obj_get_height(label) <= content_h);
        }
        CHECK(visible >= 4); // all five except an optional one

        lv_obj_delete(root);
        UpdateQueue::instance().drain();
    }
    PrintStatusWidget::destroy_formatter_for_test();
}
