// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_env_overlay_humidity_target.cpp
 * @brief The dryer's optional humidity target: the capability, the field that asks
 *        for it, and which start command the overlay sends (prestonbrown/helixscreen#1755)
 */

#include "ui_ams_environment_overlay.h"
#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_dryer.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"
#include "settings_manager.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using namespace helix::ui;

namespace {

/// A dryer-equipped mock that records which start command the overlay chose instead
/// of running the simulated cycle.
class RecordingDryerMock : public AmsBackendMock {
  public:
    struct Start {
        std::optional<float> humidity_pct;
        int duration_min = 0;
    };

    explicit RecordingDryerMock(bool humidity_target,
                                std::optional<float> default_humidity = std::nullopt,
                                std::optional<std::vector<DryingPreset>> presets = std::nullopt)
        : presets_(std::move(presets)) {
        set_dryer_enabled(true);
        set_dryer_humidity_target(humidity_target);
        set_dryer_default_humidity(default_humidity);
    }

    AmsError start_drying(float, int duration_min, int, int) override {
        starts.push_back({std::nullopt, duration_min});
        return AmsErrorHelper::success();
    }

    AmsError start_drying_to_humidity(float, int duration_min, float humidity_pct, int,
                                      int) override {
        starts.push_back({humidity_pct, duration_min});
        return AmsErrorHelper::success();
    }

    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override {
        return presets_ ? *presets_ : AmsBackendMock::get_drying_presets();
    }

    std::vector<Start> starts;

  private:
    std::optional<std::vector<DryingPreset>> presets_;
};

void reset_overlay_singleton() {
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

/// The zone the overlay is shown on: unit 0, dryer straight from the backend.
EnvironmentZone dryer_zone(const AmsBackend& backend) {
    EnvironmentZone zone;
    zone.id = "unit0";
    zone.unit_index = 0;
    zone.gates = {0, 1, 2, 3};
    zone.dryer = dryer_info(backend, 0);
    return zone;
}

lv_obj_t* find_widget(const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(lv_screen_active(), name);
    REQUIRE(obj != nullptr);
    return obj;
}

int subject_int(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}

RecordingDryerMock*
show_overlay_with(bool humidity_target, std::optional<float> default_humidity = std::nullopt,
                  std::optional<std::vector<DryingPreset>> presets = std::nullopt) {
    auto backend =
        std::make_unique<RecordingDryerMock>(humidity_target, default_humidity, std::move(presets));
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    get_ams_environment_overlay().show_zone(lv_screen_active(), {dryer_zone(*raw)}, 0, false);
    helix::ui::UpdateQueue::instance().drain();
    return raw;
}

/// The overlay on a screen with a working numeric keypad.
struct DryerKeypadFixture : LVGLUITestFixture {
    DryerKeypadFixture() {
        SettingsManager::instance().init_subjects();
        helix_test::reset_input_settings_to_defaults();
        reset_overlay_singleton();
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }

    ~DryerKeypadFixture() override {
        NavigationManager::instance().shutdown();
        reset_overlay_singleton();
        AmsState::instance().set_backend(nullptr);
        helix_test::reset_input_settings_to_defaults();
    }

    lv_obj_t* open_keypad(const char* input) {
        lv_obj_send_event(find_widget(input), LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* keypad = find_widget("keypad_panel");
        lv_obj_update_layout(keypad);
        return keypad;
    }

    void press(lv_obj_t* keypad, const char* key) {
        lv_obj_t* k = lv_obj_find_by_name(keypad, key);
        REQUIRE(k != nullptr);
        lv_obj_send_event(k, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    void confirm() {
        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, "on_keypad_confirm");
        REQUIRE(cb != nullptr);
        cb(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(600);
        helix::ui::UpdateQueue::instance().drain();
    }
};

void press_start() {
    lv_obj_send_event(find_widget("btn_start_stop"), LV_EVENT_CLICKED, nullptr);
}

} // namespace

TEST_CASE("DryerInfo::humidity_target answers only for a dryer that takes one",
          "[ams][dryer][dryer_humidity]") {
    DryerInfo dryer;
    dryer.supported = true;
    CHECK_FALSE(dryer.humidity_target(20.0f));

    dryer.supports_humidity_target = true;
    CHECK(dryer.humidity_target(20.0f) == 20.0f);
    CHECK_FALSE(dryer.humidity_target(0.0f));
    CHECK_FALSE(dryer.humidity_target(-5.0f));
    CHECK(dryer.humidity_target(150.0f) == DryerInfo::kMaxHumidityTargetPct);
}

TEST_CASE("A dryer reports no humidity target unless its backend takes one",
          "[ams][dryer][dryer_humidity]") {
    AmsBackendMock plain;
    plain.set_dryer_enabled(true);
    CHECK(plain.get_dryer_info(0).supported);
    CHECK_FALSE(plain.get_dryer_info(0).supports_humidity_target);
    CHECK(plain.start_drying_to_humidity(50.0f, 60, 20.0f, -1, 0).result ==
          AmsResult::NOT_SUPPORTED);

    AmsBackendMock targeted;
    targeted.set_dryer_humidity_target(true);
    // No dryer, no target, whatever the switch says.
    CHECK_FALSE(targeted.get_dryer_info(0).supports_humidity_target);
    targeted.set_dryer_enabled(true);
    CHECK(targeted.get_dryer_info(0).supports_humidity_target);
}

TEST_CASE_METHOD(XMLTestFixture, "The duration field takes hours and the dryer gets minutes",
                 "[ams][dryer][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(false);

    lv_textarea_set_text(find_widget("duration_input"), "4.5");
    press_start();
    REQUIRE(backend->starts.size() == 1);
    CHECK(backend->starts[0].duration_min == 270);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "The humidity target field is hidden for a dryer that takes none",
                 "[ams][dryer][dryer_humidity][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(false);

    CHECK(subject_int("ams_env_overlay_humidity_target_visible") == 0);
    CHECK(lv_obj_has_flag(find_widget("humidity_target_column"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(find_widget("duration_input"), LV_OBJ_FLAG_HIDDEN));

    // Text left in the hidden field still sends the plain temp + time start.
    lv_textarea_set_text(find_widget("humidity_target_input"), "20");
    press_start();
    REQUIRE(backend->starts.size() == 1);
    CHECK_FALSE(backend->starts[0].humidity_pct);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "The humidity target field shows for a dryer that takes one and sends it",
                 "[ams][dryer][dryer_humidity][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(true);

    CHECK(subject_int("ams_env_overlay_humidity_target_visible") == 1);
    CHECK_FALSE(lv_obj_has_flag(find_widget("humidity_target_column"), LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* input = find_widget("humidity_target_input");
    CHECK(std::string(lv_textarea_get_text(input)).empty());

    SECTION("blank sends the plain temp + time start") {
        press_start();
        REQUIRE(backend->starts.size() == 1);
        CHECK_FALSE(backend->starts[0].humidity_pct);
    }
    SECTION("a value sends the humidity-terminated start") {
        lv_textarea_set_text(input, "18");
        press_start();
        REQUIRE(backend->starts.size() == 1);
        REQUIRE(backend->starts[0].humidity_pct);
        CHECK(*backend->starts[0].humidity_pct == 18.0f);
    }

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A humidity target applies to one start, not the next open",
                 "[ams][dryer][dryer_humidity][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(true);

    lv_obj_t* input = find_widget("humidity_target_input");
    lv_textarea_set_text(input, "18");
    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();

    get_ams_environment_overlay().show_zone(lv_screen_active(), {dryer_zone(*backend)}, 0, false);
    helix::ui::UpdateQueue::instance().drain();

    // The same widget, so a blank field is the reopen clearing it, not a rebuild.
    REQUIRE(find_widget("humidity_target_input") == input);
    CHECK(std::string(lv_textarea_get_text(input)).empty());
    press_start();
    REQUIRE(backend->starts.size() == 1);
    CHECK_FALSE(backend->starts[0].humidity_pct);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "The blank humidity target shows the target the dryer applies",
                 "[ams][dryer][dryer_humidity][overlay]") {
    reset_overlay_singleton();

    SECTION("a known default reads as its percentage") {
        show_overlay_with(true, 25.0f);
        CHECK(std::string(lv_textarea_get_placeholder_text(find_widget("humidity_target_input"))) ==
              "25%");
    }
    SECTION("an unknown default reads as the dryer deciding") {
        show_overlay_with(true);
        CHECK(std::string(lv_textarea_get_placeholder_text(find_widget("humidity_target_input"))) ==
              lv_tr("auto"));
    }

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE("A dryer reports its default humidity target only alongside the capability",
          "[ams][dryer][dryer_humidity]") {
    AmsBackendMock mock;
    mock.set_dryer_enabled(true);
    mock.set_dryer_default_humidity(25.0f);
    CHECK_FALSE(mock.get_dryer_info(0).default_humidity_target);
    mock.set_dryer_humidity_target(true);
    REQUIRE(mock.get_dryer_info(0).default_humidity_target);
    CHECK(*mock.get_dryer_info(0).default_humidity_target == 25.0f);
}

TEST_CASE_METHOD(DryerKeypadFixture, "The duration keypad takes a fraction of an hour",
                 "[ams][dryer][overlay]") {
    auto* backend = show_overlay_with(false);

    lv_obj_t* keypad = open_keypad("duration_input");
    press(keypad, "btn_1");
    press(keypad, "btn_dot");
    press(keypad, "btn_5");
    confirm();

    CHECK(std::string(lv_textarea_get_text(find_widget("duration_input"))) == "1.5");
    press_start();
    REQUIRE(backend->starts.size() == 1);
    CHECK(backend->starts[0].duration_min == 90);
}

TEST_CASE_METHOD(DryerKeypadFixture, "The whole-number dryer fields refuse a decimal point",
                 "[ams][dryer][dryer_humidity][overlay]") {
    show_overlay_with(true);
    const char* input = GENERATE("temp_input", "humidity_target_input");
    CAPTURE(input);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(open_keypad(input), "btn_dot"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(XMLTestFixture, "The duration field opens in hours when no preset picks a value",
                 "[ams][dryer][overlay]") {
    reset_overlay_singleton();
    show_overlay_with(false, std::nullopt, std::vector<DryingPreset>{});

    CHECK(std::string(lv_textarea_get_text(find_widget("duration_input"))) == "4");

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A preset's fractional hours reach its label and the field",
                 "[ams][dryer][overlay]") {
    reset_overlay_singleton();
    show_overlay_with(false, std::nullopt, std::vector<DryingPreset>{{"PLA", 45.0f, 90, 50}});

    lv_obj_t* dropdown = find_widget("preset_dropdown");
    CHECK_THAT(std::string(lv_dropdown_get_options(dropdown)),
               Catch::Matchers::ContainsSubstring("/1.5h"));

    lv_obj_t* duration = find_widget("duration_input");
    lv_textarea_set_text(duration, "");
    lv_dropdown_set_selected(dropdown, 0);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(std::string(lv_textarea_get_text(duration)) == "1.5");

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "A running dry shows its phase and time left",
                 "[ams][dryer][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(false);

    EnvironmentZone zone = dryer_zone(*backend);
    zone.dryer.active = true;
    zone.dryer.phase = DryingPhase::Holding;
    zone.dryer.remaining_min = 205;
    get_ams_environment_overlay().show_zone(lv_screen_active(), {zone}, 0, false);
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* text = lv_xml_get_subject(nullptr, "ams_env_overlay_drying_text");
    REQUIRE(text != nullptr);
    CHECK(std::string(lv_subject_get_string(text)) == "Drying: 3:25 left");

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "The overlay pictures the unit it shows, and nothing without one",
                 "[ams][dryer][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay_with(false);

    const AmsSystemInfo info = backend->get_system_info();
    REQUIRE_FALSE(info.units.empty());
    const LogoArt art = AmsState::get_unit_logo(info.units[0], info);
    REQUIRE(art.path != nullptr);
    REQUIRE(art.path[0] != '\0');
    lv_subject_t* logo = lv_xml_get_subject(nullptr, "ams_env_overlay_unit_logo");
    REQUIRE(logo != nullptr);
    CHECK(std::string(lv_subject_get_string(logo)) == art.path);
    CHECK(subject_int("ams_env_overlay_unit_logo_color") == (art.color ? 1 : 0));
    CHECK(subject_int("ams_env_overlay_unit_logo_visible") == 1);

    get_ams_environment_overlay().show_zone(lv_screen_active(), {}, 0, false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(subject_int("ams_env_overlay_unit_logo_visible") == 0);

    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
}
