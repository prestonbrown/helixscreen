// tests/unit/test_job_holds_machine.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `job_holds_machine` - the lifecycle-derived answer to "does a job own the
// toolhead right now?".
//
// The subject it replaces, `print_active`, is `PRINTING || PAUSED` read off the
// wire. That cannot see a job the app has committed to but the printer has not
// reported yet, so during a host-side pre-print block - the K2's forced bed mesh
// is the motivating case - `print_active` is 0 while the toolhead homes and
// probes. Every XML binding that disables jog, motion or extrude reads that
// subject, so the controls are live during exactly the window they guard.
//
// These tests pin BOTH halves: the pure predicate over the lifecycle enum, and
// the published subject, driven through the same status/phase inputs the app
// gets. The host-side cases are the regression - each one asserts that
// job_holds_machine is 1 where print_active is 0.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// ============================================================================
// The pure predicate
// ============================================================================

TEST_CASE("job_holds_machine: true for every state in which a job owns the toolhead",
          "[core][print_state][job_holds_machine]") {
    REQUIRE(job_holds_machine(PrintState::Preparing));
    REQUIRE(job_holds_machine(PrintState::Printing));
    REQUIRE(job_holds_machine(PrintState::Paused));
}

TEST_CASE("job_holds_machine: false when no job owns the toolhead",
          "[core][print_state][job_holds_machine]") {
    REQUIRE_FALSE(job_holds_machine(PrintState::Idle));
    REQUIRE_FALSE(job_holds_machine(PrintState::Complete));
    REQUIRE_FALSE(job_holds_machine(PrintState::Cancelled));
    REQUIRE_FALSE(job_holds_machine(PrintState::Error));
}

TEST_CASE("job_holds_machine: Preparing is the whole point - it is what print_active cannot see",
          "[core][print_state][job_holds_machine]") {
    // If this ever flips, the predicate has collapsed back onto PRINTING||PAUSED
    // and every guard built on it silently loses the pre-print window.
    REQUIRE(job_holds_machine(PrintState::Preparing));
}

// ============================================================================
// The published subject
// ============================================================================

namespace {

struct JobHoldsMachineFixture : public LVGLTestFixture {
    JobHoldsMachineFixture() {
        state_.init_subjects(false);
    }

    /// set_print_start_state() defers, so drive the queue to quiescence rather
    /// than once - a handler running during a drain can queue more work.
    static void drain() {
        for (int pass = 0; pass < 8; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    void report_job_state(const char* moonraker_state) {
        state_.update_from_status(json{{"print_stats", {{"state", moonraker_state}}}});
        drain();
    }

    void set_phase(PrintStartPhase phase) {
        state_.set_print_start_state(phase, "", 0);
        drain();
    }

    int holds() {
        return lv_subject_get_int(state_.get_job_holds_machine_subject());
    }

    int print_active() {
        return lv_subject_get_int(state_.get_print_active_subject());
    }

    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(JobHoldsMachineFixture, "job_holds_machine subject: 0 at rest",
                 "[core][printer_state][job_holds_machine]") {
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: agrees with print_active "
                 "for a plain print with no pre-print phase",
                 "[core][printer_state][job_holds_machine]") {
    report_job_state("printing");
    REQUIRE(holds() == 1);
    REQUIRE(print_active() == 1);

    report_job_state("paused");
    REQUIRE(holds() == 1);
    REQUIRE(print_active() == 1);

    report_job_state("printing");
    REQUIRE(holds() == 1);

    report_job_state("complete");
    REQUIRE(holds() == 0);
    REQUIRE(print_active() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: 1 during a host-side pre-print block, where "
                 "print_active is 0",
                 "[core][printer_state][job_holds_machine]") {
    // The K2 shape: a forced bed mesh runs BEFORE the printer is handed the job,
    // so print_stats.state still reads standby while the toolhead is homing.
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);

    REQUIRE(print_active() == 0); // the blind spot, unchanged
    REQUIRE(holds() == 1);        // the fix
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: 1 during a host-side block that follows a "
                 "finished job",
                 "[core][printer_state][job_holds_machine]") {
    // print_stats holds the PREVIOUS job's terminal state through the whole
    // host-side window. Nothing on the wire distinguishes this from idle.
    report_job_state("complete");
    set_phase(PrintStartPhase::HOMING);

    REQUIRE(print_active() == 0);
    REQUIRE(holds() == 1);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: stays 1 across the hand-off from a host-side "
                 "block into the real print",
                 "[core][printer_state][job_holds_machine]") {
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);
    REQUIRE(holds() == 1);

    // Printer accepts the job while the phase is still live (firmware-side
    // PRINT_START continues past the hand-off).
    report_job_state("printing");
    REQUIRE(holds() == 1);

    set_phase(PrintStartPhase::IDLE);
    REQUIRE(holds() == 1); // now a plain print

    report_job_state("complete");
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: falls to 0 when a pre-print block is abandoned",
                 "[core][printer_state][job_holds_machine]") {
    // The failure mode that matters: a latched-true guard blocks motion for the
    // rest of the session. Assert the FALSE edge, not just the true one.
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);
    REQUIRE(holds() == 1);

    set_phase(PrintStartPhase::IDLE);
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture, "job_holds_machine subject: 0 for every terminal outcome",
                 "[core][printer_state][job_holds_machine]") {
    for (const char* terminal : {"complete", "cancelled", "error"}) {
        report_job_state("printing");
        REQUIRE(holds() == 1);
        report_job_state(terminal);
        REQUIRE(holds() == 0);
    }
}

// ============================================================================
// The XML seam
//
// Two ways the binding fails silently:
//   1. The subject is registered under a different name than the XML asks for.
//      LVGL logs "No subject was found" and the binding is simply inert - the
//      control stays enabled and nothing crashes.
//   2. A sweep "finishes the job" by putting the bindings on print_active,
//      reopening the hole that job_holds_machine closes.
// ============================================================================
#include "../test_fixtures.h"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE_METHOD(XMLTestFixture,
                 "job_holds_machine is registered in the XML scope under that exact name",
                 "[ui][xml][job_holds_machine]") {
    // Identity, not just non-null: a stale registry entry from another fixture
    // would still be non-null and would never move when this state publishes.
    lv_subject_t* from_xml = lv_xml_get_subject(nullptr, "job_holds_machine");
    REQUIRE(from_xml != nullptr);
    REQUIRE(from_xml == state().get_job_holds_machine_subject());
}

TEST_CASE_METHOD(XMLTestFixture, "the bypass tile is disabled during a host-side pre-print block",
                 "[ui][xml][job_holds_machine][bypass]") {
    REQUIRE(register_component("components/panel_widget_bypass"));
    lv_obj_t* tile = create_component("panel_widget_bypass");
    REQUIRE(tile != nullptr);

    REQUIRE_FALSE(lv_obj_has_state(tile, LV_STATE_DISABLED));

    // The K2 shape: print_stats still says standby while the toolhead is being
    // homed and probed by a host-side block, so print_active stays 0 through all
    // of it and cannot keep this tile from being tapped.
    state().update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
    state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }

    REQUIRE(lv_subject_get_int(state().get_print_active_subject()) == 0);
    REQUIRE(lv_obj_has_state(tile, LV_STATE_DISABLED));

    // And it comes back when the block is abandoned - a latched-disabled control
    // is the failure mode that would make this fix worse than the bug.
    state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE_FALSE(lv_obj_has_state(tile, LV_STATE_DISABLED));
}

// ============================================================================
// The guard arrives by construction
//
// The moves_machine attribute is what makes "did anyone forget one" answerable:
// the engine installs the job_holds_machine -> disabled binding for the
// element itself, so a machine-moving control is guarded the moment its XML
// says it moves the machine. This test pins that construction, not a spelling.
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "moves_machine installs the toolhead guard by construction",
                 "[ui][xml][job_holds_machine]") {
    const char* attrs[] = {"moves_machine", "true", nullptr};
    lv_obj_t* control = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "lv_obj", attrs));
    REQUIRE(control != nullptr);

    REQUIRE_FALSE(lv_obj_has_state(control, LV_STATE_DISABLED));

    // A host-side pre-print block: print_active stays 0 and only the
    // lifecycle subject moves.
    state().update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
    state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }

    REQUIRE(lv_subject_get_int(state().get_print_active_subject()) == 0);
    REQUIRE(lv_obj_has_state(control, LV_STATE_DISABLED));

    // And it releases - a latched-disabled control is the failure mode.
    state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE_FALSE(lv_obj_has_state(control, LV_STATE_DISABLED));
}

// ============================================================================
// The census
//
// The gate walks every .xml under ui_xml/ and asks one question per file: does
// it declare a control? A file that does must appear in exactly one of the two
// tables below - either with the number of toolhead guards its controls carry,
// or as an assertion that nothing in it commands the printer. A new panel
// therefore arrives classified or arrives red; it cannot arrive invisible.
//
// What the gate reads is the shape of the guards that exist and the
// classification a human wrote. It cannot read a callback and work out that the
// button homes the toolhead, so a control that needs a guard and sits in a file
// already classified as guarded still passes (prestonbrown/helixscreen#1518).
// ============================================================================

#include <algorithm>
#include <filesystem>
#include <set>
#include <vector>

namespace {

constexpr const char* kSubject = "job_holds_machine";
constexpr const char* kDisabled = "state=\"disabled\"";
constexpr const char* kGuardAttribute = "moves_machine=\"true\"";

struct GuardedFile {
    const char* path;
    int guards;
};

/// Files whose controls carry a toolhead guard, and how many each carries.
constexpr GuardedFile kGuardedFiles[] = {
    {"ui_xml/ams_device_operations.xml", 1},
    {"ui_xml/components/ams_sidebar.xml", 1},
    {"ui_xml/components/panel_widget_bypass.xml", 1},
    {"ui_xml/controls_panel.xml", 9},
    {"ui_xml/header_bar.xml", 1},
    {"ui_xml/micro/controls_panel.xml", 9},
    {"ui_xml/micro/header_bar.xml", 1},
    {"ui_xml/motion_panel.xml", 2},
};

/// Control-bearing files that command nothing on the printer. A guard appearing
/// in one of these is a stale classification, not a bonus.
constexpr const char* kNoMachineControlFiles[] = {
    "ui_xml/abort_progress_modal.xml",
    "ui_xml/about_settings_overlay.xml",
    "ui_xml/accel_sensor_row.xml",
    "ui_xml/advanced_panel.xml",
    "ui_xml/ams_context_menu.xml",
    "ui_xml/ams_current_tool.xml",
    "ui_xml/ams_edit_overlay.xml",
    "ui_xml/ams_environment_overlay.xml",
    "ui_xml/ams_loading_error_modal.xml",
    "ui_xml/ams_overview_panel.xml",
    "ui_xml/ams_panel.xml",
    "ui_xml/ams_selector_menu.xml",
    "ui_xml/ams_slot_view.xml",
    "ui_xml/ams_unit_card.xml",
    "ui_xml/barcode_scanner_settings.xml",
    "ui_xml/bed_mesh_calibrate_modal.xml",
    "ui_xml/bed_mesh_panel.xml",
    "ui_xml/bed_mesh_rename_modal.xml",
    "ui_xml/bed_mesh_save_config_modal.xml",
    "ui_xml/calibration_pid_panel.xml",
    "ui_xml/calibration_zoffset_panel.xml",
    "ui_xml/change_host_modal.xml",
    "ui_xml/color_picker.xml",
    "ui_xml/color_sensor_row.xml",
    "ui_xml/components/ams_environment_indicator.xml",
    "ui_xml/components/barcode_scanner_device_row.xml",
    "ui_xml/components/bed_mesh_canvas_band.xml",
    "ui_xml/components/bed_mesh_profiles_card.xml",
    "ui_xml/components/buffer_status_modal.xml",
    "ui_xml/components/camera_config_modal.xml",
    "ui_xml/components/camera_fullscreen.xml",
    "ui_xml/components/chamber_diagnostics_card.xml",
    "ui_xml/components/clog_detection_config_modal.xml",
    "ui_xml/components/color_swatch_grid.xml",
    "ui_xml/components/compact_toggle_row.xml",
    "ui_xml/components/context_menu_backdrop.xml",
    "ui_xml/components/context_menu_card.xml",
    "ui_xml/components/exclude_object_map.xml",
    "ui_xml/components/exclude_object_side_list.xml",
    "ui_xml/components/filament_catalog_add_row.xml",
    "ui_xml/components/filament_catalog_picker.xml",
    "ui_xml/components/filament_catalog_row.xml",
    "ui_xml/components/filament_catalog_selector.xml",
    "ui_xml/components/filament_mapping_tool_row.xml",
    "ui_xml/components/filament_slot_picker_row.xml",
    "ui_xml/components/filament_source_row.xml",
    "ui_xml/components/ipp_print_modal.xml",
    "ui_xml/components/lock_screen.xml",
    "ui_xml/components/page_scroll_gutter.xml",
    "ui_xml/components/panel_widget_active_spool.xml",
    "ui_xml/components/panel_widget_bed_temperature.xml",
    "ui_xml/components/panel_widget_camera.xml",
    "ui_xml/components/panel_widget_chamber_temperature.xml",
    "ui_xml/components/panel_widget_clog_detection.xml",
    "ui_xml/components/panel_widget_control_buttons.xml",
    "ui_xml/components/panel_widget_fan.xml",
    "ui_xml/components/panel_widget_fan_stack.xml",
    "ui_xml/components/panel_widget_favorite_macro.xml",
    "ui_xml/components/panel_widget_filament.xml",
    "ui_xml/components/panel_widget_firmware_restart.xml",
    "ui_xml/components/panel_widget_gcode_console.xml",
    "ui_xml/components/panel_widget_job_queue.xml",
    "ui_xml/components/panel_widget_led.xml",
    "ui_xml/components/panel_widget_led_controls.xml",
    "ui_xml/components/panel_widget_lock.xml",
    "ui_xml/components/panel_widget_macros.xml",
    "ui_xml/components/panel_widget_motion.xml",
    "ui_xml/components/panel_widget_network.xml",
    "ui_xml/components/panel_widget_notifications.xml",
    "ui_xml/components/panel_widget_power_device.xml",
    "ui_xml/components/panel_widget_preheat.xml",
    "ui_xml/components/panel_widget_print_stats.xml",
    "ui_xml/components/panel_widget_print_status.xml",
    "ui_xml/components/panel_widget_printer_image.xml",
    "ui_xml/components/panel_widget_shutdown.xml",
    "ui_xml/components/panel_widget_temp_graph.xml",
    "ui_xml/components/panel_widget_temp_stack.xml",
    "ui_xml/components/panel_widget_temperature.xml",
    "ui_xml/components/panel_widget_thermistor.xml",
    "ui_xml/components/panel_widget_tips.xml",
    "ui_xml/components/pin_entry_modal.xml",
    "ui_xml/components/preflight_check_modal.xml",
    "ui_xml/components/print_status_detailed_active.xml",
    "ui_xml/components/print_status_detailed_idle.xml",
    "ui_xml/components/print_status_fan_row.xml",
    "ui_xml/components/print_status_preview_card.xml",
    "ui_xml/components/progress_bar.xml",
    "ui_xml/components/spaghetti_detection_modal.xml",
    "ui_xml/components/temp_card_unified.xml",
    "ui_xml/components/temp_graph_config_modal.xml",
    "ui_xml/components/theme_swatch_grid.xml",
    "ui_xml/components/tool_picker_button.xml",
    "ui_xml/components/upgrade_banner.xml",
    "ui_xml/components/zone_row.xml",
    "ui_xml/components/zone_tab.xml",
    "ui_xml/console_panel.xml",
    "ui_xml/console_settings_overlay.xml",
    "ui_xml/crash_report_modal.xml",
    "ui_xml/create_filament_modal.xml",
    "ui_xml/create_vendor_modal.xml",
    "ui_xml/debug_bundle_modal.xml",
    "ui_xml/estop_confirmation_dialog.xml",
    "ui_xml/factory_reset_modal.xml",
    "ui_xml/fan_dial.xml",
    "ui_xml/fan_rename_modal.xml",
    "ui_xml/favorite_macro_config_modal.xml",
    "ui_xml/filament_mapping_modal.xml",
    "ui_xml/filament_panel.xml",
    "ui_xml/filament_product_edit_modal.xml",
    "ui_xml/filament_sensor_row.xml",
    "ui_xml/form_field.xml",
    "ui_xml/gcode_test_panel.xml",
    "ui_xml/hardware_issue_row.xml",
    "ui_xml/hidden_network_modal.xml",
    "ui_xml/history_dashboard_panel.xml",
    "ui_xml/history_detail_overlay.xml",
    "ui_xml/history_list_panel.xml",
    "ui_xml/history_list_row.xml",
    "ui_xml/home_panel.xml",
    "ui_xml/humidity_sensor_row.xml",
    "ui_xml/info_qr_modal.xml",
    "ui_xml/input_shaper_panel.xml",
    "ui_xml/job_queue_modal.xml",
    "ui_xml/klipper_recovery_dialog.xml",
    "ui_xml/label_printer_settings.xml",
    "ui_xml/led_color_swatch.xml",
    "ui_xml/led_control_overlay.xml",
    "ui_xml/led_settings_overlay.xml",
    "ui_xml/machine_limits_overlay.xml",
    "ui_xml/macro_buttons_overlay.xml",
    "ui_xml/macro_card.xml",
    "ui_xml/macro_enhance_modal.xml",
    "ui_xml/macro_panel.xml",
    "ui_xml/macro_param_modal.xml",
    "ui_xml/material_temps_overlay.xml",
    "ui_xml/micro/theme_editor_overlay.xml",
    "ui_xml/micro/theme_preview_overlay.xml",
    "ui_xml/modal_button_row.xml",
    "ui_xml/modal_dialog.xml",
    "ui_xml/modal_header.xml",
    "ui_xml/navigation_bar.xml",
    "ui_xml/network_settings_overlay.xml",
    "ui_xml/network_test_modal.xml",
    "ui_xml/numeric_keypad_panel.xml",
    "ui_xml/overlay_backdrop.xml",
    "ui_xml/overlay_panel.xml",
    "ui_xml/panel_belt_tension.xml",
    "ui_xml/plugin_install_modal.xml",
    "ui_xml/portrait/print_status_panel.xml",
    "ui_xml/portrait/print_tune_panel.xml",
    "ui_xml/power_device_row.xml",
    "ui_xml/print_completion_modal.xml",
    "ui_xml/print_file_card.xml",
    "ui_xml/print_file_detail.xml",
    "ui_xml/print_file_list_row.xml",
    "ui_xml/print_select_panel.xml",
    "ui_xml/print_status_configure_picker.xml",
    "ui_xml/print_status_panel.xml",
    "ui_xml/print_tune_panel.xml",
    "ui_xml/printer_image_list_item.xml",
    "ui_xml/printer_image_overlay.xml",
    "ui_xml/printer_list_item.xml",
    "ui_xml/printer_list_overlay.xml",
    "ui_xml/printer_manager_overlay.xml",
    "ui_xml/printer_switch_menu.xml",
    "ui_xml/probe_accuracy_modal.xml",
    "ui_xml/probe_beacon_panel.xml",
    "ui_xml/probe_bltouch_panel.xml",
    "ui_xml/probe_cartographer_panel.xml",
    "ui_xml/probe_config_edit_modal.xml",
    "ui_xml/probe_eddy_panel.xml",
    "ui_xml/probe_generic_panel.xml",
    "ui_xml/probe_overlay.xml",
    "ui_xml/probe_sensor_row.xml",
    "ui_xml/qr_scanner_overlay.xml",
    "ui_xml/restart_prompt_dialog.xml",
    "ui_xml/retraction_settings_overlay.xml",
    "ui_xml/runout_guidance_modal.xml",
    "ui_xml/save_z_offset_modal.xml",
    "ui_xml/screws_tilt_panel.xml",
    "ui_xml/screws_tilt_share_modal.xml",
    "ui_xml/security_settings_overlay.xml",
    "ui_xml/sensors_overlay.xml",
    "ui_xml/setting_action_row.xml",
    "ui_xml/setting_dropdown_row.xml",
    "ui_xml/setting_form_dropdown.xml",
    "ui_xml/setting_form_input.xml",
    "ui_xml/setting_form_macro_field.xml",
    "ui_xml/setting_section_header.xml",
    "ui_xml/setting_slider_row.xml",
    "ui_xml/setting_state_row.xml",
    "ui_xml/setting_toggle_row.xml",
    "ui_xml/setting_value_field.xml",
    "ui_xml/settings_display_sound_overlay.xml",
    "ui_xml/settings_hardware_overlay.xml",
    "ui_xml/settings_help_overlay.xml",
    "ui_xml/settings_panel.xml",
    "ui_xml/settings_printing_overlay.xml",
    "ui_xml/settings_safety_overlay.xml",
    "ui_xml/settings_system_overlay.xml",
    "ui_xml/settings_touch_overlay.xml",
    "ui_xml/shutdown_modal.xml",
    "ui_xml/spool_wizard.xml",
    "ui_xml/spoolman_context_menu.xml",
    "ui_xml/spoolman_edit_modal.xml",
    "ui_xml/spoolman_panel.xml",
    "ui_xml/spoolman_settings.xml",
    "ui_xml/spoolman_spool_item.xml",
    "ui_xml/spoolman_spool_row.xml",
    "ui_xml/step_test_panel.xml",
    "ui_xml/telemetry_data_overlay.xml",
    "ui_xml/telemetry_info_modal.xml",
    "ui_xml/temp_graph_overlay.xml",
    "ui_xml/test_panel.xml",
    "ui_xml/theme_editor_overlay.xml",
    "ui_xml/theme_preview_overlay.xml",
    "ui_xml/theme_save_as_modal.xml",
    "ui_xml/timelapse_install_overlay.xml",
    "ui_xml/timelapse_settings_overlay.xml",
    "ui_xml/timelapse_video_card.xml",
    "ui_xml/timelapse_videos_overlay.xml",
    "ui_xml/toast_notification.xml",
    "ui_xml/touch_calibration_overlay.xml",
    "ui_xml/tour_tooltip_card.xml",
    "ui_xml/update_download_modal.xml",
    "ui_xml/update_notify_modal.xml",
    "ui_xml/widget_catalog_overlay.xml",
    "ui_xml/width_sensor_row.xml",
    "ui_xml/wifi_network_item.xml",
    "ui_xml/wifi_password_modal.xml",
    "ui_xml/wizard_connection.xml",
    "ui_xml/wizard_container.xml",
    "ui_xml/wizard_fan_select.xml",
    "ui_xml/wizard_filament_row.xml",
    "ui_xml/wizard_filament_sensor_select.xml",
    "ui_xml/wizard_heater_select.xml",
    "ui_xml/wizard_input_shaper.xml",
    "ui_xml/wizard_language_chooser.xml",
    "ui_xml/wizard_led_select.xml",
    "ui_xml/wizard_printer_identify.xml",
    "ui_xml/wizard_summary.xml",
    "ui_xml/wizard_telemetry.xml",
    "ui_xml/wizard_touch_calibration.xml",
    "ui_xml/wizard_vendor_row.xml",
    "ui_xml/wizard_wifi_setup.xml",
};

std::string read_xml(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) {
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Every .xml under ui_xml/, repo-root-relative and sorted. Dot-directories
/// hold tool scratch space rather than layouts.
std::vector<std::string> walk_ui_xml() {
    namespace fs = std::filesystem;
    std::vector<std::string> paths;
    for (auto it = fs::recursive_directory_iterator("ui_xml");
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (it->path().filename().string().rfind('.', 0) == 0) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->path().extension() == ".xml") {
            paths.push_back(it->path().generic_string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

/// A control the user can operate. Component rows and event_cb elements both
/// spell their handler `callback="..."`; the rest are containers made clickable
/// and the primitive widgets that are clickable by nature.
bool declares_a_control(const std::string& xml) {
    for (const char* marker : {"callback=\"", "clickable=\"true\"", "<ui_button", "<lv_button",
                               "<ui_switch", "<lv_switch", "<lv_slider", "<lv_dropdown",
                               "<lv_checkbox", "<lv_roller", "<text_input", "<lv_textarea"}) {
        if (xml.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Each `<bind_state_*` element in the file, as raw text.
std::vector<std::string> state_binds(const std::string& xml) {
    std::vector<std::string> binds;
    for (size_t pos = 0; (pos = xml.find("<bind_state_", pos)) != std::string::npos;) {
        const size_t end = xml.find("/>", pos);
        if (end == std::string::npos) {
            binds.push_back(xml.substr(pos));
            break;
        }
        binds.push_back(xml.substr(pos, end + 2 - pos));
        pos = end + 2;
    }
    return binds;
}

size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = 0; (pos = haystack.find(needle, pos)) != std::string::npos;
         pos += needle.size()) {
        ++n;
    }
    return n;
}

struct GuardScan {
    size_t recognized = 0;                 ///< moves_machine="true" attributes
    size_t mentions = 0;                   ///< every occurrence of the subject name
    std::vector<std::string> unrecognized; ///< bindings naming the subject directly
};

GuardScan scan_guards(const std::string& xml) {
    GuardScan scan;
    scan.mentions = count_occurrences(xml, kSubject);
    scan.recognized = count_occurrences(xml, kGuardAttribute);

    // The attribute is the ONLY way to carry the guard now: a hand-written
    // bind naming the subject is the failure this gate exists to prevent
    // (it rots: it can be forgotten, or its spelling can drift). The engine
    // installs the binding for moves_machine by construction.
    for (const std::string& bind : state_binds(xml)) {
        if (bind.find(kSubject) != std::string::npos) {
            scan.unrecognized.push_back(bind);
        }
    }
    return scan;
}

} // namespace

TEST_CASE("every control-bearing XML file is classified for toolhead safety",
          "[ui][xml][job_holds_machine]") {
    const std::vector<std::string> walked = walk_ui_xml();
    INFO("the suite runs from the repo root, where ui_xml/ is");
    REQUIRE_FALSE(walked.empty());

    std::set<std::string> classified;
    for (const GuardedFile& row : kGuardedFiles) {
        INFO(row.path << " appears twice in the census");
        REQUIRE(classified.insert(row.path).second);
    }
    for (const char* path : kNoMachineControlFiles) {
        INFO(path << " appears in both census tables");
        REQUIRE(classified.insert(path).second);
    }

    std::set<std::string> bears_controls;
    std::string unclassified;
    for (const std::string& path : walked) {
        if (!declares_a_control(read_xml(path))) {
            continue;
        }
        bears_controls.insert(path);
        if (classified.count(path) == 0) {
            unclassified += "\n  " + path;
        }
    }
    {
        INFO("These files declare a control and are in neither census table:"
             << unclassified
             << "\nAdd each to kGuardedFiles with the number of toolhead guards its controls "
                "carry, or to kNoMachineControlFiles to state that none of them commands the "
                "printer.");
        REQUIRE(unclassified.empty());
    }

    std::string stale;
    for (const std::string& path : classified) {
        if (bears_controls.count(path) == 0) {
            stale += "\n  " + path;
        }
    }
    INFO("These census rows name a file that is gone or no longer declares a control:"
         << stale << "\nDrop the row.");
    REQUIRE(stale.empty());
}

TEST_CASE("no XML disables a control on the raw print_active subject",
          "[ui][xml][job_holds_machine]") {
    for (const std::string& path : walk_ui_xml()) {
        for (const std::string& bind : state_binds(read_xml(path))) {
            if (bind.find(kDisabled) == std::string::npos) {
                continue;
            }
            INFO(path << " disables a control on print_active, which reads 0 throughout a "
                         "host-side pre-print block while the toolhead homes and probes: "
                      << bind);
            REQUIRE(bind.find("print_active") == std::string::npos);
        }
    }
}

TEST_CASE("each guarded file carries exactly the toolhead guards its census row pins",
          "[ui][xml][job_holds_machine]") {
    for (const GuardedFile& row : kGuardedFiles) {
        const std::string xml = read_xml(row.path);
        INFO("reading " << row.path);
        REQUIRE_FALSE(xml.empty());

        const GuardScan scan = scan_guards(xml);
        {
            std::string odd;
            for (const std::string& bind : scan.unrecognized) {
                odd += "\n  " + bind;
            }
            INFO(row.path << " names job_holds_machine in a hand-written bind. The guard "
                             "arrives by construction from the moves_machine attribute now; "
                             "a direct bind is the failure mode this gate exists to prevent:"
                          << odd
                          << "\nCarry the guard as moves_machine=\"true\" on the "
                             "control instead.");
            REQUIRE(scan.unrecognized.empty());
        }
        {
            INFO(row.path << " names job_holds_machine " << scan.mentions << " times and only "
                          << scan.recognized
                          << " of those are covered by moves_machine guards, so the rest are "
                             "prose that claims a guard without carrying one. Say it in words "
                             "that are not the subject name.");
            REQUIRE(scan.mentions <= scan.recognized);
        }
        INFO(row.path << " pins " << row.guards << " toolhead guards and carries "
                      << scan.recognized
                      << ". If a control was legitimately added or removed, update the row - the "
                         "number being written down is what makes a silent drop visible.");
        REQUIRE(scan.recognized == static_cast<size_t>(row.guards));
    }
}

TEST_CASE("a file classified as commanding nothing carries no toolhead guard",
          "[ui][xml][job_holds_machine]") {
    for (const char* path : kNoMachineControlFiles) {
        const std::string xml = read_xml(path);
        INFO("reading " << path);
        REQUIRE_FALSE(xml.empty());
        INFO(path << " carries a toolhead guard, so a control in it does command the printer. "
                     "Move its row to kGuardedFiles.");
        REQUIRE(xml.find(kSubject) == std::string::npos);
    }
}
