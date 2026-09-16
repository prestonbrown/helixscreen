// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Mesh calibration over the mock client, through the real MoonrakerAdvancedAPI
// collector and BedMeshPanel's flow, judged by the gcode actually sent.

#include "ui_modal.h"
#include "ui_panel_bed_mesh.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bed_mesh_panel_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_advanced_api.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "temperature_controller.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using json = nlohmann::json;

namespace {

void drain() {
    for (int i = 0; i < 32; ++i) {
        helix::ui::UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    }
}

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Index of the first sent script containing @p needle, or -1.
long first_sent(const std::vector<std::string>& history, const char* needle) {
    const auto it = std::find_if(history.begin(), history.end(),
                                 [needle](const std::string& s) { return mentions(s, needle); });
    return it == history.end() ? -1 : static_cast<long>(it - history.begin());
}

bool any_sent(const std::vector<std::string>& history, const char* needle) {
    return first_sent(history, needle) >= 0;
}

/// Let the bed mesh slot resolve from macros detected among @p objects.
void detect_macros_in(const json& objects) {
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(objects);
    StandardMacros::instance().init(hardware, "");
}

class CalibrationCollectorFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    MoonrakerAdvancedAPI advanced{client, api};

    int completions = 0;
    std::vector<std::string> errors;
    std::vector<std::pair<int, int>> progress;

    CalibrationCollectorFixture() {
        state.init_subjects(false);
    }
    ~CalibrationCollectorFixture() override {
        drain();
    }

    /// A calibration whose RPC never returns, so only console lines can end it.
    /// A self-preparing sequence is always a shipped one.
    void start(bool shipped = true, bool self_prepares = true) {
        client.force_next_gcode_dropped_response("BED_MESH_CALIBRATE");
        advanced.start_bed_mesh_calibrate(
            {"BED_MESH_CALIBRATE BED_TEMP=60", shipped && self_prepares, shipped},
            [this](int current, int total) { progress.emplace_back(current, total); },
            [this]() { ++completions; },
            [this](const MoonrakerError& err) { errors.push_back(err.message); },
            /*expected_probes=*/81, /*probe_samples=*/1);
        REQUIRE(client.gcode_script_history().size() == 1);
        REQUIRE(completions == 0);
        REQUIRE(errors.empty());
    }
};

/// A TemperatureController the panel's heater sends reach, for as long as it lives.
class ScopedTemperatureController {
  public:
    ScopedTemperatureController(helix::PrinterState& state, IMoonrakerAPI* api)
        : controller_(state, api) {
        helix::PanelWidgetManager::instance().register_shared_resource(&controller_);
    }
    ~ScopedTemperatureController() {
        helix::PanelWidgetManager::instance().register_shared_resource(
            static_cast<helix::TemperatureController*>(nullptr));
    }

  private:
    helix::TemperatureController controller_;
};

class BedMeshPanelFlowFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};

    std::vector<std::string> errors;
    std::vector<std::string> successes;
    /// How many scripts had been sent when each success was reported.
    std::vector<size_t> sent_at_success;

    BedMeshPanelFlowFixture() {
        state.init_subjects(false);
        set_moonraker_api(&api);
        client.clear_gcode_script_history();
        // The panel reads bed temperatures and homing from the process-wide state,
        // whose subjects no base fixture creates.
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        get_printer_state().update_from_status({{"toolhead", {{"homed_axes", ""}}}});
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& m) { errors.push_back(m); });
        helix::ui::set_test_notification_success_hook([this](const std::string& m) {
            successes.push_back(m);
            sent_at_success.push_back(sent().size());
        });
    }
    ~BedMeshPanelFlowFixture() override {
        helix::ui::set_test_notification_error_hook(nullptr);
        helix::ui::set_test_notification_success_hook(nullptr);
        set_moonraker_api(nullptr);
        StandardMacros::instance().init(helix::PrinterDiscovery{}, "");
        drain();
    }

    static void use_printer(const std::string& printer_name) {
        StandardMacros::instance().init(helix::PrinterDiscovery{}, printer_name);
    }

    static void detect_macros(const json& objects) {
        detect_macros_in(objects);
    }

    static void set_bed(double temperature, double target) {
        get_printer_state().update_from_status(
            {{"heater_bed", {{"temperature", temperature}, {"target", target}}}});
    }

    /// Open the calibrate dialog, give the mesh a name, and let the flow run out.
    void calibrate_as(BedMeshPanel& panel, const char* name) {
        panel.start_calibration();
        drain();
        panel.submit_calibration_name(name);
        drain();
    }

    const std::vector<std::string>& sent() const {
        return client.gcode_script_history();
    }
};

} // namespace

// ============================================================================
// Collector: console lines that end a calibration
// ============================================================================

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "an unknown command in a shipped sequence fails the calibration",
                 "[bed_mesh_flow][cc1]") {
    start();
    client.dispatch_gcode_response("// probe at 9.998,9.998 is z=0.265669");
    REQUIRE(progress.size() == 1); // the collector is listening

    // Kalico's spelling: no space after the colon, the name quoted.
    client.dispatch_gcode_response("// Unknown command:\"LOAD_CELL_SAVE_TARE\"");
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "LOAD_CELL_SAVE_TARE"));
    CHECK(completions == 0);

    // A failed calibration stays failed.
    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    CHECK(completions == 0);
    CHECK(errors.size() == 1);
}

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "an unknown command fails a shipped sequence whoever does the preparing",
                 "[bed_mesh_flow]") {
    start(/*shipped=*/true, /*self_prepares=*/false);
    client.dispatch_gcode_response("// Unknown command:\"CLEAN_NOZZLE\"");
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "CLEAN_NOZZLE"));
    CHECK(completions == 0);
}

TEST_CASE_METHOD(
    CalibrationCollectorFixture,
    "an unknown command in any other calibration is logged, and the calibration runs on",
    "[bed_mesh_flow]") {
    start(/*shipped=*/false);
    client.dispatch_gcode_response("// Unknown command:\"CLEAN_NOZZLE\"");
    CHECK(errors.empty());
    CHECK(completions == 0);

    client.dispatch_gcode_response("// probe at 9.998,9.998 is z=0.265669");
    CHECK(progress.size() == 1); // still listening
    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    CHECK(completions == 1);
    CHECK(errors.empty());
}

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "an unknown BED_MESH_CALIBRATE still points at the missing [bed_mesh]",
                 "[bed_mesh_flow]") {
    SECTION("in a shipped sequence") {
        start(/*shipped=*/true);
    }
    SECTION("in any other command, since the calibration itself never ran") {
        start(/*shipped=*/false);
    }
    client.dispatch_gcode_response("// Unknown command:\"BED_MESH_CALIBRATE\"");
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "[bed_mesh]"));
}

TEST_CASE_METHOD(CalibrationCollectorFixture,
                 "a line that only mentions an unknown command does not fail calibration",
                 "[bed_mesh_flow]") {
    start();
    client.dispatch_gcode_response("// Skipping Unknown command:\"FOO\" in a narration line");
    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    CHECK(errors.empty());
    CHECK(completions == 1);
}

// ============================================================================
// Panel: the name comes first, and decides what is sent
// ============================================================================

// start_calibration() drives the panel's own calibrate-state and name
// subjects, which only init_subjects() creates: driving the state machine on
// an uninitialized panel notifies uninitialized subjects.
TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "start_calibration moves an initialized panel into naming", "[bed_mesh_flow]") {
    use_printer("");
    BedMeshPanel panel;
    panel.init_subjects();
    REQUIRE(helix::ui::BedMeshPanelTestAccess::calibrate_state_type(panel) ==
            static_cast<int>(LV_SUBJECT_TYPE_INT));

    panel.start_calibration();
    drain();

    CHECK(helix::ui::BedMeshPanelTestAccess::calibrate_state(panel) ==
          static_cast<int>(BedMeshCalibrationState::NAMING));
    CHECK(sent().empty());
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture, "the mesh is named before any calibration gcode is sent",
                 "[bed_mesh_flow]") {
    use_printer("");
    BedMeshPanel panel;
    panel.init_subjects();

    panel.start_calibration();
    drain();
    CHECK(sent().empty());

    panel.submit_calibration_name("default");
    drain();
    CHECK(any_sent(sent(), "BED_MESH_CALIBRATE"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a mesh kept as default is probed with no PROFILE and never saved to default",
                 "[bed_mesh_flow]") {
    use_printer("");
    set_bed(24.0, 0.0);
    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "default");

    const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
    REQUIRE(mesh >= 0);
    CHECK_FALSE(mentions(sent()[mesh], "PROFILE"));
    // Klipper stores it in default itself, and refuses a SAVE there.
    CHECK_FALSE(any_sent(sent(), "BED_MESH_PROFILE"));
    CHECK(errors.empty());
    REQUIRE(successes.size() == 1);
    CHECK(mentions(successes[0], "'default'"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture, "a named mesh is probed straight into that profile",
                 "[bed_mesh_flow]") {
    use_printer("");
    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "cold");

    const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
    REQUIRE(mesh >= 0);
    CHECK(mentions(sent()[mesh], "PROFILE=cold"));
    // Nothing to copy and nothing to clean up.
    CHECK_FALSE(any_sent(sent(), "BED_MESH_PROFILE"));
    CHECK(errors.empty());
    REQUIRE(successes.size() == 1);
    CHECK(mentions(successes[0], "'cold'"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a self-preparing mesh sequence is sent alone, at the bed temperature it needs",
                 "[bed_mesh_flow][cc1]") {
    use_printer("Elegoo Centauri Carbon");
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::BedMesh).get_source() ==
            MacroSource::SHIPPED);

    SECTION("a set bed target is the probe temperature") {
        set_bed(24.0, 70.0);
        REQUIRE(lv_subject_get_int(get_printer_state().get_bed_target_subject()) == 700);

        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");

        // No preheat wait and no G28 ahead of it: the sequence does both itself.
        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_CALIBRATE BED_TEMP=70");
    }

    SECTION("a target below a hot bed is not a temperature to cool to") {
        set_bed(100.4, 60.0);

        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");

        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_CALIBRATE BED_TEMP=100");
    }

    SECTION("an idle bed still hot is probed at the temperature it has") {
        set_bed(87.4, 0.0);
        REQUIRE(lv_subject_get_int(get_printer_state().get_bed_temp_subject()) == 874);

        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");

        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_CALIBRATE BED_TEMP=87");
    }

    SECTION("a named mesh reaches the firmware's macro as PROFILE") {
        set_bed(24.0, 0.0);

        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "cold");

        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_CALIBRATE PROFILE=cold BED_TEMP=60");
        CHECK(errors.empty());
        REQUIRE(successes.size() == 1);
        CHECK(mentions(successes[0], "'cold'"));
    }
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a self-preparing sequence turns off afterwards the heaters that were off",
                 "[bed_mesh_flow][cc1]") {
    use_printer("Elegoo Centauri Carbon");
    ScopedTemperatureController heaters(get_printer_state(), &api);
    get_printer_state().update_from_status(
        {{"extruder", {{"temperature", 25.0}, {"target", 0.0}}}});

    SECTION("bed and nozzle both off") {
        set_bed(24.0, 0.0);
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");

        // Nothing goes out ahead of the sequence: it heats the printer itself.
        REQUIRE(first_sent(sent(), "BED_MESH_CALIBRATE") == 0);
        CHECK(first_sent(sent(), "HEATER=extruder TARGET=0") > 0);
        CHECK(first_sent(sent(), "HEATER=heater_bed TARGET=0") > 0);
    }

    SECTION("a bed the user was heating stays on") {
        set_bed(24.0, 70.0);
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");

        REQUIRE(first_sent(sent(), "BED_MESH_CALIBRATE") == 0);
        CHECK(first_sent(sent(), "HEATER=extruder TARGET=0") > 0);
        CHECK_FALSE(any_sent(sent(), "HEATER=heater_bed TARGET=0"));
    }
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a plain BED_MESH_CALIBRATE still gets the panel's preheat and homing",
                 "[bed_mesh_flow]") {
    use_printer("");
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::BedMesh).get_source() !=
            MacroSource::SHIPPED);
    set_bed(24.0, 0.0);

    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "default");

    const long wait = first_sent(sent(), "TEMPERATURE_WAIT");
    const long home = first_sent(sent(), "G28");
    const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
    REQUIRE(mesh >= 0);
    CHECK(wait >= 0);
    CHECK(home >= 0);
    CHECK(wait < home);
    CHECK(home < mesh);
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a command that cannot be named is copied into the chosen profile afterwards",
                 "[bed_mesh_flow]") {
    detect_macros({"gcode_macro G29"});
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::BedMesh).get_macro() == "G29");
    client.force_next_mesh_calibration("G29", "default");

    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "cold");

    const long mesh = first_sent(sent(), "G29");
    const long load = first_sent(sent(), "BED_MESH_PROFILE LOAD=default");
    const long save = first_sent(sent(), "BED_MESH_PROFILE SAVE=cold");
    REQUIRE(mesh >= 0);
    REQUIRE(load >= 0);
    REQUIRE(save >= 0);
    CHECK(mesh < load);
    CHECK(load < save);
    CHECK_FALSE(any_sent(sent(), "REMOVE"));
    CHECK(errors.empty());
    REQUIRE(successes.size() == 1);
    CHECK(mentions(successes[0], "'cold'"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a refused save is an error, and the only copy of the mesh is not removed",
                 "[bed_mesh_flow]") {
    detect_macros({"gcode_macro G29"});
    client.force_next_mesh_calibration("G29", "default");
    client.force_next_gcode_console_reply(
        "BED_MESH_PROFILE SAVE=",
        "// Unable to save to profile [cold], the bed has not been probed");

    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "cold");

    REQUIRE(any_sent(sent(), "BED_MESH_PROFILE SAVE=cold")); // the save was attempted
    CHECK_FALSE(any_sent(sent(), "REMOVE"));
    CHECK(successes.empty());
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "cold"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a BED_MESH_CALIBRATE that drops its PROFILE is copied from default",
                 "[bed_mesh_flow]") {
    detect_macros({"gcode_macro BED_MESH_CALIBRATE"});
    client.force_next_mesh_calibration("BED_MESH_CALIBRATE", "default");

    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "cold");

    const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
    REQUIRE(mesh >= 0);
    REQUIRE(mentions(sent()[mesh], "PROFILE=cold"));
    const long load = first_sent(sent(), "BED_MESH_PROFILE LOAD=default");
    const long save = first_sent(sent(), "BED_MESH_PROFILE SAVE=cold");
    CHECK(mesh < load);
    CHECK(load < save);
    CHECK(errors.empty());
    REQUIRE(successes.size() == 1);
    CHECK(mentions(successes[0], "'cold'"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a calibration that stored no new mesh is an error, never a success",
                 "[bed_mesh_flow]") {
    SECTION("the copy plan, whose default did not change") {
        detect_macros({"gcode_macro G29"});
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "cold");
        REQUIRE(any_sent(sent(), "G29"));
        // The default already there is not this calibration's mesh.
        CHECK_FALSE(any_sent(sent(), "BED_MESH_PROFILE"));
    }

    SECTION("stored under a profile nobody chose") {
        detect_macros({"gcode_macro BED_MESH_CALIBRATE"});
        client.force_next_mesh_calibration("BED_MESH_CALIBRATE", "adaptive");
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "cold");
        REQUIRE(any_sent(sent(), "BED_MESH_CALIBRATE"));
        CHECK_FALSE(any_sent(sent(), "BED_MESH_PROFILE"));
    }

    CHECK(successes.empty());
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "cold"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a Complete line while the calibration still runs starts the copy, and the "
                 "SAVE_CONFIG prompt follows it",
                 "[bed_mesh_flow]") {
    detect_macros({"gcode_macro G29"});
    client.force_next_mesh_calibration("G29", "default");
    // The command keeps running after its console reports the mesh, as a macro
    // that goes on to measure a Z offset does.
    client.force_next_gcode_dropped_response("G29");

    BedMeshPanel panel;
    panel.init_subjects();
    calibrate_as(panel, "cold");
    const long mesh = first_sent(sent(), "G29");
    REQUIRE(mesh >= 0);
    REQUIRE_FALSE(any_sent(sent(), "BED_MESH_PROFILE"));
    REQUIRE(successes.empty());

    client.dispatch_gcode_response("// Mesh Bed Leveling Complete");
    drain();

    const long load = first_sent(sent(), "BED_MESH_PROFILE LOAD=default");
    const long save = first_sent(sent(), "BED_MESH_PROFILE SAVE=cold");
    CHECK(mesh < load);
    CHECK(load < save);
    REQUIRE(successes.size() == 1);
    CHECK(mentions(successes[0], "'cold'"));
    // Reported, with the prompt that follows, only once the copy is stored...
    CHECK(sent_at_success[0] > static_cast<size_t>(save));
    // ...and SAVE_CONFIG waits for the user's answer.
    CHECK_FALSE(any_sent(sent(), "SAVE_CONFIG"));
    CHECK(errors.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock printer's calibration runs the command it was given",
                 "[bed_mesh_flow]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    bool done = false;
    api.advanced().start_bed_mesh_calibrate(
        {"BED_MESH_CALIBRATE PROFILE=cold", /*self_prepares=*/false}, nullptr,
        [&done]() { done = true; }, nullptr, /*expected_probes=*/0, /*probe_samples=*/1);
    REQUIRE(wait_until([&done]() { return done; }, 20000));

    CHECK(any_sent(client.gcode_script_history(), "BED_MESH_CALIBRATE PROFILE=cold"));
    const auto& profiles = client.get_bed_mesh_profiles();
    CHECK(std::find(profiles.begin(), profiles.end(), "cold") != profiles.end());
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "only a shipped sequence is failed by a command the printer does not define",
                 "[bed_mesh_flow][cc1]") {
    SECTION("the Centauri Carbon's shipped sequence") {
        use_printer("Elegoo Centauri Carbon");
        client.force_next_gcode_console_reply("BED_MESH_CALIBRATE",
                                              "// Unknown command:\"CLEAN_NOZZLE\"");
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");
        REQUIRE(any_sent(sent(), "BED_MESH_CALIBRATE"));
        CHECK(successes.empty());
    }

    SECTION("a detected BED_MESH_CALIBRATE") {
        detect_macros({"gcode_macro BED_MESH_CALIBRATE"});
        client.force_next_gcode_console_reply("BED_MESH_CALIBRATE",
                                              "// Unknown command:\"CLEAN_NOZZLE\"");
        BedMeshPanel panel;
        panel.init_subjects();
        calibrate_as(panel, "default");
        REQUIRE(any_sent(sent(), "BED_MESH_CALIBRATE"));
        CHECK(errors.empty());
        REQUIRE(successes.size() == 1);
        CHECK(mentions(successes[0], "'default'"));
    }
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture, "loading and deleting a profile quote its name",
                 "[bed_mesh_flow]") {
    BedMeshPanel panel;
    panel.init_subjects();

    SECTION("load") {
        helix::ui::BedMeshPanelTestAccess::set_profile_name(panel, 0, "PEI Sheet");
        panel.load_profile(0);
        drain();
        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_PROFILE LOAD=\"PEI Sheet\"");
    }

    SECTION("delete") {
        panel.show_delete_confirm_modal("PEI Sheet");
        panel.confirm_delete_profile();
        drain();
        REQUIRE(sent().size() == 1);
        CHECK(sent()[0] == "BED_MESH_PROFILE REMOVE=\"PEI Sheet\"");
    }
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a name Klipper would cut short is refused before probing", "[bed_mesh_flow]") {
    BedMeshPanel panel;
    panel.init_subjects();
    panel.start_calibration();
    drain();
    panel.submit_calibration_name("cold;hot");
    drain();
    CHECK(sent().empty());
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "renaming a profile to default is refused before anything is sent",
                 "[bed_mesh_flow]") {
    BedMeshPanel panel;
    panel.init_subjects();
    panel.show_rename_modal("cold");
    panel.rename_profile_checked("default");
    drain();

    CHECK(sent().empty());
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "default"));
}

TEST_CASE_METHOD(BedMeshPanelFlowFixture,
                 "a rename whose save is refused keeps the original profile", "[bed_mesh_flow]") {
    client.force_next_gcode_console_reply(
        "BED_MESH_PROFILE SAVE=",
        "// Unable to save to profile [warm], the bed has not been probed");

    BedMeshPanel panel;
    panel.init_subjects();
    panel.show_rename_modal("cold");
    panel.rename_profile_checked("warm");
    drain();

    REQUIRE(any_sent(sent(), "BED_MESH_PROFILE SAVE=warm")); // the save was attempted
    CHECK_FALSE(any_sent(sent(), "REMOVE"));
    REQUIRE(errors.size() == 1);
    CHECK(mentions(errors[0], "warm"));
}

// ============================================================================
// Dialogs: the real calibrate and confirmation modals, built from their XML
// ============================================================================

namespace {

class BedMeshDialogFixture : public LVGLUITestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    BedMeshPanel panel;

    BedMeshDialogFixture() {
        set_moonraker_api(&api);
        StandardMacros::instance().init(helix::PrinterDiscovery{}, "");
        // The dialog binds the panel's calibration subjects by name.
        panel.init_subjects();
        client.clear_gcode_script_history();
    }
    ~BedMeshDialogFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            settle();
        }
        settle();
        set_moonraker_api(nullptr);
    }

    /// Let a close finish: the deferred widget delete and any dismissal report.
    void settle() {
        drain();
        process_lvgl(50);
        drain();
    }

    /// The printer stores default and adaptive, as the mock starts, plus @p extra.
    void store_profiles_on_printer(std::initializer_list<const char*> extra) {
        api.execute_gcode("BED_MESH_PROFILE LOAD=default", nullptr, nullptr);
        for (const char* name : extra) {
            api.execute_gcode(std::string("BED_MESH_PROFILE SAVE=") + name, nullptr, nullptr);
        }
        settle();
        client.clear_gcode_script_history();
        const auto stored = api.advanced().get_bed_mesh_profiles();
        REQUIRE(std::find(stored.begin(), stored.end(), "default") != stored.end());
    }

    static void tap_backdrop(lv_obj_t* dialog) {
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);
        lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
    }

    static void press_esc(lv_obj_t* dialog) {
        lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
        REQUIRE(backdrop != nullptr);
        uint32_t key = LV_KEY_ESC;
        lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    }

    static void click(lv_obj_t* dialog, const char* button) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    }

    /// The calibrate dialog's profile name field.
    static lv_obj_t* name_field(lv_obj_t* dialog) {
        REQUIRE(dialog != nullptr);
        lv_obj_t* field = lv_obj_find_by_name(dialog, "calibrate_profile_name_input");
        REQUIRE(field != nullptr);
        return field;
    }

    const std::vector<std::string>& sent() const {
        return client.gcode_script_history();
    }
};

} // namespace

TEST_CASE_METHOD(BedMeshDialogFixture, "dismissing the naming dialog leaves Probe working",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    panel.start_calibration();
    settle();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    SECTION("by a backdrop tap") {
        tap_backdrop(dialog);
    }
    SECTION("by ESC") {
        press_esc(dialog);
    }
    settle();
    REQUIRE(Modal::get_top() == nullptr);

    panel.start_calibration();
    settle();
    CHECK(Modal::get_top() != nullptr);
    CHECK(sent().empty());
}

TEST_CASE_METHOD(BedMeshDialogFixture,
                 "the name typed into the dialog is the profile the mesh is probed into",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    panel.start_calibration();
    settle();
    lv_obj_t* field = name_field(Modal::get_top());
    CHECK(std::string(lv_textarea_get_text(field)) == "default");

    lv_textarea_set_text(field, "cold");
    panel.submit_calibration_name_field();
    settle();

    const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
    REQUIRE(mesh >= 0);
    CHECK(mentions(sent()[mesh], "PROFILE=cold"));
}

TEST_CASE_METHOD(BedMeshDialogFixture, "the naming dialog opens on default every time",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    panel.start_calibration();
    settle();
    lv_textarea_set_text(name_field(Modal::get_top()), "cold");

    // Its Cancel button.
    panel.hide_all_modals();
    settle();
    REQUIRE(Modal::get_top() == nullptr);

    panel.start_calibration();
    settle();
    CHECK(std::string(lv_textarea_get_text(name_field(Modal::get_top()))) == "default");
}

TEST_CASE_METHOD(BedMeshDialogFixture, "keeping the name default replaces that mesh without asking",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    store_profiles_on_printer({});
    panel.start_calibration();
    settle();
    lv_obj_t* naming = Modal::get_top();

    panel.submit_calibration_name("default");
    settle();

    CHECK(any_sent(sent(), "BED_MESH_CALIBRATE"));
    CHECK(Modal::get_top() != naming); // no Replace dialog in its place
}

TEST_CASE_METHOD(BedMeshDialogFixture, "replacing any other stored profile asks first",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    store_profiles_on_printer({"cold"});
    panel.start_calibration();
    settle();
    lv_obj_t* naming = Modal::get_top();
    REQUIRE(naming != nullptr);

    panel.submit_calibration_name("cold");
    settle();
    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm != naming);
    REQUIRE_FALSE(any_sent(sent(), "BED_MESH_CALIBRATE"));

    SECTION("Replace probes into it") {
        click(confirm, "btn_primary");
        settle();
        const long mesh = first_sent(sent(), "BED_MESH_CALIBRATE");
        REQUIRE(mesh >= 0);
        CHECK(mentions(sent()[mesh], "PROFILE=cold"));
    }

    SECTION("Cancel goes back to the name and forgets the replacement") {
        click(confirm, "btn_secondary");
        settle();
        CHECK(Modal::get_top() == naming);
        // Nothing is left pending for a later answer to act on.
        panel.confirm_overwrite();
        settle();
        CHECK_FALSE(any_sent(sent(), "BED_MESH_CALIBRATE"));
    }

    SECTION("a dismissal is a Cancel") {
        tap_backdrop(confirm);
        settle();
        CHECK(Modal::get_top() == naming);
        panel.confirm_overwrite();
        settle();
        CHECK_FALSE(any_sent(sent(), "BED_MESH_CALIBRATE"));
    }
}

namespace {

/// Whether any label under @p obj shows text containing @p needle.
bool shows_text(lv_obj_t* obj, const char* needle) {
    if (lv_obj_check_type(obj, &lv_label_class) &&
        std::string(lv_label_get_text(obj)).find(needle) != std::string::npos) {
        return true;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (shows_text(lv_obj_get_child(obj, static_cast<int32_t>(i)), needle)) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE_METHOD(BedMeshDialogFixture,
                 "a command that stores in default first asks before replacing default",
                 "[bed_mesh_flow][bed_mesh_dialog]") {
    store_profiles_on_printer({"cold"});
    detect_macros_in({"gcode_macro G29"});
    panel.start_calibration();
    settle();
    lv_obj_t* naming = Modal::get_top();
    REQUIRE(naming != nullptr);

    SECTION("under a new name") {
        panel.submit_calibration_name("warm");
        settle();
        lv_obj_t* confirm = Modal::get_top();
        REQUIRE(confirm != naming);
        CHECK(shows_text(confirm, "'default'"));
        CHECK_FALSE(any_sent(sent(), "G29"));

        click(confirm, "btn_primary");
        settle();
        CHECK(any_sent(sent(), "G29"));
    }

    SECTION("under a stored name, both in one question") {
        panel.submit_calibration_name("cold");
        settle();
        lv_obj_t* confirm = Modal::get_top();
        REQUIRE(confirm != naming);
        CHECK(shows_text(confirm, "'cold'"));
        CHECK(shows_text(confirm, "'default'"));
        CHECK_FALSE(any_sent(sent(), "G29"));
    }

    SECTION("as default itself") {
        panel.submit_calibration_name("default");
        settle();
        CHECK(Modal::get_top() != naming);
        CHECK(any_sent(sent(), "G29"));
    }
}
