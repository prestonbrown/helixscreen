// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_fan_overlay_chamber_air.cpp
 * @brief The fan overlay's chamber air fans: shown while the accessory is fitted,
 *        dropped the moment it is removed, never twice when the printer
 *        republishes one as a Klipper fan. Also: a fan card's tachometer reading
 *        refreshes on its own.
 */

#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_settings_fans.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "chamber_air_settings.h"
#include "config.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
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

void cover(bool fitted, int mode = 0, double exhaust_speed = 0.0) {
    get_printer_state().update_from_status(
        json{{"purifier",
              {{"power_detected", fitted},
               {"mode", mode},
               {"exhaust_fan", {{"speed", exhaust_speed}, {"speed_threshold", 0.6}}},
               {"inner_fan", {{"speed", 0.0}, {"speed_threshold", 0.6}}}}}});
}

/// The arc of the dial whose card reads @p name.
lv_obj_t* dial_arc(lv_obj_t* root, const std::string& name) {
    lv_obj_t* fans = lv_obj_find_by_name(root, "fans_container");
    for (uint32_t i = 0; i < lv_obj_get_child_count(fans); ++i) {
        lv_obj_t* card = lv_obj_get_child(fans, static_cast<int32_t>(i));
        if (has_label(card, name)) {
            return lv_obj_find_by_name(card, "dial_arc");
        }
    }
    return nullptr;
}

/// A finger turning @p arc to @p pct and lifting.
void turn(lv_obj_t* arc, int pct) {
    lv_arc_set_value(arc, pct);
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_send_event(arc, LV_EVENT_RELEASED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}

/// Lets the deferred fan rebuilds an open queues run, so the dials found next are the live ones.
void settle() {
    for (int i = 0; i < 2; ++i) {
        helix::ui::UpdateQueue::instance().drain();
        UITest::advance_ms(50);
    }
    helix::ui::UpdateQueue::instance().drain();
}

/// Fits or removes the cover and lets the overlay's deferred rebuild run.
void refit(bool fitted) {
    cover(fitted);
    helix::ui::UpdateQueue::instance().drain();
    UITest::advance_ms(50);
    helix::ui::UpdateQueue::instance().drain();
}

/// A U1 with @p fans configured, the cover fitted in @p mode, and the fan overlay open.
lv_obj_t* open_on_u1(lv_obj_t* screen, std::initializer_list<const char*> extra_objects,
                     const std::vector<std::string>& fans, int mode = 0,
                     double exhaust_speed = 0.0) {
    get_printer_state().set_hardware(u1(extra_objects));
    get_printer_state().fan_state().init_fans(fans);
    cover(true, mode, exhaust_speed);
    seed_nav_panels(screen);
    lv_obj_t* root = helix::open_fan_control_overlay(screen);
    REQUIRE(root != nullptr);
    settle();
    return root;
}

uint32_t observers(lv_subject_t* s) {
    return lv_ll_get_len(&s->subs_ll);
}

/// The overlay's commands go to a mock printer for the test's lifetime.
struct RecordedApi {
    MockPrinter printer;
    RecordedApi() {
        set_moonraker_api(&printer.api);
    }
    ~RecordedApi() {
        set_moonraker_api(nullptr);
    }
    const std::vector<std::string>& sent() const {
        return printer.client.gcode_script_history();
    }
    bool sent_containing(const std::string& text) const {
        for (const auto& g : sent()) {
            if (g.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: the top cover's fans appear while fitted and go when removed",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    lv_obj_t* root = open_on_u1(test_screen(), {}, {"fan"});
    CHECK(has_label(root, "Exhaust fan"));
    CHECK(has_label(root, "Recirculation fan"));
    lv_obj_t* row = lv_obj_find_by_name(root, "chamber_air_row");
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    refit(false);
    CHECK_FALSE(has_label(root, "Exhaust fan"));
    CHECK_FALSE(has_label(root, "Recirculation fan"));
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    refit(true);
    CHECK(has_label(root, "Exhaust fan"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan overlay: a republished cover fan is not shown twice",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    lv_obj_t* root =
        open_on_u1(test_screen(), {"fan_generic exhaust_fan"}, {"fan", "fan_generic exhaust_fan"});
    // The republished exhaust fan keeps its own dial; only recirculation is added.
    CHECK_FALSE(has_label(root, "Exhaust fan"));
    CHECK(has_label(root, "Recirculation fan"));
    // Part fan, the republished exhaust fan, the added recirculation fan.
    CHECK(fan_cards(root) == 3);

    // Without the cover the republished fan's dial goes too: its writes would be dropped.
    refit(false);
    CHECK(fan_cards(root) == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan dial: a held dial sends its new speed only once the finger lifts",
                 "[fan_overlay][chamber_air]") {
    auto run = [this](FanDial& dial) {
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
    };
    FanDial held(test_screen(), "Exhaust fan", "chamber_air_exhaust", 0);
    held.set_hold_until_release(true);
    SECTION("as built") {
        run(held);
    }
    SECTION("move-constructed") {
        FanDial moved(std::move(held));
        run(moved);
    }
    SECTION("move-assigned") {
        FanDial target(test_screen(), "Other", "other", 0);
        target = std::move(held);
        run(target);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: an air mode tap sends the targets the settings row stored",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    RecordedApi api;
    Config* cfg = Config::get_instance();
    cfg->set<json>(cfg->df() + "chamber_air", json());
    lv_subject_set_int(lv_xml_get_subject(nullptr, "chamber_air_exhaust_target_pct"), 85);
    helix::settings::get_fan_settings_overlay().register_callbacks();
    lv_event_cb_t commit = lv_xml_get_event_cb(nullptr, "on_chamber_air_targets_commit");
    REQUIRE(commit != nullptr);
    commit(nullptr);
    CHECK(helix::chamber_air::load_targets().exhaust_pct == 85);

    lv_obj_t* root = open_on_u1(test_screen(), {}, {"fan"});
    lv_obj_send_event(lv_obj_find_by_name(root, "btn_chamber_air_exhaust"), LV_EVENT_CLICKED,
                      nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(api.sent_containing("SET_PURIFIER_MODE MODE=1 FAN_SPEED=0.85"));
    cfg->set<json>(cfg->df() + "chamber_air", json());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: a republished cover fan waits for the finger to lift",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    set_moonraker_api(api());
    lv_obj_t* root =
        open_on_u1(test_screen(), {"fan_generic exhaust_fan"}, {"fan", "fan_generic exhaust_fan"});
    lv_subject_t* speed = state().fan_state().get_fan_speed_subject("fan_generic exhaust_fan");
    REQUIRE(speed != nullptr);

    lv_obj_t* fans = lv_obj_find_by_name(root, "fans_container");
    REQUIRE(lv_obj_get_child_count(fans) >= 2);
    lv_obj_t* arc = lv_obj_find_by_name(lv_obj_get_child(fans, 1), "dial_arc");
    REQUIRE(arc != nullptr);
    lv_obj_add_state(arc, LV_STATE_PRESSED);
    lv_arc_set_value(arc, 40);
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(1600);
    // The send's optimistic update moves the printer's speed: still held, it has not.
    CHECK(lv_subject_get_int(speed) == 0);
    lv_obj_remove_state(arc, LV_STATE_PRESSED);
    process_lvgl(1200);
    CHECK(lv_subject_get_int(speed) == 40);
    set_moonraker_api(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: a refused or questioned cover fan change puts that dial back",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    RecordedApi api;

    SECTION("recirculating refuses the exhaust fan") {
        lv_obj_t* root = open_on_u1(test_screen(), {}, {"fan"}, 3);
        lv_obj_t* exhaust = dial_arc(root, "Exhaust fan");
        REQUIRE(exhaust != nullptr);
        turn(exhaust, 40);
        CHECK(lv_arc_get_value(exhaust) == 0);
        CHECK(api.sent().empty());
    }
    SECTION("one lower-speed question at a time") {
        lv_obj_t* root = open_on_u1(test_screen(), {}, {"fan"}, 1, 0.8);
        lv_obj_t* exhaust = dial_arc(root, "Exhaust fan");
        REQUIRE(exhaust != nullptr);
        turn(exhaust, 30);
        lv_obj_t* question = ModalStack::instance().top_dialog();
        REQUIRE(question != nullptr);
        CHECK(api.sent().empty());
        turn(exhaust, 20);
        CHECK(ModalStack::instance().top_dialog() == question);
        CHECK(lv_arc_get_value(exhaust) == 80);
        Modal::hide(question);
        process_lvgl(50);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan overlay: rebuilding the fans and losing the tree leave no observer behind",
                 "[fan_overlay][chamber_air]") {
    FreshOverlays fresh;
    get_printer_state().set_hardware(u1({"heater_fan hotend_fan"}));
    state().fan_state().init_fans({"fan", "heater_fan hotend_fan"});
    cover(true);
    seed_nav_panels(test_screen());
    lv_subject_t* hotend = state().fan_state().get_fan_speed_subject("heater_fan hotend_fan");
    REQUIRE(hotend != nullptr);
    lv_subject_t* available = get_printer_state().chamber_air().available_subject();
    const uint32_t hotend_before = observers(hotend);
    const uint32_t available_before = observers(available);

    lv_obj_t* root = helix::open_fan_control_overlay(test_screen());
    REQUIRE(root != nullptr);
    settle();
    const uint32_t hotend_open = observers(hotend);
    CHECK(hotend_open > hotend_before);
    for (int i = 0; i < 2; ++i) {
        cover(false);
        helix::ui::UpdateQueue::instance().drain();
        cover(true);
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK(observers(hotend) == hotend_open);

    lv_obj_delete(root);
    CHECK(observers(available) == available_before);
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
    std::string part_name, aux_name;
    for (const auto& f : state().fan_state().get_fans()) {
        (f.object_name == "fan" ? part_name : aux_name) = f.display_name;
    }
    // No queue drain and no further status yet: a steady reading comes from the dial's build.
    CHECK(shows(dial_rpm_label(root, part_name), "3200 RPM"));
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
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

    SECTION("a reading that goes away, or a stopped fan's 0 rpm, hides the label") {
        for (const json& gone : {json(nullptr), json(0.0)}) {
            get_printer_state().update_from_status(json{{"fan", {{"rpm", 3200.0}}}});
            get_printer_state().update_from_status(json{{"fan", {{"rpm", gone}}}});
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
            CHECK(lv_obj_has_flag(part, LV_OBJ_FLAG_HIDDEN));
        }
    }
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
    hw.parse_objects(json::array({"extruder", "heater_fan hotend_fan", "controller_fan board"}));
    get_printer_state().set_hardware(hw);
    state().fan_state().init_fans({"heater_fan hotend_fan", "controller_fan board"});
    // A second auto fan's card keeps its own speed line.
    get_printer_state().update_from_status(
        json{{"heater_fan hotend_fan", {{"speed", 0.5}, {"rpm", 3000.0}}},
             {"controller_fan board", {{"speed", 0.25}}}});
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
    CHECK(has_label(root, "25%"));
    CHECK(lv_anim_get(icon, helix::ui::fan_spin_anim_cb) != nullptr);
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) >= turned);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan dial: two dials keep their own speed, switch text and rpm",
                 "[fan_overlay][rpm]") {
    FanDial dial(test_screen(), "Part Fan", "fan", 40);
    FanDial other(test_screen(), "Exhaust", "fan", 0, /*label_bottom=*/true);
    auto named = [](FanDial& d, const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(d.get_root(), name);
        REQUIRE(obj != nullptr);
        return obj;
    };
    auto visible = [](lv_obj_t* obj) { return !lv_obj_is_hidden(obj); };
    lv_obj_t* rpm = named(dial, "rpm_label");
    lv_obj_t* speed = named(dial, "speed_label");
    CHECK(lv_obj_is_hidden(rpm));
    CHECK(shows(speed, "40%"));
    CHECK(shows(named(other, "speed_label"), "Off"));
    CHECK(visible(named(dial, "onoff_label_on")));
    CHECK_FALSE(visible(named(dial, "onoff_label")));
    CHECK(visible(named(other, "onoff_label")));
    CHECK_FALSE(visible(named(other, "onoff_label_on")));
    // The carousel layout names the fan under the arc; the dial's arc has a knob to grab.
    CHECK(visible(named(dial, "name_label")));
    CHECK_FALSE(visible(named(other, "name_label")));
    CHECK(visible(named(other, "name_label_bottom")));
    CHECK(lv_obj_get_style_bg_opa(named(dial, "dial_arc"), LV_PART_KNOB) == LV_OPA_COVER);

    dial.set_rpm(2500);
    CHECK(shows(rpm, "2500 RPM"));
    CHECK(shows(speed, "40%"));
    CHECK(dial.get_speed() == 40);
    CHECK_FALSE(visible(named(other, "rpm_label")));

    other.set_speed(75);
    CHECK(shows(named(other, "speed_label"), "75%"));
    CHECK(visible(named(other, "onoff_label_on")));
    CHECK(shows(speed, "40%"));

    dial.set_rpm(std::nullopt);
    CHECK(lv_obj_is_hidden(rpm));
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

    SECTION("the dial's own switch turned Off hides it at once") {
        lv_obj_t* sw = lv_obj_find_by_name(dial.get_root(), "onoff_switch");
        REQUIRE(sw != nullptr);
        REQUIRE(lv_obj_has_state(sw, LV_STATE_CHECKED));
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        REQUIRE(dial.get_speed() == 0);
        CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("the dial's arc dragged to 0 hides it at once") {
        lv_obj_t* arc = lv_obj_find_by_name(dial.get_root(), "dial_arc");
        REQUIRE(arc != nullptr);
        lv_arc_set_value(arc, 0);
        lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, nullptr);
        REQUIRE(dial.get_speed() == 0);
        CHECK(lv_obj_has_flag(rpm, LV_OBJ_FLAG_HIDDEN));
    }
}
