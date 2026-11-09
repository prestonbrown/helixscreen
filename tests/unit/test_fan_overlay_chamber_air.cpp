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
#include "display_settings_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "ui/fan_spin_animation.h"

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

namespace {

/// The rpm label of the dial whose name label reads @p fan_name.
lv_obj_t* dial_rpm_label(lv_obj_t* root, const std::string& fan_name) {
    lv_obj_t* container = lv_obj_find_by_name(root, "fans_container");
    if (!container) {
        return nullptr;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_obj_t* card = lv_obj_get_child(container, static_cast<int32_t>(i));
        lv_obj_t* name = lv_obj_find_by_name(card, "name_label");
        if (name && fan_name == lv_label_get_text(name)) {
            return lv_obj_find_by_name(card, "rpm_label");
        }
    }
    return nullptr;
}

bool shows(lv_obj_t* label, const char* text) {
    return label && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN) &&
           std::string(lv_label_get_text(label)) == text;
}

} // namespace

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "fan overlay: a fan dial shows its tachometer reading, and none without one (#2082)",
    "[fan_overlay][rpm]") {
    FreshOverlays fresh;
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "fan", "fan_generic aux"}));
    get_printer_state().set_hardware(hw);
    state().fan_state().init_fans({"fan", "fan_generic aux"});
    get_printer_state().update_from_status(
        json{{"fan", {{"speed", 0.5}, {"rpm", 3200.0}}},
             {"fan_generic aux", {{"speed", 0.5}, {"rpm", nullptr}}}});
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();

    const auto& fans = state().fan_state().get_fans();
    std::string part_name, aux_name;
    for (const auto& f : fans) {
        (f.object_name == "fan" ? part_name : aux_name) = f.display_name;
    }
    lv_obj_t* part = dial_rpm_label(root, part_name);
    lv_obj_t* aux = dial_rpm_label(root, aux_name);
    REQUIRE(part != nullptr);
    REQUIRE(aux != nullptr);
    CHECK(shows(part, "3200 RPM"));
    CHECK(lv_obj_has_flag(aux, LV_OBJ_FLAG_HIDDEN));

    SECTION("an rpm change alone refreshes the reading") {
        get_printer_state().update_from_status(json{{"fan", {{"rpm", 4100.0}}}});
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        CHECK(shows(part, "4100 RPM"));
        CHECK(lv_obj_has_flag(aux, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a reading that goes away hides the label") {
        get_printer_state().update_from_status(json{{"fan", {{"rpm", nullptr}}}});
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        CHECK(lv_obj_has_flag(part, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a stopped fan reading 0 rpm shows none") {
        get_printer_state().update_from_status(json{{"fan", {{"rpm", 0.0}}}});
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        CHECK(lv_obj_has_flag(part, LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: a dial built for a fan at a steady rpm shows it at once (#2082)",
                 "[fan_overlay][rpm]") {
    FreshOverlays fresh;
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "fan"}));
    get_printer_state().set_hardware(hw);
    state().fan_state().init_fans({"fan"});
    get_printer_state().update_from_status(json{{"fan", {{"speed", 0.5}, {"rpm", 3200.0}}}});
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);

    // No queue drain and no further status: the reading comes from the dial's build.
    lv_obj_t* part = dial_rpm_label(root, state().fan_state().get_fans().front().display_name);
    REQUIRE(part != nullptr);
    CHECK(shows(part, "3200 RPM"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: an rpm-only refresh keeps an auto fan's icon spinning (#2071)",
                 "[fan_overlay][rpm]") {
    FreshOverlays fresh;
    // The fixture turns animations off; the subject is set directly so no Config is written.
    struct AnimationsOn {
        lv_subject_t* subject =
            helix::DisplaySettingsManager::instance().subject_animations_enabled();
        int32_t before = lv_subject_get_int(subject);
        AnimationsOn() {
            lv_subject_set_int(subject, 1);
        }
        ~AnimationsOn() {
            lv_subject_set_int(subject, before);
        }
    } animations_on;

    PrinterDiscovery hw;
    hw.parse_objects(json::array({"extruder", "heater_fan hotend_fan"}));
    get_printer_state().set_hardware(hw);
    state().fan_state().init_fans({"heater_fan hotend_fan"});
    get_printer_state().update_from_status(
        json{{"heater_fan hotend_fan", {{"speed", 0.5}, {"rpm", 3000.0}}}});
    seed_nav_panels(test_screen());

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* icon = lv_obj_find_by_name(root, "fan_icon");
    REQUIRE(icon != nullptr);
    REQUIRE(lv_anim_get(icon, helix::ui::fan_spin_anim_cb) != nullptr);
    process_lvgl(1000);
    const int32_t turned = lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN);
    REQUIRE(turned > 0);

    get_printer_state().update_from_status(json{{"heater_fan hotend_fan", {{"rpm", 3100.0}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(1);
    REQUIRE(has_label(root, "50% \xC2\xB7 3100 RPM"));
    CHECK(lv_anim_get(icon, helix::ui::fan_spin_anim_cb) != nullptr);
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) >= turned);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan dial: an rpm reading leaves the speed display alone",
                 "[fan_overlay][rpm]") {
    FanDial dial(test_screen(), "Part Fan", "fan", 40);
    lv_obj_t* rpm = lv_obj_find_by_name(dial.get_root(), "rpm_label");
    lv_obj_t* speed = lv_obj_find_by_name(dial.get_root(), "speed_label");
    REQUIRE(rpm != nullptr);
    REQUIRE(speed != nullptr);
    CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));
    const std::string speed_text = lv_label_get_text(speed);

    dial.set_rpm(2500);
    CHECK(shows(rpm, "2500 RPM"));
    CHECK(std::string(lv_label_get_text(speed)) == speed_text);
    CHECK(dial.get_speed() == 40);

    dial.set_rpm(std::nullopt);
    CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan dial: a dial set to Off shows no rpm over its zero knob",
                 "[fan_overlay][rpm]") {
    FanDial dial(test_screen(), "Part Fan", "fan", 40);
    lv_obj_t* rpm = lv_obj_find_by_name(dial.get_root(), "rpm_label");
    REQUIRE(rpm != nullptr);
    dial.set_rpm(6857);
    REQUIRE(shows(rpm, "6857 RPM"));

    // A fan spinning down after M107 still reads RPM for a few seconds.
    dial.set_speed(0);
    CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));
    dial.set_rpm(3000);
    CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));

    dial.set_speed(25);
    CHECK(shows(rpm, "3000 RPM"));
}
