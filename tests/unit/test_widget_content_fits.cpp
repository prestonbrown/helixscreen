// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_content_fits.cpp
 * @brief Do home widgets actually render their content at their minimum size?
 *
 * Every other check in this area is arithmetic: the band table proves a span
 * lands in a pixel band, and test_panel_widget_manager_cell_px.cpp proves the
 * manager hands a widget the pixels it promised. Nothing measured whether the
 * widget's content fits in those pixels. A widget can be handed a correct,
 * in-band size and still push a control past its own edge, ellipsize a label
 * into uselessness, or need more room than the box can show.
 *
 * The minimum is the size worth driving: it is what a widget gets when the grid
 * is full, and what the placement engine falls back to under scarcity
 * (GridLayout::find_available_bottom_min).
 *
 * Two halves:
 *
 *  - The detector's own proof. Each of the three checks in
 *    helix::collect_overflow() is driven red against a hand-built tree, and the
 *    intentional-truncation exceptions are driven green. Without these, a
 *    detector that silently stopped firing would read as "no widget clips".
 *  - The sweep. Every PanelWidgetDef is built through its registry factory —
 *    the same path PanelWidgetManager uses — sized to its authored minimum on
 *    each shipping geometry, and measured.
 *
 * A growth pass renders every self-sizing tile again at two cells square, where
 * a scaled-up glyph or face can spill instead, and pins that a tile given more
 * than a cell on a small panel draws a larger glyph than at one cell.
 *
 * The sweep asserts against kKnownClipping, an enumerated baseline of what
 * clips today. Fixing those is a per-widget judgement call (raise the authored
 * minimum, add a smaller layout branch, or accept the truncation) and is not
 * done here. The baseline may shrink; a widget/geometry pair that is not in it
 * and clips fails this test.
 */

#include "ui_breakpoint.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tips_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "display_metrics.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// ---------------------------------------------------------------------------
// Detector proof
// ---------------------------------------------------------------------------

bool has_kind(const OverflowReport& r, OverflowKind k) {
    return std::any_of(r.findings.begin(), r.findings.end(),
                       [k](const OverflowFinding& f) { return f.kind == k; });
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

/// One shipping geometry: the panel resolution and the home grid container's
/// MEASURED content box at that resolution, which is what get_dimensions()
/// divides. Same numbers as tests/unit/test_grid_square_cells.cpp — read off a
/// live instance, not recomputed here.
struct Geometry {
    const char* name;
    int panel_w, panel_h;
    int content_w, content_h;
    int gutter;
};

/// NOT COVERED HERE: the high-DPI UI scale factor (DisplayMetrics).
///
/// The sweep varies geometry per row, but the UI scale is process-wide: the ~46
/// static px tokens (icon_badge_size, chip_height, swatch_size) are registered
/// once at XML init, and lv_xml_register_const is first-write-wins, so a row
/// cannot re-register them at another scale. A scaled row would therefore
/// measure scaled fonts inside unscaled boxes — a combination the app never
/// produces — and report failures that do not exist at runtime.
///
/// Scaled coverage needs its own test that sets the scale BEFORE registering
/// XML. Until then, verify it against a running instance:
/// `-s 1080x2400 --dpi 405`.

// clang-format off
const std::vector<Geometry> kShipping = {
    {"480x272",   480,  272,   430, 264,  2},
    {"272x480",   272,  480,   264, 394,  2},
    {"480x320",   480,  320,   418, 312,  2},
    {"480x400",   480,  400,   414, 388,  4},
    {"800x480",   800,  480,   710, 466,  5},
    {"480x800",   480,  800,   466, 664,  5},
    {"1024x600", 1024,  600,   904, 584,  6},
    {"1280x720", 1280,  720,  1128, 700,  8},

    // Phone-class panel — a 1080x2400 handset, the geometry the DPI work exists
    // for. Measured off a live instance the way the rest of the table was:
    // `-s 1080x2400 -vv`, reading the `[PanelWidgetManager] Grid layout:` and
    // `Track geometry:` lines. Adding it immediately surfaced two widgets that
    // had always clipped at this size and had simply never been measured.
    {"1080x2400", 1080, 2400,  1056, 2236, 10},
};
// clang-format on

/// Subtrees whose overflow is the design.
///
/// Named rather than flag-tested — see OverflowExceptions. Every entry needs a
/// reason; a list that grows without one is how a gate goes quiet.
const OverflowExceptions& exceptions() {
    static const OverflowExceptions ex{{
        // The G-code console is a transcript: it is scrolled to read history,
        // and its content exceeding the box is its normal state, not a size
        // failure.
        "gcode_console_output",
        // active_spool's wide text column scrolls each line (long_mode
        // scroll_circular), so a material name wider than the column is read
        // by scrolling, not clipped.
        "spoolman_text_stack",
    }};
    return ex;
}

/// One widget/geometry pair that does not fit today. `geometry` is "*" when it
/// clips on every shipping geometry.
struct KnownClip {
    const char* widget;
    const char* geometry;
};

bool operator<(const KnownClip& a, const KnownClip& b) {
    int c = std::string(a.widget).compare(b.widget);
    return c != 0 ? c < 0 : std::string(a.geometry) < std::string(b.geometry);
}

/// Baseline: what does not fit at its authored minimum today. Debt, not
/// approval — each entry is a widget whose content needs more room than the
/// span its own definition claims it can live in, and the fix is a per-widget
/// choice between raising that minimum, adding a smaller layout branch, and
/// accepting the truncation.
///
/// Shrinking this list is the point. Adding to it means a widget that used to
/// fit no longer does.
///
/// Re-pinned once, deliberately. Every number here before that point was
/// recorded while ui_icon memoized icon_font_* for the life of the process, so
/// the sweep measured all eight geometries with whichever face the first icon
/// built in the binary happened to pin — on seven of them, an icon three sizes
/// too small. This list was therefore describing the defect, not the widgets.
/// The re-pin removed that residue; it is not the baseline being widened to
/// silence a failure.
///
/// The 1024x600 cluster below has one shared cause, not one per widget. The
/// icon_size ladder in globals.xml steps "lg" (48px) -> "xl" (64px) at the
/// large tier, while the home grid's cell height is 112px at BOTH 800x480 and
/// 1024x600 — so a tile whose icon is sized with #icon_size gains 16px of glyph
/// and no room to put it. Fixing it is a design-ladder decision (cap
/// icon_size_large, or give home tiles their own rung), not a per-widget edit,
/// which is why these are recorded rather than patched.
// clang-format off
const std::vector<KnownClip> kKnownClipping = {
    // Whole-widget: does not fit at its minimum on any shipping panel.
    {"preheat",          "*"},
    {"print_status",     "*"},
    {"printer_image",    "*"},

    // Geometry-specific.
    {"ams",              "272x480"},

    // The #icon_size cluster that used to live here - bed_temperature,
    // chamber_temperature, gcode_console, led, macros, motion, network at
    // 1024x600 and 1280x720, plus led_controls on every panel - is gone. It was
    // never widget debt: icon_size stepped to "xl" at the large tier while
    // GRID_CELL repeated 60 for medium and large, so the glyph grew 48->64 with
    // no more cell to grow into. The rung is "lg" now (ui_xml/globals.xml), and
    // all ten pairs fit.


    // Below the ladder's floor: at the authored minimum tile these
    // geometries cannot hold the stack (the two smallest panels are narrower
    // than icon + value at the compact font; 1024x600's minimum tile floors a
    // pixel over at the large tier's fonts), and the card scrolls rather than
    // clipping silently. Every wider geometry holds the ladder's output with
    // no entry.
    {"nozzle_temps",     "272x480"},  {"nozzle_temps",     "480x400"},
    {"nozzle_temps",     "1024x600"},

    // Measured against the LONGEST title in the database, which is what the
    // sweep now pins — the two entries this used to hold were whatever the
    // random rotation happened to draw. tip_container ends up taller than the
    // tile on seven of the nine geometries, overflowing top and bottom
    // equally (2px at 480x800, 34px at 1080x2400): the title wraps to more
    // lines than the authored minimum height has room for. Only 480x320 and
    // 800x480 hold it. The fix is a line clamp on the title label rather than
    // a bigger minimum, which is why these are recorded rather than resized.
    {"tips",             "272x480"},  {"tips",             "480x272"},
    {"tips",             "480x400"},  {"tips",             "480x800"},
    {"tips",             "1024x600"}, {"tips",             "1280x720"},
    {"tips",             "1080x2400"},
};
// clang-format on

bool is_known(const std::string& widget, const std::string& geometry) {
    return std::any_of(kKnownClipping.begin(), kKnownClipping.end(), [&](const KnownClip& k) {
        return widget == k.widget && (geometry == k.geometry || std::string("*") == k.geometry);
    });
}

/// Forces mock backends for the widget-construction sweep below.
///
/// Without this, get_runtime_config()->test_mode defaults to false in the
/// unit-test binary (it is only ever flipped on by the real app's --test CLI
/// parsing, which this binary never runs). NetworkWidget's attach() calls
/// get_wifi_manager(), a process-lifetime singleton that is created lazily on
/// first use; with test_mode false, WifiBackend::create() built the REAL
/// NetworkManager/wpa_supplicant backend against this machine's actual WiFi
/// state and spawned a genuine, unjoined init thread — a correctness problem
/// (a "unit" test touching live system network state) as well as the
/// [ISOLATION-LEAK] thread leak that first surfaced it. RAII so REQUIRE-driven
/// early exits still restore the previous value for later tests in this
/// process.
struct TestModeGuard {
    RuntimeConfig* rc;
    bool prev;
    explicit TestModeGuard(RuntimeConfig* r) : rc(r), prev(r->test_mode) {
        rc->test_mode = true;
    }
    ~TestModeGuard() {
        rc->test_mode = prev;
    }
};

/// Fixture teardown for the singletons this file seeds. ToolState is outside
/// LVGLUITestFixture's own init/deinit chain, so a file that populates it must
/// clear it or later files in the same binary inherit stale tools (same trap
/// as NozzleTempsFixture in test_widget_size_nozzle_temps.cpp).
struct ContentFitsFixture : public LVGLUITestFixture {
    ~ContentFitsFixture() override {
        // Before ToolState, and before the base fixture's reset_all() gets to
        // it: PrintStatusWidget's formatter is a process-lifetime static whose
        // observers sit on ToolState and PrinterState subjects. Tearing those
        // subjects down first frees the observer nodes, and the formatter's
        // guards then call lv_observer_remove() on freed memory.
        PrintStatusWidget::destroy_formatter_for_test();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        // The sweep drives the display through eight resolutions; put the token
        // table back where the rest of the suite expects it.
        if (lv_display_t* d = lv_display_get_default()) {
            theme_manager_refresh_layout_constants(d);
        }
    }
};

/// Two tools and one extruder, so tool_switcher and nozzle_temps have content
/// to lay out instead of rendering an empty container that trivially fits.
void seed_printer_topology(PrinterState& state) {
    ToolState::instance().deinit_subjects();
    ToolState::instance().init_subjects(false);

    ToolTopology topo;
    topo.tool_count = 2;
    topo.active_tool = 0;
    ToolState::instance().set_ams_topology(topo);

    state.temperature_state().init_extruders({"extruder"});
    helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

/// The filament buffer tile binds to AmsState subjects, so it only has content
/// to lay out while a pressure reading with a set point is published. Scoped to
/// that tile so the AmsState subjects exist only while a widget that binds them
/// is measured.
struct BufferReadingSeed {
    BufferReadingSeed() {
        AmsState::instance().init_subjects(true);
        AmsStateTestAccess::sync_buffer(AmsState::instance(), test::fps_units({0.71f}), 0);
    }
    ~BufferReadingSeed() {
        AmsStateTestAccess::sync_buffer(AmsState::instance(), AmsSystemInfo{}, 0);
        AmsStateTestAccess::clear_buffer_traces(AmsState::instance());
        AmsState::instance().deinit_subjects();
    }
};

/// The Clog Detection tile binds the clog_meter_* subjects, so unseeded it
/// measures placeholder text. Two readings cover the widest cases: FlowGuard
/// (end labels, signed centre text) and the encoder (longest mode text).
struct ClogReadingSeed {
    ClogReadingSeed() {
        auto& ams = AmsState::instance();
        ams.init_subjects(true);
        auto mock = std::make_unique<AmsBackendMock>();
        backend_ = mock.get();
        backend_->set_operation_delay(0);
        ams.set_backend(std::move(mock));
        backend_->start();
    }
    ~ClogReadingSeed() {
        AmsState::instance().set_backend(nullptr);
        AmsState::instance().deinit_subjects();
    }
    // The widget re-syncs from the backend when it attaches, so the reading has
    // to live there rather than on the subjects.
    void flowguard() {
        FlowguardInfo fg;
        fg.enabled = true;
        fg.level = 0.45f;
        backend_->set_encoder_clog_info(EncoderClogInfo{}, 0);
        backend_->set_flowguard_info(fg);
        AmsState::instance().sync_from_backend();
    }
    void encoder() {
        backend_->set_flowguard_info(FlowguardInfo{});
        EncoderClogInfo enc;
        enc.enabled = true;
        enc.detection_mode = 1;
        enc.detection_length = 20.0f;
        enc.headroom = 12.5f;
        enc.desired_headroom = 5.0f;
        enc.min_headroom = 8.0f;
        backend_->set_encoder_clog_info(enc, 1);
        AmsState::instance().sync_from_backend();
    }

  private:
    AmsBackendMock* backend_ = nullptr;
};

struct Rendered {
    bool built = false;
    OverflowReport report;
    int icon_rung = -1; ///< the tile's published rung; -1 for a widget that does not size itself
    int icon_h = 0;     ///< the named icon's drawn height, when one is asked for
};

/// Build @p def through its registry factory, size it to @p c x @p r tracks of
/// grid @p m with the arithmetic PanelWidgetManager applies (truncation
/// included), and measure what spills.
Rendered render_at(lv_obj_t* screen, const PanelWidgetDef& def, const CellMetrics& m, int c, int r,
                   const char* icon_name = nullptr) {
    Rendered out;
    {
        RegistryWidgetHarness h(screen, def, &m);
        if (!h.created()) {
            return out;
        }
        out.built = true;
        h.resize(c, r, static_cast<int>(grid_track_extent(m.cell_w, m.gutter, c)),
                 static_cast<int>(grid_track_extent(m.cell_h, m.gutter, r)));
        // Several widgets rebuild their content from an observer that fires
        // through UpdateQueue rather than inline (observe<int> queues its
        // handler), so measuring before the drain measures the pre-rebuild tree.
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        lv_obj_update_layout(h.root());
        out.report = collect_overflow(h.root(), exceptions());
        if (lv_subject_t* rung =
                lv_xml_get_subject(nullptr, (std::string(def.id) + "_tile_icon").c_str())) {
            out.icon_rung = lv_subject_get_int(rung);
        }
        if (lv_obj_t* icon = icon_name ? lv_obj_find_by_name(h.root(), icon_name) : nullptr) {
            // The layout box, which a glyph drawn above its face's size fills.
            out.icon_h = lv_obj_get_height(icon);
        }
    }
    // Anything the widget queued on its way out must run while the objects it
    // captured are still the most recent ones deleted.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    return out;
}

} // namespace

// ===========================================================================
// The detector must go red before anything it reports can be believed
// ===========================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "overflow detector: a child past the content box is geometry",
                 "[content_fits][detector]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 100, 100);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);

    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_set_size(child, 180, 40);
    lv_obj_set_pos(child, 0, 0);
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    CHECK(has_kind(r, OverflowKind::Geometry));

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "overflow detector: padding is not usable space, so it measures content coords",
                 "[content_fits][detector]") {
    // The mutation target for the geometric check: comparing against
    // lv_obj_get_coords() instead of lv_obj_get_content_coords() makes this
    // case pass, because the child fits the OUTER box exactly and only
    // overruns once the 20px padding is taken out.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 100, 100);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 20, 0);

    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_set_size(child, 100, 100);
    lv_obj_set_pos(child, -20, -20); // Flush with the parent's outer edge.
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    CHECK(has_kind(r, OverflowKind::Geometry));

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "overflow detector: content the layout repositioned still reports scroll",
                 "[content_fits][detector]") {
    // The mutation target for the scroll check: delete the
    // lv_obj_get_scroll_bottom() branch and this goes red.
    //
    // The two checks are not redundant, but they do agree here. For an
    // unscrolled parent with no floating children, LVGL's scroll extent is the
    // same measurement the per-child comparison makes, so both fire. They
    // diverge on a container that has already been scrolled (children get
    // repositioned back inside the box while the content still does not fit)
    // and on LV_OBJ_FLAG_FLOATING children, which the scroll extent excludes
    // and the geometric walk does not.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 100, 60);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* c = lv_obj_create(parent);
        lv_obj_set_size(c, 80, 30);
    }
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    CHECK(has_kind(r, OverflowKind::Scroll));

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture, "overflow detector: a clipped string reports text overflow",
                 "[content_fits][detector]") {
    require_font_tokens_distinct();

    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 60, 40);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);

    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, theme_manager_get_font("font_body"), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(lbl, 50, 20);
    lv_label_set_text(lbl, "a string far too long for fifty pixels");
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    CHECK(has_kind(r, OverflowKind::Text));

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "overflow detector: a label that declares truncation is skipped, visibly",
                 "[content_fits][detector]") {
    require_font_tokens_distinct();

    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 60, 40);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);

    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, theme_manager_get_font("font_body"), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(lbl, 50, 20);
    lv_label_set_text(lbl, "a filename that is supposed to ellipsize.gcode");
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    CHECK_FALSE(has_kind(r, OverflowKind::Text));
    // The skip has to be countable, not silent.
    CHECK_FALSE(r.skipped.empty());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture, "overflow detector: a tree that fits reports nothing",
                 "[content_fits][detector]") {
    // The other half of the gate's contract: it must stay quiet on legitimate
    // layout, or it gets switched off.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 200, 120);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_border_width(parent, 0, 0);

    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_set_size(child, 100, 40);
    lv_obj_set_pos(child, 0, 0);

    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, theme_manager_get_font("font_xs"), 0);
    lv_obj_set_pos(lbl, 0, 60);
    lv_label_set_text(lbl, "ok");
    lv_obj_update_layout(parent);

    OverflowReport r = collect_overflow(parent);
    for (const auto& f : r.findings) {
        UNSCOPED_INFO(std::string(overflow_kind_name(f.kind)) + " " + f.path + ": " + f.detail);
    }
    CHECK(r.clean());

    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "overflow detector: a named scrollable subtree is exempt and reported as skipped",
                 "[content_fits][detector]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 100, 60);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_name(parent, "deliberate_scroller");
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* c = lv_obj_create(parent);
        lv_obj_set_size(c, 80, 30);
    }
    lv_obj_update_layout(parent);

    // Without the name in the list it fires.
    CHECK_FALSE(collect_overflow(parent).clean());

    OverflowExceptions ex{{"deliberate_scroller"}};
    OverflowReport r = collect_overflow(parent, ex);
    CHECK(r.clean());
    CHECK(r.skipped.size() == 1);

    lv_obj_delete(parent);
}

// ===========================================================================
// The sweep
// ===========================================================================

TEST_CASE_METHOD(ContentFitsFixture,
                 "every home widget renders its content at its authored minimum",
                 "[content_fits][sweep]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    TestModeGuard test_mode_guard(get_runtime_config());

    PanelWidgetManager::instance().init_widget_subjects();
    require_font_tokens_distinct();
    seed_printer_topology(state());

    // The tips tile shows a randomly chosen title, so without this the sweep
    // measures a different string every run and reports whichever geometries
    // that one happened to overflow. Pinned to the longest title in the
    // database: a tile that holds the worst case holds the rest.
    const std::string pinned_tip =
        TipsManagerTestAccess::pin_longest_title(*TipsManager::get_instance());
    REQUIRE_FALSE(pinned_tip.empty());
    INFO("tips pinned to the longest title: \"" << pinned_tip << "\"");

    const auto& defs = get_all_widget_defs();
    REQUIRE(defs.size() > 10);

    std::set<KnownClip> observed;
    int checked = 0;
    int unbuildable = 0;
    int too_large_for_grid = 0;
    std::vector<std::string> tight; // Passed, but by two pixels or fewer.

    std::vector<std::string> unplaced; // TooLargeForGrid — never placed at all.

    for (const auto& g : kShipping) {
        ScopedResolution res(disp, g.panel_w, g.panel_h);
        theme_manager_refresh_layout_constants(disp);

        const UiBreakpoint bp = breakpoint_for(std::min(g.panel_w, g.panel_h));
        const GridDimensions dims = GridLayout::get_dimensions(bp, g.content_w, g.content_h);
        const CellMetrics m =
            grid_cell_metrics(g.content_w, g.content_h, dims.cols, dims.rows, g.gutter);

        for (const auto& def : defs) {
            // The smallest span the widget is actually OFFERED here, not the
            // registry floor. A widget that declines a size through fits_at is
            // grown past it by the resize clamp and by the load path alike, so
            // measuring the floor would measure a box the runtime never hands
            // it. grow_span_to_fit is the same helper both of those use, so
            // this cannot drift from what they do.
            int min_c = def.effective_min_colspan();
            int min_r = def.effective_min_rowspan();
            if (def.factory) {
                auto probe = def.factory(def.id);
                if (probe) {
                    // PanelWidgetManager hands every tile the live tracks before
                    // it asks fits_at; the nominal tier cell is larger than the
                    // track some panels build, and would refuse a span the
                    // runtime accepts.
                    if (TileSizing* sizing = probe->tile_sizing()) {
                        sizing->set_cell_metrics(m);
                    }
                    const auto [fit_c, fit_r] = grow_span_to_fit(
                        [&probe](int w, int h) { return probe->fits_at(w, h); }, min_c, min_r,
                        def.effective_max_colspan(), def.effective_max_rowspan(),
                        def.supports_half_col ? 1 : GridLayout::TRACKS_PER_CELL,
                        def.supports_half_row ? 1 : GridLayout::TRACKS_PER_CELL, m);
                    min_c = fit_c;
                    min_r = fit_r;
                }
            }
            if (min_c > dims.cols || min_r > dims.rows) {
                // GridLayout::PlacementFailure::TooLargeForGrid — the widget is
                // never placed here, so there is no size to measure. Recorded
                // by name, not just counted: a widget that vanishes entirely is
                // a worse outcome than one that clips, and the UI scale can
                // push a widget over this line by shrinking the track count.
                ++too_large_for_grid;
                unplaced.push_back(std::string(def.id) + " @ " + g.name + " (needs " +
                                   std::to_string(min_c) + "x" + std::to_string(min_r) +
                                   ", grid is " + std::to_string(dims.cols) + "x" +
                                   std::to_string(dims.rows) + ")");
                continue;
            }

            const int w_px = static_cast<int>(grid_track_extent(m.cell_w, m.gutter, min_c));
            const int h_px = static_cast<int>(grid_track_extent(m.cell_h, m.gutter, min_r));
            std::optional<BufferReadingSeed> buffer_seed;
            if (std::string_view(def.id) == "filament_buffer")
                buffer_seed.emplace();
            std::optional<ClogReadingSeed> clog_seed;
            if (std::string_view(def.id) == "clog_detection")
                clog_seed.emplace();
            for (int pass = 0; pass < (clog_seed ? 2 : 1); ++pass) {
                if (clog_seed) {
                    pass == 0 ? clog_seed->flowguard() : clog_seed->encoder();
                }
                const Rendered rendered = render_at(test_screen(), def, m, min_c, min_r);
                if (!rendered.built) {
                    spdlog::warn("[content_fits] {} @ {}: component would not build", def.id,
                                 g.name);
                    ++unbuildable;
                    continue;
                }
                const OverflowReport& r = rendered.report;
                ++checked;

                if (!r.clean()) {
                    observed.insert({def.id, g.name});
                    for (const auto& f : r.findings) {
                        spdlog::warn("[content_fits] {} @ {} ({}x{}px, min span {}x{}) [{}] {}: {}",
                                     def.id, g.name, w_px, h_px, min_c, min_r,
                                     overflow_kind_name(f.kind), f.path, f.detail);
                    }
                    for (const auto& s : r.skipped) {
                        spdlog::info("[content_fits] {} @ {} skipped {}", def.id, g.name, s);
                    }
                } else if (r.min_text_slack_px <= 2 && r.min_text_slack_px != INT32_MAX) {
                    // One translation away from clipping. Text slack only: a child
                    // sitting flush against its parent's content box is what
                    // LV_PCT(100) is for and says nothing about the next language,
                    // whereas a string with 2px to spare in English will not
                    // survive German.
                    tight.push_back(std::string(def.id) + " @ " + g.name + " (text slack " +
                                    std::to_string(r.min_text_slack_px) + "px)");
                }
            }
        }
    }

    for (const auto& t : tight) {
        spdlog::warn("[content_fits] TIGHT {}", t);
    }
    for (const auto& u : unplaced) {
        spdlog::warn("[content_fits] UNPLACED {}", u);
    }
    spdlog::info("[content_fits] checked {} widget/geometry pairs, {} clipped, {} tight, "
                 "{} unbuildable, {} too large for the grid",
                 checked, observed.size(), tight.size(), unbuildable, too_large_for_grid);

    CHECK(checked > 0);
    CHECK(unbuildable == 0);

    // Baseline entries nothing hit any more. Reported, not failed: the ratchet
    // is one-directional on purpose, so fixing a widget never turns this red.
    // A "*" entry still matches while the widget clips on any one geometry.
    for (const auto& k : kKnownClipping) {
        const bool still = std::any_of(observed.begin(), observed.end(), [&](const KnownClip& o) {
            return std::string(o.widget) == k.widget &&
                   (std::string("*") == k.geometry || std::string(o.geometry) == k.geometry);
        });
        if (!still) {
            spdlog::warn("[content_fits] STALE baseline entry: {} @ {} now fits — drop it",
                         k.widget, k.geometry);
        }
    }

    // Anything clipping that the baseline does not already record.
    std::vector<std::string> regressions;
    for (const auto& o : observed) {
        if (!is_known(o.widget, o.geometry)) {
            regressions.push_back(std::string(o.widget) + " @ " + o.geometry);
        }
    }
    INFO("new clipping (see the [content_fits] warnings above for the measured detail):\n  " + [&] {
        std::string s;
        for (const auto& x : regressions)
            s += x + "\n  ";
        return s;
    }());
    CHECK(regressions.empty());
}

TEST_CASE_METHOD(ContentFitsFixture, "every icon tile renders its content grown to two cells",
                 "[content_fits][growth]") {
    // The sweep above measures the smallest span; a tile that scales its glyph
    // and text up with its box can just as well spill when given room, so each
    // one is rendered again at two cells square, capped by its own maximum and
    // by the grid.
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    TestModeGuard test_mode_guard(get_runtime_config());
    PanelWidgetManager::instance().init_widget_subjects();
    require_font_tokens_distinct();
    seed_printer_topology(state());

    constexpr int kTwoCells = 2 * GridLayout::TRACKS_PER_CELL;
    int checked = 0;
    std::vector<std::string> spilled;
    for (const auto& g : kShipping) {
        ScopedResolution res(disp, g.panel_w, g.panel_h);
        theme_manager_refresh_layout_constants(disp);
        const UiBreakpoint bp = breakpoint_for(std::min(g.panel_w, g.panel_h));
        const GridDimensions dims = GridLayout::get_dimensions(bp, g.content_w, g.content_h);
        const CellMetrics m =
            grid_cell_metrics(g.content_w, g.content_h, dims.cols, dims.rows, g.gutter);

        for (const auto& def : get_all_widget_defs()) {
            if (!def.factory) {
                continue;
            }
            auto probe = def.factory(def.id);
            if (!probe || !probe->tile_sizing()) {
                continue; // only tiles that scale themselves
            }
            const int c = std::min({kTwoCells, def.effective_max_colspan(), dims.cols});
            const int r = std::min({kTwoCells, def.effective_max_rowspan(), dims.rows});
            const Rendered rendered = render_at(test_screen(), def, m, c, r);
            REQUIRE(rendered.built);
            ++checked;
            if (!rendered.report.clean()) {
                for (const auto& f : rendered.report.findings) {
                    spilled.push_back(std::string(def.id) + " @ " + g.name + " " +
                                      std::to_string(c) + "x" + std::to_string(r) + " [" +
                                      overflow_kind_name(f.kind) + "] " + f.path + ": " + f.detail);
                }
            }
        }
    }

    CHECK(checked > 0);
    std::string joined;
    for (const auto& s : spilled) {
        joined += "\n  " + s;
    }
    INFO("tiles that spill when grown to two cells:" << joined);
    CHECK(spilled.empty());
}

TEST_CASE_METHOD(ContentFitsFixture,
                 "a one-cell action tile draws a bigger glyph when given two cells",
                 "[content_fits][growth]") {
    // At the tiny tier a one-cell tile keeps the rung the screen was designed
    // with, and anything bigger has room that rung would leave empty. At the
    // medium tier a cell already draws the tier's largest authored face, so
    // two cells take the xxl rung, drawn from a larger face or scaled up.
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    TestModeGuard test_mode_guard(get_runtime_config());
    PanelWidgetManager::instance().init_widget_subjects();
    require_font_tokens_distinct();

    for (const char* geometry : {"480x320", "800x480"}) {
        const auto g = std::find_if(kShipping.begin(), kShipping.end(), [&](const Geometry& x) {
            return std::string(x.name) == geometry;
        });
        REQUIRE(g != kShipping.end());
        ScopedResolution res(disp, g->panel_w, g->panel_h);
        theme_manager_refresh_layout_constants(disp);
        const UiBreakpoint bp = breakpoint_for(std::min(g->panel_w, g->panel_h));
        const GridDimensions dims = GridLayout::get_dimensions(bp, g->content_w, g->content_h);
        const CellMetrics m =
            grid_cell_metrics(g->content_w, g->content_h, dims.cols, dims.rows, g->gutter);

        constexpr int kCell = GridLayout::TRACKS_PER_CELL;
        // The tiles drawn by home_action_tile, and the icon each names.
        const std::pair<const char*, const char*> kTiles[] = {
            {"motion", "motion_icon"},
            {"shutdown", "shutdown_icon"},
            {"macros", "macros_icon"},
            {"led_controls", "led_controls_icon"},
            {"firmware_restart", "firmware_restart_icon"},
            {"lock", "lock_icon_closed"}};
        for (const auto& [id, icon] : kTiles) {
            const PanelWidgetDef* def = find_widget_def(id);
            REQUIRE(def != nullptr);
            const Rendered one = render_at(test_screen(), *def, m, kCell, kCell, icon);
            const Rendered two = render_at(test_screen(), *def, m, 2 * kCell, 2 * kCell, icon);
            INFO(geometry << " " << id << ": rung " << one.icon_rung << " / " << one.icon_h
                          << "px at one cell, " << two.icon_rung << " / " << two.icon_h
                          << "px at two");
            REQUIRE(one.icon_rung >= 0);
            REQUIRE(one.icon_h > 0);
            CHECK(two.icon_rung > one.icon_rung);
            CHECK(two.icon_h > one.icon_h);
        }
    }
}
