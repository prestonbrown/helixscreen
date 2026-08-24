// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_catalog_placement.cpp
 * @brief A widget can be enabled and still hold no grid cell — the catalog has
 *        to offer it, not label it "Placed".
 *
 * `enabled == true` with `col/row == -1` is a real, reachable state: the setup
 * wizard enables widgets without a position (ui_wizard.cpp), a hardware gate can
 * open after the grid has filled, and the GridFull eviction path
 * (panel_widget_manager.cpp) deliberately drops the POSITION rather than
 * disabling the entry so the widget can come back on its own.
 *
 * PanelWidgetManager::populate_widgets() understands that state — it builds its
 * auto-place candidates from `entry.enabled` alone. The catalog did not: it
 * asked PanelWidgetConfig::is_enabled(), which never reads col/row, so a widget
 * in limbo rendered dimmed with a "Placed" badge and no click handler. It was
 * absent from the dashboard (no cell), unselectable in edit mode (no on-screen
 * object to select), and refused by the one surface that could have rescued it.
 * PanelWidgetConfig::migrate_stuck_ams_filament_swap() exists because installs
 * really did get stuck this way.
 *
 * The catalog's question is "is it on the grid", not "is it configured on", so
 * it now asks is_placed(). The multi-instance "(N Placed)" count gets the same
 * treatment for the same reason.
 *
 * The last test covers the other half: placing such a widget from the catalog
 * while editing a DIFFERENT page must move its entry, not mint a second one
 * with the same ID.
 */

#include "ui_breakpoint.h"
#include "ui_nav_manager.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Widget IDs used below, all single-instance and un-gated (no
// hardware_gate_subject in panel_widget_registry.cpp) — a gated row is dimmed
// and unclickable for an unrelated reason, which would mask what these tests
// assert. ("lock" took this role after "shutdown" grew the
// platform_host_power_supported gate.)
constexpr const char* PLACED_ID = "lock";            // enabled, has a cell
constexpr const char* LIMBO_ID = "motion";           // enabled, no cell
constexpr const char* DISABLED_ID = "gcode_console"; // not enabled
constexpr const char* MULTI_ID = "favorite_macro";   // multi_instance base

/// The catalog labels a row with the widget's registry display name, so that is
/// how a test finds the row it means.
std::string display_name_of(const char* widget_id) {
    const auto* def = find_widget_def(widget_id);
    REQUIRE(def != nullptr);
    REQUIRE(def->display_name != nullptr);
    return lv_tr(def->display_name);
}

void collect_label_texts(lv_obj_t* obj, std::vector<std::string>& out) {
    if (!obj) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* txt = lv_label_get_text(obj);
        if (txt) {
            out.emplace_back(txt);
        }
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        collect_label_texts(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

/// Every label text anywhere under a catalog row. Position-independent on
/// purpose: the row's children are icon / text column / badge group, and an
/// index-based reader would silently start asserting on the wrong label the
/// first time create_row() gains or loses one.
std::vector<std::string> label_texts(lv_obj_t* row) {
    std::vector<std::string> out;
    collect_label_texts(row, out);
    return out;
}

bool has_exact_label(lv_obj_t* row, const std::string& text) {
    for (const auto& t : label_texts(row)) {
        if (t == text) {
            return true;
        }
    }
    return false;
}

/// The single catalog row carrying @p text as one of its labels.
lv_obj_t* find_row(lv_obj_t* scroll, const std::string& text) {
    lv_obj_t* found = nullptr;
    uint32_t n = lv_obj_get_child_count(scroll);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(scroll, static_cast<int32_t>(i));
        if (has_exact_label(row, text)) {
            INFO("duplicate catalog row for '" << text << "'");
            REQUIRE(found == nullptr);
            found = row;
        }
    }
    return found;
}

/// Page 0 layout. Registry defaults are appended to page 0 by
/// parse_widget_array(), so the widgets NOT listed here still get rows — the
/// tests only ever look at the four IDs above.
nlohmann::json make_layout() {
    nlohmann::json widgets = nlohmann::json::array();
    widgets.push_back({{"id", PLACED_ID},
                       {"enabled", true},
                       {"col", 0},
                       {"row", 0},
                       {"colspan", 1},
                       {"rowspan", 1}});
    // The bug's state: configured on, but holding no cell.
    widgets.push_back({{"id", LIMBO_ID},
                       {"enabled", true},
                       {"col", -1},
                       {"row", -1},
                       {"colspan", 1},
                       {"rowspan", 1}});
    widgets.push_back({{"id", DISABLED_ID},
                       {"enabled", false},
                       {"col", -1},
                       {"row", -1},
                       {"colspan", 1},
                       {"rowspan", 1}});
    // One placed instance and one in limbo — the "(N Placed)" count must say 1.
    widgets.push_back({{"id", std::string(MULTI_ID) + ":1"},
                       {"enabled", true},
                       {"col", 1},
                       {"row", 0},
                       {"colspan", 1},
                       {"rowspan", 1}});
    widgets.push_back({{"id", std::string(MULTI_ID) + ":2"},
                       {"enabled", true},
                       {"col", -1},
                       {"row", -1},
                       {"colspan", 1},
                       {"rowspan", 1}});
    return nlohmann::json{{"main_page_index", 0},
                          {"next_page_id", 2},
                          {"pages", {{{"id", "main"}, {"widgets", std::move(widgets)}}}}};
}

} // namespace

/// Opens the real catalog overlay over the layout above and exposes its rows.
class CatalogRowFixture : public LVGLUITestFixture {
  public:
    CatalogRowFixture() : config_("test_widget_catalog_placement", *Config::get_instance()) {
        auto* cfg = Config::get_instance();
        cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/test_widget_catalog_placement",
                                 make_layout());
        config_.load();

        // Guard the premise: parse_widget_array() drops IDs the registry does
        // not know, which would leave these tests asserting on rows that were
        // never seeded.
        REQUIRE(entry_for(PLACED_ID).has_grid_position());
        REQUIRE(entry_for(LIMBO_ID).enabled);
        REQUIRE_FALSE(entry_for(LIMBO_ID).has_grid_position());
        REQUIRE_FALSE(entry_for(DISABLED_ID).enabled);

        WidgetCatalogOverlay::show(test_screen(), config_,
                                   [this](const std::string& id) { selected_ = id; });
        process_lvgl(10);

        // Also proves show() actually ran: it returns early (warning only) when
        // a previous test leaked an open catalog, and every assertion below
        // would then be reading a stale tree.
        scroll_ = lv_obj_find_by_name(test_screen(), "catalog_scroll");
        REQUIRE(scroll_ != nullptr);
        REQUIRE(lv_obj_get_child_count(scroll_) > 0);
    }

    ~CatalogRowFixture() override {
        // Clears g_catalog_state via the close callback show() registered, so
        // the next test's show() is not a no-op. A test that clicked a row has
        // already been through close_catalog() — going back a second time would
        // pop whatever is under the catalog instead.
        if (selected_.empty()) {
            NavigationManager::instance().go_back();
        }
        process_lvgl(10);
    }

    const PanelWidgetEntry& entry_for(const char* id) {
        for (const auto& e : config_.entries()) {
            if (e.id == id) {
                return e;
            }
        }
        FAIL("widget '" << id << "' missing from the seeded layout");
        return config_.entries()[0]; // unreachable
    }

    lv_obj_t* row_for(const char* widget_id) {
        lv_obj_t* row = find_row(scroll_, display_name_of(widget_id));
        INFO("no catalog row for '" << widget_id << "'");
        REQUIRE(row != nullptr);
        return row;
    }

    PanelWidgetConfig config_;
    lv_obj_t* scroll_ = nullptr;
    std::string selected_;
};

// The accessor the catalog now asks. is_enabled() keeps its meaning
// ("configured on") because seven occupancy-map builders rely on it.
TEST_CASE("PanelWidgetConfig::is_placed separates a grid cell from being enabled",
          "[widget_catalog][panel_widget]") {
    const std::string panel_id = "test_widget_catalog_is_placed";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, make_layout());

    PanelWidgetConfig config(panel_id, *cfg);
    config.load();

    CHECK(config.is_enabled(PLACED_ID));
    CHECK(config.is_placed(PLACED_ID));

    // The state the catalog got wrong: on, but on no grid.
    CHECK(config.is_enabled(LIMBO_ID));
    CHECK_FALSE(config.is_placed(LIMBO_ID));

    CHECK_FALSE(config.is_enabled(DISABLED_ID));
    CHECK_FALSE(config.is_placed(DISABLED_ID));

    // Unknown IDs are neither.
    CHECK_FALSE(config.is_placed("no_such_widget"));
}

// Unchanged behaviour: a widget that really does hold a cell stays dimmed and
// labelled. Without this, "make everything clickable" would pass the next test.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: a widget holding a grid cell reads as Placed",
                 "[widget_catalog][panel_widget]") {
    lv_obj_t* row = row_for(PLACED_ID);
    CHECK(has_exact_label(row, lv_tr("Placed")));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
}

// The bug.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: an enabled widget with no grid cell is offered",
                 "[widget_catalog][panel_widget][regression]") {
    lv_obj_t* row = row_for(LIMBO_ID);
    INFO("labels: " << [&] {
        std::string s;
        for (const auto& t : label_texts(row))
            s += "[" + t + "]";
        return s;
    }());
    CHECK_FALSE(has_exact_label(row, lv_tr("Placed")));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));

    // Clickable is only half of it — the handler has to be wired too, or the
    // row is a dead target that looks alive.
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    process_lvgl(10);
    CHECK(selected_ == LIMBO_ID);
}

// Unchanged behaviour: a switched-off widget was always offered.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: a disabled widget is offered",
                 "[widget_catalog][panel_widget]") {
    lv_obj_t* row = row_for(DISABLED_ID);
    CHECK_FALSE(has_exact_label(row, lv_tr("Placed")));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
}

// The multi-instance header counts instances the same blind way, so it claimed
// two macro buttons were on the dashboard when only one was.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: the placed count skips instances with no cell",
                 "[widget_catalog][panel_widget][regression]") {
    const std::string base = display_name_of(MULTI_ID);
    const std::string expected = base + " (1 " + lv_tr("Placed") + ")";
    const std::string overcounted = base + " (2 " + lv_tr("Placed") + ")";

    INFO("expected row label: " << expected);
    CHECK(find_row(scroll_, overcounted) == nullptr);
    CHECK(find_row(scroll_, expected) != nullptr);
}

// Placing a limbo widget from the catalog while editing another page used to
// push a SECOND entry with the same ID: place_widget_from_catalog() only ever
// searched the page being edited, while the catalog's placed test searches all
// pages. Two enabled entries with one ID render the widget on two pages, and
// delete_entry() only ever removes the first.
TEST_CASE_METHOD(XMLTestFixture, "Catalog placement moves a limbo entry instead of duplicating it",
                 "[widget_catalog][grid_edit][regression]") {
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    lv_subject_set_int(bp_subj, to_int(UiBreakpoint::Medium));

    const std::string panel_id = "test_catalog_cross_page_place";
    auto* cfg = Config::get_instance();
    // The limbo entry lives on page 1; edit mode is scoped to page 0.
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 3},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "second"},
                          {"widgets",
                           {{{"id", LIMBO_ID},
                             {"enabled", true},
                             {"col", -1},
                             {"row", -1},
                             {"colspan", 1},
                             {"rowspan", 1},
                             {"config", {{"marker", 42}}}}}}}}}});

    PanelWidgetConfig config(panel_id, *cfg);
    config.load();
    REQUIRE(config.page_count() == 2);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    process_lvgl(10);

    GridEditMode em;
    em.enter(container, &config, /*page_index=*/0);
    GridEditModeTestAccess::place_from_catalog(em, LIMBO_ID);
    em.exit();
    process_lvgl(10);

    int total = 0;
    const PanelWidgetEntry* placed = nullptr;
    for (size_t p = 0; p < config.page_count(); ++p) {
        for (const auto& e : config.page_entries(p)) {
            if (e.id == LIMBO_ID) {
                ++total;
                if (e.has_grid_position()) {
                    placed = &e;
                }
            }
        }
    }
    INFO("entries named '" << LIMBO_ID << "' across all pages: " << total);
    CHECK(total == 1);
    REQUIRE(placed != nullptr);
    CHECK(placed->enabled);
    // The per-widget config travels with the entry — dropping the old entry
    // must not drop what the user configured on it.
    CHECK(placed->config.value("marker", 0) == 42);
}
