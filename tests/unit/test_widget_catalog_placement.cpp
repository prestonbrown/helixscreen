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
 * The last two tests cover the other half, placing from the catalog: a widget
 * whose entry sits on a DIFFERENT page has that entry moved, not a second one
 * minted with the same ID, and a new multi-instance ID gets an entry whose
 * config is an object.
 */

#include "ui_breakpoint.h"
#include "ui_nav_manager.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "lvgl.h"
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
constexpr const char* kPlacedId = "lock";            // enabled, has a cell
constexpr const char* kLimboId = "motion";           // enabled, no cell
constexpr const char* kDisabledId = "gcode_console"; // not enabled
constexpr const char* kMultiId = "favorite_macro";   // multi_instance base

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
    widgets.push_back({{"id", kPlacedId},
                       {"enabled", true},
                       {"col", 0},
                       {"row", 0},
                       {"colspan", 1},
                       {"rowspan", 1}});
    // The bug's state: configured on, but holding no cell.
    widgets.push_back({{"id", kLimboId},
                       {"enabled", true},
                       {"col", -1},
                       {"row", -1},
                       {"colspan", 1},
                       {"rowspan", 1}});
    widgets.push_back({{"id", kDisabledId},
                       {"enabled", false},
                       {"col", -1},
                       {"row", -1},
                       {"colspan", 1},
                       {"rowspan", 1}});
    // One placed instance and one in limbo — the "(N Placed)" count must say 1.
    widgets.push_back({{"id", std::string(kMultiId) + ":1"},
                       {"enabled", true},
                       {"col", 1},
                       {"row", 0},
                       {"colspan", 1},
                       {"rowspan", 1}});
    widgets.push_back({{"id", std::string(kMultiId) + ":2"},
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
        REQUIRE(entry_for(kPlacedId).has_grid_position());
        REQUIRE(entry_for(kLimboId).enabled);
        REQUIRE_FALSE(entry_for(kLimboId).has_grid_position());
        REQUIRE_FALSE(entry_for(kDisabledId).enabled);

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
            // A drilled-in category is its own pushed page: pop it first, or the
            // single go_back() below lands on it and leaves the catalog open for
            // the next test's show() to refuse.
            if (WidgetCatalogOverlay::active_category_root() != nullptr) {
                NavigationManager::instance().go_back();
                process_lvgl(10);
            }
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

    /// The sub-page scroll holding one row per widget in @p widget_id's
    /// category, drilling in first if the catalog is still at the top level.
    ///
    /// The catalog lists CATEGORIES at its root and only builds widget rows
    /// inside a pushed sub-page, so a top-level search finds no widget at all.
    /// Mirrors category_scroll() in test_widget_catalog_categories.cpp.
    lv_obj_t* category_scroll_for(const char* widget_id) {
        const auto* def = find_widget_def(widget_id);
        REQUIRE(def != nullptr);

        // Drive it by CLICK, the way a user reaches it: show_category() is
        // private, and the click path is what test_widget_catalog_categories.cpp
        // exercises. Row order follows get_widget_categories().
        lv_obj_t* root = WidgetCatalogOverlay::active_root();
        REQUIRE(root != nullptr);
        lv_obj_t* group = lv_obj_find_by_name(root, "category_group");
        REQUIRE(group != nullptr);

        const auto& categories = get_widget_categories();
        size_t index = categories.size();
        for (size_t i = 0; i < categories.size(); ++i) {
            if (categories[i].id == def->category) {
                index = i;
                break;
            }
        }
        INFO("no category row for '" << widget_id << "'");
        REQUIRE(index < categories.size());
        REQUIRE(index < lv_obj_get_child_count(group));

        lv_obj_send_event(lv_obj_get_child(group, static_cast<int32_t>(index)), LV_EVENT_CLICKED,
                          nullptr);
        process_lvgl(10);
        lv_obj_t* page = WidgetCatalogOverlay::active_category_root();
        INFO("no category sub-page for '" << widget_id << "'");
        REQUIRE(page != nullptr);
        lv_obj_t* scroll = lv_obj_find_by_name(page, "catalog_scroll");
        REQUIRE(scroll != nullptr);
        return scroll;
    }

    lv_obj_t* row_for(const char* widget_id) {
        lv_obj_t* row = find_row(category_scroll_for(widget_id), display_name_of(widget_id));
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

    CHECK(config.is_enabled(kPlacedId));
    CHECK(config.is_placed(kPlacedId));

    // The state the catalog got wrong: on, but on no grid.
    CHECK(config.is_enabled(kLimboId));
    CHECK_FALSE(config.is_placed(kLimboId));

    CHECK_FALSE(config.is_enabled(kDisabledId));
    CHECK_FALSE(config.is_placed(kDisabledId));

    // Unknown IDs are neither.
    CHECK_FALSE(config.is_placed("no_such_widget"));
}

// Unchanged behaviour: a widget that really does hold a cell stays dimmed and
// labelled. Without this, "make everything clickable" would pass the next test.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: a widget holding a grid cell reads as Placed",
                 "[widget_catalog][panel_widget]") {
    lv_obj_t* row = row_for(kPlacedId);
    CHECK(has_exact_label(row, lv_tr("Placed")));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
}

// The bug.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: an enabled widget with no grid cell is offered",
                 "[widget_catalog][panel_widget][regression]") {
    lv_obj_t* row = row_for(kLimboId);
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
    CHECK(selected_ == kLimboId);
}

// Unchanged behaviour: a switched-off widget was always offered.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: a disabled widget is offered",
                 "[widget_catalog][panel_widget]") {
    lv_obj_t* row = row_for(kDisabledId);
    CHECK_FALSE(has_exact_label(row, lv_tr("Placed")));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
}

// The multi-instance header counts instances the same blind way, so it claimed
// two macro buttons were on the dashboard when only one was.
TEST_CASE_METHOD(CatalogRowFixture, "Catalog: the placed count skips instances with no cell",
                 "[widget_catalog][panel_widget][regression]") {
    const std::string base = display_name_of(kMultiId);
    const std::string expected = base + " (1 " + lv_tr("Placed") + ")";
    const std::string overcounted = base + " (2 " + lv_tr("Placed") + ")";

    lv_obj_t* scroll = category_scroll_for(kMultiId);
    INFO("expected row label: " << expected);
    CHECK(find_row(scroll, overcounted) == nullptr);
    CHECK(find_row(scroll, expected) != nullptr);
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
                           {{{"id", kLimboId},
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
    GridEditModeTestAccess::place_from_catalog(em, kLimboId);
    em.exit();
    process_lvgl(10);

    int total = 0;
    const PanelWidgetEntry* placed = nullptr;
    for (size_t p = 0; p < config.page_count(); ++p) {
        for (const auto& e : config.page_entries(p)) {
            if (e.id == kLimboId) {
                ++total;
                if (e.has_grid_position()) {
                    placed = &e;
                }
            }
        }
    }
    INFO("entries named '" << kLimboId << "' across all pages: " << total);
    CHECK(total == 1);
    REQUIRE(placed != nullptr);
    CHECK(placed->enabled);
    // The per-widget config travels with the entry — dropping the old entry
    // must not drop what the user configured on it.
    CHECK(placed->config.value("marker", 0) == 42);
}

// A minted multi-instance ID names no entry on any page, so placing it creates
// one. The entry's config reaches the widget's set_config(), which reads it with
// .value(), and .value() throws on a JSON null: load gives every entry an
// object, and so must placement.
TEST_CASE_METHOD(XMLTestFixture,
                 "Catalog placement of a new instance creates its entry with an object config",
                 "[widget_catalog][grid_edit]") {
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    lv_subject_set_int(bp_subj, to_int(UiBreakpoint::Medium));

    const std::string panel_id = "test_catalog_new_instance_place";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages", {{{"id", "main"}, {"widgets", nlohmann::json::array()}}}}});

    PanelWidgetConfig config(panel_id, *cfg);
    config.load();
    REQUIRE(config.page_count() == 1);

    const std::string instance_id = config.mint_instance_id(kMultiId);
    for (const auto& e : config.page_entries(0)) {
        REQUIRE(e.id != instance_id);
    }

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    process_lvgl(10);

    GridEditMode em;
    em.enter(container, &config, /*page_index=*/0);
    GridEditModeTestAccess::place_from_catalog(em, instance_id);
    em.exit();
    process_lvgl(10);

    const PanelWidgetEntry* placed = nullptr;
    for (const auto& e : config.page_entries(0)) {
        if (e.id == instance_id) {
            placed = &e;
        }
    }
    INFO("instance id: " << instance_id);
    REQUIRE(placed != nullptr);
    CHECK(placed->is_placed());
    CHECK(placed->config.is_object());
}

// Crash-loop safe mode shows the default layout and saves nothing, so any
// layout edit would vanish on the next reload. Edit mode refuses to start, and
// says why, so neither the catalog nor a rearrange can be reached.
TEST_CASE_METHOD(XMLTestFixture, "Grid edit: crash-loop safe mode refuses to enter with a toast",
                 "[widget_catalog][grid_edit][crash_loop]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->crash_loop_safe_mode = true;

    PanelWidgetConfig config("test_catalog_safe_mode", *Config::get_instance());
    config.load();
    const auto before = config.page_entries(0);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 800, 480);
    process_lvgl(10);

    std::vector<ToastSeverity> toasts;
    helix::ui::set_test_toast_hook(
        [&](ToastSeverity sev, const std::string&, uint32_t) { toasts.push_back(sev); });
    GridEditMode em;
    const bool entered = em.enter(container, &config, /*page_index=*/0);
    helix::ui::set_test_toast_hook(nullptr);

    CHECK_FALSE(entered);
    CHECK_FALSE(em.is_active());
    CHECK(toasts == std::vector<ToastSeverity>{ToastSeverity::WARNING});
    CHECK(config.page_entries(0) == before);
}
