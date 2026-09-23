// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_home_add_page_tile.cpp
 * @brief The add-page tile must be legible and withdrawn at the page cap.
 *
 * The tile is the only way to create a page: it occupies the carousel
 * position past the last configured page, and tapping it adds a page on the
 * spot. Two properties keep it usable. The caption names the action — a bare
 * "+" glyph floating on an otherwise empty page reads as decoration, not a
 * control. And the 8-page cap withdraws the tile, because a tap past the cap
 * can only fail.
 *
 * build_carousel() is driven through the real path: a panel tree with the
 * named carousel_host, and the widget config the PanelWidgetManager reads
 * seeded with N empty pages.
 */

#include "ui_carousel.h"
#include "ui_panel_home.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/home_panel_test_access.h"
#include "panel_widget_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

/// Seed the home panel widget config with `page_count` empty pages for the
/// duration of a scope, so PanelWidgetManager::get_widget_config("home")
/// reads them. The pages are written into the real Config's data rather than
/// a swapped-in Config instance: a cached PanelWidgetConfig holds a `Config&`
/// bound at creation and is never destroyed, so a swapped instance would
/// dangle inside that cache for the rest of the process.
/// clear_all_panel_configs() marks the cache dirty on both sides of the
/// change so the next load() re-reads.
class ScopedHomePages {
  public:
    explicit ScopedHomePages(int page_count) {
        json pages = json::array();
        for (int i = 0; i < page_count; ++i) {
            pages.push_back(
                {{"id", std::string("p") + std::to_string(i)}, {"widgets", json::array()}});
        }
        json home = {{"pages", pages}, {"main_page_index", 0}, {"next_page_id", page_count}};
        Config* config = Config::get_instance();
        saved_data_ = ConfigTestAccess::data(*config);
        saved_printer_id_ = ConfigTestAccess::active_printer_id(*config);
        ConfigTestAccess::data(*config) = {
            {"config_version", 3},
            {"active_printer_id", "default"},
            {"printers", {{"default", {{"panel_widgets", {{"home", home}}}}}}},
        };
        ConfigTestAccess::active_printer_id(*config) = "default";
        PanelWidgetManager::instance().clear_all_panel_configs();
    }

    ~ScopedHomePages() {
        Config* config = Config::get_instance();
        ConfigTestAccess::data(*config) = saved_data_;
        ConfigTestAccess::active_printer_id(*config) = saved_printer_id_;
        PanelWidgetManager::instance().clear_all_panel_configs();
    }

    ScopedHomePages(const ScopedHomePages&) = delete;
    ScopedHomePages& operator=(const ScopedHomePages&) = delete;

  private:
    json saved_data_;
    std::string saved_printer_id_;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "home add-page tile names its action and stops at the page cap",
                 "[home][carousel][add_page]") {
    HomePanel& panel = get_global_home_panel();

    // Minimal stand-in for the home_panel XML tree: setup() only stores the
    // pointers, and build_carousel() looks the host up by name.
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* host = lv_obj_create(root);
    lv_obj_set_name(host, "carousel_host");
    panel.setup(root, test_screen());

    SECTION("below the cap the tile carries a caption") {
        ScopedHomePages pages(2);
        HomePanelTestAccess::build_carousel(panel);

        lv_obj_t* tile = HomePanelTestAccess::add_page_tile(panel);
        REQUIRE(tile != nullptr);

        lv_obj_t* caption = lv_obj_find_by_name(tile, "add_page_caption");
        REQUIRE(caption != nullptr);
        CHECK(std::string(lv_label_get_text(caption)) == "Add page");

        // The tile is an extra carousel position, not a page: the indicator
        // dots count only configured pages.
        CarouselState* state = ui_carousel_get_state(HomePanelTestAccess::carousel(panel));
        REQUIRE(state != nullptr);
        CHECK(state->real_page_count == 2);
    }

    SECTION("at the 8-page cap the tile is withdrawn") {
        ScopedHomePages pages(8);
        HomePanelTestAccess::build_carousel(panel);

        CHECK(HomePanelTestAccess::add_page_tile(panel) == nullptr);
    }

    HomePanelTestAccess::teardown_carousel(panel);
    lv_obj_delete(root);
}
