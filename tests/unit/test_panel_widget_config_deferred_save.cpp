// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_widget_config_deferred_save.cpp
 * @brief PanelWidgetConfig::save_soon() holds a save until edits settle, and
 *        every path that would lose the held edits writes them first.
 */

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/grid_edit_scene.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "tap_lockout.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// One widget on a page, so a moved column is visible in the stored layout.
void seed_layout(const std::string& panel_id) {
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id,
                             nlohmann::json{{"main_page_index", 0},
                                            {"next_page_id", 1},
                                            {"pages",
                                             {{{"id", "main"},
                                               {"widgets",
                                                {{{"id", "temperature"},
                                                  {"enabled", true},
                                                  {"col", 0},
                                                  {"row", 0},
                                                  {"colspan", 2},
                                                  {"rowspan", 2}}}}}}}});
}

/// The column of 'temperature' as the config stores it at @p path.
int stored_col(const std::string& path) {
    const auto node = Config::get_instance()->get<nlohmann::json>(path, nlohmann::json());
    if (!node.is_object() || !node.contains("pages") || node["pages"].empty()) {
        return -1;
    }
    for (const auto& w : node["pages"][0]["widgets"]) {
        if (w["id"] == "temperature") {
            return w["col"].get<int>();
        }
    }
    return -1;
}

struct DeferredSaveScene {
    std::string panel_id;
    std::string path;
    PanelWidgetConfig config;

    explicit DeferredSaveScene(const std::string& id)
        : panel_id(id), path(Config::get_instance()->df() + "panel_widgets/" + id),
          config(id, *Config::get_instance()) {
        seed_layout(id);
        config.load();
        REQUIRE(config.page_entries(0).size() >= 1);
    }

    /// Move 'temperature' two columns over in memory and ask for a deferred save.
    void edit_and_save_soon() {
        REQUIRE(config.place_entry("temperature", 0, 2, 0, 2, 2) >= 0);
        config.save_soon();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "PanelWidgetConfig: save_soon writes once edits settle",
                 "[panel_widget][deferred_save]") {
    DeferredSaveScene scene("test_deferred_save_settle");
    scene.edit_and_save_soon();
    CHECK(scene.config.save_pending());
    CHECK(stored_col(scene.path) == 0); // not written yet

    // A second edit inside the window restarts it.
    process_lvgl(PanelWidgetConfig::kSaveSettleMs / 2);
    scene.config.save_soon();
    process_lvgl(PanelWidgetConfig::kSaveSettleMs / 2 + 100);
    CHECK(stored_col(scene.path) == 0);

    process_lvgl(PanelWidgetConfig::kSaveSettleMs);
    CHECK_FALSE(scene.config.save_pending());
    CHECK(stored_col(scene.path) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "PanelWidgetConfig: flush_pending_save writes now",
                 "[panel_widget][deferred_save]") {
    DeferredSaveScene scene("test_deferred_save_flush");
    scene.edit_and_save_soon();
    scene.config.flush_pending_save();
    CHECK_FALSE(scene.config.save_pending());
    CHECK(stored_col(scene.path) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PanelWidgetConfig: a printer switch writes a held save to the old printer",
                 "[panel_widget][deferred_save]") {
    auto* cfg = Config::get_instance();
    const std::string original = cfg->get_active_printer_id();
    const nlohmann::json printers_before = cfg->get<nlohmann::json>("/printers", nlohmann::json());
    const std::string panel_id = "test_deferred_save_switch";
    const std::string old_path = cfg->df() + "panel_widgets/" + panel_id;
    seed_layout(panel_id);
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    PanelWidgetConfig& config = mgr.get_widget_config(panel_id);
    REQUIRE(config.place_entry("temperature", 0, 2, 0, 2, 2) >= 0);
    config.save_soon();

    // PrinterSession::switch_printer() moves df() first, then invalidates every
    // cached config through the cache registry.
    cfg->add_printer("deferred-save-other", nlohmann::json::object());
    REQUIRE(cfg->set_active_printer("deferred-save-other"));
    const std::string new_path = cfg->df() + "panel_widgets/" + panel_id;
    REQUIRE(new_path != old_path);
    mgr.clear_all_panel_configs();

    CHECK_FALSE(config.save_pending());
    CHECK(stored_col(old_path) == 2);
    CHECK(stored_col(new_path) == -1); // the new printer's layout is untouched

    ConfigTestAccess::data(*cfg)["printers"] = printers_before;
    ConfigTestAccess::active_printer_id(*cfg) = original;
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a session destroyed while live writes its held save",
                 "[grid_edit][deferred_save]") {
    GridEditScene scene(test_screen(), "test_deferred_save_destroyed_session");
    const std::string path = Config::get_instance()->df() + "panel_widgets/" + scene.panel_id;
    {
        GridEditMode em;
        em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
        em.select_widget(scene.widget);
        GridEditModeTestAccess::commit_resize(
            em,
            {0, 0, GridEditScene::kColspan + GridLayout::TRACKS_PER_CELL, GridEditScene::kRowspan});
        REQUIRE(scene.config->save_pending());
        // The app shutting down destroys the home panel, and its session, here.
    }
    CHECK_FALSE(scene.config->save_pending());
    const auto node = Config::get_instance()->get<nlohmann::json>(path, nlohmann::json());
    REQUIRE(node.contains("pages"));
    CHECK(node["pages"][1]["widgets"][0]["colspan"] ==
          GridEditScene::kColspan + GridLayout::TRACKS_PER_CELL);
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(LVGLTestFixture, "PanelWidgetConfig: an immediate save replaces a held one",
                 "[panel_widget][deferred_save]") {
    DeferredSaveScene scene("test_deferred_save_replace");
    scene.edit_and_save_soon();
    scene.config.save();
    CHECK_FALSE(scene.config.save_pending());
    CHECK(stored_col(scene.path) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "PanelWidgetConfig: a destroyed config never fires its save",
                 "[panel_widget][deferred_save]") {
    std::string path;
    {
        DeferredSaveScene scene("test_deferred_save_destroy");
        path = scene.path;
        scene.edit_and_save_soon();
    }
    // The timer's callback holds a raw `this`: the destructor cancels it, so
    // running past the window writes nothing for the freed object.
    process_lvgl(PanelWidgetConfig::kSaveSettleMs + 200);
    CHECK(stored_col(path) == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: every edit asks for a save once edits settle",
                 "[grid_edit][deferred_save]") {
    GridEditScene scene(test_screen(), "test_deferred_save_every_edit");
    GridEditMode em;
    bool rebuilt = false;
    em.set_rebuild_callback([&rebuilt]() { rebuilt = true; });

    SECTION("entering syncs a drifted position") {
        // The entry says column 4; the widget is laid out in column 0.
        scene.config->page_entries_mut(GridEditScene::kPageIndex)[0].col = 4;
        em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
        REQUIRE(scene.config->page_entries(GridEditScene::kPageIndex)[0].col == 0);
        CHECK(scene.config->save_pending());
    }
    SECTION("removing a widget") {
        em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
        scene.config->flush_pending_save();
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/tile_edit_callout.xml") ==
                LV_RESULT_OK);
        em.select_widget(scene.widget);
        em.open_tile_callout();
        lv_obj_t* remove =
            lv_obj_find_by_name(lv_obj_get_screen(scene.container), "tile_callout_remove");
        REQUIRE(remove != nullptr);
        lv_tick_inc(helix::ui::TapLockout::kDefaultMs);
        lv_obj_send_event(remove, LV_EVENT_CLICKED, nullptr);
        REQUIRE_FALSE(scene.config->is_placed("temperature"));
        CHECK(scene.config->save_pending());
    }
    SECTION("placing a widget from the catalog") {
        em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
        scene.config->flush_pending_save();
        GridEditModeTestAccess::place_from_catalog(em, "clock");
        REQUIRE(scene.config->is_placed("clock"));
        CHECK(scene.config->save_pending());
    }

    em.exit();
    CHECK_FALSE(scene.config->save_pending());
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
