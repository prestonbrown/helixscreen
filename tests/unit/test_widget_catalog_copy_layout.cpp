// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_catalog_copy_layout.cpp
 * @brief The catalog's "Copy layout from printer" action: shown only on a
 *        remote screen, fetches the layout the printer's own screen published,
 *        and replaces this screen's home layout after a confirmation.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "home_layout_share.h"
#include "home_layout_sync.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "printer_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json widget(const char* id, int col, int row, int colspan, int rowspan) {
    return json{{"id", id},   {"enabled", true},    {"col", col},
                {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
}

json home_on_12x8() {
    return json{
        {"pages", json::array({json{{"id", "main"},
                                    {"widgets", json::array({widget("temperature", 0, 0, 2, 2),
                                                             widget("fan", 2, 0, 2, 2)})}}})},
        {"main_page_index", 0},
        {"next_page_id", 1},
        {"grid", "12x8"}};
}

json published_layout() {
    return json{
        {"schema", home_layout::kSchema},
        {"grid", "12x8"},
        {"main_page_index", 0},
        {"pages", json::array({json{{"id", "main"},
                                    {"widgets", json::array({widget("printer_image", 0, 0, 4, 4),
                                                             widget("temperature", 4, 0, 2, 2),
                                                             widget("fan", 6, 0, 2, 2)})}}})}};
}

class CopyLayoutFixture : public LVGLUITestFixture {
  public:
    CopyLayoutFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        api_ = std::make_unique<MoonrakerAPIMock>(client_, get_printer_state());
        config_ = Config::get_instance();
        REQUIRE(config_ != nullptr);
        saved_active_id_ = ConfigTestAccess::active_printer_id(*config_);
        ConfigTestAccess::active_printer_id(*config_) = "printer_a";
        get_printer_state().clear_moonraker_locality();
        drain();
        HomeLayoutSync::instance().init(*api_);

        saved_home_ = config_->get<json>(home_path(), json());
        config_->set<json>(home_path(), home_on_12x8());
        reload_home();
        REQUIRE(home().stamped_grid() == std::pair<int, int>(12, 8));
        edited_ = seats();

        PanelWidgetManager::instance().register_rebuild_callback("home", [this] { ++rebuilds_; });
        WidgetCatalogOverlay::show(test_screen(), home(), [](const std::string&) {});
        process_lvgl(10);
        row_ = lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "action_button_2");
        REQUIRE(row_ != nullptr);
    }

    ~CopyLayoutFixture() override {
        if (WidgetCatalogOverlay::active_root() != nullptr) {
            WidgetCatalogOverlay::close();
        }
        process_lvgl(10);
        PanelWidgetManager::unregister_rebuild_callback("home");
        HomeLayoutSync::instance().shutdown();
        get_printer_state().clear_moonraker_locality();
        drain();
        config_->set<json>(home_path(), saved_home_);
        reload_home();
        ConfigTestAccess::active_printer_id(*config_) = saved_active_id_;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string home_path() const {
        return config_->df() + "panel_widgets/home";
    }

    void reload_home() {
        auto& mgr = PanelWidgetManager::instance();
        mgr.get_widget_config("home").mark_dirty();
        mgr.clear_panel_config("home");
    }

    PanelWidgetConfig& home() {
        return PanelWidgetManager::instance().get_widget_config("home");
    }

    std::vector<std::string> seats() {
        std::vector<std::string> out;
        for (const auto& e : home().page_entries(0)) {
            if (e.enabled && e.is_placed()) {
                out.push_back(e.id + "@" + std::to_string(e.col) + "," + std::to_string(e.row));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void set_locality(bool remote) {
        get_printer_state().set_moonraker_is_remote(remote);
        drain();
        process_lvgl(10);
    }

    void tap_copy() {
        lv_obj_send_event(row_, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        drain();
    }

    void answer_modal(const char* button_name) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* button = lv_obj_find_by_name(dialog, button_name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        drain();
    }

    MoonrakerClientMock client_;
    std::unique_ptr<MoonrakerAPIMock> api_;
    Config* config_ = nullptr;
    std::string saved_active_id_;
    json saved_home_;
    std::vector<std::string> edited_;
    lv_obj_t* row_ = nullptr;
    int rebuilds_ = 0;
};

} // namespace

TEST_CASE_METHOD(CopyLayoutFixture, "copy layout action hidden when Local or Unknown",
                 "[widget_catalog][copy_layout]") {
    // Unknown until the connection's verdict lands.
    CHECK(lv_obj_has_flag(row_, LV_OBJ_FLAG_HIDDEN));
    set_locality(false);
    CHECK(lv_obj_has_flag(row_, LV_OBJ_FLAG_HIDDEN));
    set_locality(true);
    CHECK_FALSE(lv_obj_has_flag(row_, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(CopyLayoutFixture, "copy layout confirm imports and notifies",
                 "[widget_catalog][copy_layout]") {
    set_locality(true);
    client_.mock_db_set(home_layout::kNamespace, home_layout::kKey, published_layout());

    tap_copy();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK(seats() == edited_);
    CHECK(rebuilds_ == 0);

    answer_modal("btn_primary");

    const auto now = seats();
    CHECK(now == std::vector<std::string>{"fan@6,0", "printer_image@0,0", "temperature@4,0"});
    CHECK(rebuilds_ == 1);
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
}

TEST_CASE_METHOD(CopyLayoutFixture, "copy layout cancel leaves the layout alone",
                 "[widget_catalog][copy_layout]") {
    set_locality(true);
    client_.mock_db_set(home_layout::kNamespace, home_layout::kKey, published_layout());

    tap_copy();
    answer_modal("btn_secondary");

    CHECK(seats() == edited_);
    CHECK(rebuilds_ == 0);
}

TEST_CASE_METHOD(CopyLayoutFixture,
                 "copy layout with nothing published: no modal, layout unchanged",
                 "[widget_catalog][copy_layout]") {
    set_locality(true);
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook([&toasts](ToastSeverity, const std::string& message, uint32_t) {
        toasts.push_back(message);
    });

    tap_copy();
    helix::ui::set_test_toast_hook(nullptr);

    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK(seats() == edited_);
    CHECK(rebuilds_ == 0);
    CHECK(toasts == std::vector<std::string>{"This printer's screen has not shared a layout"});
}

TEST_CASE_METHOD(CopyLayoutFixture, "copy layout ignores a second tap while the fetch is in flight",
                 "[widget_catalog][copy_layout]") {
    set_locality(true);
    client_.mock_db_set(home_layout::kNamespace, home_layout::kKey, published_layout());

    // Both taps land before the queued reply is drained.
    lv_obj_send_event(row_, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(row_, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    drain();

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    answer_modal("btn_secondary");
    CHECK(ModalStack::instance().top_dialog() == nullptr);

    // The flag cleared with the reply: a later tap fetches again.
    tap_copy();
    CHECK(ModalStack::instance().top_dialog() != nullptr);
}

TEST_CASE_METHOD(CopyLayoutFixture, "copy layout confirm after the catalog closed does nothing",
                 "[widget_catalog][copy_layout]") {
    set_locality(true);
    client_.mock_db_set(home_layout::kNamespace, home_layout::kKey, published_layout());

    tap_copy();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    WidgetCatalogOverlay::close();
    process_lvgl(50);
    drain();
    REQUIRE(WidgetCatalogOverlay::active_root() == nullptr);

    answer_modal("btn_primary");

    CHECK(seats() == edited_);
    CHECK(rebuilds_ == 0);
}
