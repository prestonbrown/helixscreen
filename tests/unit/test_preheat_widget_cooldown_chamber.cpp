// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_preheat_widget_cooldown_chamber.cpp
 * @brief PreheatWidget's Cool Down, like FilamentPanel's, must address the
 *        chamber heater PrinterState resolved for THIS printer rather than the
 *        one a platform preset's persisted macro text assumes.
 *
 * Companion to test_filament_panel_chamber.cpp's k2-preset cases, over the
 * home panel's cooldown path instead of the filament panel's.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/preheat_widget_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "preheat_widget.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::PreheatWidgetTestAccess;

namespace {

/// Wires a real Config with the shipped presets/k2.json applied, and a mock
/// MoonrakerAPI registered globally, so PreheatWidget::handle_cooldown() (which
/// reads Config::get_instance() and get_moonraker_api(), not injected
/// pointers) sees exactly what an installed K2 printer would.
struct K2PreheatCooldownHarness {
    helix::Config config;
    std::string temp_dir;
    std::string saved_config_dir_;
    std::string saved_data_dir_;
    bool had_config_dir_ = false;
    bool had_data_dir_ = false;
    helix::Config* saved_instance_ = nullptr;
    IMoonrakerAPI* saved_api_ = nullptr;

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};

    K2PreheatCooldownHarness() {
        namespace fs = std::filesystem;
        temp_dir =
            (fs::temp_directory_path() / ("test_k2_preheat_cooldown_" + std::to_string(getpid())))
                .string();
        fs::create_directories(temp_dir + "/presets");

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        if (const char* prev = std::getenv("HELIX_DATA_DIR")) {
            saved_data_dir_ = prev;
            had_data_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);
        setenv("HELIX_DATA_DIR", temp_dir.c_str(), 1);

        fs::path shipped = fs::current_path() / "assets" / "config" / "presets" / "k2.json";
        INFO("reading " << shipped.string() << " (tests must run from the repo root)");
        REQUIRE(fs::exists(shipped));
        std::error_code ec;
        fs::copy_file(shipped, fs::path(temp_dir) / "presets" / "k2.json",
                      fs::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);

        helix::ConfigTestAccess::path(config) = temp_dir + "/settings.json";
        helix::ConfigTestAccess::active_printer_id(config) = "default";
        helix::ConfigTestAccess::data(config) = {
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "127.0.0.1"}, {"wizard_completed", false}}}}}};

        REQUIRE(config.apply_preset_file("k2"));

        saved_instance_ = helix::ConfigTestAccess::instance_ref();
        helix::ConfigTestAccess::instance_ref() = &config;

        saved_api_ = get_moonraker_api();
        set_moonraker_api(&api);
    }

    ~K2PreheatCooldownHarness() {
        set_moonraker_api(saved_api_);
        helix::ConfigTestAccess::instance_ref() = saved_instance_;
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");

        namespace fs = std::filesystem;
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        if (had_data_dir_) {
            setenv("HELIX_DATA_DIR", saved_data_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_DATA_DIR");
        }
    }

    void discover(const char* heater_assignment, std::initializer_list<const char*> objects) {
        helix::SettingsManager::instance().set_chamber_heater_assignment(heater_assignment);
        helix::PrinterDiscovery hw;
        nlohmann::json list = nlohmann::json::array();
        for (const char* object : objects) {
            list.push_back(object);
        }
        hw.parse_objects(list);
        get_printer_state().set_hardware(std::move(hw));
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        client.clear_gcode_script_history();
    }

    bool sent(const std::string& fragment) const {
        for (const auto& gcode : client.gcode_script_history()) {
            if (gcode.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "PreheatWidget Cool Down on a base K2 never addresses the chamber heater the shared k2 "
    "preset assumes",
    "[preheat][chamber][presets]") {
    K2PreheatCooldownHarness h;
    helix::PreheatWidget widget(get_printer_state());

    h.discover("auto", {"temperature_fan chamber_fan", "temperature_sensor chamber_temp",
                        "extruder", "heater_bed"});
    TA::handle_cooldown(widget);

    REQUIRE(h.sent("HEATER=extruder TARGET=0"));
    REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
    CHECK_FALSE(h.sent("chamber_heater"));
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "PreheatWidget Cool Down on a K2 Plus still turns off its chamber heater via the shared "
    "k2 preset",
    "[preheat][chamber][presets]") {
    K2PreheatCooldownHarness h;
    helix::PreheatWidget widget(get_printer_state());

    h.discover("auto", {"heater_generic chamber_heater", "extruder", "heater_bed"});
    TA::handle_cooldown(widget);

    REQUIRE(h.sent("HEATER=extruder TARGET=0"));
    REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
    CHECK(h.sent("HEATER=chamber_heater TARGET=0"));
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "PreheatWidget Cool Down runs a user-customized macro verbatim, even with a resolved "
    "chamber heater",
    "[preheat][chamber][presets]") {
    K2PreheatCooldownHarness h;
    helix::PreheatWidget widget(get_printer_state());

    // A user edited their Cool Down macro after the k2 preset installed it.
    helix::ConfigTestAccess::data(h.config)["printers"]["default"]["default_macros"]["cooldown"] =
        "MY_CUSTOM_COOLDOWN_MACRO";

    h.discover("auto", {"heater_generic chamber_heater", "extruder", "heater_bed"});
    TA::handle_cooldown(widget);

    CHECK(h.sent("MY_CUSTOM_COOLDOWN_MACRO"));
    CHECK_FALSE(h.sent("HEATER="));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PreheatWidget Cool Down on a toolchanger turns off every extruder heater",
                 "[preheat][cooldown][presets]") {
    K2PreheatCooldownHarness h;
    helix::PreheatWidget widget(get_printer_state());

    h.discover("auto", {"extruder", "extruder1", "extruder2", "heater_bed"});
    auto& temps = get_printer_state().temperature_state();
    temps.init_extruders({"extruder", "extruder1", "extruder2", "heater_bed"});
    TA::handle_cooldown(widget);

    CHECK(h.sent("HEATER=extruder TARGET=0"));
    CHECK(h.sent("HEATER=extruder1 TARGET=0"));
    CHECK(h.sent("HEATER=extruder2 TARGET=0"));
    CHECK(h.sent("HEATER=heater_bed TARGET=0"));

    temps.init_extruders({"extruder"});
}
