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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/tile_boxes.h"
#include "app_globals.h"
#include "material_z_offset_state.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "save_config_restart.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/z_offset_widget.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <functional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;
namespace mz = helix::material_zoffset;

namespace {

class MaterialStateFixture : public LVGLUITestFixture {
  public:
    MaterialStateFixture() {
        get_printer_state().deinit_subjects();
        get_printer_state().init_subjects(true);
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(true);
        helix::SettingsManager::instance().init_subjects();
        helix::SettingsManager::instance().set_z_offset_save_mode(0);
        state().init_subjects();
        previous_api_ = get_moonraker_api();
        set_moonraker_api(&api); // saves resolve the api at each step
        helix::zoffset::deinit_save_available_subject();
        helix::zoffset::init_save_available_subject(true);
    }

    ~MaterialStateFixture() override {
        state().set_enabled(nullptr, false);
        state().reset_printer_scope();
        set_moonraker_api(previous_api_);
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

    /// Turn the setting on, and have the printer report the switch it sends
    /// (a vendor-macro printer has none and ignores the frame); the switch is
    /// not what a case examines.
    void enable() {
        state().set_enabled(&api, true);
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 1}}}}}});
        api.sent.clear();
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
        state().save_offset(material, microns, [&](bool ok, const std::string& message) {
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
    helix::IMoonrakerAPI* previous_api_ = nullptr;
    helix::ui::SaveConfigWatch watch;
    helix::test::GcodeRecordingApi api{client, get_printer_state()};
};

const json kPetgApplied = {
    {"save_variables",
     {{"variables",
       {{"helix_zoffset_petg", 0.05},
        {"helix_zoffset_default", 0},
        {"helix_material_offset_enabled", 1}}}}},
    {"gcode_macro _HELIX_MATERIAL", {{"applied", "PETG"}, {"apply_count", 1}}}};

/// Config transfers where another writer edits a file right after this
/// session's Nth read of it.
class EditedBetweenReads : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;
    void download_file(const std::string& root, const std::string& path, StringCallback on_success,
                       ErrorCallback on_error) override {
        MoonrakerFileTransferAPIMock::download_file(root, path, on_success, on_error);
        if (path == watched && ++reads == edit_after && edit) {
            edit();
        }
    }
    std::string watched;
    int reads = 0;
    int edit_after = 0;
    std::function<void()> edit;
};

class EditedBetweenReadsApi : public helix::test::GcodeRecordingApi {
  public:
    EditedBetweenReadsApi(MoonrakerClientMock& client, helix::PrinterState& state)
        : GcodeRecordingApi(client, state) {
        auto transfers = std::make_unique<EditedBetweenReads>(client, get_http_base_url());
        edits = transfers.get();
        file_transfer_api_ = std::move(transfers);
    }
    EditedBetweenReads* edits = nullptr;
};

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
        enable();
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
        enable();
        live_offset_mm(0.08);
        CHECK(state().save_withheld());
        CHECK_FALSE(helix::zoffset::current_save_availability().manual_save_supported);
        status(kPetgApplied);
        CHECK_FALSE(state().save_withheld());
    }
    SECTION("the macro ran without a material: DEFAULT") {
        discover_helix_macro();
        enable();
        status(json{{"gcode_macro _HELIX_MATERIAL", {{"applied", ""}, {"apply_count", 2}}}});
        auto target = state().save_target();
        REQUIRE(target.has_value());
        CHECK(target->material == "DEFAULT");
    }
    SECTION("without [save_variables] the feature waits for setup") {
        discover({"HELIX_MATERIAL_OFFSET"}, false);
        enable();
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
    enable();
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
    enable();
    auto offer = state().mesh_offer(materials, petg_bed);
    REQUIRE(offer.has_value());
    CHECK(offer->first == "PETG");
    CHECK(offer->second == "helix_petg");

    // The vendor start macro keeps no mesh per material.
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    CHECK_FALSE(state().mesh_offer(materials, petg_bed).has_value());
}

TEST_CASE("material zoffset: where a Save goes", "[material_zoffset][zoffset]") {
    using helix::zoffset::save_route;
    using R = helix::zoffset::SaveRoute;
    //                 available  per-material  macro applies  motion blocked
    CHECK(save_route(false, true, true, true) == R::None);
    CHECK(save_route(true, true, true, true) == R::PerMaterial);
    CHECK(save_route(true, true, false, false) == R::PerMaterial);
    CHECK(save_route(true, false, true, false) == R::MacroOwnsOffset);
    CHECK(save_route(true, false, true, true) == R::MacroOwnsOffset);
    CHECK(save_route(true, false, false, false) == R::MachineWide);
    CHECK(save_route(true, false, false, true) == R::Refused);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a Save while a print holds the machine",
                 "[material_zoffset][zoffset]") {
    helix::zoffset::reset_shared_save();
    helix::IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    discover_helix_macro();
    status(kPetgApplied);
    live_offset_mm(0.08);
    lv_subject_t* blocked = get_printer_state().print_state().get_machine_motion_blocked_subject();
    lv_subject_set_int(blocked, 1);
    lv_subject_t* guard = helix::zoffset::get_save_motion_blocked_subject();
    REQUIRE(guard != nullptr);

    SECTION("per material: allowed, one SAVE_VARIABLE, nothing that moves or restarts") {
        enable();
        REQUIRE(helix::zoffset::save_available());
        CHECK(lv_subject_get_int(guard) == 0);
        helix::zoffset::save_dirty_offsets_shared();
        process_lvgl(50);
        helix::test::answer_top_modal("btn_primary");
        REQUIRE(wait_until([&] { return !api.sent.empty(); }, 5000));
        CHECK(api.sent ==
              std::vector<std::string>{"SAVE_VARIABLE VARIABLE=helix_zoffset_petg VALUE=0.080"});
        CHECK_FALSE(sent_anything_that_folds_or_restarts());
    }
    SECTION("per material: the home tile's Save goes through too") {
        enable();
        helix::PanelWidgetManager::instance().init_widget_subjects();
        helix::PanelWidgetHarness<helix::ZOffsetWidget> tile(test_screen(), get_printer_state());
        tile.resize(1, 1, helix::test::kFull.w, helix::test::kFull.h);
        lv_obj_t* save_btn = tile.child("z_offset_save_full");
        REQUIRE(save_btn != nullptr);
        REQUIRE_FALSE(lv_obj_is_hidden(save_btn));
        lv_obj_send_event(save_btn, LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        helix::test::answer_top_modal("btn_primary");
        REQUIRE(wait_until([&] { return !api.sent.empty(); }, 5000));
        CHECK(api.sent ==
              std::vector<std::string>{"SAVE_VARIABLE VARIABLE=helix_zoffset_petg VALUE=0.080"});
    }
    SECTION("machine-wide: a caller that skips the route is refused as well") {
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 0}}}}}});
        REQUIRE_FALSE(state().start_macro_applies());
        std::string error;
        helix::zoffset::save_dirty_offsets(
            &api, watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE,
            get_printer_state().get_discovery(), true, []() {},
            [&error](const std::string& e) { error = e; }, &get_printer_state());
        process_lvgl(50);
        CHECK(error.find("after the print") != std::string::npos);
        CHECK(api.sent.empty());
    }
    SECTION("machine-wide: the guard holds and a tap that gets through is refused") {
        // The macro switched off: nothing owns the offset but the probe.
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 0}}}}}});
        REQUIRE_FALSE(state().start_macro_applies());
        REQUIRE(helix::zoffset::save_available());
        CHECK(lv_subject_get_int(guard) == 1);
        helix::zoffset::save_dirty_offsets_shared();
        process_lvgl(50);
        // Not even the restart confirmation: the save never starts.
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK(api.sent.empty());
    }
    lv_subject_set_int(blocked, 0);
    helix::zoffset::reset_shared_save();
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: no fold into the probe while the start macro applies "
                 "offsets",
                 "[material_zoffset][zoffset]") {
    helix::zoffset::reset_shared_save();
    helix::IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    auto tap_save = [this]() {
        helix::zoffset::save_dirty_offsets_shared();
        process_lvgl(50);
    };
    // The tool-offset panel's path: a caller asking to save the global offset.
    auto save_global_directly = [this]() {
        bool done = false;
        helix::zoffset::save_dirty_offsets(
            &api, watch, ZOffsetCalibrationStrategy::PROBE_CALIBRATE,
            get_printer_state().get_discovery(), true, [&done]() { done = true; },
            [&done](const std::string&) { done = true; }, &get_printer_state());
        process_lvgl(50);
        return done;
    };

    SECTION("vendor macro, setting off") {
        discover({"START_PRINT", "_START_PRINT_VARS"}, false);
        status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.08}}}});
        live_offset_mm(0.09);
        REQUIRE(helix::zoffset::save_available());
        CHECK_FALSE(helix::zoffset::folds_global(helix::zoffset::current_save_availability()));
        tap_save();
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK(save_global_directly());
        CHECK_FALSE(sent_anything_that_folds_or_restarts());
    }
    SECTION("HelixScreen's macro switched on, setting off") {
        discover_helix_macro();
        status(kPetgApplied);
        live_offset_mm(0.09);
        REQUIRE(state().start_macro_applies());
        REQUIRE(helix::zoffset::save_available());
        tap_save();
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK(save_global_directly());
        CHECK_FALSE(sent_anything_that_folds_or_restarts());
    }
    SECTION("HelixScreen's macro switched off: the machine-wide save is offered as before") {
        discover_helix_macro();
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 0}}}}}});
        live_offset_mm(0.09);
        REQUIRE(helix::zoffset::save_available());
        CHECK(helix::zoffset::folds_global(helix::zoffset::current_save_availability()));
        tap_save();
        CHECK(ModalStack::instance().top_dialog() != nullptr); // the restart confirmation
        helix::test::dismiss_top_modal();
    }
    helix::zoffset::reset_shared_save();
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: the setting switches HelixScreen's macro",
                 "[material_zoffset]") {
    discover_helix_macro();
    status(kPetgApplied);
    state().set_enabled(&api, true);
    CHECK(api.sent.back() == "SAVE_VARIABLE VARIABLE=helix_material_offset_enabled VALUE=1");
    CHECK(state().active());
    state().set_enabled(&api, false);
    CHECK(api.sent.back() == "SAVE_VARIABLE VARIABLE=helix_material_offset_enabled VALUE=0");

    SECTION("off in this screen but on on the printer: reconciled off") {
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 1}}}}}});
        REQUIRE(state().switch_disagrees());
        api.sent.clear();
        state().reconcile(&api);
        CHECK(api.sent == std::vector<std::string>{
                              "SAVE_VARIABLE VARIABLE=helix_material_offset_enabled VALUE=0"});
    }
    SECTION("on in this screen but off on the printer: not active, and reconciled") {
        state().set_enabled(nullptr, true);
        status(json{{"save_variables", {{"variables", {{"helix_material_offset_enabled", 0}}}}}});
        CHECK_FALSE(state().active());
        api.sent.clear();
        state().reconcile(&api);
        CHECK(api.sent == std::vector<std::string>{
                              "SAVE_VARIABLE VARIABLE=helix_material_offset_enabled VALUE=1"});
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: every screen start passes or clears the hint",
                 "[material_zoffset]") {
    discover_helix_macro();
    status(kPetgApplied);
    const std::string cleared =
        "SET_GCODE_VARIABLE MACRO=_HELIX_MATERIAL VARIABLE=next VALUE='\"\"'\n"
        "SET_GCODE_VARIABLE MACRO=_HELIX_MATERIAL VARIABLE=next_file VALUE='\"\"'";
    SECTION("feature off: cleared, so an old hint cannot apply") {
        state().note_screen_start(&api, "PETG", "a.gcode");
        CHECK(api.sent == std::vector<std::string>{cleared});
    }
    SECTION("feature on, no material: cleared") {
        enable();
        state().note_screen_start(&api, "", "a.gcode");
        CHECK(api.sent == std::vector<std::string>{cleared});
    }
    SECTION("feature on: the material, for this file only") {
        enable();
        state().note_screen_start(&api, "PETG", "dir/a.gcode");
        REQUIRE(api.sent.size() == 1);
        CHECK(api.sent.front().find("VALUE='\"PETG\"'") != std::string::npos);
        CHECK(api.sent.front().find("VARIABLE=next_file VALUE='\"dir/a.gcode\"'") !=
              std::string::npos);
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a file search cut short by the cap refuses",
                 "[material_zoffset]") {
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    enable();
    status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.02}}}});
    state().note_print_file("PETG");
    std::map<std::string, std::string> files;
    std::string printer_cfg;
    for (int i = 0; i < 70; ++i) {
        const std::string name = "inc" + std::to_string(i) + ".cfg";
        printer_cfg += "[include " + name + "]\n";
        files[name] = "[gcode_macro M" + std::to_string(i) + "]\ngcode:\n";
    }
    // The one definition sits in the first file read; the rest go unread.
    files["inc0.cfg"] = "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PETG: 0.02\n";
    files["printer.cfg"] = printer_cfg;
    api.set_config_files(files);

    auto [ok, message] = save("PETG", 80);
    CHECK_FALSE(ok);
    CHECK(message.find("more than") != std::string::npos);
    CHECK(api.get_uploaded_config("inc0.cfg") == files["inc0.cfg"]);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a per-material Save leaves dirty tool offsets alone",
                 "[material_zoffset][zoffset]") {
    // Saving a tool offset on klipper-toolchanger ends in SAVE_CONFIG, which the
    // per-material Save promises never to send; tools keep their own Save.
    helix::zoffset::reset_shared_save();
    helix::IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    json objects =
        json::array({"gcode_move", "toolhead", "bed_mesh", "save_variables", "toolchanger",
                     "tool T0", "tool T1", "extruder", "extruder1",
                     "gcode_macro HELIX_MATERIAL_OFFSET", "gcode_macro _HELIX_MATERIAL"});
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    get_printer_state().set_hardware(hw);
    state().on_discovery(get_printer_state().get_discovery());
    helix::ToolState::instance().init_tools(get_printer_state().get_discovery());
    helix::SettingsManager::instance().set_z_offset_save_mode(1);
    status(kPetgApplied);
    enable();
    live_offset_mm(0.08);
    helix::ToolState::instance().set_tool_offset_local(1, helix::Axis::Z, 60);
    REQUIRE(lv_subject_get_int(helix::ToolState::instance().get_any_tool_offset_dirty_subject()) ==
            1);
    REQUIRE(state().save_target().has_value());

    helix::zoffset::save_dirty_offsets_shared();
    process_lvgl(50);
    helix::test::answer_top_modal("btn_primary");
    REQUIRE(wait_until([&] { return !api.sent.empty(); }, 5000));
    process_lvgl(200);
    CHECK(api.sent.front() == "SAVE_VARIABLE VARIABLE=helix_zoffset_petg VALUE=0.080");
    CHECK_FALSE(sent_anything_that_folds_or_restarts());
    CHECK(api.sent.size() == 1);
    CHECK(lv_subject_get_int(helix::ToolState::instance().get_any_tool_offset_dirty_subject()) ==
          1);

    helix::SettingsManager::instance().set_z_offset_save_mode(0);
    helix::zoffset::reset_shared_save();
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: an oversized loaded file stops the search",
                 "[material_zoffset]") {
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    enable();
    status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.02}}}});
    state().note_print_file("PETG");
    const std::string vars = "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PETG: 0.02\n";
    // A literal include too large to read could hold a second definition.
    api.set_config_files({{"printer.cfg", "[include vars.cfg]\n[include huge.cfg]\n"},
                          {"vars.cfg", vars},
                          {"huge.cfg", std::string(1100 * 1024, '#')}});
    auto [ok, message] = save("PETG", 80);
    CHECK_FALSE(ok);
    CHECK(api.get_uploaded_config("vars.cfg") == vars);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: saves to one file never lose each other's line",
                 "[material_zoffset]") {
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    enable();
    status(json{{"gcode_macro _START_PRINT_VARS",
                 {{"offset_petg", 0.02}, {"offset_default", 0}, {"offset_pla", 0}}}});
    const std::string vars = "[gcode_macro _START_PRINT_VARS]\n"
                             "variable_offset_PLA: 0\n"
                             "variable_offset_PETG: 0.02\n"
                             "variable_offset_DEFAULT: 0\n";

    SECTION("a print Save and a Settings edit at once are written one after the other") {
        api.set_config_files(
            {{"printer.cfg", "[include overrides.cfg]\n"}, {"overrides.cfg", vars}});
        api.hold_acks = true; // both reach the file before either live write answers
        int finished = 0;
        state().save_offset("PETG", 80, [&](bool, const std::string&) { ++finished; });
        state().save_offset("", -30, [&](bool, const std::string&) { ++finished; });
        for (int i = 0; i < 50 && finished < 2; ++i) {
            api.ack_held();
            process_lvgl(20);
        }
        REQUIRE(finished == 2);
        const std::string after = api.get_uploaded_config("overrides.cfg").value_or("");
        CHECK(after.find("variable_offset_PETG: 0.080") != std::string::npos);
        CHECK(after.find("variable_offset_DEFAULT: -0.030") != std::string::npos);
    }
    SECTION("another writer's edit between the read and the upload is kept") {
        EditedBetweenReadsApi editing{client, get_printer_state()};
        editing.set_config_files(
            {{"printer.cfg", "[include overrides.cfg]\n"}, {"overrides.cfg", vars}});
        editing.edits->watched = "overrides.cfg";
        editing.edits->edit_after = 2; // the search reads it once, the edit reads it next
        editing.edits->edit = [&editing, vars]() {
            std::string changed = vars;
            changed.replace(changed.find("variable_offset_PLA: 0"), 22,
                            "variable_offset_PLA: 0.050");
            editing.set_config_files(
                {{"printer.cfg", "[include overrides.cfg]\n"}, {"overrides.cfg", changed}});
        };
        set_moonraker_api(&editing);
        bool finished = false;
        bool ok = false;
        state().save_offset("PETG", 80, [&](bool good, const std::string&) {
            finished = true;
            ok = good;
        });
        REQUIRE(wait_until([&] { return finished; }, 5000));
        CHECK(ok);
        const std::string after = editing.get_uploaded_config("overrides.cfg").value_or("");
        CHECK(after.find("variable_offset_PLA: 0.050") != std::string::npos);
        CHECK(after.find("variable_offset_PETG: 0.080") != std::string::npos);
        set_moonraker_api(&api);
    }
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "material zoffset state: a save in flight does not outlive the printer",
                 "[material_zoffset]") {
    helix::IMoonrakerAPI* previous = get_moonraker_api();
    set_moonraker_api(&api);
    const std::string vars = "[gcode_macro _START_PRINT_VARS]\nvariable_offset_PETG: 0.02\n";
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    enable();
    status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.02}}}});
    api.set_config_files({{"printer.cfg", "[include overrides.cfg]\n"}, {"overrides.cfg", vars}});
    api.hold_acks = true;
    bool old_done = false;
    state().save_offset("PETG", 80, [&](bool, const std::string&) { old_done = true; });
    REQUIRE(api.held_acks.size() == 1); // the save is in flight

    // The switch: the old printer's api goes away, then its answer arrives late.
    state().reset_printer_scope();
    set_moonraker_api(nullptr);
    api.ack_held();
    process_lvgl(100);
    CHECK_FALSE(old_done);
    CHECK(api.get_uploaded_config("overrides.cfg") == vars); // the late answer touched nothing

    // The next printer keeps its section elsewhere, and its saves still run.
    set_moonraker_api(&api);
    api.hold_acks = false;
    discover({"START_PRINT", "_START_PRINT_VARS"}, false);
    status(json{{"gcode_macro _START_PRINT_VARS", {{"offset_petg", 0.02}}}});
    api.set_config_files({{"printer.cfg", "[include other.cfg]\n"}, {"other.cfg", vars}});
    bool done = false;
    state().save_offset("PETG", 90, [&](bool, const std::string&) { done = true; });
    REQUIRE(wait_until([&] { return done; }, 3000));
    CHECK(api.get_uploaded_config("other.cfg").value_or("").find("variable_offset_PETG: 0.090") !=
          std::string::npos);
    set_moonraker_api(previous);
}

TEST_CASE_METHOD(MaterialStateFixture,
                 "the shared Save confirms a restart only when the save will restart Klipper",
                 "[zoffset][save_watch]") {
    helix::zoffset::reset_shared_save();
    // Each tool keeps its own Z here, so the shared Save shows only when asked for.
    helix::SettingsManager::instance().set_z_offset_save_mode(
        static_cast<int>(helix::zoffset::SaveMode::Show));
    // klipper-toolchanger: a tool's offset is staged, and SAVE_CONFIG commits it.
    auto discover_changer = [this](bool kalico) {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_move", "toolhead", "extruder", "extruder1",
                                      "toolchanger", "tool T0", "tool T1"}));
        if (kalico) {
            const json info{{"app", "Kalico"}, {"software_version", "v2025.1"}};
            hw.apply_printer_info(info);
            api.hardware().apply_printer_info(info);
        }
        get_printer_state().set_hardware(hw);
        auto& ts = helix::ToolState::instance();
        ts.init_tools(get_printer_state().get_discovery());
        ts.set_tool_offset_local(1, helix::Axis::Z, 100);
    };

    SECTION("mainline Klipper: the restart is confirmed before anything is sent") {
        discover_changer(false);
        REQUIRE(helix::zoffset::save_available());
        helix::zoffset::save_dirty_offsets_shared();
        process_lvgl(50);
        CHECK(ModalStack::instance().top_dialog() != nullptr);
        CHECK(api.sent.empty());
        helix::test::dismiss_top_modal();
    }
    SECTION("Kalico with nothing else pending: saved with RESTART=0 and no confirmation") {
        helix::ScopedEnv env{"HELIX_MOCK_KALICO", "1"};
        discover_changer(true);
        REQUIRE(helix::zoffset::save_available());
        helix::zoffset::save_dirty_offsets_shared();
        REQUIRE(wait_until([this] { return api.contains("SAVE_CONFIG"); }, 3000));
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK(api.contains("SAVE_CONFIG RESTART=0"));
    }
    helix::zoffset::reset_shared_save();
}
