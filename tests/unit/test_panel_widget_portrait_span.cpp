// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_portrait_span.cpp
 * @brief Regression tests for prestonbrown/helixscreen#1216 — portrait silently
 *        disabled (and PERSISTED enabled=false for) any widget whose default
 *        colspan exceeded the grid width, even when the widget's own
 *        min_colspan would have fit.
 *
 * `tips` is the widest widget in the registry, and a portrait panel has far
 * fewer columns than a landscape one of the same class. Auto-placement asked
 * find_available_bottom() for the authored colspan, got nullopt (its
 * `for (c = ncols - colspan; ...)` loop never runs when colspan > ncols), and
 * the caller wrote enabled=false, col=-1, row=-1 into settings.json and toasted
 * "'Tips' removed — grid full" on a grid with entirely free rows.
 *
 * Track counts are derived from the live panel, so these tests read the grid
 * they are about to exercise rather than naming a dimension: a premise that
 * hardcodes "2 columns" stops describing the scenario the moment the grid
 * changes, and a regression test whose premise has quietly stopped holding is
 * worse than no test.
 */

#include "../test_fixtures.h"
#include "../test_helpers/layout_manager_test_access.h"
#include "../test_helpers/scoped_widget_factory.h"
#include "../test_helpers/scoped_xml_component_stub.h"
#include "config.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "layout_manager.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix_test::ScopedWidgetFactory;

namespace {

/// Spy widget standing in for `tips`. Records the span populate_widgets() told
/// it about via on_size_changed(), which is the only place a widget learns how
/// much of the grid it actually got.
struct SpanSpyWidget : helix::PanelWidget {
    static int s_colspan;
    static int s_rowspan;
    static int s_size_changed_count;
    static int s_attach_count;

    static void reset() {
        s_colspan = -1;
        s_rowspan = -1;
        s_size_changed_count = 0;
        s_attach_count = 0;
    }

    void attach(lv_obj_t*, lv_obj_t*) override {
        ++s_attach_count;
    }
    void detach() override {}
    void on_size_changed(int colspan, int rowspan, int, int) override {
        s_colspan = colspan;
        s_rowspan = rowspan;
        ++s_size_changed_count;
    }
    const char* id() const override {
        return "tips";
    }
    std::string get_component_name() const override {
        return "test_span_spy_widget";
    }
};

int SpanSpyWidget::s_colspan = -1;
int SpanSpyWidget::s_rowspan = -1;
int SpanSpyWidget::s_size_changed_count = 0;
int SpanSpyWidget::s_attach_count = 0;

/// Dependency-free stand-in for the 1x1 companion widget.
struct PlainSpyWidget : helix::PanelWidget {
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "shutdown";
    }
    std::string get_component_name() const override {
        return "test_span_spy_widget";
    }
};

/// Temporarily rewrite a widget definition's span limits. The registry hands out
/// const pointers because callers must not edit definitions; a test that needs a
/// widget too large for ANY grid has no other lever, and the mutation is undone
/// in the destructor.
class ScopedSpanOverride {
  public:
    ScopedSpanOverride(const char* id, int colspan, int rowspan, int min_colspan, int min_rowspan) {
        def_ = const_cast<PanelWidgetDef*>(helix::find_widget_def(id));
        REQUIRE(def_ != nullptr);
        saved_ = *def_;
        def_->colspan = colspan;
        def_->rowspan = rowspan;
        def_->min_colspan = min_colspan;
        def_->min_rowspan = min_rowspan;
    }
    ~ScopedSpanOverride() {
        def_->colspan = saved_.colspan;
        def_->rowspan = saved_.rowspan;
        def_->min_colspan = saved_.min_colspan;
        def_->min_rowspan = saved_.min_rowspan;
    }

  private:
    PanelWidgetDef* def_ = nullptr;
    PanelWidgetDef saved_{};
};

/// The breakpoint PanelWidgetManager builds its grid from
/// (panel_widget_manager.cpp). It comes from the theme's display, not from
/// LayoutManager, so a test premise that names a tier by hand can quietly
/// describe a different grid than the code under test uses.
UiBreakpoint active_breakpoint() {
    lv_subject_t* subject = theme_manager_get_breakpoint_subject();
    return subject ? as_breakpoint(lv_subject_get_int(subject)) : UiBreakpoint::Medium;
}

/// The grid PanelWidgetManager will build inside a container of this outer
/// size. Track counts come from the CONTENT box, and lv_obj's default padding
/// and border sit between that and the outer size, so a premise stated against
/// the outer size describes a different grid than the code under test uses.
/// Measures a throwaway container rather than assuming the inset.
GridDimensions grid_for_container(lv_obj_t* parent, int w, int h) {
    lv_obj_t* probe = lv_obj_create(parent);
    lv_obj_set_size(probe, w, h);
    lv_obj_update_layout(probe);
    const GridDimensions dims = GridLayout::get_dimensions(
        active_breakpoint(), lv_obj_get_content_width(probe), lv_obj_get_content_height(probe));
    lv_obj_delete(probe);
    return dims;
}

/// Register the dependency-free XML component the spy resolves to.
void register_spy_component() {
    lv_xml_register_component_from_data(
        "test_span_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
}

/// Build a two-page layout whose page 1 (a secondary page — no registry-default
/// append) holds exactly one `tips` entry with the supplied placement.
///
/// `with_companion` adds an auto-placed 1x1 `shutdown` widget. Its placement is
/// what makes populate_widgets() call save(), which is the only way the
/// disable-and-persist half of #1216 reaches disk — a lone rejected widget
/// leaves `placed` empty, so nothing is written and the corruption stays in
/// memory.
nlohmann::json make_tips_layout(int col, int row, int colspan, int rowspan,
                                bool with_companion = false) {
    nlohmann::json widgets = nlohmann::json::array();
    widgets.push_back({{"id", "tips"},
                       {"enabled", true},
                       {"col", col},
                       {"row", row},
                       {"colspan", colspan},
                       {"rowspan", rowspan}});
    if (with_companion) {
        widgets.push_back({{"id", "shutdown"},
                           {"enabled", true},
                           {"col", -1},
                           {"row", -1},
                           {"colspan", GridLayout::TRACKS_PER_CELL},
                           {"rowspan", GridLayout::TRACKS_PER_CELL}});
    }
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

} // namespace

/// Fixture: portrait 320x1480 (Waveshare 11.9") — the geometry from the report.
class PortraitSpanFixture : public XMLTestFixture {
  public:
    PortraitSpanFixture() {
        helix::init_widget_registrations();
        register_spy_component();
        SpanSpyWidget::reset();
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        helix::LayoutManager::instance().init(320, 1480);
    }
    ~PortraitSpanFixture() override {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }

    // populate_widgets() injects the firmware_restart tile while Klipper is not
    // READY; a quiet stand-in keeps that build path free of its dependencies.
    helix::test::ScopedXmlComponentStub firmware_restart_stub_{"panel_widget_firmware_restart"};
};

// The core data-loss regression: a widget whose DEFAULT colspan exceeds the grid
// but whose declared minimum fits must be placed, at the reduced span — never
// disabled, and never persisted as enabled=false.
TEST_CASE_METHOD(PortraitSpanFixture,
                 "Portrait auto-place shrinks an over-wide widget to its minimum",
                 "[panel_widget][manager][regression][1216]") {
    // Premise: `tips` is authored wider than this grid, but its declared
    // minimum fits, so it must be seated at a reduced span rather than
    // disabled. Read off the live grid rather than a dimension table — track
    // counts come from the container, so a hardcoded number would either fail
    // or, worse, describe a grid where nothing is over-wide and prove nothing.
    const auto grid = grid_for_container(test_screen(), 320, 1480);
    const auto* tips_def = helix::find_widget_def("tips");
    REQUIRE(tips_def != nullptr);
    REQUIRE(tips_def->colspan > grid.cols);                  // authored for landscape
    REQUIRE(tips_def->effective_min_colspan() <= grid.cols); // …but its minimum fits

    ScopedWidgetFactory factory("tips", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<SpanSpyWidget>();
    });

    const std::string panel_id = "test_portrait_span_autoplace";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    // col/row = -1 -> no saved position -> auto-place path.
    cfg->set<nlohmann::json>(panel_path,
                             make_tips_layout(-1, -1, tips_def->colspan, tips_def->rowspan));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 320, 1480);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // The widget was built and told its REDUCED span, not the authored default.
    CHECK(SpanSpyWidget::s_attach_count == 1);
    CHECK(SpanSpyWidget::s_size_changed_count == 1);
    CHECK(SpanSpyWidget::s_colspan == grid.cols);
    CHECK(SpanSpyWidget::s_rowspan == tips_def->rowspan);

    // And it stayed enabled with a real grid position.
    const auto& entries = mgr.get_widget_config(panel_id).page_entries(1);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const PanelWidgetEntry& e) { return e.id == "tips"; });
    REQUIRE(it != entries.end());
    CHECK(it->enabled);
    CHECK(it->col >= 0);
    CHECK(it->row >= 0);

    mgr.clear_panel_config(panel_id);
}

// The persisted half of the same bug: settings.json must not be rewritten with
// enabled=false when the widget could have fit at its declared minimum. This is
// what made the loss survive a rotation back to landscape.
TEST_CASE_METHOD(PortraitSpanFixture,
                 "Portrait auto-place does not persist enabled=false for a shrinkable widget",
                 "[panel_widget][manager][regression][1216]") {
    ScopedWidgetFactory factory("tips", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<SpanSpyWidget>();
    });
    ScopedWidgetFactory companion("shutdown",
                                  [](const std::string&) -> std::unique_ptr<PanelWidget> {
                                      return std::make_unique<PlainSpyWidget>();
                                  });

    const std::string panel_id = "test_portrait_span_persist";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    const auto* tips_def = helix::find_widget_def("tips");
    REQUIRE(tips_def != nullptr);
    cfg->set<nlohmann::json>(
        panel_path,
        make_tips_layout(-1, -1, tips_def->colspan, tips_def->rowspan, /*with_companion=*/true));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 320, 1480);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    nlohmann::json after = cfg->get<nlohmann::json>(panel_path, nlohmann::json());
    INFO("persisted layout: " << after.dump());
    REQUIRE(after.contains("pages"));
    const auto& tips = find_persisted(after, "tips");

    // The reported symptom: enabled=false / col=-1 / row=-1 written to
    // settings.json, so the widget never came back on rotating to landscape.
    CHECK(tips["enabled"] == true);
    CHECK(tips["col"].get<int>() >= 0);
    CHECK(tips["row"].get<int>() >= 0);

    // A span reduced only to survive this orientation must not be written back —
    // otherwise rotating to landscape once would leave `tips` permanently narrow.
    CHECK(tips["colspan"].get<int>() == tips_def->colspan);

    mgr.clear_panel_config(panel_id);
}

// The saved-position pass clamped col/row into the grid but not colspan/rowspan,
// so a stored landscape span fell straight through to auto-place.
TEST_CASE_METHOD(PortraitSpanFixture, "Portrait saved-position pass clamps an over-wide span",
                 "[panel_widget][manager][regression][1216]") {
    ScopedWidgetFactory factory("tips", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<SpanSpyWidget>();
    });

    const std::string panel_id = "test_portrait_span_saved";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    // Explicit saved position from a landscape session: (0,0) at the authored span.
    const auto grid = grid_for_container(test_screen(), 320, 1480);
    const auto* tips_def = helix::find_widget_def("tips");
    REQUIRE(tips_def != nullptr);
    REQUIRE(tips_def->colspan > grid.cols);
    cfg->set<nlohmann::json>(panel_path,
                             make_tips_layout(0, 0, tips_def->colspan, tips_def->rowspan));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 320, 1480);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // Placed at its saved anchor, span clamped to the grid width.
    CHECK(SpanSpyWidget::s_attach_count == 1);
    CHECK(SpanSpyWidget::s_colspan == grid.cols);
    CHECK(SpanSpyWidget::s_rowspan == tips_def->rowspan);

    const auto& entries = mgr.get_widget_config(panel_id).page_entries(1);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const PanelWidgetEntry& e) { return e.id == "tips"; });
    REQUIRE(it != entries.end());
    CHECK(it->enabled);
    CHECK(it->col == 0);
    CHECK(it->row == 0);

    mgr.clear_panel_config(panel_id);
}

// The other side of the branch: a widget that genuinely cannot fit even at its
// declared minimum IS still disabled. Without this, a placement that never
// disables anything would pass and the "grid full" path would rot untested.
TEST_CASE_METHOD(PortraitSpanFixture, "Portrait still disables a widget that cannot fit at minimum",
                 "[panel_widget][manager][regression][1216]") {
    // A minimum wider than the whole grid — unplaceable at any span. Derived
    // from the live track count so the scenario cannot become satisfiable.
    const auto grid = grid_for_container(test_screen(), 320, 1480);
    const int too_wide = grid.cols + GridLayout::TRACKS_PER_CELL;
    ScopedSpanOverride span("tips", /*colspan=*/too_wide, /*rowspan=*/2,
                            /*min_colspan=*/too_wide, /*min_rowspan=*/2);
    ScopedWidgetFactory factory("tips", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<SpanSpyWidget>();
    });

    const std::string panel_id = "test_portrait_span_toobig";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, make_tips_layout(-1, -1, too_wide, 2));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 320, 1480);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    CHECK(SpanSpyWidget::s_attach_count == 0);

    const auto& entries = mgr.get_widget_config(panel_id).page_entries(1);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [](const PanelWidgetEntry& e) { return e.id == "tips"; });
    REQUIRE(it != entries.end());
    CHECK_FALSE(it->enabled);
    CHECK(it->col == -1);
    CHECK(it->row == -1);

    // …and the reason is reported as "too large", not "grid full" — the grid was
    // completely empty.
    GridLayout empty_grid(active_breakpoint(), grid);
    auto fit = empty_grid.find_available_bottom_min(too_wide, 2);
    CHECK(fit.failure == GridLayout::PlacementFailure::TooLargeForGrid);

    mgr.clear_panel_config(panel_id);
}

// =============================================================================
// Minimum-first allocation (#1216, second round)
// =============================================================================
//
// Asking for the largest span that fits made auto-placement actively worse than
// no fix at all: at 480x800 `tips` took a reduced 3x2 — 6 of 18 cells — and
// fan_stack, ams and notifications were then all disabled with "grid full",
// three stacked toasts where main lost only one widget. The policy is now
// minimum-first: everything is placed at its declared minimum, and only the
// leftover cells are handed back out as growth.

namespace {

/// Records the span populate_widgets() handed each widget. One instance per
/// widget id, so a whole page can be inspected at once.
struct RecordingWidget : helix::PanelWidget {
    static std::map<std::string, std::pair<int, int>> s_spans;
    static std::vector<std::string> s_attached;

    static void reset() {
        s_spans.clear();
        s_attached.clear();
    }

    explicit RecordingWidget(std::string wid) : wid_(std::move(wid)) {}

    void attach(lv_obj_t*, lv_obj_t*) override {
        s_attached.push_back(wid_);
    }
    void detach() override {}
    void on_size_changed(int colspan, int rowspan, int, int) override {
        s_spans[wid_] = {colspan, rowspan};
    }
    const char* id() const override {
        return wid_.c_str();
    }
    std::string get_component_name() const override {
        return "test_span_spy_widget";
    }

  private:
    std::string wid_;
};

std::map<std::string, std::pair<int, int>> RecordingWidget::s_spans;
std::vector<std::string> RecordingWidget::s_attached;

/// Point a whole list of widget ids at RecordingWidget, restoring every factory
/// on destruction.
class ScopedRecordingFactories {
  public:
    explicit ScopedRecordingFactories(const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            const auto* def = helix::find_widget_def(id);
            REQUIRE(def != nullptr);
            saved_.emplace_back(id, def->factory);
            helix::register_widget_factory(id, [id](const std::string&) {
                return std::unique_ptr<PanelWidget>(new RecordingWidget(id));
            });
        }
    }
    ~ScopedRecordingFactories() {
        for (auto& [id, factory] : saved_) {
            helix::register_widget_factory(id, factory);
        }
    }

  private:
    std::vector<std::pair<std::string, WidgetFactory>> saved_;
};

/// Two-page layout whose page 1 holds the supplied ids, all auto-placed.
nlohmann::json make_autoplace_layout(const std::vector<std::string>& ids) {
    nlohmann::json widgets = nlohmann::json::array();
    for (const auto& id : ids) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        widgets.push_back({{"id", id},
                           {"enabled", true},
                           {"col", -1},
                           {"row", -1},
                           {"colspan", def->colspan},
                           {"rowspan", def->rowspan}});
    }
    return nlohmann::json{{"main_page_index", 0},
                          {"next_page_id", 2},
                          {"pages",
                           {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                            {{"id", "spy"}, {"widgets", std::move(widgets)}}}}};
}

} // namespace

/// Fixture parameterised on geometry, so the same page can be run portrait and
/// landscape.
class GeometryFixture : public XMLTestFixture {
  public:
    GeometryFixture(int w, int h) {
        helix::init_widget_registrations();
        register_spy_component();
        RecordingWidget::reset();
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        helix::LayoutManager::instance().init(w, h);
    }
    ~GeometryFixture() override {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }
};

class LandscapeFixture : public GeometryFixture {
  public:
    LandscapeFixture() : GeometryFixture(800, 480) {}
};

class Portrait480x800Fixture : public GeometryFixture {
  public:
    Portrait480x800Fixture() : GeometryFixture(480, 800) {}
};

// Zero-regression guard for the roomy case: on a grid with space to spare,
// minimum-first must end up exactly where the greedy allocator did — every
// widget at the span its registry definition authors. A landscape dashboard
// that visibly shrank would be a worse bug than the one being fixed.
TEST_CASE_METHOD(LandscapeFixture, "Landscape auto-place ends at the authored default span",
                 "[panel_widget][manager][regression][1216][landscape]") {
    const std::vector<std::string> ids = {"printer_image", "print_status",  "tips",
                                          "clock",         "notifications", "shutdown"};
    ScopedRecordingFactories factories(ids);

    // Premise: the authored spans leave room to spare, so nothing has any
    // excuse to shrink. Computed from the live grid — a hardcoded dimension
    // would silently turn this into the scarce case it is the counterpart to.
    const auto grid = grid_for_container(test_screen(), 800, 480);
    int authored_tracks = 0;
    for (const auto& id : ids) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        authored_tracks += def->colspan * def->rowspan;
    }
    INFO("authored " << authored_tracks << " of " << (grid.cols * grid.rows) << " track cells");
    REQUIRE(authored_tracks < grid.cols * grid.rows);

    const std::string panel_id = "test_landscape_authored_span";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, make_autoplace_layout(ids));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    CHECK(RecordingWidget::s_attached.size() == ids.size());
    for (const auto& id : ids) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        auto it = RecordingWidget::s_spans.find(id);
        INFO("widget " << id);
        REQUIRE(it != RecordingWidget::s_spans.end());
        CHECK(it->second.first == def->colspan);
        CHECK(it->second.second == def->rowspan);
    }

    // …and every widget is still enabled with a real position.
    const auto& entries = mgr.get_widget_config(panel_id).page_entries(1);
    for (const auto& id : ids) {
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const PanelWidgetEntry& e) { return e.id == id; });
        REQUIRE(it != entries.end());
        INFO("widget " << id);
        CHECK(it->enabled);
        CHECK(it->col >= 0);
        CHECK(it->row >= 0);
    }

    mgr.clear_panel_config(panel_id);
}

// The regression this round fixes: at 480x800 the authored spans want more of
// the grid than exists, so SOMETHING has to give. Greedy allocation gave the
// earliest widgets their full size and disabled the rest; minimum-first must
// keep every widget, because every widget fits at its declared minimum.
//
// The widget list is sized so the scenario stays scarce: the two REQUIREs below
// assert the oversubscription from the live grid, so a grid that outgrows it
// fails here rather than leaving a green test that exercises nothing.
TEST_CASE_METHOD(Portrait480x800Fixture,
                 "Portrait 480x800 loses no widget that fits at its minimum",
                 "[panel_widget][manager][regression][1216][portrait]") {
    const std::vector<std::string> ids = {
        "printer_image", "print_status", "tips",       "clock",           "notifications",
        "shutdown",      "macros",       "motion",     "gcode_console",   "lock",
        "temperature",   "network",      "temp_stack", "bed_temperature", "tool_switcher"};
    ScopedRecordingFactories factories(ids);

    const auto grid = grid_for_container(test_screen(), 480, 800);
    const int capacity = grid.cols * grid.rows;
    int authored_tracks = 0;
    int minimum_tracks = 0;
    for (const auto& id : ids) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        authored_tracks += def->colspan * def->rowspan;
        minimum_tracks += def->effective_min_colspan() * def->effective_min_rowspan();
    }
    INFO("grid " << grid.cols << "x" << grid.rows << " = " << capacity << " track cells; authored "
                 << authored_tracks << ", minimum " << minimum_tracks);
    REQUIRE(authored_tracks > capacity); // the authored spans cannot all be granted…
    REQUIRE(minimum_tracks < capacity);  // …but every widget fits at its declared minimum

    const std::string panel_id = "test_portrait_no_loss";
    auto* cfg = Config::get_instance();
    const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
    cfg->set<nlohmann::json>(panel_path, make_autoplace_layout(ids));

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 480, 800);
    process_lvgl(10);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // Nothing dropped.
    INFO("attached: " << RecordingWidget::s_attached.size() << " of " << ids.size());
    CHECK(RecordingWidget::s_attached.size() == ids.size());

    const auto& entries = mgr.get_widget_config(panel_id).page_entries(1);
    std::string disabled;
    for (const auto& id : ids) {
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const PanelWidgetEntry& e) { return e.id == id; });
        REQUIRE(it != entries.end());
        if (!it->enabled || it->col < 0 || it->row < 0) {
            disabled += (disabled.empty() ? "" : ", ") + id;
        }
    }
    INFO("disabled: " << disabled);
    CHECK(disabled.empty());

    // Every widget got at least its declared minimum…
    for (const auto& id : ids) {
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        auto it = RecordingWidget::s_spans.find(id);
        INFO("widget " << id);
        REQUIRE(it != RecordingWidget::s_spans.end());
        CHECK(it->second.first >= std::min(def->effective_min_colspan(), grid.cols));
        CHECK(it->second.second >= def->effective_min_rowspan());
        // …and nothing grew past what its definition authors.
        CHECK(it->second.first <= def->colspan);
        CHECK(it->second.second <= def->rowspan);
    }

    // The grid is not over-subscribed.
    int cells = 0;
    for (const auto& [id, span] : RecordingWidget::s_spans) {
        (void)id;
        cells += span.first * span.second;
    }
    INFO("track cells consumed: " << cells << " of " << capacity);
    CHECK(cells <= capacity);

    mgr.clear_panel_config(panel_id);
}

// Determinism: the same config on the same geometry must produce the same
// spans every time, or a rebuild (gate observer, hot reload, rotation back and
// forth) would reshuffle the dashboard under the user.
TEST_CASE_METHOD(Portrait480x800Fixture, "Portrait placement is deterministic across rebuilds",
                 "[panel_widget][manager][regression][1216][determinism]") {
    const std::vector<std::string> ids = {"printer_image", "print_status",  "tips",
                                          "clock",         "notifications", "shutdown"};
    ScopedRecordingFactories factories(ids);

    auto* cfg = Config::get_instance();
    auto& mgr = PanelWidgetManager::instance();

    auto run = [&](const std::string& panel_id) {
        RecordingWidget::reset();
        const std::string panel_path = cfg->df() + "panel_widgets/" + panel_id;
        cfg->set<nlohmann::json>(panel_path, make_autoplace_layout(ids));
        mgr.get_widget_config(panel_id).mark_dirty();
        mgr.clear_panel_config(panel_id);

        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, 480, 800);
        process_lvgl(10);
        auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);
        auto spans = RecordingWidget::s_spans;
        mgr.clear_panel_config(panel_id);
        return spans;
    };

    auto first = run("test_portrait_determinism_a");
    auto second = run("test_portrait_determinism_b");
    auto third = run("test_portrait_determinism_c");
    CHECK(first == second);
    CHECK(second == third);
    CHECK_FALSE(first.empty());
}
