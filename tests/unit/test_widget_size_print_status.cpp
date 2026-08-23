// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_print_status.cpp
 * @brief print_status picks its layout from physical pixels, not
 * colspan/rowspan — the last and most entangled widget in the migration set.
 * `on_size_changed` (print_status_widget.cpp:447-497) derives a three-way
 * width band (compact/normal/wide) from `width_px` against
 * `widget_size::W_NORMAL`/`W_WIDE`, and every one of its five span-reading
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
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "panel_widget_size.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "tool_state.h"

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
        h.resize(5, 5, W_NORMAL - 1, 300);
        CHECK(width_band() == 0);

        // Normal band. Contradicting span (colspan=1 — old code's colspan==2
        // default-gap bind would not have applied at colspan=1).
        h.resize(1, 1, W_NORMAL, 300);
        CHECK(width_band() == 1);

        // Wide band. Contradicting span (colspan=1 — old code's colspan==3
        // wide-gap bind would not have applied at colspan=1).
        h.resize(1, 1, W_WIDE, 300);
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
        h.resize(5, 5, W_NORMAL - 1, 300);
        CHECK(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 0);

        // At/over the normal floor. Contradicting span (colspan=1 — old
        // predicate colspan>=2 would NOT have activated here).
        h.resize(1, 1, W_NORMAL, 300);
        CHECK(lv_subject_get_int(PrintStatusWidget::layout_effective_subject_for_test()) == 1);

        // Switching back to library must revert effective to 0 regardless of
        // width — re-run on_size_changed at the SAME pixels (set_config
        // alone doesn't recompute this subject; only on_size_changed does).
        h.widget().set_config({{"layout_style", "library"}});
        h.resize(1, 1, W_NORMAL, 300);
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
        h.resize(1, 5, W_WIDE, 300);
        process_lvgl(30);
        CHECK(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) == 2); // idle_detailed
        CHECK_FALSE(lv_obj_has_flag(idle_detailed, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(idle_compact, LV_OBJ_FLAG_HIDDEN));

        // Compact band. Contradicting span (colspan=5 — old predicate
        // colspan<=1 would NOT have gone compact here).
        h.resize(5, 5, W_NORMAL - 1, 300);
        process_lvgl(30);
        CHECK(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
              1); // idle_library_compact
        CHECK_FALSE(lv_obj_has_flag(idle_compact, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_flag(idle_detailed, LV_OBJ_FLAG_HIDDEN));
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

        lv_obj_t* layout = h.child("print_card_layout");
        lv_obj_t* thumb_wrap = h.child("print_card_thumb_wrap");
        REQUIRE(layout != nullptr);
        REQUIRE(thumb_wrap != nullptr);

        // Column: normal band + tall enough. Contradicting span (colspan=3 —
        // old predicate required colspan==2 exactly, so 3 would have stayed
        // row).
        h.resize(3, 1, W_NORMAL, H_TALL);
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
        CHECK(column_mode() == 1);
        CHECK(lv_obj_get_style_width(thumb_wrap, LV_PART_MAIN) == LV_PCT(100));

        // Row: wide band. Contradicting span (colspan=2, rowspan=3 — old
        // predicate colspan==2 && rowspan>=2 would have gone column).
        h.resize(2, 3, W_WIDE, H_TALL);
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
        CHECK(column_mode() == 0);
        CHECK(lv_obj_get_style_width(thumb_wrap, LV_PART_MAIN) == LV_PCT(40));

        // Row: normal band but too short. Contradicting span (colspan=2,
        // rowspan=5 — old predicate colspan==2 && rowspan>=2 would have gone
        // column).
        h.resize(2, 5, W_NORMAL, H_TALL - 1);
        process_lvgl(30);
        CHECK(lv_obj_get_style_flex_flow(layout, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
        CHECK(column_mode() == 0);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

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
    lv_subject_set_int(ps.get_print_filament_used_subject(), 0);

    {
        PanelWidgetHarness<PrintStatusWidget> h(test_screen(),
                                                HarnessConfig{{{"layout_style", "detailed"}}});
        // LVGL fires a freshly-subscribed observer immediately on subscribe;
        // drain once before driving anything else so that initial fire
        // doesn't mask a broken predicate later.
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());

        lv_obj_t* data_col = h.child("detailed_data_col");
        REQUIRE(data_col != nullptr);
        REQUIRE(lv_obj_get_child_count(data_col) == 3); // layer, time, filament
        lv_obj_t* filament_label = h.child("detailed_filament_text");
        REQUIRE(filament_label != nullptr);

        h.widget().on_print_state_changed_for_test(PrintState::Printing);
        process_lvgl(30);
        REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) ==
                4); // active_detailed

        // Wide band, no filament yet: hidden. Contradicting span (colspan=1
        // — old predicate colspan>=3 would not have shown it here).
        h.resize(1, 1, W_WIDE, 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // Wide band, filament now used: visible. Isolates on_size_changed's
        // half of the gate (:461) — re-run on_size_changed at the SAME
        // pixels so its own recomputation also agrees with the new used_mm.
        lv_subject_set_int(ps.get_print_filament_used_subject(), 1500);
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        h.resize(1, 1, W_WIDE, 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);
        CHECK_FALSE(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // Normal (not wide) band, filament still used: hidden again.
        // Contradicting span (colspan=5 — old predicate colspan>=3 would
        // have kept it shown).
        h.resize(5, 1, W_NORMAL, 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        // --- Mirror (:1712): back to wide band via one resize call...
        h.resize(1, 1, W_WIDE, 400);
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);

        // ...then flip used_mm twice with NO further on_size_changed call.
        // Only DetailedFormatter::update_filament_text()'s mirror
        // (:1728-1731) can be responsible for the gate tracking these
        // transitions.
        lv_subject_set_int(ps.get_print_filament_used_subject(), 0); // e.g. print re-sliced
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 0);
        CHECK(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));

        lv_subject_set_int(ps.get_print_filament_used_subject(), 2000); // extrusion resumes
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        CHECK(lv_subject_get_int(PrintStatusWidget::show_filament_active_subject_for_test()) == 1);
        CHECK_FALSE(lv_obj_has_flag(filament_label, LV_OBJ_FLAG_HIDDEN));
    }
    PrintStatusWidget::destroy_formatter_for_test();
}
