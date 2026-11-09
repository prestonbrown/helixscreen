// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fan_overlay_chamber_air.cpp
 * @brief The fan overlay's chamber air fans: shown while the accessory is fitted,
 *        dropped the moment it is removed, never twice when the printer
 *        republishes one as a Klipper fan. Also: a fan card's tachometer reading
 *        refreshes on its own.
 */

#include "ui_fan_control_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

void seed_nav_panels(lv_obj_t* screen) {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(screen);
    NavigationManager::instance().set_panels(panels.data());
}

void close_all() {
    helix::ui::UpdateQueue::instance().drain();
    NavigationManager::instance().shutdown();
}

struct FreshOverlays {
    FreshOverlays() {
        reset();
    }
    ~FreshOverlays() {
        close_all();
        get_printer_state().set_hardware(PrinterDiscovery{});
        reset();
    }
    static void reset() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
    }
};

void collect_labels(lv_obj_t* obj, std::vector<std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        out.emplace_back(lv_label_get_text(obj));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_labels(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

/// Whether any label under @p root reads @p text. A dial draws its name more
/// than once (one label per layout it can take), so presence is what is asked.
bool has_label(lv_obj_t* root, const std::string& text) {
    std::vector<std::string> labels;
    collect_labels(root, labels);
    for (const auto& l : labels) {
        if (l == text) {
            return true;
        }
    }
    return false;
}

int fan_cards(lv_obj_t* root) {
    return static_cast<int>(lv_obj_get_child_count(lv_obj_find_by_name(root, "fans_container")));
}

PrinterDiscovery u1(std::initializer_list<const char*> extra = {}) {
    json objects = json::array({"purifier", "fm175xx_reader", "tool", "extruder", "fan"});
    for (const char* o : extra) {
        objects.push_back(o);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

void cover(bool fitted, int mode = 0) {
    get_printer_state().update_from_status(
        json{{"purifier",
              {{"power_detected", fitted},
               {"mode", mode},
               {"exhaust_fan", {{"speed", 0.0}, {"speed_threshold", 0.6}}},
               {"inner_fan", {{"speed", 0.0}, {"speed_threshold", 0.6}}}}}});
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: the top cover's fans appear while fitted and go when removed",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    get_printer_state().set_hardware(u1());
    state().fan_state().init_fans({"fan"});
    cover(true);
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(has_label(root, "Exhaust fan"));
    CHECK(has_label(root, "Recirculation fan"));
    lv_obj_t* row = lv_obj_find_by_name(root, "chamber_air_row");
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    cover(false);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(has_label(root, "Exhaust fan"));
    CHECK_FALSE(has_label(root, "Recirculation fan"));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    cover(true);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(has_label(root, "Exhaust fan"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan overlay: a republished cover fan is not shown twice",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    get_printer_state().set_hardware(u1({"fan_generic exhaust_fan"}));
    state().fan_state().init_fans({"fan", "fan_generic exhaust_fan"});
    cover(true);
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    // The republished exhaust fan keeps its own dial; only recirculation is added.
    CHECK_FALSE(has_label(root, "Exhaust fan"));
    CHECK(has_label(root, "Recirculation fan"));
    // Part fan, the republished exhaust fan, the added recirculation fan.
    CHECK(fan_cards(root) == 3);

    // Without the cover the republished fan's dial goes too: its writes would be dropped.
    cover(false);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fan_cards(root) == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan dial: a held dial sends its new speed only once the finger lifts",
                 "[fan_overlay][chamber_air]") {
    FanDial dial(test_screen(), "Exhaust fan", "chamber_air_exhaust", 0);
    dial.set_hold_until_release(true);
    std::vector<int> sent;
    dial.set_on_speed_changed([&sent](const std::string&, int pct) { sent.push_back(pct); });
    lv_obj_t* arc = lv_obj_find_by_name(dial.get_root(), "dial_arc");
    REQUIRE(arc != nullptr);

    lv_obj_add_state(arc, LV_STATE_PRESSED);
    lv_arc_set_value(arc, 40);
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, nullptr);
    // Well past the mid-drag debounce: still held, so nothing is sent.
    process_lvgl(1600);
    CHECK(sent.empty());

    lv_obj_remove_state(arc, LV_STATE_PRESSED);
    process_lvgl(1200);
    CHECK(sent == std::vector<int>{40});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: a tachometer reading refreshes while the speed holds (#2071)",
                 "[fan_overlay][rpm]") {
    FreshOverlays fresh;
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "fan", "heater_fan hotend_fan"}));
    get_printer_state().set_hardware(hw);
    state().fan_state().init_fans({"fan", "heater_fan hotend_fan"});
    get_printer_state().update_from_status(
        json{{"heater_fan hotend_fan", {{"speed", 1.0}, {"rpm", 0.0}}}});
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    // Let the deferred first rebuild land, so the reading below meets live cards.
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(has_label(root, "100%"));

    // Spin-up: Klipper reports a new rpm with no speed change.
    get_printer_state().update_from_status(json{{"heater_fan hotend_fan", {{"rpm", 6000.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    CHECK(has_label(root, "100% \xC2\xB7 6000 RPM"));
}
