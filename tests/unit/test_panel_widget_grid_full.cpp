// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_grid_full.cpp
 * @brief Regression tests for the GridFull half of auto-placement (debug bundle
 *        XGVDYEB5) — the sibling of prestonbrown/helixscreen#1216, which fixed
 *        the TooLargeForGrid half.
 *
 * On a Pi at 800x480 the home grid is 6x4 and ten placed widgets consumed all 24
 * cells, so an eleventh (`fan_stack`) had nowhere to go. populate_widgets()
 * disabled it in memory and toasted "'Fan Speeds' removed — grid full", but the
 * save at the end of the function is gated on a PLACED widget's col/row having
 * changed, which in steady state is false. Nothing reached disk, the next launch
 * reloaded the widget as enabled, and the same toast fired again — forever.
 *
 * The fix does NOT persist enabled=false. The layout is stored once per printer
 * (/printers/<id>/panel_widgets/<panel>) with no breakpoint key, so a disable
 * forced by ONE screen's occupancy would take the widget away at every size —
 * the same mistake #1216 already rejected for grid-forced span reductions. The
 * entry stays enabled and only loses its position, which is both true (it is
 * configured, it just has nowhere to go) and self-erasing: a widget with no
 * saved position was never on screen, so no later launch claims it was removed.
 */

#include "ui_notification.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "connection_state.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "layout_manager.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// Access LayoutManager internals for test setup.
// Note: LayoutManagerTestAccess is also defined in test_layout_manager.cpp,
// test_grid_layout.cpp and test_panel_widget_portrait_span.cpp with an identical
// body — Catch2 amalgamated builds compile each test file separately, so no ODR
// conflict. It has to keep this exact name: LayoutManager befriends it.
class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

namespace {

/// Dependency-free stand-in for any registry widget. Records which ids actually
/// got built, which is the only observable difference between "placed" and
/// "dropped" from outside populate_widgets().
struct GridFullSpyWidget : helix::PanelWidget {
    static std::vector<std::string> s_attached;

    static void reset() {
        s_attached.clear();
    }

    explicit GridFullSpyWidget(std::string wid) : wid_(std::move(wid)) {}

    void attach(lv_obj_t*, lv_obj_t*) override {
        s_attached.push_back(wid_);
    }
    void detach() override {}
    const char* id() const override {
        return wid_.c_str();
    }
    std::string get_component_name() const override {
        return "test_grid_full_spy_widget";
    }

  private:
    std::string wid_;
};

std::vector<std::string> GridFullSpyWidget::s_attached;

/// Point a list of widget ids at GridFullSpyWidget, restoring every factory on
/// destruction.
class ScopedSpyFactories {
  public:
    explicit ScopedSpyFactories(const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            const auto* def = helix::find_widget_def(id);
            REQUIRE(def != nullptr);
            saved_.emplace_back(id, def->factory);
            helix::register_widget_factory(id, [id](const std::string&) {
                return std::unique_ptr<PanelWidget>(new GridFullSpyWidget(id));
            });
        }
    }
    ~ScopedSpyFactories() {
        for (auto& [id, factory] : saved_) {
            helix::register_widget_factory(id, factory);
        }
    }

  private:
    std::vector<std::pair<std::string, WidgetFactory>> saved_;
};

void register_spy_component() {
    lv_xml_register_component_from_data(
        "test_grid_full_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    lv_xml_register_component_from_data(
        "panel_widget_firmware_restart",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
}

/// The panel geometry every test in this file lays out on, and the track counts
/// GridLayout quantises it to.
///
/// Derived, never pinned. get_cols()/get_rows() return TRACKS - a cell is
/// GridLayout::TRACKS_PER_CELL of them - and they quantise from the content
/// rectangle, so GRID_CELL or the quantiser moving changes these numbers. The
/// bundle's Pi happened to land on 6x4 cells; hard-coding that would make the
/// "the anchor fills the grid exactly" premise silently false the next time
/// either moves, and every GridFull assertion below would keep passing while
/// testing nothing.
constexpr int kPanelW = 800;
constexpr int kPanelH = 480;

int grid_cols() {
    return GridLayout::get_cols(UiBreakpoint::Medium, kPanelW, kPanelH);
}
int grid_rows() {
    return GridLayout::get_rows(UiBreakpoint::Medium, kPanelW, kPanelH);
}

/// The grid cell a widget actually landed in. populate_widgets() names every
/// tile with its config id (lv_obj_set_name) and places it with
/// lv_obj_set_grid_cell, so this reads back the real placement rather than
/// inferring it. Asserting on "did it get attached" cannot see this bug: an
/// anchored widget and an auto-placed one are both attached, just in different
/// cells at different spans.
struct PlacedCell {
    int col = -1, row = -1, colspan = -1, rowspan = -1;
    bool found = false;
    bool operator==(const PlacedCell& o) const {
        return found && o.found && col == o.col && row == o.row && colspan == o.colspan &&
               rowspan == o.rowspan;
    }
};

PlacedCell placed_cell(lv_obj_t* container, const char* widget_id) {
    PlacedCell out;
    if (!container) {
        return out;
    }
    uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        const char* nm = child ? lv_obj_get_name(child) : nullptr;
        if (!nm || std::string(nm) != widget_id) {
            continue;
        }
        out.col = static_cast<int>(lv_obj_get_style_grid_cell_column_pos(child, LV_PART_MAIN));
        out.row = static_cast<int>(lv_obj_get_style_grid_cell_row_pos(child, LV_PART_MAIN));
        out.colspan = static_cast<int>(lv_obj_get_style_grid_cell_column_span(child, LV_PART_MAIN));
        out.rowspan = static_cast<int>(lv_obj_get_style_grid_cell_row_span(child, LV_PART_MAIN));
        out.found = true;
        return out;
    }
    return out;
}

/// Two-page layout whose page 1 (a secondary page — no registry-default append)
/// holds a `printer_image` anchored at the origin and spanning the whole grid,
/// so it occupies every cell, plus a one-cell `shutdown` at the supplied
/// position (spans are in tracks, so one cell is TRACKS_PER_CELL of them). The
/// anchored pass runs in entry order, so `printer_image` claims the grid and
/// `shutdown` always falls through to auto-place and fails with GridFull.
nlohmann::json make_full_grid_layout(int shutdown_col, int shutdown_row) {
    nlohmann::json widgets = nlohmann::json::array();
    widgets.push_back({{"id", "printer_image"},
                       {"enabled", true},
                       {"col", 0},
                       {"row", 0},
                       {"colspan", grid_cols()},
                       {"rowspan", grid_rows()}});
    widgets.push_back({{"id", "shutdown"},
                       {"enabled", true},
                       {"col", shutdown_col},
                       {"row", shutdown_row},
                       {"colspan", GridLayout::TRACKS_PER_CELL},
                       {"rowspan", GridLayout::TRACKS_PER_CELL}});
    return nlohmann::json{{"main_page_index", 0},
                          {"next_page_id", 2},
                          {"pages",
                           {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                            {{"id", "spy"}, {"widgets", std::move(widgets)}}}}};
}

/// Find one widget entry inside a persisted page-1 layout.
const nlohmann::json& find_persisted(const nlohmann::json& layout, const char* id) {
    const auto& page_widgets = layout["pages"][1]["widgets"];
    for (const auto& w : page_widgets) {
        if (w["id"] == id)
            return w;
    }
    FAIL("widget '" << id << "' missing from persisted layout: " << layout.dump());
    return page_widgets[0]; // unreachable
}

const PanelWidgetEntry& find_entry(const std::vector<PanelWidgetEntry>& entries, const char* id) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const PanelWidgetEntry& e) { return e.id == id; });
    REQUIRE(it != entries.end());
    return *it;
}

/// Records every user-visible warning toast raised while installed.
///
/// `ui_notification.o` is excluded from the test binary (mk/tests.mk
/// TEST_APP_OBJS) and tests/ui_test_utils.cpp stubs ui_notification_warning()
/// to a log-only no-op, so this hook is the ONLY way a test can see that a
/// toast was raised. Watching any other channel (the toast stack,
/// PendingStartupWarnings) records nothing and turns every "…must be silent"
/// assertion into a vacuous pass.
struct WarningSpy {
    WarningSpy() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { messages.push_back(msg); });
    }
    ~WarningSpy() {
        helix::ui::set_test_notification_warning_hook(nullptr);
    }

    std::vector<std::string> take() {
        std::vector<std::string> out;
        out.swap(messages);
        return out;
    }

    /// Prove the hook is actually wired before trusting an empty result. A
    /// silent-toast assertion is only meaningful if a real toast would be seen.
    void self_test() {
        messages.clear();
        ui_notification_warning("grid-full spy canary");
        REQUIRE(messages.size() == 1);
        messages.clear();
    }

    std::vector<std::string> messages;
};

bool mentions(const std::vector<std::string>& warnings, const char* needle) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

} // namespace

/// Fixture: landscape 800x480 — the geometry from bundle XGVDYEB5.
class GridFullFixture : public XMLTestFixture {
  public:
    GridFullFixture() {
        helix::init_widget_registrations();
        register_spy_component();
        GridFullSpyWidget::reset();
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        helix::LayoutManager::instance().init(800, 480);
        warnings.self_test();

        // The whole premise is a grid that a 6x4 anchor fills exactly, and
        // populate_widgets() reads the breakpoint from this subject — not from
        // LayoutManager. Pin it rather than read it: an earlier test in the same
        // binary may have left it elsewhere, and the bundle's Pi ran at Medium
        // ("6cols x 4rows (bp=3)" in its log).
        lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
        REQUIRE(bp_subj != nullptr);
        REQUIRE(bp_subj->type == LV_SUBJECT_TYPE_INT);
        lv_subject_set_int(bp_subj, to_int(UiBreakpoint::Medium));
        // The premise is an anchor that fills the grid exactly, which
        // make_full_grid_layout() builds from these same two calls. Assert only
        // that the grid is big enough to hold a cell at all, so a quantiser
        // change surfaces here rather than as a mystery placement success.
        REQUIRE(grid_cols() >= GridLayout::TRACKS_PER_CELL);
        REQUIRE(grid_rows() >= GridLayout::TRACKS_PER_CELL);
    }
    ~GridFullFixture() override {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }

    WarningSpy warnings;

    /// One "launch": drop every cache so the config is re-read from the JSON
    /// store, then lay the panel out on a fresh container.
    void relaunch(const std::string& panel_id) {
        auto& mgr = PanelWidgetManager::instance();
        mgr.get_widget_config(panel_id).mark_dirty();
        mgr.clear_panel_config(panel_id);
        GridFullSpyWidget::reset();

        lv_obj_t* container = lv_obj_create(test_screen());
        // Zero the chrome so the CONTENT rect is exactly kPanelW x kPanelH: that
        // is the rectangle grid_cols()/grid_rows() quantise, and the manager
        // quantises the content rect. Left at the theme's default padding the
        // two disagree and the anchor no longer spans the grid it is placed on.
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_size(container, kPanelW, kPanelH);
        process_lvgl(10);
        auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
        (void)widgets;
        last_container = container; // for placed_cell() — see #1414
    }

    /// The container the most recent relaunch() laid out, so a test can read
    /// back where widgets actually landed rather than only whether they exist.
    lv_obj_t* last_container = nullptr;
};

// The reported symptom: a widget that has NEVER held a grid cell must not be
// announced as "removed", and must not be disabled — on every launch the grid
// was full, the widget was dropped, nothing was written, and the toast fired
// again next boot.
TEST_CASE_METHOD(GridFullFixture, "A never-placed widget on a full grid is never announced",
                 "[panel_widget][manager][regression][grid_full]") {
    ScopedSpyFactories factories({"printer_image", "shutdown"});

    const std::string panel_id = "test_grid_full_never_placed";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, make_full_grid_layout(/*col=*/-1, /*row=*/-1));

    for (int launch = 1; launch <= 3; ++launch) {
        INFO("launch " << launch);
        relaunch(panel_id);

        // Only the anchor fits; that part is expected and unchanged.
        CHECK(GridFullSpyWidget::s_attached == std::vector<std::string>{"printer_image"});

        auto raised = warnings.take();
        INFO("warnings: " << raised.size());
        CHECK(raised.empty());

        // …and the user's widget is still configured, not quietly switched off.
        const auto& entries =
            PanelWidgetManager::instance().get_widget_config(panel_id).page_entries(1);
        CHECK(find_entry(entries, "shutdown").enabled);

        nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
        REQUIRE(after.contains("pages"));
        CHECK(find_persisted(after, "shutdown")["enabled"] == true);
    }

    PanelWidgetManager::instance().clear_panel_config(panel_id);
}

// A widget that WAS on screen and got squeezed out is worth telling the user
// about — exactly once. The eviction has to reach disk for that to hold across
// launches, and what reaches disk must be "no position", never "disabled".
TEST_CASE_METHOD(GridFullFixture, "Eviction from a full grid warns once, not on every launch",
                 "[panel_widget][manager][regression][grid_full]") {
    ScopedSpyFactories factories({"printer_image", "shutdown"});

    const std::string panel_id = "test_grid_full_evict_once";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    // The last track on each axis is inside the anchor's footprint, so the saved
    // position is already taken by the time the anchored pass reaches it.
    cfg->set<nlohmann::json>(panel_path, make_full_grid_layout(grid_cols() - 1, grid_rows() - 1));

    relaunch(panel_id);

    auto first = warnings.take();
    INFO("first launch warnings: " << first.size());
    CHECK(first.size() == 1);
    CHECK(mentions(first, "Shutdown"));

    // The eviction must be durable, or the next launch repeats the toast. What
    // is durable is the LOST POSITION — the widget stays enabled so it can come
    // back on its own (see the next test).
    nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
    REQUIRE(after.contains("pages"));
    const auto& persisted = find_persisted(after, "shutdown");
    INFO("persisted shutdown: " << persisted.dump());
    CHECK(persisted["enabled"] == true);
    CHECK(persisted["col"].get<int>() == -1);
    CHECK(persisted["row"].get<int>() == -1);

    for (int launch = 2; launch <= 4; ++launch) {
        INFO("launch " << launch);
        relaunch(panel_id);
        auto raised = warnings.take();
        INFO("warnings: " << raised.size());
        CHECK(raised.empty());

        const auto& entries =
            PanelWidgetManager::instance().get_widget_config(panel_id).page_entries(1);
        CHECK(find_entry(entries, "shutdown").enabled);
    }

    PanelWidgetManager::instance().clear_panel_config(panel_id);
}

// The reason a grid-full rejection must not persist enabled=false: the grid is
// full only for as long as the other widgets fill it. Free a row and the evicted
// widget has to come back by itself, with no trip through the widget catalog.
TEST_CASE_METHOD(GridFullFixture, "An evicted widget returns on its own when a cell frees",
                 "[panel_widget][manager][regression][grid_full]") {
    ScopedSpyFactories factories({"printer_image", "shutdown"});

    const std::string panel_id = "test_grid_full_returns";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, make_full_grid_layout(grid_cols() - 1, grid_rows() - 1));

    relaunch(panel_id);
    REQUIRE(GridFullSpyWidget::s_attached == std::vector<std::string>{"printer_image"});
    warnings.take();

    // Shrink the anchor by one whole CELL of rows IN PLACE - freeing a single
    // track would leave no cell for `shutdown` to occupy - leaving whatever
    // populate_widgets()
    // persisted for `shutdown` untouched — this must run against the real stored
    // state, not a freshly authored layout.
    nlohmann::json layout = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
    REQUIRE(layout.contains("pages"));
    bool shrank = false;
    for (auto& w : layout["pages"][1]["widgets"]) {
        if (w["id"] == "printer_image") {
            w["rowspan"] = grid_rows() - GridLayout::TRACKS_PER_CELL;
            shrank = true;
        }
    }
    REQUIRE(shrank);
    cfg->set<nlohmann::json>(panel_path, layout);

    relaunch(panel_id);

    INFO("attached: " << (GridFullSpyWidget::s_attached.empty()
                              ? std::string("<none>")
                              : GridFullSpyWidget::s_attached.back()));
    CHECK(GridFullSpyWidget::s_attached.size() == 2);
    CHECK(std::find(GridFullSpyWidget::s_attached.begin(), GridFullSpyWidget::s_attached.end(),
                    "shutdown") != GridFullSpyWidget::s_attached.end());

    const auto& entries =
        PanelWidgetManager::instance().get_widget_config(panel_id).page_entries(1);
    const auto& shutdown = find_entry(entries, "shutdown");
    CHECK(shutdown.enabled);
    CHECK(shutdown.col >= 0);
    CHECK(shutdown.row >= 0);

    PanelWidgetManager::instance().clear_panel_config(panel_id);
}
