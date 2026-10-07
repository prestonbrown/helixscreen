// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_reseat_on_config_change.cpp
 * @brief A Home config change that only moves widgets re-seats the built tiles; one
 *        that changes which widgets are shown builds the page again.
 */

#include "ui_panel_home.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/home_panel_test_access.h"
#include "../test_helpers/scoped_widget_factory.h"
#include "app_globals.h"
#include "config.h"
#include "grid_layout.h"
#include "panel_widget.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PanelWidget;
using helix::PanelWidgetManager;
using helix_test::ScopedWidgetFactory;

namespace {

constexpr int TPC = helix::GridLayout::TRACKS_PER_CELL;

struct StubWidget : PanelWidget {
    std::string id_;
    explicit StubWidget(std::string id) : id_(std::move(id)) {}
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return id_.c_str();
    }
    std::string get_component_name() const override {
        return "test_home_reseat_stub";
    }
    void save_widget_config_for_test(const nlohmann::json& config) {
        save_widget_config(config);
    }
};

helix::WidgetFactory stub_factory() {
    return [](const std::string& wid) { return std::unique_ptr<PanelWidget>(new StubWidget(wid)); };
}

nlohmann::json entry(const char* id, bool enabled, int col, int row) {
    return {{"id", id},   {"enabled", enabled}, {"col", col},
            {"row", row}, {"colspan", TPC},     {"rowspan", TPC}};
}

/// The widgets on a second page: the main page also takes every registry widget its
/// saved list lacks, auto-placed, which no in-place path can keep.
nlohmann::json layout(nlohmann::json widgets) {
    return {{"main_page_index", 0},
            {"next_page_id", 2},
            {"pages",
             {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
              {{"id", "second"}, {"widgets", std::move(widgets)}}}}};
}

/// Holds the active printer's Home layout for the test and puts it back after.
struct ScopedHomeLayout {
    helix::Config* cfg = helix::Config::get_instance();
    std::string key = cfg->df() + "panel_widgets/home";
    nlohmann::json saved = cfg->get<nlohmann::json>(key, nlohmann::json());
    ~ScopedHomeLayout() {
        cfg->set<nlohmann::json>(key, saved);
        PanelWidgetManager::instance().clear_panel_config("home");
        PanelWidgetManager::instance().get_widget_config("home").mark_dirty();
    }
    void set(const nlohmann::json& home) {
        cfg->set<nlohmann::json>(key, home);
        PanelWidgetManager::instance().clear_panel_config("home");
        PanelWidgetManager::instance().get_widget_config("home").mark_dirty();
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "Home config change: moved widgets keep their tiles, a new widget set rebuilds",
                 "[home][reseat]") {
    helix::init_widget_registrations();
    lv_xml_register_component_from_data(
        "test_home_reseat_stub",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");
    REQUIRE(theme_manager_get_spacing("space_xs") > 0);
    ScopedWidgetFactory a("shutdown", stub_factory());
    ScopedWidgetFactory b("lock", stub_factory());
    ScopedHomeLayout home;
    home.set(layout({entry("shutdown", true, 0, 0), entry("lock", true, 2 * TPC, 0)}));

    HomePanel& panel = get_global_home_panel();
    lv_obj_t* main_container = lv_obj_create(test_screen());
    lv_obj_t* container = lv_obj_create(test_screen());
    for (lv_obj_t* c : {main_container, container}) {
        lv_obj_set_size(c, 800, 480);
        lv_obj_update_layout(c);
    }
    HomePanelTestAccess::set_page_containers(panel, {main_container, container});
    HomePanelTestAccess::register_config_rebuild_callback(panel);
    HomePanelTestAccess::populate(panel);
    lv_obj_update_layout(container);
    lv_obj_t* shutdown = lv_obj_find_by_name(container, "shutdown");
    lv_obj_t* lock = lv_obj_find_by_name(container, "lock");
    REQUIRE(shutdown != nullptr);
    REQUIRE(lock != nullptr);

    // Same widgets, swapped: the tiles move, nothing is rebuilt.
    home.set(layout({entry("shutdown", true, 2 * TPC, 0), entry("lock", true, 0, 0)}));
    PanelWidgetManager::instance().notify_config_changed("home");
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(container);
    CHECK(lv_obj_find_by_name(container, "shutdown") == shutdown);
    CHECK(lv_obj_find_by_name(container, "lock") == lock);
    CHECK(lv_obj_get_style_grid_cell_column_pos(shutdown, LV_PART_MAIN) == 2 * TPC);
    CHECK(lv_obj_get_style_grid_cell_column_pos(lock, LV_PART_MAIN) == 0);

    // A widget that saved its config applied it in place: the next change rebuilds, even
    // back to the config the page was built from.
    {
        StubWidget saver("lock");
        saver.set_panel_id("home");
        saver.save_widget_config_for_test({{"style", "compact"}});
    }
    home.set(layout({entry("shutdown", true, 0, 0), entry("lock", true, 2 * TPC, 0)}));
    PanelWidgetManager::instance().notify_config_changed("home");
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(container);
    lv_obj_t* after_save = lv_obj_find_by_name(container, "lock");
    REQUIRE(after_save != nullptr);
    CHECK(after_save != lock);
    shutdown = lv_obj_find_by_name(container, "shutdown");
    REQUIRE(shutdown != nullptr);

    // One widget dropped: the page is built again.
    home.set(layout({entry("shutdown", true, 2 * TPC, 0), entry("lock", false, 0, 0)}));
    PanelWidgetManager::instance().notify_config_changed("home");
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(container);
    CHECK(lv_obj_find_by_name(container, "lock") == nullptr);
    lv_obj_t* rebuilt = lv_obj_find_by_name(container, "shutdown");
    REQUIRE(rebuilt != nullptr);
    CHECK(rebuilt != shutdown);

    // Same widget, new per-widget config: built again with it.
    nlohmann::json configured = entry("shutdown", true, 2 * TPC, 0);
    configured["config"] = {{"style", "compact"}};
    home.set(layout({configured, entry("lock", false, 0, 0)}));
    PanelWidgetManager::instance().notify_config_changed("home");
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(container);
    lv_obj_t* reconfigured = lv_obj_find_by_name(container, "shutdown");
    REQUIRE(reconfigured != nullptr);
    CHECK(reconfigured != rebuilt);

    PanelWidgetManager::instance().unregister_rebuild_callback("home");
    HomePanelTestAccess::clear_page_containers(panel);
}
