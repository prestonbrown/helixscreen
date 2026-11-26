// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolhead_settings_overlay.cpp
 * @brief The Toolhead settings overlay: a card per physical head, its size dropdown on the
 *        size the head reports, and a pick that asks before it changes anything.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_settings_toolhead.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "nozzle_size.h"
#include "printer_state.h"

#include <array>
#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

struct ToolheadFixture : LVGLUITestFixture {
    ToolheadFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
    }
    ~ToolheadFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        get_printer_state().temperature_state().init_extruders({"extruder"});
    }

    void open(const std::vector<std::string>& extruders, const nlohmann::json& status) {
        auto& state = get_printer_state();
        state.temperature_state().init_extruders(extruders);
        state.update_from_status(status);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(helix::settings::get_toolhead_settings_overlay().show(lv_screen_active()));
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    static lv_obj_t* size_dropdown(const char* head) {
        lv_obj_t* card = lv_obj_find_by_name(lv_screen_active(), head);
        return card ? lv_obj_find_by_name(card, "dropdown") : nullptr;
    }
};

} // namespace

TEST_CASE_METHOD(ToolheadFixture, "toolhead settings: each head's dropdown shows its own size",
                 "[toolhead_settings][nozzle_size]") {
    open({"extruder", "extruder1"},
         {{"extruder", {{"nozzle_diameter", 0.4}}}, {"extruder1", {{"nozzle_diameter", 0.6}}}});
    lv_obj_t* head0 = size_dropdown("head_0");
    lv_obj_t* head1 = size_dropdown("head_1");
    REQUIRE((head0 && head1));
    char buf[32];
    lv_dropdown_get_selected_str(head0, buf, sizeof buf);
    CHECK(std::string(buf) == "0.4 mm");
    lv_dropdown_get_selected_str(head1, buf, sizeof buf);
    CHECK(std::string(buf) == "0.6 mm");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead settings: a pick asks about the head it was made on",
                 "[toolhead_settings][nozzle_size]") {
    open({"extruder", "extruder1"},
         {{"extruder", {{"nozzle_diameter", 0.4}}}, {"extruder1", {{"nozzle_diameter", 0.4}}}});

    const auto asked_about = [](const char* head) {
        lv_obj_t* dd = size_dropdown(head);
        REQUIRE(dd != nullptr);
        lv_dropdown_set_selected(dd, 5); // 0.6 mm
        lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(ModalStack::instance().top_dialog() != nullptr);
        const std::string text = helix::test::top_modal_text();
        helix::test::answer_top_modal("btn_secondary");
        helix::ui::UpdateQueue::instance().drain();
        return text;
    };
    const std::string first = asked_about("head_0");
    const std::string second = asked_about("head_1");
    // The question names the head: two heads, two different questions.
    CHECK(first != second);
}
