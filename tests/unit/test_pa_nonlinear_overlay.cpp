// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_component_keypad.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_pa_nonlinear.h"
#include "ui_overlay_pa_tuning.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <array>
#include <initializer_list>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using helix::ui::PaNonlinearOverlay;
using nlohmann::json;

namespace {

/// The procedure saved under the printer's settings, gone.
void clear_saved_session() {
    auto* cfg = Config::get_instance();
    if (!cfg->exists(cfg->df() + "pa_nonlinear_session")) {
        return;
    }
    std::string printer = cfg->df();
    printer.pop_back();
    cfg->get_json(printer).erase("pa_nonlinear_session");
}

bool session_saved() {
    auto* cfg = Config::get_instance();
    return cfg->exists(cfg->df() + "pa_nonlinear_session");
}

// LVGLUITestFixture: the confirm dialogs and the keypad are XML components.
struct NonlinearFixture : LVGLUITestFixture {
    // Firmware with selectable models, and its tower module unless a case
    // says otherwise. Declared before the client, which reads them.
    helix::ScopedEnv pa_model_env;
    helix::ScopedEnv pa_test_env;
    MoonrakerClientMock client;
    MoonrakerAPI api;
    PaNonlinearOverlay overlay;

    explicit NonlinearFixture(bool tower = true)
        : pa_model_env("HELIX_MOCK_PA_MODEL", "linear"),
          pa_test_env("HELIX_MOCK_PA_TEST", tower ? "1" : "0"), api(client, get_printer_state()) {
        auto& state = get_printer_state();
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        PrinterDiscovery hw;
        json objects = json::array({"extruder", "heater_bed", "print_stats"});
        if (tower) {
            objects.push_back("pa_test");
        }
        hw.parse_objects(objects);
        state.set_hardware(hw);
        state.temperature_state().init_extruders({"extruder"});
        client.connect("ws://mock/websocket", []() {}, []() {});
        overlay.init_subjects();
        overlay.set_api(&api);
        overlay.set_raise_on_start_for_test(false);
        status({{"extruder", {{"pressure_advance_model", "linear"}, {"pressure_advance", 0.04}}}});
        clear_saved_session();
    }
    ~NonlinearFixture() override {
        clear_saved_session();
        overlay.cleanup();
        status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});
        client.disconnect();
    }

    void status(const json& s) {
        get_printer_state().update_from_status(s);
        helix::ui::UpdateQueue::instance().drain();
    }

    pa::TestPrintParams params() const {
        pa::TestPrintParams p;
        p.heater = "extruder";
        p.nozzle_temp = 215;
        p.bed_temp = 60;
        p.nozzle_diameter_mm = 0.4f;
        p.material = "PLA";
        return p;
    }

    void tower(const char* state) {
        status({{"print_stats",
                 {{"state", state}, {"filename", PaNonlinearOverlay::kTowerPrintName}}}});
    }

    /// Print the current pass and let the tower end in @p end.
    void print_tower(const char* end = "complete") {
        overlay.print_pass_for_test();
        helix::ui::UpdateQueue::instance().drain();
        tower("printing");
        tower(end);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Press @p button ("btn_primary" / "btn_secondary") on the dialog on top.
    static void press_dialog(const char* button) {
        lv_obj_t* dialog = helix::ui::modal_get_top();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The scripts sent since @p from, joined.
    std::string sent_since(size_t from) const {
        const auto& h = client.gcode_script_history();
        std::string out;
        for (size_t i = from; i < h.size(); ++i) {
            out += h[i] + "\n";
        }
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a pass readies the machine, switches the model and starts the "
                 "firmware tower",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    REQUIRE(overlay.stage() == PaNonlinearOverlay::READY);
    const size_t before = client.gcode_script_history().size();
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);

    const std::string sent = sent_since(before);
    INFO(sent);
    const auto at = [&sent](const char* needle) { return sent.find(needle); };
    // Heated to exactly the target the tower checks, then the model, then the
    // sweep, and the tower last: PRINT_PA_TOWER returns as soon as it starts.
    REQUIRE(at("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") != std::string::npos);
    REQUIRE(at("MODEL=tanh") != std::string::npos);
    REQUIRE(at("PARAMETER=OFFSET") != std::string::npos);
    REQUIRE(at("PRINT_PA_TOWER NOZZLE=0.40 TARGET_TEMP=215") != std::string::npos);
    CHECK(at("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215") < at("MODEL=tanh"));
    CHECK(at("MODEL=tanh") < at("PARAMETER=OFFSET"));
    CHECK(at("PARAMETER=OFFSET") < at("PRINT_PA_TOWER"));
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the tower's own print drives the stages and the judgement "
                 "advances the procedure",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();

    // Another file printing is not the tower.
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    status({{"print_stats", {{"state", "standby"}, {"filename", ""}}}});

    tower("printing");
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PRINTING);
    const size_t before_end = client.gcode_script_history().size();
    tower("complete");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    // The sweep's last value is replaced by the procedure's before the machine
    // is released.
    const std::string ending = sent_since(before_end);
    INFO(ending);
    CHECK(ending.find("MODEL=tanh") != std::string::npos);
    // The firmware's sweep outlives the tower; the ending replaces it.
    CHECK(ending.find("TUNING_TOWER COMMAND=G4") != std::string::npos);
    CHECK(ending.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0") != std::string::npos);

    // Pass 1 judges the left side alone: the height is the answer.
    overlay.adjust_height_for_test(50); // 25 -> 30 mm
    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    const auto* proc = overlay.procedure_for_test();
    REQUIRE(proc != nullptr);
    CHECK(proc->values().nonlinear_offset == Catch::Approx(0.30f));
    CHECK(proc->pass_number() == 2);
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(client.last_send_script().find("OFFSET=0.300000") != std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: a tower that errors goes back to the pass",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    tower("printing");
    tower("error");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    REQUIRE(overlay.procedure_for_test() != nullptr);
    CHECK(overlay.procedure_for_test()->pass_number() == 1);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: discard puts the linear K back",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.discard_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(client.last_send_script() ==
          "TUNING_TOWER COMMAND=G4 PARAMETER=P START=0 FACTOR=0\n"
          "SET_PRESSURE_ADVANCE EXTRUDER=extruder MODEL=linear ADVANCE=0.040000");
    CHECK_FALSE(overlay.session_active());
}

TEST_CASE("nonlinear tuner: without the firmware tower a pass does not start",
          "[pa_nonlinear][pa_tuner]") {
    NonlinearFixture fx(false);
    fx.overlay.start_for_test(fx.params(), pa::ExtruderKind::DirectDrive);
    const size_t before = fx.client.gcode_script_history().size();
    fx.overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(fx.client.gcode_script_history().size() == before);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "pa_nl_has_tower")) == 0);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a tower the firmware refuses after the command returns "
                 "leaves the heating screen",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    // The refusal arrives as an error in one frame, with no printing before it.
    const size_t before = client.gcode_script_history().size();
    tower("error");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(sent_since(before).find("TUNING_TOWER COMMAND=G4") != std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the reset before the next tower starts is not its end",
                 "[pa_nonlinear][pa_tuner]") {
    // The previous pass's tower is still the printer's last job.
    tower("complete");
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    const size_t before = client.gcode_script_history().size();
    // Loading the tower resets print_stats to standby before it prints.
    tower("standby");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(sent_since(before).find("M84") == std::string::npos);
    tower("printing");
    CHECK(overlay.stage() == PaNonlinearOverlay::PRINTING);
    // A standby once it has printed (a restart mid-tower) still ends the pass.
    tower("standby");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a tower that never starts gives up after the timeout",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    process_lvgl(PaNonlinearOverlay::kTowerStartTimeoutMs - 1000);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    process_lvgl(2000);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: discard works once the tower command has returned",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    overlay.discard_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(overlay.session_active());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: starting over with another extruder kind puts the old model "
                 "back, but never mid-tower",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    REQUIRE(overlay.procedure_for_test()->pass_count() == 3);

    const size_t before = client.gcode_script_history().size();
    overlay.start_for_test(params(), pa::ExtruderKind::Bowden);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.procedure_for_test()->pass_count() == 4);
    CHECK(sent_since(before).find("MODEL=linear ADVANCE=0.040000") != std::string::npos);

    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(overlay.procedure_for_test()->pass_count() == 4);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a start the printer never answered is still running, not "
                 "refused",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    client.force_next_gcode_error(MoonrakerErrorType::TIMEOUT, "request timed out");
    const size_t before = client.gcode_script_history().size();
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    // Nothing chases the script: a disarm would queue behind it and land mid-tower.
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(sent_since(before).find("TUNING_TOWER COMMAND=G4") == std::string::npos);
    // The tower the script was always going to start is followed as usual.
    tower("printing");
    CHECK(overlay.stage() == PaNonlinearOverlay::PRINTING);
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: a start the printer refuses ends the pass",
                 "[pa_nonlinear][pa_tuner]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown command");
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a stopped tower ends without the end macro, a completed one "
                 "with it",
                 "[pa_nonlinear][pa_tuner][pa_nl_finish]") {
    auto p = params();
    p.macros.end = "PRINT_END";
    overlay.start_for_test(p, pa::ExtruderKind::DirectDrive);

    SECTION("canceled") {
        overlay.print_pass_for_test();
        helix::ui::UpdateQueue::instance().drain();
        tower("printing");
        const size_t before = client.gcode_script_history().size();
        tower("cancelled");
        helix::ui::UpdateQueue::instance().drain();
        const std::string ending = sent_since(before);
        INFO(ending);
        // The cancel macro already ran the end of the print and cooled the
        // nozzle: a second end macro retracts into a cold nozzle.
        CHECK(ending.find("PRINT_END") == std::string::npos);
        CHECK(ending.find("TUNING_TOWER COMMAND=G4") != std::string::npos);
        CHECK(ending.find("MODEL=tanh") != std::string::npos);
        CHECK(ending.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0") != std::string::npos);
        CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
    }
    SECTION("completed") {
        overlay.print_pass_for_test();
        helix::ui::UpdateQueue::instance().drain();
        tower("printing");
        const size_t before = client.gcode_script_history().size();
        tower("complete");
        helix::ui::UpdateQueue::instance().drain();
        const std::string ending = sent_since(before);
        INFO(ending);
        CHECK(ending.find("PRINT_END") != std::string::npos);
    }
}

namespace {

/// The keypad opens over the active screen, with navigation seeded as the app does.
struct KeypadHost {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    KeypadHost() {
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }
    ~KeypadHost() {
        NavigationManager::instance().shutdown();
    }
    KeypadHost(const KeypadHost&) = delete;
    KeypadHost& operator=(const KeypadHost&) = delete;
};

} // namespace

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the judged height moves in half millimeters, stays on the "
                 "tower, and can be typed",
                 "[pa_nonlinear][pa_tuner][pa_nl_height]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    const float start = overlay.height_for_test();

    overlay.adjust_height_for_test(5);
    CHECK(overlay.height_for_test() == Catch::Approx(start + 0.5f));
    overlay.adjust_height_for_test(-50);
    CHECK(overlay.height_for_test() == Catch::Approx(start - 4.5f));
    overlay.adjust_height_for_test(-1000);
    CHECK(overlay.height_for_test() == Catch::Approx(0.0f));
    overlay.adjust_height_for_test(1000);
    CHECK(overlay.height_for_test() == Catch::Approx(50.0f));

    KeypadHost host;
    overlay.edit_height_for_test();
    process_lvgl(100);
    REQUIRE(ui_keypad_is_visible());
    lv_obj_t* keypad = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    REQUIRE(keypad != nullptr);
    for (const char* key : {"btn_backspace", "btn_backspace", "btn_backspace", "btn_backspace",
                            "btn_backspace", "btn_1", "btn_2", "btn_dot", "btn_5"}) {
        lv_obj_t* btn = lv_obj_find_by_name(keypad, key);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    }
    lv_xml_get_event_cb(nullptr, "on_keypad_confirm")(nullptr);
    process_lvgl(600);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.height_for_test() == Catch::Approx(12.5f));
    // The value the tower printed at that height follows the height.
    const std::string value =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "pa_nl_height_value"));
    CHECK(value.find("0.1250") != std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: back re-opens the previous judgement with the values its "
                 "tower printed with",
                 "[pa_nonlinear][pa_tuner][pa_nl_back]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    const auto can_back = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "pa_nl_can_back"));
    };
    CHECK(can_back() == 0);
    print_tower();
    overlay.adjust_height_for_test(50); // 30 mm
    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.procedure_for_test()->pass_number() == 2);
    REQUIRE(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(can_back() == 1);

    overlay.back_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
    CHECK(overlay.procedure_for_test()->pass_number() == 1);
    CHECK(overlay.height_for_test() == Catch::Approx(30.0f));
    // The printer runs what pass 1's tower printed with again.
    CHECK(client.last_send_script().find("OFFSET=0.000000") != std::string::npos);
    CHECK(can_back() == 0);

    // Nothing is taken back while a tower prints. The statuses above ended the
    // tower for the overlay only: the mock's own pass-1 tower is still running,
    // and its PRINT_PA_TOWER refuses a second one until that job ends.
    overlay.judge_for_test(1);
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    tower("printing");
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PRINTING);
    overlay.back_for_test();
    CHECK(overlay.stage() == PaNonlinearOverlay::PRINTING);
    CHECK(overlay.procedure_for_test()->pass_number() == 2);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: judged passes survive a restart of the screen and can be "
                 "continued or started over",
                 "[pa_nonlinear][pa_tuner][pa_nl_session]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    overlay.adjust_height_for_test(50); // 30 mm
    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(session_saved());
    // By now the extruder runs the procedure's values: the model to put back
    // on Discard is the one from before the procedure.
    status({{"extruder", {{"pressure_advance_model", "tanh"}, {"linear_advance", 0.0}}}});

    overlay.forget_for_test();
    REQUIRE(overlay.procedure_for_test() == nullptr);

    SECTION("continue") {
        overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
        press_dialog("btn_primary");
        const auto* proc = overlay.procedure_for_test();
        REQUIRE(proc != nullptr);
        CHECK(proc->pass_number() == 2);
        CHECK(proc->values().nonlinear_offset == Catch::Approx(0.30f));
        CHECK(proc->can_go_back());

        overlay.discard_for_test();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client.last_send_script().find("MODEL=linear ADVANCE=0.040000") != std::string::npos);
        CHECK_FALSE(session_saved());
    }
    SECTION("start over") {
        overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
        press_dialog("btn_secondary");
        REQUIRE(overlay.procedure_for_test() != nullptr);
        CHECK(overlay.procedure_for_test()->pass_number() == 1);
        CHECK_FALSE(session_saved());
        // The extruder still runs the old session's values; Discard puts back
        // the model from before that session, not those values.
        overlay.discard_for_test();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client.last_send_script().find("MODEL=linear ADVANCE=0.040000") != std::string::npos);
    }
    SECTION("another extruder kind is not offered the session") {
        overlay.start_for_test(params(), pa::ExtruderKind::Bowden);
        CHECK(helix::ui::modal_get_top() == nullptr);
    }
}

TEST_CASE_METHOD(NonlinearFixture, "nonlinear tuner: one press of Stop cancels the tower",
                 "[pa_nonlinear][pa_tuner][pa_nl_stop]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    tower("printing");
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PRINTING);

    const uint32_t before = api.job().cancels_sent();
    overlay.stop_for_test();
    press_dialog("btn_primary");
    CHECK(api.job().cancels_sent() == before + 1);
    process_lvgl(1000);
    CHECK(helix::ui::modal_get_top() == nullptr);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the E-stop stays up while the start script homes and heats",
                 "[pa_nonlinear][pa_tuner][pa_nl_busy]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    CHECK_FALSE(overlay.holds_machine_busy_for_test());
    overlay.print_pass_for_test();
    // The start script homes and heats.
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK(overlay.holds_machine_busy_for_test());
    // PRINT_PA_TOWER answers before the start macro's moves drain and before
    // the tower's job exists: the token holds until the job reports printing.
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.holds_machine_busy_for_test());
    tower("printing");
    CHECK_FALSE(overlay.holds_machine_busy_for_test());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the screen stays awake while a judgement or the result waits",
                 "[pa_nonlinear][pa_tuner][pa_nl_awake]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    CHECK_FALSE(overlay.keeps_display_awake_for_test());
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    tower("printing");
    CHECK_FALSE(overlay.keeps_display_awake_for_test());
    tower("complete");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    CHECK(overlay.keeps_display_awake_for_test());
    CHECK(helix::display_awake_hold().is_held());

    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK_FALSE(overlay.keeps_display_awake_for_test());
    CHECK_FALSE(helix::display_awake_hold().is_held());

    // Leaving the screen lets it sleep.
    overlay.back_for_test();
    REQUIRE(overlay.keeps_display_awake_for_test());
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    CHECK_FALSE(helix::display_awake_hold().is_held());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the live value follows a swept value that changes without the "
                 "height moving",
                 "[pa_nonlinear][pa_tuner][pa_nl_live]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    overlay.print_pass_for_test();
    helix::ui::UpdateQueue::instance().drain();
    tower("printing");
    REQUIRE(overlay.stage() == PaNonlinearOverlay::PRINTING);
    const auto live = [] {
        return std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "pa_nl_live_value")));
    };
    // Pass 1 sweeps the offset; the firmware sets the first height's value after
    // the move that reached it.
    status({{"extruder", {{"nonlinear_offset", 0.002}}}});
    CHECK(live() == "0.0020");
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: back leaves a print that holds the machine alone",
                 "[pa_nonlinear][pa_tuner][pa_nl_back]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    overlay.judge_for_test(1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::READY);
    status({{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}});
    const size_t before = client.gcode_script_history().size();
    overlay.back_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
    CHECK(sent_since(before).find("SET_PRESSURE_ADVANCE") == std::string::npos);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: the result neither keeps the screen awake nor raises the "
                 "E-stop while it saves",
                 "[pa_nonlinear][pa_tuner][pa_nl_busy][pa_nl_awake]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    while (overlay.stage() != PaNonlinearOverlay::RESULT) {
        REQUIRE(overlay.procedure_for_test() != nullptr);
        REQUIRE(overlay.procedure_for_test()->pass_number() <= 3);
        print_tower();
        // The overlay has seen the tower end; the mock's own job ends too.
        client.cancel_print_internal();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
        overlay.judge_for_test(1); // Use This Height, or Same Height
        helix::ui::UpdateQueue::instance().drain();
    }
    CHECK_FALSE(overlay.keeps_display_awake_for_test());
    overlay.save_for_test();
    CHECK(overlay.stage() == PaNonlinearOverlay::PREPARING);
    CHECK_FALSE(overlay.holds_machine_busy_for_test());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: pass 2 compares the left's and the front's heights itself",
                 "[pa_nonlinear][pa_tuner][pa_two_heights]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.judge_primary_for_test(); // pass 1 at 25 mm
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.procedure_for_test()->current().judge == pa::TowerSide::LeftVsFront);
    const float offset = overlay.procedure_for_test()->values().nonlinear_offset;

    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    // Left best at 24 mm, front at 27.5: the left is lower, so the offset drops
    // and the same pass prints again.
    // The first step on a "--" height shows the middle of the tower; the next moves from it.
    overlay.adjust_height_for_test(-10);
    CHECK(overlay.height_for_test() == Catch::Approx(25.0f));
    overlay.adjust_height_for_test(-10);
    overlay.adjust_front_height_for_test(25);
    overlay.adjust_front_height_for_test(25);
    CHECK(overlay.height_for_test() == Catch::Approx(24.0f));
    CHECK(overlay.front_height_for_test() == Catch::Approx(27.5f));
    overlay.judge_primary_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::READY);
    CHECK(overlay.procedure_for_test()->pass_number() == 2);
    CHECK(overlay.procedure_for_test()->repeats() == 1);
    CHECK(overlay.procedure_for_test()->values().nonlinear_offset < offset);

    // Within a millimeter they agree: the pass takes the middle and moves on.
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.adjust_height_for_test(20); // 25 mm: the middle first
    overlay.adjust_height_for_test(20); // 27 mm
    overlay.adjust_front_height_for_test(25);
    overlay.adjust_front_height_for_test(25); // 27.5 mm
    overlay.judge_primary_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.procedure_for_test()->pass_number() == 3);
    CHECK(overlay.procedure_for_test()->values().linear_advance == Catch::Approx(0.02725f));
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: a tap on the judged tower sets the height of the face it lands "
                 "on",
                 "[pa_nonlinear][pa_tuner][pa_two_heights]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.tower_tap_for_test(0, 12.34f);
    CHECK(overlay.height_for_test() == Catch::Approx(12.3f));
    overlay.judge_primary_for_test();
    helix::ui::UpdateQueue::instance().drain();

    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.procedure_for_test()->current().judge == pa::TowerSide::LeftVsFront);
    overlay.tower_tap_for_test(1, 33.3f);
    CHECK(overlay.front_height_for_test() == Catch::Approx(33.3f));
    overlay.tower_tap_for_test(0, 20.0f);
    CHECK(overlay.height_for_test() == Catch::Approx(20.0f));
    CHECK(overlay.front_height_for_test() == Catch::Approx(33.3f));
}

namespace {
bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* w = lv_obj_find_by_name(root, name);
    REQUIRE(w != nullptr);
    return lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN);
}
bool disabled(lv_obj_t* root, const char* name) {
    lv_obj_t* w = lv_obj_find_by_name(root, name);
    REQUIRE(w != nullptr);
    return lv_obj_has_state(w, LV_STATE_DISABLED);
}
void set_int(const char* subject, int v) {
    lv_subject_set_int(lv_xml_get_subject(nullptr, subject), v);
}
} // namespace

TEST_CASE_METHOD(
    NonlinearFixture,
    "nonlinear tuner: Back shows only with a judgement to take back, and the stage picks "
    "the header action and the height entry",
    "[pa_nonlinear][pa_tuner][pa_nl_bindings]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "pa_nonlinear_overlay", nullptr));
    REQUIRE(root != nullptr);

    set_int("pa_nl_can_back", 0);
    CHECK(hidden(root, "pa_nl_btn_back"));
    set_int("pa_nl_can_back", 1);
    CHECK_FALSE(hidden(root, "pa_nl_btn_back"));

    // A tower on its way stops from the header; a pass waiting on the user discards.
    set_int("pa_nl_stage", PaNonlinearOverlay::PRINTING);
    CHECK_FALSE(hidden(root, "pa_nl_btn_stop"));
    CHECK(hidden(root, "pa_nl_btn_discard"));
    set_int("pa_nl_stage", PaNonlinearOverlay::READY);
    CHECK(hidden(root, "pa_nl_btn_stop"));
    CHECK_FALSE(hidden(root, "pa_nl_btn_discard"));

    // One height for a one-sided pass, two for left against front.
    set_int("pa_nl_stage", PaNonlinearOverlay::JUDGE);
    set_int("pa_nl_two_sided", 0);
    CHECK_FALSE(hidden(root, "pa_nl_one_height"));
    CHECK(hidden(root, "pa_nl_two_heights"));
    set_int("pa_nl_two_sided", 1);
    CHECK(hidden(root, "pa_nl_one_height"));
    CHECK_FALSE(hidden(root, "pa_nl_two_heights"));
    lv_obj_delete(root);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "linear tuner: the start and Save to Config follow what the tuner allows",
                 "[pa_tune][pa_tuner][pa_tune_bindings]") {
    helix::ui::PaTuningOverlay linear;
    linear.init_subjects();
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "pa_tuning_overlay", nullptr));
    REQUIRE(root != nullptr);

    // A reachable printer that nothing else holds: only the tuner's own rule decides.
    for (const char* gate : {"machine_motion_blocked", "nav_buttons_enabled"}) {
        REQUIRE(lv_xml_get_subject(nullptr, gate) != nullptr);
    }
    set_int("machine_motion_blocked", 0);
    set_int("nav_buttons_enabled", 1);
    set_int("pa_tune_can_start", 0);
    CHECK(disabled(root, "pa_btn_start"));
    set_int("pa_tune_can_start", 1);
    CHECK_FALSE(disabled(root, "pa_btn_start"));
    // Linear K on an extruder that runs a nonlinear model: the start refuses, so it can't be
    // pressed.
    // Motion blocked (another job, an open E-stop): the start can't be pressed either.
    set_int("machine_motion_blocked", 1);
    CHECK(disabled(root, "pa_btn_start"));
    set_int("machine_motion_blocked", 0);
    CHECK_FALSE(disabled(root, "pa_btn_start"));
    set_int("pa_tune_lin_blocked", 1);
    CHECK(disabled(root, "pa_btn_start"));
    set_int("pa_tune_lin_blocked", 0);
    CHECK_FALSE(disabled(root, "pa_btn_start"));

    set_int("pa_tune_can_save_config", 0);
    CHECK(disabled(root, "pa_btn_save_config"));
    set_int("pa_tune_can_save_config", 1);
    CHECK_FALSE(disabled(root, "pa_btn_save_config"));

    // The band row shows only when a band was picked; a held value has none.
    set_int("pa_tune_band_picked", 0);
    CHECK(hidden(root, "pa_result_band"));
    set_int("pa_tune_band_picked", 1);
    CHECK_FALSE(hidden(root, "pa_result_band"));

    // Save to Spool shows only with a spool to save to.
    set_int("pa_tune_can_save_spool", 0);
    CHECK(hidden(root, "pa_btn_save_spool"));
    set_int("pa_tune_can_save_spool", 1);
    CHECK_FALSE(hidden(root, "pa_btn_save_spool"));
    lv_obj_delete(root);
    linear.deinit_subjects();
}

TEST_CASE("nonlinear tuner: the firmware tower's footprint comes from [pa_test] when it sets both",
          "[pa_nonlinear][pa_tuner][pa_tower_layout]") {
    PrinterDiscovery hw;
    hw.parse_pa_test_size(json{{"pa_test", {{"size_x", 60.0}, {"size_y", 45.0}}}});
    REQUIRE(hw.pa_test_size_mm().has_value());
    CHECK(hw.pa_test_size_mm()->first == Catch::Approx(60.0f));
    CHECK(hw.pa_test_size_mm()->second == Catch::Approx(45.0f));

    // One size alone, or none, leaves the firmware's default.
    hw.parse_pa_test_size(json{{"pa_test", {{"size_x", 60.0}}}});
    CHECK_FALSE(hw.pa_test_size_mm().has_value());
    hw.parse_pa_test_size(json{{"pa_test", {{"size_x", 0.0}, {"size_y", 0.0}}}});
    CHECK_FALSE(hw.pa_test_size_mm().has_value());
    hw.parse_pa_test_size(json::object());
    CHECK_FALSE(hw.pa_test_size_mm().has_value());
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: pass 2 can't be accepted until both faces have a height",
                 "[pa_nonlinear][pa_tuner][pa_two_heights]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.judge_primary_for_test(); // pass 1
    helix::ui::UpdateQueue::instance().drain();
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.stage() == PaNonlinearOverlay::JUDGE);
    const auto heights_set = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "pa_nl_heights_set"));
    };
    CHECK(heights_set() == 0);

    // The left alone: the judgement does nothing.
    overlay.tower_tap_for_test(0, 24.6f);
    CHECK(heights_set() == 0);
    overlay.judge_primary_for_test();
    CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
    CHECK(overlay.procedure_for_test()->pass_number() == 2);
    CHECK(overlay.procedure_for_test()->repeats() == 0);

    // The front too: now it takes.
    overlay.adjust_front_height_for_test(5);
    CHECK(heights_set() == 1);
    overlay.judge_primary_for_test();
    CHECK(overlay.stage() != PaNonlinearOverlay::JUDGE);
}

TEST_CASE_METHOD(NonlinearFixture,
                 "nonlinear tuner: Back into pass 2 brings back both heights as entered",
                 "[pa_nonlinear][pa_tuner][pa_two_heights][pa_nl_back]") {
    overlay.start_for_test(params(), pa::ExtruderKind::DirectDrive);
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.judge_primary_for_test(); // pass 1
    helix::ui::UpdateQueue::instance().drain();
    print_tower();
    client.cancel_print_internal();
    helix::ui::UpdateQueue::instance().drain();
    overlay.tower_tap_for_test(0, 18.0f);
    overlay.tower_tap_for_test(1, 30.0f);
    overlay.judge_primary_for_test(); // apart: the pass repeats
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(overlay.procedure_for_test()->repeats() == 1);

    overlay.back_for_test();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(overlay.stage() == PaNonlinearOverlay::JUDGE);
    CHECK(overlay.height_for_test() == Catch::Approx(18.0f));
    CHECK(overlay.front_height_for_test() == Catch::Approx(30.0f));
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "pa_nl_heights_set")) == 1);
    // Still 12 mm apart: judging again repeats the pass, as before.
    overlay.judge_primary_for_test();
    CHECK(overlay.procedure_for_test()->repeats() == 1);
    CHECK(overlay.procedure_for_test()->pass_number() == 2);
}
