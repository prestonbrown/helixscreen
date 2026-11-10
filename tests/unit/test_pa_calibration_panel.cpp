// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pa_calibration_panel.cpp
 * @brief The PA calibration screen across a screensaver suspend and wake
 *
 * Run with: ./build/bin/helix-tests "[pa_cal][1516]"
 *
 * The screen is opened through NavigationManager and blanked with
 * suspend_active() / resume_active(), the path the screensaver takes.
 */

#include "ui_nav_manager.h"
#include "ui_panel_calibration_pa.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "app_globals.h"
#include "cleanup_hold.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "temperature_controller.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::get_global_pa_cal_panel;
using helix::ui::PACalibrationPanel;

namespace {

class PACalPanelFixture : public LVGLUITestFixture {
  protected:
    PACalPanelFixture() {
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
        REQUIRE(panel().show(lv_screen_active()));
        settle();
    }

    ~PACalPanelFixture() override {
        NavigationManager::instance().resume_active();
        panel().set_state_for_test(PACalibrationPanel::IDLE);
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    static PACalibrationPanel& panel() {
        return get_global_pa_cal_panel();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

class PAHeaterFixture : public PACalPanelFixture {
  protected:
    explicit PAHeaterFixture(
        MoonrakerClientMock::PrinterType type = MoonrakerClientMock::PrinterType::VORON_24)
        : client_(type) {
        auto& state = get_printer_state();
        state.temperature_state().init_extruders({"extruder"});
        state.set_klippy_state_sync(helix::KlippyState::READY);
        set_link(true);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, state);
        controller_ =
            std::make_unique<helix_test::ScopedSharedResource<helix::TemperatureController>>(
                std::make_shared<helix::TemperatureController>(state, api_.get()));
        panel().set_api(api_.get());
        set_print_state("standby");
        feed_nozzle(25, 0);
        client_.clear_gcode_script_history();
    }

    ~PAHeaterFixture() override {
        panel().before_disconnect();
        panel().set_api(nullptr);
        set_link(false);
        set_print_state("standby");
        feed_nozzle(25, 0);
        controller_.reset();
        settle();
        client_.disconnect();
        api_.reset();
    }

    void feed_nozzle(double actual_c, double target_c) {
        get_printer_state().update_from_status(
            {{"extruder", {{"temperature", actual_c}, {"target", target_c}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    static void set_link(bool up) {
        lv_subject_set_int(
            get_printer_state().network_state().get_printer_connection_state_subject(), up ? 2 : 0);
        helix::ui::UpdateQueue::instance().drain();
    }

    void set_print_state(const char* wire) {
        get_printer_state().update_from_status({{"print_stats", {{"state", wire}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] int run_target() const {
        return panel().target_temp_for_test();
    }

    /// Start a run from a cold nozzle and see the printer report its target.
    void begin_heating() {
        panel().begin_run_for_test();
        REQUIRE(panel().get_state() == PACalibrationPanel::HEATING);
        feed_nozzle(25, run_target());
        client_.clear_gcode_script_history();
    }

    /// Index of the first script containing `needle`, or -1.
    [[nodiscard]] int script_index(const char* needle) const {
        const auto& history = client_.gcode_script_history();
        for (size_t i = 0; i < history.size(); ++i) {
            if (history[i].find(needle) != std::string::npos) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] bool heater_off_sent() const {
        return script_index("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0") >= 0;
    }

    MoonrakerClientMock client_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<helix_test::ScopedSharedResource<helix::TemperatureController>> controller_;
};

/// A stock Z-Mod Creator 5, whose sweep this app drives.
class PASweepPanelFixture : public PAHeaterFixture {
  protected:
    PASweepPanelFixture()
        : PAHeaterFixture(MoonrakerClientMock::PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
        helix::PrinterDiscovery hw;
        const std::vector<std::string> objects = {"extruder", "output_pin enable_pin_tmc_x",
                                                  "output_pin enable_pin_tmc_y"};
        hw.parse_objects(objects);
        hw.set_printer_objects(objects);
        hw.parse_config_keys(nlohmann::json{{"pa_adjust", nlohmann::json::object()}});
        get_printer_state().set_hardware(std::move(hw));
    }
    ~PASweepPanelFixture() override {
        panel().before_disconnect();
        settle();
        get_printer_state().set_hardware(helix::PrinterDiscovery{});
    }
};

} // namespace

TEST_CASE_METHOD(PASweepPanelFixture,
                 "PA calibration: a driven sweep restores the printer before the heater off",
                 "[pa_cal][pa_sweep]") {
    begin_heating();
    feed_nozzle(run_target(), run_target());
    REQUIRE(wait_until([this]() { return panel().get_state() == PACalibrationPanel::COMPLETE; },
                       20000));
    settle();
    const int restore = script_index("RESTORE_GCODE_STATE NAME=helix_pa_sweep");
    const int off = script_index("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");
    REQUIRE(restore >= 0);
    REQUIRE(off >= 0);
    CHECK(restore < off);
    CHECK(script_index("SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.020000") >= 0);
    // The note says the value is live only until a restart, all of it.
    lv_subject_t* note = lv_xml_get_subject(nullptr, "pa_cal_keep_note");
    REQUIRE(note != nullptr);
    CHECK(std::string(lv_subject_get_string(note)) ==
          "The printer is using this value until Klipper restarts. To keep it, add it as "
          "pressure_advance in this tool's extruder section of printer.cfg.");
}

TEST_CASE_METHOD(PACalPanelFixture, "PA calibration: the screensaver does not stop a measurement",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    NavigationManager::instance().suspend_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::MEASURING);

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::MEASURING);
}

TEST_CASE_METHOD(PACalPanelFixture,
                 "PA calibration: a heat-up pauses its clock behind the screensaver and resumes it",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::HEATING);
    panel().arm_eta_timer_for_test();
    REQUIRE(panel().eta_timer_for_test() != nullptr);

    NavigationManager::instance().suspend_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::HEATING);
    CHECK(panel().eta_timer_for_test() == nullptr);

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::HEATING);
    CHECK(panel().eta_timer_for_test() != nullptr);
}

TEST_CASE_METHOD(PACalPanelFixture, "PA calibration: a result is still shown on wake",
                 "[pa_cal][1516]") {
    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    NavigationManager::instance().suspend_active();
    panel().on_result_for_test(0.042f);
    settle();

    NavigationManager::instance().resume_active();
    settle();
    CHECK(panel().get_state() == PACalibrationPanel::COMPLETE);
    CHECK(panel().last_result() == Catch::Approx(0.042f));
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: SIGTERM sees the heater held only during a run",
                 "[pa_cal][sigterm]") {
    REQUIRE_FALSE(helix::CleanupHold::any_held_signal_safe());

    begin_heating();
    CHECK(helix::CleanupHold::any_held_signal_safe());

    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    CHECK(panel().heater_held_for_test());

    SECTION("released by a result") {
        panel().on_result_for_test(0.04f);
    }
    SECTION("released by a firmware error") {
        panel().on_error_for_test("abort calibration: no flow");
    }
    SECTION("released by a disconnect") {
        panel().before_disconnect();
    }
    CHECK_FALSE(panel().heater_held_for_test());
    CHECK_FALSE(helix::CleanupHold::any_held_signal_safe());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: a disconnect mid-heat turns the nozzle off",
                 "[pa_cal][sigterm]") {
    begin_heating();

    panel().before_disconnect();

    CHECK(heater_off_sent());
    CHECK(panel().get_state() == PACalibrationPanel::IDLE);
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: a disconnect mid-measurement queues the off behind it",
                 "[pa_cal][sigterm]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    panel().before_disconnect();

    CHECK(heater_off_sent());
    CHECK(panel().get_state() == PACalibrationPanel::IDLE);
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: a preheat someone else set is never held",
                 "[pa_cal][sigterm]") {
    feed_nozzle(120, 200);

    panel().begin_run_for_test();
    REQUIRE(panel().get_state() == PACalibrationPanel::HEATING);
    client_.clear_gcode_script_history();

    CHECK_FALSE(panel().heater_held_for_test());
    panel().before_disconnect();
    CHECK_FALSE(heater_off_sent());
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: a target changed mid-heat stays on at disconnect",
                 "[pa_cal][sigterm]") {
    begin_heating();
    feed_nozzle(30, 180);

    panel().before_disconnect();

    CHECK_FALSE(heater_off_sent());
    CHECK_FALSE(panel().heater_held_for_test());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: a job that took the machine keeps the nozzle",
                 "[pa_cal][sigterm]") {
    begin_heating();
    set_print_state("printing");

    panel().before_disconnect();

    CHECK_FALSE(heater_off_sent());
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: a disconnect before the target is reported "
                 "still turns the nozzle off",
                 "[pa_cal][sigterm]") {
    panel().begin_run_for_test();
    REQUIRE(panel().get_state() == PACalibrationPanel::HEATING);
    client_.clear_gcode_script_history();

    panel().before_disconnect();

    CHECK(heater_off_sent());
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: a printer with no procedure does not keep the heat",
                 "[pa_cal][sigterm]") {
    begin_heating();

    feed_nozzle(run_target(), run_target());

    REQUIRE(panel().get_state() == PACalibrationPanel::ERROR);
    CHECK(heater_off_sent());
    CHECK_FALSE(panel().heater_held_for_test());
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: walking away queues the off behind a measurement",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    panel().on_deactivating(DeactivateReason::NavigateAway);

    CHECK(heater_off_sent());
    CHECK(panel().get_state() == PACalibrationPanel::IDLE);
    CHECK_FALSE(panel().heater_held_for_test());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: Stop mid-measurement queues the off too",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    panel().stop_run_for_test(/*user_requested=*/true);

    CHECK(heater_off_sent());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: walking away mid-heat honours the same guards",
                 "[pa_cal][walkaway]") {
    SECTION("owned heat goes off") {
        begin_heating();
        panel().on_deactivating(DeactivateReason::NavigateAway);
        CHECK(heater_off_sent());
    }
    SECTION("someone else's preheat stays") {
        feed_nozzle(120, 200);
        panel().begin_run_for_test();
        client_.clear_gcode_script_history();
        panel().on_deactivating(DeactivateReason::NavigateAway);
        CHECK_FALSE(heater_off_sent());
    }
    SECTION("a retarget stays") {
        begin_heating();
        feed_nozzle(30, 180);
        panel().on_deactivating(DeactivateReason::NavigateAway);
        CHECK_FALSE(heater_off_sent());
    }
    SECTION("a job keeps it") {
        begin_heating();
        set_print_state("printing");
        panel().on_deactivating(DeactivateReason::NavigateAway);
        CHECK_FALSE(heater_off_sent());
    }
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: a heat-up that never arrives gives up",
                 "[pa_cal][walkaway]") {
    begin_heating();
    REQUIRE(panel().run_timer_for_test() != nullptr);

    panel().fire_run_timer_for_test();

    CHECK(heater_off_sent());
    CHECK(panel().get_state() == PACalibrationPanel::ERROR);
    CHECK_FALSE(panel().heater_held_for_test());
    CHECK(panel().run_timer_for_test() == nullptr);
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: a measurement with no answer gives up",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);

    panel().fire_run_timer_for_test();

    CHECK(heater_off_sent());
    CHECK(panel().get_state() == PACalibrationPanel::ERROR);
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: an ended run stops its deadline",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().stop_run_for_test(/*user_requested=*/false);
    CHECK(panel().run_timer_for_test() == nullptr);
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: leaving with the link down owes the off",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    set_link(false);

    SECTION("left the screen") {
        panel().on_deactivating(DeactivateReason::NavigateAway);
    }
    SECTION("firmware error") {
        panel().on_error_for_test("connection lost");
    }

    CHECK_FALSE(heater_off_sent());
    REQUIRE(panel().run_timer_for_test() == nullptr);

    set_link(true);
    REQUIRE(panel().owed_off_settling_for_test());
    CHECK_FALSE(heater_off_sent()); // waits for the printer's fresh status

    panel().flush_owed_off_for_test();
    CHECK(heater_off_sent());
    CHECK_FALSE(panel().owed_off_settling_for_test());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: SIGTERM stays held while an off is settling",
                 "[pa_cal][walkaway][sigterm]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    set_link(false);
    panel().on_deactivating(DeactivateReason::NavigateAway);

    set_link(true);
    REQUIRE(panel().owed_off_settling_for_test());
    CHECK(panel().heater_held_for_test());

    panel().flush_owed_off_for_test();
    CHECK_FALSE(panel().heater_held_for_test());
}

TEST_CASE_METHOD(PAHeaterFixture, "PA calibration: the end of a run on a preheat is guarded",
                 "[pa_cal][walkaway]") {
    feed_nozzle(120, 200);
    panel().begin_run_for_test();
    REQUIRE(panel().get_state() == PACalibrationPanel::HEATING);
    feed_nozzle(120, run_target());

    SECTION("a plain result clears the nozzle") {
        panel().set_state_for_test(PACalibrationPanel::MEASURING);
        client_.clear_gcode_script_history();
        panel().on_result_for_test(0.04f);
        CHECK(heater_off_sent());
    }
    SECTION("a job that started keeps it on a result") {
        panel().set_state_for_test(PACalibrationPanel::MEASURING);
        set_print_state("printing");
        client_.clear_gcode_script_history();
        panel().on_result_for_test(0.04f);
        CHECK_FALSE(heater_off_sent());
    }
    SECTION("a job that started keeps it on an error") {
        panel().set_state_for_test(PACalibrationPanel::MEASURING);
        set_print_state("printing");
        client_.clear_gcode_script_history();
        panel().on_error_for_test("failed");
        CHECK_FALSE(heater_off_sent());
    }
    SECTION("a new target keeps it on a user stop") {
        feed_nozzle(120, 180);
        client_.clear_gcode_script_history();
        panel().stop_run_for_test(/*user_requested=*/true);
        CHECK_FALSE(heater_off_sent());
    }
    SECTION("walking away leaves the preheat and owes nothing") {
        client_.clear_gcode_script_history();
        panel().on_deactivating(DeactivateReason::NavigateAway);
        CHECK_FALSE(heater_off_sent());
        CHECK_FALSE(panel().heater_held_for_test());
    }
}

TEST_CASE_METHOD(PAHeaterFixture,
                 "PA calibration: a job found after the outage keeps the owed heat",
                 "[pa_cal][walkaway]") {
    begin_heating();
    panel().set_state_for_test(PACalibrationPanel::MEASURING);
    set_link(false);
    panel().on_deactivating(DeactivateReason::NavigateAway);

    set_link(true);
    set_print_state("printing");
    panel().flush_owed_off_for_test();

    CHECK_FALSE(heater_off_sent());
}
