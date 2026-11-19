// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_chamber.cpp
 * @brief FilamentPanel's chamber heater is the one PrinterState resolved, the
 *        same heater TemperatureController sends to.
 *
 * Cool Down's default gcode and a material's chamber target are where the panel
 * asks whether the printer has a chamber heater. The rule itself is pinned in
 * test_chamber_heater_assignment.cpp.
 */

#include "ui_panel_filament.h"
#include "ui_temperature_utils.h"

#include "../../include/moonraker_client_mock.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_state.h"
#include "config.h"
#include "filament_database.h"
#include "moonraker_api.h"
#include "preset_materials.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "tool_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// A FilamentPanel over a mock printer, so the gcode Cool Down sends is observable.
struct ChamberPanelHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    ChamberPanelHarness() {
        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        state.init_subjects(false);
        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
    }

    ~ChamberPanelHarness() {
        panel.reset();
        helix::AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    }

    /// Discovery lands with these objects under this chamber heater assignment.
    void discover(const char* heater_assignment, std::initializer_list<const char*> objects) {
        helix::SettingsManager::instance().set_chamber_heater_assignment(heater_assignment);
        helix::PrinterDiscovery hw;
        nlohmann::json list = nlohmann::json::array();
        for (const char* object : objects) {
            list.push_back(object);
        }
        hw.parse_objects(list);
        state.set_hardware(std::move(hw));
        state.set_klippy_state_sync(helix::KlippyState::READY);
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

/// A FilamentPanel with the REAL shipped presets/k2.json applied to a real
/// Config, so Cool Down's gcode reflects the preset's persisted
/// default_macros.cooldown the way an installed printer's does, not just
/// FilamentPanel's own in-memory fallback (which `cfg->get_macro()` only ever
/// reaches when no printer profile has been created, i.e. never in the field).
/// Local to this file, like VariantPresetFixture in test_printer_detector.cpp.
struct K2PresetCooldownHarness {
    helix::Config config;
    std::string temp_dir;
    std::string saved_config_dir_;
    std::string saved_data_dir_;
    bool had_config_dir_ = false;
    bool had_data_dir_ = false;
    helix::Config* saved_instance_ = nullptr;

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<FilamentPanel> panel;

    K2PresetCooldownHarness() {
        namespace fs = std::filesystem;
        temp_dir =
            (fs::temp_directory_path() / ("test_k2_cooldown_" + std::to_string(getpid()))).string();
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

        ToolState::instance().init_subjects(true);
        helix::AmsState::instance().init_subjects(true);
        state.init_subjects(false);
        panel = std::make_unique<FilamentPanel>(state, &api);
        panel->init_subjects();
    }

    ~K2PresetCooldownHarness() {
        panel.reset();
        helix::AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");

        helix::ConfigTestAccess::instance_ref() = saved_instance_;

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
        state.set_hardware(std::move(hw));
        state.set_klippy_state_sync(helix::KlippyState::READY);
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

/// The first preset slot whose material asks for a heated chamber.
int first_slot_with_chamber_temp() {
    for (int slot = 0; slot < helix::presets::kPresetCount; ++slot) {
        auto material = filament::find_material(helix::presets::name(slot));
        if (material && material->chamber_temp_c > 0) {
            return slot;
        }
    }
    return -1;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Cool Down turns off the chamber heater PrinterState resolved",
                 "[filament][chamber]") {
    ChamberPanelHarness h;

    SECTION("an assigned heater no chamber keyword names") {
        h.discover("heater_generic ptc_heater",
                   {"heater_generic ptc_heater", "extruder", "heater_bed"});
        TA::handle_cooldown(*h.panel);

        REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
        CHECK(h.sent("HEATER=ptc_heater TARGET=0"));
    }
    SECTION("never a heater the printer is set not to use") {
        h.discover("none", {"heater_generic chamber", "extruder", "heater_bed"});
        TA::handle_cooldown(*h.panel);

        REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
        CHECK_FALSE(h.sent("HEATER=chamber TARGET=0"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A material's chamber temperature targets only the chamber heater PrinterState "
                 "resolved",
                 "[filament][chamber]") {
    const int slot = first_slot_with_chamber_temp();
    REQUIRE(slot >= 0);
    const auto material = filament::find_material(helix::presets::name(slot));
    REQUIRE(material);
    ChamberPanelHarness h;

    SECTION("an assigned heater no chamber keyword names") {
        h.discover("heater_generic ptc_heater",
                   {"heater_generic ptc_heater", "extruder", "heater_bed"});
        h.panel->set_material(slot);

        CHECK(TA::chamber_target(*h.panel) ==
              helix::temperature_rules::degrees_to_deci(material->chamber_temp_c));
    }
    SECTION("never a heater the printer is set not to use") {
        h.discover("none", {"heater_generic chamber", "extruder", "heater_bed"});
        h.panel->set_material(slot);

        int nozzle_target = 0;
        h.panel->get_temp(nullptr, &nozzle_target);
        // set_material ran for this material.
        REQUIRE(nozzle_target == material->nozzle_recommended());
        CHECK(TA::chamber_target(*h.panel) == 0);
    }
}

// ============================================================================
// The shared k2 preset's cooldown macro and a family member without the
// heater it assumes
// ============================================================================

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "Cool Down on a base K2 never addresses the chamber heater the shared k2 preset assumes",
    "[filament][chamber][presets]") {
    K2PresetCooldownHarness h;

    // Real base K2 shape: no heater_generic chamber_heater object; chamber_fan
    // is the only chamber-related object Klipper reports (docs/devel/CHAMBER_HEATER.md).
    h.discover("auto", {"temperature_fan chamber_fan", "temperature_sensor chamber_temp",
                        "extruder", "heater_bed"});
    TA::handle_cooldown(*h.panel);

    REQUIRE(h.sent("HEATER=extruder TARGET=0"));
    REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
    CHECK_FALSE(h.sent("chamber_heater"));
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "Cool Down on a K2 Plus still turns off its chamber heater via the shared k2 preset",
    "[filament][chamber][presets]") {
    K2PresetCooldownHarness h;

    h.discover("auto", {"heater_generic chamber_heater", "extruder", "heater_bed"});
    TA::handle_cooldown(*h.panel);

    REQUIRE(h.sent("HEATER=extruder TARGET=0"));
    REQUIRE(h.sent("HEATER=heater_bed TARGET=0"));
    CHECK(h.sent("HEATER=chamber_heater TARGET=0"));
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "Cool Down runs a user-customized macro verbatim, even with a resolved chamber heater",
    "[filament][chamber][presets]") {
    K2PresetCooldownHarness h;

    // A user edited their Cool Down macro after the k2 preset installed it.
    helix::ConfigTestAccess::data(h.config)["printers"]["default"]["default_macros"]["cooldown"] =
        "MY_CUSTOM_COOLDOWN_MACRO";

    h.discover("auto", {"heater_generic chamber_heater", "extruder", "heater_bed"});
    TA::handle_cooldown(*h.panel);

    CHECK(h.sent("MY_CUSTOM_COOLDOWN_MACRO"));
    CHECK_FALSE(h.sent("HEATER="));
}
