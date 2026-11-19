// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_z_offset_state.cpp
 * @brief The live side of Z offset by material (prestonbrown/helixscreen#2142):
 *        where a Save goes, what it sends, and the one config file it edits.
 *
 * The hazard these guard is #1401's: with the start macro re-applying an
 * absolute offset every print, a machine-wide save folds that offset into the
 * probe and the next print doubles it. So a per-material save must never send
 * Z_OFFSET_APPLY_* or SAVE_CONFIG, and the feature turned on must keep the
 * machine-wide save off the table.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_recording_api.h"
#include "app_globals.h"
#include "material_z_offset_state.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;
namespace mz = helix::material_zoffset;

namespace {

class MaterialStateFixture : public LVGLTestFixture {
  public:
    MaterialStateFixture() {
        get_printer_state().deinit_subjects();
        get_printer_state().init_subjects(true);
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(true);
        helix::SettingsManager::instance().init_subjects();
        helix::SettingsManager::instance().set_z_offset_save_mode(0);
        state().init_subjects();
        helix::zoffset::deinit_save_available_subject();
        helix::zoffset::init_save_available_subject(true);
    }

    ~MaterialStateFixture() override {
        state().set_enabled(false);
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
        state().on_discovery(get_printer_state().get_discovery());
        helix::zoffset::deinit_save_available_subject();
        helix::ToolState::instance().deinit_subjects();
        get_printer_state().deinit_subjects();
    }

    static mz::MaterialZOffsetState& state() {
        return mz::MaterialZOffsetState::instance();
    }

    /// Discover a printer with these gcode macros, and save_variables when asked.
    void discover(std::initializer_list<const char*> macros, bool save_variables) {
        json objects = json::array({"gcode_move", "toolhead", "bed_mesh"});
        if (save_variables) {
            objects.push_back("save_variables");
        }
        for (const char* m : macros) {
            objects.push_back(std::string("gcode_macro ") + m);
        }
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        get_printer_state().set_hardware(hw);
        state().on_discovery(get_printer_state().get_discovery());
    }

    void discover_helix_macro() {
        discover({"HELIX_MATERIAL_OFFSET", "_HELIX_MATERIAL"}, true);
    }

    void status(const json& frame) {
        get_printer_state().update_from_status(frame);
        state().update_from_status(frame);
    }

    void live_offset_mm(double mm) {
        status(json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
    }

    /// Run a save and drain until its callback reports.
    std::pair<bool, std::string> save(const std::string& material, int microns) {
        bool finished = false;
        std::pair<bool, std::string> result;
        state().save_offset(&api, material, microns, [&](bool ok, const std::string& message) {
            finished = true;
            result = {ok, message};
        });
        REQUIRE(wait_until([&] { return finished; }, 5000));
        return result;
    }

    bool sent_anything_that_folds_or_restarts() const {
        return api.contains("Z_OFFSET_APPLY") || api.contains("SAVE_CONFIG");
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::test::GcodeRecordingApi api{client, get_printer_state()};
};

const json kPetgApplied = {
    {"save_variables",
     {{"variables", {{"helix_zoffset_petg", 0.05}, {"helix_zoffset_default", 0}}}}},
    {"gcode_macro _HELIX_MATERIAL", {{"applied", "PETG"}, {"apply_count", 1}}}};

} // namespace

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a save writes only the applied material's entry",
                 "[material_zoffset][zoffset]") {
    discover_helix_macro();
    status(kPetgApplied);
    live_offset_mm(0.08);
    REQUIRE(state().status() == mz::Status::Ready);

    SECTION("off: the machine-wide save is untouched") {
        CHECK_FALSE(state().save_target().has_value());
        CHECK_FALSE(state().save_withheld());
    }
    SECTION("on: the target is PETG, dirty against PETG's stored value") {
        state().set_enabled(true);
        auto target = state().save_target();
        REQUIRE(target.has_value());
        CHECK(target->material == "PETG");
        CHECK(target->stored == 50);
        auto facts = helix::zoffset::current_save_availability();
        CHECK(facts.manual_save_supported);
        CHECK(facts.global_dirty);

        auto [ok, message] = save(target->material, 80);
        CHECK(ok);
        CHECK(message.find("PETG") != std::string::npos);
        CHECK(api.sent ==
              std::vector<std::string>{"SAVE_VARIABLE VARIABLE=helix_zoffset_petg VALUE=0.080"});
        CHECK_FALSE(sent_anything_that_folds_or_restarts());

        // The store reports back: nothing left to save.
        status(json{{"save_variables", {{"variables", {{"helix_zoffset_petg", 0.08}}}}}});
        CHECK_FALSE(helix::zoffset::current_save_availability().global_dirty);
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: no save while the material is unknown",
                 "[material_zoffset][zoffset]") {
    SECTION("HelixScreen's macro has not run since Klipper started") {
        discover_helix_macro();
        state().set_enabled(true);
        live_offset_mm(0.08);
        CHECK(state().save_withheld());
        CHECK_FALSE(helix::zoffset::current_save_availability().manual_save_supported);
        status(kPetgApplied);
        CHECK_FALSE(state().save_withheld());
    }
    SECTION("the macro ran without a material: DEFAULT") {
        discover_helix_macro();
        state().set_enabled(true);
        status(json{{"gcode_macro _HELIX_MATERIAL", {{"applied", ""}, {"apply_count", 2}}}});
        auto target = state().save_target();
        REQUIRE(target.has_value());
        CHECK(target->material == "DEFAULT");
    }
    SECTION("without [save_variables] the feature waits for setup") {
        discover({"HELIX_MATERIAL_OFFSET"}, false);
        state().set_enabled(true);
        status(kPetgApplied);
        CHECK(state().status() == mz::Status::NeedsSetup);
        CHECK_FALSE(state().active());
        CHECK_FALSE(state().save_target().has_value());
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: _START_PRINT_VARS edits the one file that defines it",
                 "[material_zoffset][zoffset]") {
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    state().set_enabled(true);
    status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.02}, {"offset_default", 0}}}});
    state().note_print_file("PETG");
    const std::string vars = "[gcode_macro _START_PRINT_VARS]\n"
                             "variable_offset_PETG: 0.02\n"
                             "variable_offset_DEFAULT: 0\n"
                             "gcode:\n";
    // A backup copy nothing includes does not count as a second definition.
    const std::string backup = "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PETG: 0.5\n";

    SECTION("found through printer.cfg's includes") {
        api.set_config_files({{"printer.cfg", "[include overrides.cfg]\n[include macros/*.cfg]\n"},
                              {"overrides.cfg", vars},
                              {"macros/start.cfg", "[gcode_macro START_PRINT]\ngcode:\n  G28\n"},
                              {"printer-20261118_101500.cfg", backup}});
        auto target = state().save_target();
        REQUIRE(target.has_value());
        CHECK(target->material == "PETG");

        auto [ok, message] = save("PETG", 80);
        INFO(message);
        CHECK(ok);
        CHECK(api.sent == std::vector<std::string>{"SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS "
                                                   "VARIABLE=offset_petg VALUE=0.080"});
        CHECK_FALSE(sent_anything_that_folds_or_restarts());
        CHECK(api.get_uploaded_config("overrides.cfg") == "[gcode_macro _START_PRINT_VARS]\n"
                                                          "variable_offset_PETG: 0.080\n"
                                                          "variable_offset_DEFAULT: 0\n"
                                                          "gcode:\n");
        CHECK(api.get_uploaded_config("printer-20261118_101500.cfg") == backup);
    }
    SECTION("defined twice among the loaded files: refuses the file, says why") {
        api.set_config_files({{"printer.cfg", "[include a.cfg]\n[include b.cfg]\n"},
                              {"a.cfg", vars},
                              {"b.cfg", vars}});
        auto [ok, message] = save("PETG", 80);
        CHECK_FALSE(ok);
        CHECK(message.find("2") != std::string::npos);
        CHECK(api.get_uploaded_config("a.cfg") == vars);
        CHECK(api.get_uploaded_config("b.cfg") == vars);
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a fresh mesh is offered to the closest material",
                 "[material_zoffset][bed_mesh]") {
    const std::vector<std::string> materials = {"PLA", "PETG", "ABS"};
    const int petg_bed = mz::usual_bed_temp("PETG");
    REQUIRE(petg_bed > 0);

    discover_helix_macro();
    CHECK_FALSE(state().mesh_offer(materials, petg_bed).has_value()); // feature off
    state().set_enabled(true);
    auto offer = state().mesh_offer(materials, petg_bed);
    REQUIRE(offer.has_value());
    CHECK(offer->first == "PETG");
    CHECK(offer->second == "helix_petg");

    // The vendor start macro keeps no mesh per material.
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    CHECK_FALSE(state().mesh_offer(materials, petg_bed).has_value());
}
