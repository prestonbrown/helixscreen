// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_calibration_pid.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"

#include "app_globals.h"
#include "config.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "klipper_config_editor.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "preset_materials.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "temperature_service.h"
#include "thermal_rate_model.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <lvgl.h>
#include <memory>
#include <string>

// ============================================================================
// STATIC SUBJECT
// ============================================================================

// State subject (0=IDLE, 1=CALIBRATING, 2=SAVING, 3=COMPLETE, 4=ERROR, 5=MIGRATING)
static lv_subject_t s_pid_cal_state;
static bool s_callbacks_registered = false;

// "Taking longer than expected" backstop thresholds (#988). Generous on purpose: a normal
// run completes via the result line well before these, so the reassuring status only shows
// when calibration is genuinely overrunning. Bed allows much longer than extruder because
// slow-cooling beds (e.g. AD5M Pro) legitimately take far longer to cycle.
static constexpr uint32_t PID_OVERTIME_EXTRUDER_MS = 12u * 60u * 1000u; // 12 min
static constexpr uint32_t PID_OVERTIME_BED_MS = 30u * 60u * 1000u;      // 30 min

// Non-creating view of the global instance (defined with it at the bottom of
// this file). Returns nullptr when the panel does not currently exist, so
// shutdown callbacks can skip instead of resurrecting it through the getter.
static PIDCalibrationPanel* existing_pid_cal_panel();

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PIDCalibrationPanel::PIDCalibrationPanel() {
    // Zero out buffers
    std::memset(buf_temp_display_, 0, sizeof(buf_temp_display_));
    std::memset(buf_temp_hint_, 0, sizeof(buf_temp_hint_));
    std::memset(buf_calibrating_heater_, 0, sizeof(buf_calibrating_heater_));
    std::memset(buf_pid_kp_, 0, sizeof(buf_pid_kp_));
    std::memset(buf_pid_ki_, 0, sizeof(buf_pid_ki_));
    std::memset(buf_pid_kd_, 0, sizeof(buf_pid_kd_));
    std::memset(buf_error_message_, 0, sizeof(buf_error_message_));
    std::memset(buf_pid_progress_text_, 0, sizeof(buf_pid_progress_text_));
    std::memset(buf_pid_eta_, 0, sizeof(buf_pid_eta_));
    std::memset(buf_mpc_heat_capacity_, 0, sizeof(buf_mpc_heat_capacity_));
    std::memset(buf_mpc_sensor_resp_, 0, sizeof(buf_mpc_sensor_resp_));
    std::memset(buf_mpc_ambient_transfer_, 0, sizeof(buf_mpc_ambient_transfer_));
    std::memset(buf_mpc_fan_transfer_, 0, sizeof(buf_mpc_fan_transfer_));
    std::memset(buf_fan_speed_text_, 0, sizeof(buf_fan_speed_text_));
    std::memset(buf_wattage_display_, 0, sizeof(buf_wattage_display_));

    spdlog::trace("[PIDCal] Instance created");
}

PIDCalibrationPanel::~PIDCalibrationPanel() {
    deinit_subjects();

    // stop_progress_tracking() cancels this on the normal path, but a teardown
    // that destroys the panel mid-calibration skips it. StaticPanelRegistry::
    // destroy_all() runs BEFORE lv_deinit(), so a live tick would then fire into
    // a freed `this` (#1173).
    cancel_eta_timer();

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    parent_screen_ = nullptr;

    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[PIDCal] Destroyed");
    }
}

// ============================================================================
// SUBJECT REGISTRATION
// ============================================================================

void PIDCalibrationPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[PIDCal] Subjects already initialized");
        return;
    }

    spdlog::debug("[PIDCal] Initializing subjects");

    // Register state subject (shared across all instances)
    UI_MANAGED_SUBJECT_INT(s_pid_cal_state, 0, "pid_cal_state", subjects_);

    // Initialize string subjects with initial values
    UI_MANAGED_SUBJECT_STRING(subj_temp_display_, buf_temp_display_, "200°C", "pid_temp_display",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_temp_hint_, buf_temp_hint_, "Recommended: 200°C for extruder",
                              "pid_temp_hint", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_calibrating_heater_, buf_calibrating_heater_,
                              "Extruder PID Tuning", "pid_calibrating_heater", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_pid_kp_, buf_pid_kp_, "0.000", "pid_kp", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_pid_ki_, buf_pid_ki_, "0.000", "pid_ki", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_pid_kd_, buf_pid_kd_, "0.000", "pid_kd", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_result_summary_, buf_result_summary_,
                              "Temperature control has been optimized.", "pid_result_summary",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_error_message_, buf_error_message_,
                              "An error occurred during calibration.", "pid_error_message",
                              subjects_);

    // Int subject: 1 when extruder selected, 0 when bed selected (controls fan/preset visibility)
    UI_MANAGED_SUBJECT_INT(subj_heater_is_extruder_, 1, "pid_heater_is_extruder", subjects_);

    // Int subject: 1 when not idle (disables Start button in header)
    UI_MANAGED_SUBJECT_INT(subj_cal_not_idle_, 0, "pid_cal_not_idle", subjects_);

    // Progress tracking for calibration
    UI_MANAGED_SUBJECT_INT(subj_pid_progress_, 0, "pid_cal_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(subj_pid_progress_text_, buf_pid_progress_text_, "Starting...",
                              "pid_progress_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(subj_pid_eta_, buf_pid_eta_, "", "pid_cal_eta", subjects_);

    // MPC-related subjects
    UI_MANAGED_SUBJECT_INT(subj_is_kalico_, 0, "cal_is_kalico", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_method_is_mpc_, 0, "cal_method_is_mpc", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_show_wattage_, 0, "cal_show_wattage", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_needs_migration_, 0, "cal_needs_migration", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_show_fan_config_, 0, "cal_show_fan_config", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_fan_is_quick_, 1, "cal_fan_is_quick", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_fan_is_detailed_, 0, "cal_fan_is_detailed", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_fan_is_thorough_, 0, "cal_fan_is_thorough", subjects_);
    UI_MANAGED_SUBJECT_INT(subj_show_pid_fan_, 1, "cal_show_pid_fan", subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_fan_speed_text_, buf_fan_speed_text_, "0%", "cal_fan_speed_text",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(subj_wattage_display_, buf_wattage_display_, "50W",
                              "cal_wattage_display", subjects_);

    // MPC result subjects
    UI_MANAGED_SUBJECT_STRING(subj_mpc_heat_capacity_, buf_mpc_heat_capacity_, "",
                              "mpc_block_heat_capacity", subjects_);
    UI_MANAGED_SUBJECT_STRING(subj_mpc_sensor_resp_, buf_mpc_sensor_resp_, "",
                              "mpc_sensor_responsiveness", subjects_);
    UI_MANAGED_SUBJECT_STRING(subj_mpc_ambient_transfer_, buf_mpc_ambient_transfer_, "",
                              "mpc_ambient_transfer", subjects_);
    UI_MANAGED_SUBJECT_STRING(subj_mpc_fan_transfer_, buf_mpc_fan_transfer_, "",
                              "mpc_fan_ambient_transfer", subjects_);

    subjects_initialized_ = true;

    // Register shutdown cleanup to prevent crashes during lv_deinit().
    // Test the pointer instead of calling get_global_pid_cal_panel(): this
    // callback runs from StaticSubjectRegistry::deinit_all(), which is sequenced
    // AFTER StaticPanelRegistry::destroy_all() has already destroyed the panel.
    // The auto-creating getter would build a replacement whose destructor then
    // runs during static destruction, with LVGL and spdlog already gone.
    StaticSubjectRegistry::instance().register_deinit("PIDCalibrationPanel", []() {
        if (auto* panel = existing_pid_cal_panel()) {
            panel->deinit_subjects();
        }
    });

    // Register XML event callbacks (once globally)
    if (!s_callbacks_registered) {
        register_xml_callbacks({
            {"on_pid_heater_extruder", on_heater_extruder_clicked},
            {"on_pid_heater_bed", on_heater_bed_clicked},
            {"on_pid_temp_up", on_temp_up},
            {"on_pid_temp_down", on_temp_down},
            {"on_pid_start", on_start_clicked},
            {"on_pid_abort", on_abort_clicked},
            {"on_pid_done", on_done_clicked},
            {"on_pid_retry", on_retry_clicked},
            // Material preset callbacks — one per heater, slot read from the
            // button name (replaces 8 per-material trampolines).
            {"on_pid_preset_material", on_pid_preset_material},
            {"on_pid_preset_bed_material", on_pid_preset_bed_material},
            // MPC method/config callbacks
            {"on_cal_method_pid", on_method_pid_clicked},
            {"on_cal_method_mpc", on_method_mpc_clicked},
            {"on_cal_wattage_up", on_wattage_up},
            {"on_cal_wattage_down", on_wattage_down},
            {"on_cal_fan_quick", on_fan_quick_clicked},
            {"on_cal_fan_detailed", on_fan_detailed_clicked},
            {"on_cal_fan_thorough", on_fan_thorough_clicked},
        });
        s_callbacks_registered = true;
    }

    spdlog::debug("[PIDCal] Subjects and callbacks registered");
}

void PIDCalibrationPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[PIDCal] Subjects deinitialized");
}

// ============================================================================
// CREATE / SETUP
// ============================================================================

lv_obj_t* PIDCalibrationPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[PIDCal] Overlay already created");
        return overlay_root_;
    }

    parent_screen_ = parent;

    spdlog::debug("[PIDCal] Creating overlay from XML");

    // Create from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "calibration_pid_panel", nullptr));
    if (!overlay_root_) {
        spdlog::error("[PIDCal] Failed to create panel from XML");
        return nullptr;
    }

    // Initially hidden (will be shown by show())
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Setup widget references
    setup_widgets();

    spdlog::info("[PIDCal] Overlay created");
    return overlay_root_;
}

void PIDCalibrationPanel::setup_widgets() {
    if (!overlay_root_) {
        spdlog::error("[PIDCal] NULL overlay_root_");
        return;
    }

    // Fan speed slider — imperative lv_obj_add_event_cb is required here because
    // XML event_cb does not support VALUE_CHANGED events (continuous slider updates).
    fan_slider_ = lv_obj_find_by_name(overlay_root_, "fan_speed_slider");
    if (fan_slider_) {
        lv_obj_add_event_cb(fan_slider_, on_fan_slider_changed, LV_EVENT_VALUE_CHANGED, this);
    }

    // Event callbacks are registered via XML <event_cb> elements
    // State visibility is controlled via subject binding in XML

    // Set initial state
    set_state(State::IDLE);
    update_temp_display();
    update_temp_hint();

    spdlog::debug("[PIDCal] Widget setup complete");
}

// ============================================================================
// SHOW
// ============================================================================

void PIDCalibrationPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[PIDCal] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[PIDCal] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[PIDCal] Overlay shown");
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void PIDCalibrationPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[PIDCal] on_activate()");

    // Reset to idle state with default values
    set_state(State::IDLE);
    selected_heater_ = Heater::EXTRUDER;
    target_temp_ = EXTRUDER_DEFAULT_TEMP;
    fan_speed_ = 0;
    selected_material_.clear();
    has_old_values_ = false;
    update_fan_slider(0);
    lv_subject_set_int(&subj_heater_is_extruder_, 1);

    update_temp_display();
    update_temp_hint();

    // Reset MPC state
    selected_method_ = CalibMethod::PID;
    lv_subject_set_int(&subj_method_is_mpc_, 0);
    lv_subject_set_int(&subj_show_wattage_, 0);
    lv_subject_set_int(&subj_needs_migration_, 0);
    lv_subject_set_int(&subj_is_kalico_, 0);
    fan_breakpoints_ = FAN_BP_QUICK;
    lv_subject_set_int(&subj_fan_is_quick_, 1);
    lv_subject_set_int(&subj_fan_is_detailed_, 0);
    lv_subject_set_int(&subj_fan_is_thorough_, 0);
    heater_wattage_ = WATTAGE_DEFAULT_EXTRUDER;
    update_wattage_display();
    needs_migration_ = false;
    is_kalico_ = false;

    update_fan_section_visibility();

    // Fetch current PID values now (while no gcode traffic) for delta display later
    fetch_old_pid_values();

    // Check PrinterDiscovery for Kalico detection (primary source)
    if (get_printer_state().get_capability_overrides().is_kalico()) {
        is_kalico_ = true;
        // Only expose MPC UI to beta users
        lv_subject_t* beta = lv_xml_get_subject(nullptr, "show_beta_features");
        if (beta && lv_subject_get_int(beta) == 1) {
            lv_subject_set_int(&subj_is_kalico_, 1);
        }
    }

    // Detect heater control type (only relevant for Kalico/MPC)
    if (api_ && is_kalico_) {
        detect_heater_control_type();
    }

    // Demo mode: inject results after on_activate() finishes its reset
    if (demo_inject_pending_) {
        demo_inject_pending_ = false;
        inject_demo_results();
    }
}

void PIDCalibrationPanel::on_deactivate() {
    spdlog::debug("[PIDCal] on_deactivate()");

    // Stop progress tracking
    stop_progress_tracking();

    // Teardown graph before deactivating
    teardown_pid_graph();

    // Turn off fan if it was running
    turn_off_fan();

    // If calibration is in progress, abort it
    if (state_ == State::CALIBRATING) {
        spdlog::info("[PIDCal] Aborting calibration on deactivate");
        EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);
        if (api_) {
            api_->execute_gcode("TURN_OFF_HEATERS", nullptr, nullptr);
        }
    }

    // Call base class
    OverlayBase::on_deactivate();
}

void PIDCalibrationPanel::cleanup() {
    spdlog::debug("[PIDCal] Cleaning up");

    // Stop progress tracking before cleanup
    stop_progress_tracking();

    // Teardown graph before cleanup
    teardown_pid_graph();

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Clear slider references
    fan_slider_ = nullptr;

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();

    // Clear references
    parent_screen_ = nullptr;
}

// ============================================================================
// FAN CONTROL
// ============================================================================

void PIDCalibrationPanel::turn_off_fan() {
    if (fan_speed_ > 0 && api_) {
        api_->execute_gcode("M107", nullptr, nullptr);
        spdlog::debug("[PIDCal] Fan turned off after calibration");
    }
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void PIDCalibrationPanel::set_state(State new_state) {
    spdlog::debug("[PIDCal] State change: {} -> {}", static_cast<int>(state_),
                  static_cast<int>(new_state));

    // Teardown graph when leaving CALIBRATING state
    if (state_ == State::CALIBRATING && new_state != State::CALIBRATING) {
        teardown_pid_graph();
    }

    state_ = new_state;

    // Update subjects - XML bindings handle visibility automatically
    // State mapping: 0=IDLE, 1=CALIBRATING, 2=SAVING, 3=COMPLETE, 4=ERROR, 5=MIGRATING
    lv_subject_set_int(&s_pid_cal_state, static_cast<int>(new_state));
    // Disable Start button in header when not idle
    lv_subject_set_int(&subj_cal_not_idle_, new_state != State::IDLE ? 1 : 0);

    // Setup graph and progress tracking when entering CALIBRATING state
    if (new_state == State::CALIBRATING) {
        setup_pid_graph();
        // Reset progress
        cal_start_tick_ms_ = lv_tick_get();
        pid_estimated_total_ = 3;
        has_kalico_progress_ = false;
        lv_subject_set_int(&subj_pid_progress_, 0);
        lv_subject_copy_string(&subj_pid_progress_text_, lv_tr("Starting..."));
        lv_subject_copy_string(&subj_pid_eta_, "");
        start_progress_tracking();
    } else {
        cal_start_tick_ms_ = 0;
        stop_progress_tracking();
    }
}

// ============================================================================
// UI UPDATES
// ============================================================================

void PIDCalibrationPanel::update_fan_slider(int speed) {
    if (fan_slider_)
        lv_slider_set_value(fan_slider_, speed, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", speed);
    lv_subject_copy_string(&subj_fan_speed_text_, buf);
}

void PIDCalibrationPanel::format_pid_value(char* buf, size_t buf_size, float new_val,
                                           float old_val) {
    if (has_old_values_ && old_val > 0.001f) {
        float pct = ((new_val - old_val) / old_val) * 100.0f;
        snprintf(buf, buf_size, "%.3f (%+.0f%%)", new_val, pct);
    } else {
        snprintf(buf, buf_size, "%.3f", new_val);
    }
}

void PIDCalibrationPanel::update_wattage_display() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%dW", heater_wattage_);
    lv_subject_copy_string(&subj_wattage_display_, buf);
}

void PIDCalibrationPanel::update_fan_section_visibility() {
    bool is_extruder = (selected_heater_ == Heater::EXTRUDER);
    bool is_mpc = (selected_method_ == CalibMethod::MPC);
    lv_subject_set_int(&subj_show_pid_fan_, (is_extruder && !is_mpc) ? 1 : 0);
    lv_subject_set_int(&subj_show_fan_config_, (is_extruder && is_mpc) ? 1 : 0);
}

void PIDCalibrationPanel::update_temp_display() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d°C", target_temp_);
    lv_subject_copy_string(&subj_temp_display_, buf);
}

void PIDCalibrationPanel::update_temp_hint() {
    if (!selected_material_.empty()) {
        auto mat = filament::find_material(selected_material_);
        if (mat) {
            char hint[64];
            if (selected_heater_ == Heater::EXTRUDER) {
                snprintf(hint, sizeof(hint),
                         "%s: %d-%d\xC2\xB0"
                         "C range",
                         selected_material_.c_str(), mat->nozzle_min, mat->nozzle_max);
            } else {
                snprintf(hint, sizeof(hint),
                         "%s: bed temp %d\xC2\xB0"
                         "C",
                         selected_material_.c_str(), mat->bed_temp);
            }
            lv_subject_copy_string(&subj_temp_hint_, hint);
            return;
        }
    }
    lv_subject_copy_string(&subj_temp_hint_, "Select a material or adjust temperature");
}

// ============================================================================
// TEMPERATURE GRAPH
// ============================================================================

void PIDCalibrationPanel::set_temp_control_panel(TemperatureService* tcp) {
    temp_control_panel_ = tcp;
    spdlog::trace("[{}] TemperatureService set", get_name());
}

void PIDCalibrationPanel::setup_pid_graph() {
    if (pid_graph_)
        return; // Already set up

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "pid_temp_graph_container");
    if (!container) {
        spdlog::warn("[{}] pid_temp_graph_container not found", get_name());
        return;
    }

    pid_graph_ = ui_temp_graph_create(container);
    if (!pid_graph_) {
        spdlog::error("[{}] Failed to create PID temp graph", get_name());
        return;
    }

    // Size chart to fill container
    lv_obj_t* chart = ui_temp_graph_get_chart(pid_graph_);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(100));

    // Configure for PID calibration view
    bool is_extruder = (selected_heater_ == Heater::EXTRUDER);
    float max_temp = is_extruder ? 300.0f : 150.0f;
    ui_temp_graph_set_temp_range(pid_graph_, 0.0f, max_temp);
    ui_temp_graph_set_point_count(pid_graph_, 300); // 5 min at 1Hz
    ui_temp_graph_set_y_axis(pid_graph_, is_extruder ? 100.0f : 50.0f, true);
    ui_temp_graph_set_axis_size(pid_graph_, "xs");

    // Add single series for the active heater
    const char* heater_name = is_extruder ? "Nozzle" : "Bed";
    lv_color_t color = is_extruder ? lv_color_hex(0xFF4444) : lv_color_hex(0x00CED1);
    pid_graph_series_id_ = ui_temp_graph_add_series(pid_graph_, heater_name, color);

    if (pid_graph_series_id_ >= 0) {
        // Show target temperature line
        ui_temp_graph_set_series_target(pid_graph_, pid_graph_series_id_,
                                        static_cast<float>(target_temp_), true);

        // Register with TemperatureService for live updates
        if (temp_control_panel_) {
            std::string klipper_heater = is_extruder ? "extruder" : "heater_bed";
            temp_control_panel_->register_heater_graph(pid_graph_, pid_graph_series_id_,
                                                       klipper_heater);
        }
    }

    spdlog::debug("[{}] PID temp graph created for {}", get_name(), heater_name);
}

void PIDCalibrationPanel::teardown_pid_graph() {
    if (!pid_graph_)
        return;

    // Unregister from TemperatureService first
    if (temp_control_panel_) {
        temp_control_panel_->unregister_heater_graph(pid_graph_);
    }

    ui_temp_graph_destroy(pid_graph_);
    pid_graph_ = nullptr;
    pid_graph_series_id_ = -1;

    spdlog::debug("[{}] PID temp graph destroyed", get_name());
}

// ============================================================================
// GCODE COMMANDS
// ============================================================================

void PIDCalibrationPanel::send_pid_calibrate() {
    if (!api_) {
        spdlog::error("[PIDCal] No IMoonrakerAPI");
        on_calibration_result(false, 0, 0, 0, "No printer connection");
        return;
    }

    const char* heater_name = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";

    // Set fan speed before calibration (extruder only)
    if (selected_heater_ == Heater::EXTRUDER && fan_speed_ > 0 && api_) {
        char fan_cmd[32];
        snprintf(fan_cmd, sizeof(fan_cmd), "M106 S%d", fan_speed_ * 255 / 100);
        spdlog::info("[PIDCal] Setting fan: {}", fan_cmd);
        api_->execute_gcode(fan_cmd, nullptr, nullptr);
    }

    // Update calibrating state label
    const char* label = (selected_heater_ == Heater::EXTRUDER) ? lv_tr("Extruder PID Tuning")
                                                               : lv_tr("Heated Bed PID Tuning");
    lv_subject_copy_string(&subj_calibrating_heater_, label);

    spdlog::info("[PIDCal] Starting PID calibration: {} at {}°C", heater_name, target_temp_);

    auto token = lifetime_.token();
    api_->advanced().start_pid_calibrate(
        heater_name, target_temp_,
        [this, token](float kp, float ki, float kd) {
            if (token.expired())
                return;
            // Callback from background thread - marshal to UI thread
            token.defer([this, kp, ki, kd]() {
                // Ignore results if user already aborted
                if (state_ != State::CALIBRATING) {
                    spdlog::info("[PIDCal] Ignoring PID result (state={}, user likely aborted)",
                                 static_cast<int>(state_));
                    return;
                }
                turn_off_fan();
                on_calibration_result(true, kp, ki, kd);
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            std::string msg = err.message;
            token.defer([this, msg]() {
                if (state_ != State::CALIBRATING) {
                    spdlog::info("[PIDCal] Ignoring PID error (state={}, user likely aborted)",
                                 static_cast<int>(state_));
                    return;
                }
                turn_off_fan();
                on_calibration_result(false, 0, 0, 0, msg);
            });
        },
        [this, token](int sample, float tolerance) {
            if (token.expired())
                return;
            token.defer([this, sample, tolerance]() { on_pid_progress(sample, tolerance); });
        });
}

void PIDCalibrationPanel::send_save_config() {
    if (!api_)
        return;

    // SAVE_CONFIG triggers an expected Klipper restart
    helix::ui::begin_expected_klippy_restart("Saving config... Klipper will restart.");

    spdlog::info("[PIDCal] Sending SAVE_CONFIG");
    auto token = lifetime_.token();
    api_->advanced().save_config(
        [this, token]() {
            if (token.expired())
                return;
            token.defer([this]() {
                if (state_ == State::SAVING) {
                    set_state(State::COMPLETE);
                }
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            std::string msg = err.message;
            token.defer([this, msg]() {
                // Still show results even if save fails
                spdlog::warn("[PIDCal] Save config failed: {}", msg);
                if (state_ == State::SAVING) {
                    set_state(State::COMPLETE);
                }
            });
        });
}

// ============================================================================
// FETCH OLD PID VALUES
// ============================================================================

void PIDCalibrationPanel::fetch_old_pid_values() {
    has_old_values_ = false;
    if (!api_) {
        spdlog::debug("[PIDCal] fetch_old_pid_values: no API, bailing");
        return;
    }

    const char* heater_name = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";
    spdlog::debug("[PIDCal] Fetching old PID values for '{}'", heater_name);

    auto token = lifetime_.token();
    api_->advanced().get_heater_pid_values(
        heater_name,
        [this, token](float kp, float ki, float kd) {
            if (token.expired())
                return;
            token.defer([this, kp, ki, kd]() {
                old_kp_ = kp;
                old_ki_ = ki;
                old_kd_ = kd;
                has_old_values_ = true;
                spdlog::debug("[PIDCal] Got old PID values: Kp={:.3f} Ki={:.3f} Kd={:.3f}", kp, ki,
                              kd);
            });
        },
        [heater_name](const MoonrakerError& err) {
            spdlog::warn("[PIDCal] Failed to fetch old PID for '{}': {}", heater_name, err.message);
        });
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void PIDCalibrationPanel::handle_heater_extruder_clicked() {
    if (state_ != State::IDLE)
        return;

    spdlog::debug("[PIDCal] Extruder selected");
    selected_heater_ = Heater::EXTRUDER;
    target_temp_ = EXTRUDER_DEFAULT_TEMP;
    selected_material_.clear();
    lv_subject_set_int(&subj_heater_is_extruder_, 1);
    update_temp_display();
    update_temp_hint();
    update_fan_section_visibility();
    fetch_old_pid_values();

    // Update MPC defaults for extruder
    if (is_kalico_) {
        heater_wattage_ = WATTAGE_DEFAULT_EXTRUDER;
        update_wattage_display();
        detect_heater_control_type();
    }
}

void PIDCalibrationPanel::handle_heater_bed_clicked() {
    if (state_ != State::IDLE)
        return;

    spdlog::debug("[PIDCal] Heated bed selected");
    selected_heater_ = Heater::BED;
    target_temp_ = BED_DEFAULT_TEMP;
    selected_material_.clear();
    fan_speed_ = 0;
    update_fan_slider(0);
    lv_subject_set_int(&subj_heater_is_extruder_, 0);
    update_temp_display();
    update_temp_hint();
    update_fan_section_visibility();
    fetch_old_pid_values();

    // Update MPC defaults for bed (higher wattage, no fan config)
    if (is_kalico_) {
        heater_wattage_ = WATTAGE_DEFAULT_BED;
        update_wattage_display();
        update_fan_section_visibility();
        detect_heater_control_type();
    }
}

void PIDCalibrationPanel::handle_temp_up() {
    if (state_ != State::IDLE)
        return;

    int max_temp = (selected_heater_ == Heater::EXTRUDER) ? EXTRUDER_MAX_TEMP : BED_MAX_TEMP;

    if (target_temp_ < max_temp) {
        target_temp_ += 5;
        selected_material_.clear();
        update_temp_display();
        update_temp_hint();
    }
}

void PIDCalibrationPanel::handle_temp_down() {
    if (state_ != State::IDLE)
        return;

    int min_temp = (selected_heater_ == Heater::EXTRUDER) ? EXTRUDER_MIN_TEMP : BED_MIN_TEMP;

    if (target_temp_ > min_temp) {
        target_temp_ -= 5;
        selected_material_.clear();
        update_temp_display();
        update_temp_hint();
    }
}

void PIDCalibrationPanel::handle_start_clicked() {
    spdlog::debug("[PIDCal] Start clicked (method={})",
                  selected_method_ == CalibMethod::MPC ? "MPC" : "PID");
    if (selected_method_ == CalibMethod::MPC) {
        if (needs_migration_) {
            set_state(State::MIGRATING);
            start_migration();
        } else {
            set_state(State::CALIBRATING);
            send_mpc_calibrate();
        }
    } else {
        set_state(State::CALIBRATING);
        send_pid_calibrate();
    }
}

void PIDCalibrationPanel::handle_abort_clicked() {
    spdlog::info("[PIDCal] Abort clicked, sending emergency stop + firmware restart");

    // E-stop + firmware restart: klippy comes back, so this is an expected
    // reconnect, not a fault
    helix::ui::begin_expected_klippy_restart("Firmware restarting...");

    // M112 emergency stop halts immediately at MCU level (bypasses blocked gcode queue),
    // then firmware restart brings Klipper back online
    if (api_) {
        api_->emergency_stop(
            [this]() {
                spdlog::debug("[PIDCal] Emergency stop sent, sending firmware restart");
                if (api_) {
                    api_->restart_firmware(
                        []() { spdlog::debug("[PIDCal] Firmware restart initiated"); },
                        [](const MoonrakerError& err) {
                            spdlog::warn("[PIDCal] Firmware restart failed: {}", err.message);
                        });
                }
            },
            [](const MoonrakerError& err) {
                spdlog::warn("[PIDCal] Emergency stop failed: {}", err.message);
            });
    }

    set_state(State::IDLE);
}

void PIDCalibrationPanel::handle_preset_clicked(int temp, const char* material_name) {
    if (state_ != State::IDLE)
        return;

    spdlog::debug("[PIDCal] Preset: {} at {}°C", material_name, temp);
    target_temp_ = temp;
    selected_material_ = material_name;
    update_temp_display();
    update_temp_hint();
}

void PIDCalibrationPanel::handle_done_clicked() {
    spdlog::debug("[PIDCal] Done clicked");
    set_state(State::IDLE);
    NavigationManager::instance().go_back();
}

void PIDCalibrationPanel::handle_retry_clicked() {
    spdlog::debug("[PIDCal] Retry clicked");
    set_state(State::IDLE);
}

// ============================================================================
// PUBLIC METHODS
// ============================================================================

void PIDCalibrationPanel::on_calibration_result(bool success, float kp, float ki, float kd,
                                                const std::string& error_message) {
    if (success) {
        // Set progress to 100% on completion
        progress_tracker_.mark_complete();
        lv_subject_set_int(&subj_pid_progress_, 100);
        lv_subject_copy_string(&subj_pid_progress_text_, lv_tr("Complete!"));

        // Persist measured rates for future calibrations
        save_calibration_history();

        // Store results
        result_kp_ = kp;
        result_ki_ = ki;
        result_kd_ = kd;

        spdlog::debug("[PIDCal] on_calibration_result: has_old_values_={} old_kp_={:.3f}",
                      has_old_values_, old_kp_);

        // Format values with delta if old values are available
        char val_buf[32];
        format_pid_value(val_buf, sizeof(val_buf), kp, old_kp_);
        lv_subject_copy_string(&subj_pid_kp_, val_buf);

        format_pid_value(val_buf, sizeof(val_buf), ki, old_ki_);
        lv_subject_copy_string(&subj_pid_ki_, val_buf);

        format_pid_value(val_buf, sizeof(val_buf), kd, old_kd_);
        lv_subject_copy_string(&subj_pid_kd_, val_buf);

        // Set human-readable result summary. One whole sentence with both
        // variables as placeholders, and the heater name translated too -- the
        // bare snprintf here left the entire line English in all nine locales.
        const char* heater_label =
            (selected_heater_ == Heater::EXTRUDER) ? lv_tr("extruder") : lv_tr("heated bed");
        const std::string summary = fmt::format(
            lv_tr("Temperature control optimized for {} at {}°C."), heater_label, target_temp_);
        lv_subject_copy_string(&subj_result_summary_, summary.c_str());

        // Save config (will transition to COMPLETE when done)
        set_state(State::SAVING);
        send_save_config();
    } else {
        lv_subject_copy_string(&subj_error_message_, error_message.c_str());
        set_state(State::ERROR);
    }
}

// ============================================================================
// DEMO INJECTION
// ============================================================================

void PIDCalibrationPanel::inject_demo_results() {
    spdlog::info("[PIDCal] Injecting demo results for screenshot mode");

    // Configure heater selection and target
    selected_heater_ = Heater::EXTRUDER;
    target_temp_ = 200;
    lv_subject_set_int(&subj_heater_is_extruder_, 1);

    // Simulate having old PID values (90% of new) for delta display
    has_old_values_ = true;
    old_kp_ = 20.579f; // ~90% of 22.865
    old_ki_ = 1.163f;  // ~90% of 1.292
    old_kd_ = 91.060f; // ~90% of 101.178

    // Mock extruder PID values (from moonraker_client_mock.cpp:1421-1423)
    float kp = 22.865f;
    float ki = 1.292f;
    float kd = 101.178f;

    result_kp_ = kp;
    result_ki_ = ki;
    result_kd_ = kd;

    // Format values with delta percentages
    char val_buf[32];
    format_pid_value(val_buf, sizeof(val_buf), kp, old_kp_);
    lv_subject_copy_string(&subj_pid_kp_, val_buf);

    format_pid_value(val_buf, sizeof(val_buf), ki, old_ki_);
    lv_subject_copy_string(&subj_pid_ki_, val_buf);

    format_pid_value(val_buf, sizeof(val_buf), kd, old_kd_);
    lv_subject_copy_string(&subj_pid_kd_, val_buf);

    // Set descriptive labels
    lv_subject_copy_string(&subj_calibrating_heater_, lv_tr("Extruder PID Tuning"));
    lv_subject_copy_string(&subj_result_summary_,
                           "Temperature control optimized for extruder at 200\xC2\xB0"
                           "C.");

    // Go directly to COMPLETE (skip SAVING)
    set_state(State::COMPLETE);
}

// ============================================================================
// PROGRESS HANDLER
// ============================================================================

void PIDCalibrationPanel::on_pid_progress(int sample, float tolerance) {
    // First sample callback: switch to Kalico mode
    if (!has_kalico_progress_) {
        has_kalico_progress_ = true;
        spdlog::info("[PIDCal] Kalico sample progress detected");
    }

    // Dynamically adjust estimated total
    if (sample >= pid_estimated_total_) {
        pid_estimated_total_ = sample + 1;
    }

    progress_tracker_.on_kalico_sample(sample, pid_estimated_total_);
    update_progress_display();

    spdlog::debug("[PIDCal] Progress: sample={}/{} tolerance={:.3f} bar={}%", sample,
                  pid_estimated_total_, tolerance, progress_tracker_.progress_percent());
}

// ============================================================================
// PHASE-AWARE PROGRESS TRACKING
// ============================================================================

void PIDCalibrationPanel::start_progress_tracking() {
    stop_progress_tracking();

    // Read current temperature as starting point
    auto& state = get_printer_state();
    lv_subject_t* temp_subj = nullptr;

    auto heater_type = (selected_heater_ == Heater::EXTRUDER) ? PidProgressTracker::Heater::EXTRUDER
                                                              : PidProgressTracker::Heater::BED;

    if (selected_heater_ == Heater::EXTRUDER) {
        temp_subj = state.get_extruder_temp_subject("extruder", progress_temp_lifetime_);
    } else {
        temp_subj = state.get_bed_temp_subject(progress_temp_lifetime_);
    }

    if (!temp_subj) {
        spdlog::warn("[PIDCal] Could not get temperature subject for progress tracking");
        return;
    }

    float current_temp = helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(temp_subj));
    progress_tracker_.start(heater_type, target_temp_, current_temp);

    // Load historical data if available
    auto* config = helix::Config::get_instance();
    if (config) {
        const char* heater_key = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";

        // Heat rate from shared ThermalRateManager (ensure loaded from config)
        auto& thermal_mgr = ThermalRateManager::instance();
        thermal_mgr.load_from_config(*config);
        float heat_rate = thermal_mgr.get_model(heater_key).best_rate();

        // Oscillation duration from PID-specific config path
        std::string osc_path =
            std::string("/calibration/pid_history/") + heater_key + "/oscillation_duration";
        float osc_dur = config->get<float>(osc_path, 0.0f);
        if (heat_rate > 0 && osc_dur > 0) {
            progress_tracker_.set_history(heat_rate, osc_dur);
            spdlog::info("[PIDCal] Loaded historical rates: heat={:.2f} s/deg, osc={:.0f}s",
                         heat_rate, osc_dur);
        }
    }

    // Observe temperature for phase tracking (both extruder and bed need lifetime tokens)
    progress_temp_observer_ = helix::ui::observe_int_sync<PIDCalibrationPanel>(
        temp_subj, this,
        [](PIDCalibrationPanel* self, int value) { self->on_progress_temperature(value); },
        progress_temp_lifetime_);

    // Start ETA refresh timer (every 3 seconds)
    eta_update_timer_ = lv_timer_create(on_eta_timer_tick, 1000, this);

    // Show initial ETA estimate immediately
    update_progress_display();

    spdlog::info("[PIDCal] Progress tracking started (target={}C, start={:.1f}C)", target_temp_,
                 current_temp);
}

void PIDCalibrationPanel::stop_progress_tracking() {
    // Reset lifetime BEFORE observer (CLAUDE.md mandatory ordering)
    progress_temp_lifetime_.reset();
    progress_temp_observer_.reset();

    cancel_eta_timer();
}

void PIDCalibrationPanel::arm_eta_timer_for_test() {
    cancel_eta_timer();
    eta_update_timer_ = lv_timer_create(on_eta_timer_tick, 1000, this);
}

void PIDCalibrationPanel::cancel_eta_timer() {
    // Neuter rather than delete: the tick runs inside lv_timer_handler, where
    // deleting a timer can corrupt the list (#750, #751). lv_timer_cancel_safe()
    // also no-ops once LVGL is gone, so the destructor can share this.
    if (eta_update_timer_ && lv_is_initialized()) {
        helix::ui::lv_timer_cancel_safe(eta_update_timer_);
    }
    eta_update_timer_ = nullptr;
}

void PIDCalibrationPanel::on_progress_temperature(int temp_tenths) {
    if (state_ != State::CALIBRATING)
        return;

    float temp = helix::ui::temperature::deci_to_degrees_f(temp_tenths);
    uint32_t now = lv_tick_get();

    progress_tracker_.on_temperature(temp, now);
    update_progress_display();
}

void PIDCalibrationPanel::update_progress_display() {
    int progress = progress_tracker_.progress_percent();
    // Cap at 95% until actual completion
    if (progress > 95 && progress_tracker_.phase() != PidProgressTracker::Phase::COMPLETE)
        progress = 95;
    lv_subject_set_int(&subj_pid_progress_, progress);

    // Overtime backstop (#988): once a run exceeds its generous per-heater threshold,
    // reassure the user that the printer may still be working and point at the existing
    // Abort button, instead of leaving them staring at a stalled progress bar. The RPC
    // timeout no longer hard-fails, so this is the only "something may be wrong" signal.
    const uint32_t overtime_ms =
        (selected_heater_ == Heater::EXTRUDER) ? PID_OVERTIME_EXTRUDER_MS : PID_OVERTIME_BED_MS;
    if (cal_start_tick_ms_ != 0 && lv_tick_elaps(cal_start_tick_ms_) > overtime_ms) {
        lv_subject_copy_string(&subj_pid_progress_text_,
                               lv_tr("Still working... taking longer than usual"));
        lv_subject_copy_string(&subj_pid_eta_, lv_tr("Press Abort to stop"));
        return;
    }

    // Status text
    char buf[64];
    auto phase = progress_tracker_.phase();
    if (phase == PidProgressTracker::Phase::HEATING) {
        snprintf(buf, sizeof(buf),
                 lv_tr("Heating to %d\xC2\xB0"
                       "C... %.0f\xC2\xB0"
                       "C"),
                 target_temp_, progress_tracker_.last_temp());
    } else if (phase == PidProgressTracker::Phase::OSCILLATING) {
        if (has_kalico_progress_) {
            snprintf(buf, sizeof(buf), lv_tr("Sample %d/%d"), progress_tracker_.kalico_sample(),
                     pid_estimated_total_);
        } else {
            snprintf(buf, sizeof(buf), lv_tr("Oscillating... cycle %d/%d"),
                     progress_tracker_.oscillation_count(), PidProgressTracker::EXPECTED_CYCLES);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", lv_tr("Starting..."));
    }
    lv_subject_copy_string(&subj_pid_progress_text_, buf);

    // ETA text
    auto eta = progress_tracker_.eta_seconds();
    if (eta.has_value() && *eta > 0) {
        int mins = *eta / 60;
        int secs = *eta % 60;
        char eta_buf[32];
        snprintf(eta_buf, sizeof(eta_buf), "~%d:%02d %s", mins, secs, lv_tr("remaining"));
        lv_subject_copy_string(&subj_pid_eta_, eta_buf);
    } else if (progress >= 95) {
        lv_subject_copy_string(&subj_pid_eta_, lv_tr("Finishing up..."));
    } else {
        lv_subject_copy_string(&subj_pid_eta_, lv_tr("Estimating..."));
    }
}

void PIDCalibrationPanel::on_eta_timer_tick(lv_timer_t* timer) {
    auto* self = static_cast<PIDCalibrationPanel*>(lv_timer_get_user_data(timer));
    if (self->state_ != State::CALIBRATING)
        return;
    self->update_progress_display();
}

// ============================================================================
// HISTORICAL CALIBRATION DATA PERSISTENCE
// ============================================================================

void PIDCalibrationPanel::save_calibration_history() {
    auto* config = helix::Config::get_instance();
    if (!config)
        return;

    float heat_rate = progress_tracker_.measured_heat_rate();
    float osc_duration = progress_tracker_.measured_oscillation_duration();

    // Only save if we have meaningful measurements
    if (heat_rate <= 0 && osc_duration <= 0)
        return;

    const char* heater_key = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";

    // Save heat rate to shared path at /thermal/rates/{heater}/heat_rate
    // The progress tracker's thermal model has both history and measured data,
    // so blended_rate_for_save() handles the 70/30 new/old weighting.
    if (heat_rate > 0) {
        const auto& pid_model = progress_tracker_.thermal_model();
        float blended = pid_model.blended_rate_for_save();
        if (blended <= 0)
            blended = heat_rate;
        std::string heat_path = "/thermal/rates/" + std::string(heater_key) + "/heat_rate";
        config->set<float>(heat_path, blended);
    }

    // Save oscillation duration to PID-specific config path (with blending)
    std::string osc_path =
        std::string("/calibration/pid_history/") + heater_key + "/oscillation_duration";
    float old_osc = config->get<float>(osc_path, 0.0f);
    float new_osc = osc_duration;
    if (old_osc > 0 && osc_duration > 0) {
        new_osc = 0.7f * osc_duration + 0.3f * old_osc;
    }
    if (new_osc > 0) {
        config->set<float>(osc_path, new_osc);
    }
    config->save();

    spdlog::info("[PIDCal] Saved calibration history for {}: heat_rate={:.2f} s/deg, osc={:.0f}s",
                 heater_key, heat_rate, new_osc);
}

// ============================================================================
// MPC: DETECTION, MIGRATION, CALIBRATION
// ============================================================================

void PIDCalibrationPanel::detect_heater_control_type() {
    if (!api_)
        return;

    const char* heater = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";
    spdlog::debug("[PIDCal] Querying heater control type for '{}'", heater);

    auto token = lifetime_.token();
    api_->advanced().get_heater_control_type(
        heater,
        [this, token](const std::string& type) {
            if (token.expired())
                return;
            token.defer([this, type]() {
                // Query succeeded, firmware supports control type query (Kalico)
                is_kalico_ = true;
                // Only expose MPC UI to beta users
                lv_subject_t* beta = lv_xml_get_subject(nullptr, "show_beta_features");
                if (beta && lv_subject_get_int(beta) == 1) {
                    lv_subject_set_int(&subj_is_kalico_, 1);
                }

                if (type == "mpc") {
                    // Already MPC, no migration needed
                    selected_method_ = CalibMethod::MPC;
                    needs_migration_ = false;
                    lv_subject_set_int(&subj_method_is_mpc_, 1);
                    lv_subject_set_int(&subj_needs_migration_, 0);
                    lv_subject_set_int(&subj_show_wattage_, 0);
                    update_fan_section_visibility();
                    spdlog::info("[PIDCal] Heater already using MPC control");
                } else {
                    // PID mode — MPC needs migration, pre-select MPC (recommended)
                    selected_method_ = CalibMethod::MPC;
                    needs_migration_ = true;
                    lv_subject_set_int(&subj_method_is_mpc_, 1);
                    lv_subject_set_int(&subj_needs_migration_, 1);
                    lv_subject_set_int(&subj_show_wattage_, 1);
                    update_fan_section_visibility();
                    spdlog::info("[PIDCal] Heater using '{}' control, MPC migration available",
                                 type);
                }
            });
        },
        [this, token](const MoonrakerError&) {
            if (token.expired())
                return;
            // Can't determine control type, not Kalico — default to PID
            token.defer([this]() {
                is_kalico_ = false;
                lv_subject_set_int(&subj_is_kalico_, 0);
                spdlog::debug("[PIDCal] Heater control type query failed, defaulting to PID");
            });
        });
}

void PIDCalibrationPanel::start_migration() {
    if (!api_)
        return;

    const char* section = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";
    std::vector<helix::system::ConfigEdit> edits = {
        {helix::system::ConfigEdit::Type::SET_VALUE, "control", "mpc"},
        {helix::system::ConfigEdit::Type::ADD_KEY, "heater_power", std::to_string(heater_wattage_)},
    };

    spdlog::info("[PIDCal] Starting PID->MPC migration for '{}' with heater_power={}W", section,
                 heater_wattage_);

    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::EXTRA);

    auto token = lifetime_.token();
    config_editor_.safe_multi_edit(
        *api_, section, edits,
        [this, token]() {
            if (token.expired())
                return;
            token.defer([this]() {
                needs_migration_ = false;
                lv_subject_set_int(&subj_needs_migration_, 0);
                spdlog::info("[PIDCal] Migration complete, starting MPC calibration");
                set_state(State::CALIBRATING);
                send_mpc_calibrate();
            });
        },
        [this, token](const std::string& err) {
            if (token.expired())
                return;
            token.defer([this, err]() {
                spdlog::error("[PIDCal] Migration failed: {}", err);
                lv_subject_copy_string(&subj_error_message_, err.c_str());
                set_state(State::ERROR);
            });
        },
        30000);
}

void PIDCalibrationPanel::send_mpc_calibrate() {
    if (!api_) {
        spdlog::error("[PIDCal] No IMoonrakerAPI for MPC calibration");
        lv_subject_copy_string(&subj_error_message_, "No printer connection");
        set_state(State::ERROR);
        return;
    }

    const char* heater = (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heater_bed";
    const char* label = (selected_heater_ == Heater::EXTRUDER)
                            ? lv_tr("Extruder MPC Calibration")
                            : lv_tr("Heated Bed MPC Calibration");
    lv_subject_copy_string(&subj_calibrating_heater_, label);

    spdlog::info("[PIDCal] Starting MPC calibration: {} at {}°C, fan_breakpoints={}", heater,
                 target_temp_, fan_breakpoints_);

    auto token = lifetime_.token();
    api_->advanced().start_mpc_calibrate(
        heater, target_temp_, fan_breakpoints_,
        [this, token](const MoonrakerAdvancedAPI::MPCResult& result) {
            if (token.expired())
                return;
            token.defer([this, result]() {
                if (state_ != State::CALIBRATING)
                    return;
                on_mpc_result(result);
            });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            std::string msg = err.message;
            token.defer([this, msg]() {
                if (state_ != State::CALIBRATING)
                    return;
                spdlog::error("[PIDCal] MPC calibration failed: {}", msg);
                lv_subject_copy_string(&subj_error_message_, msg.c_str());
                set_state(State::ERROR);
            });
        },
        [this, token](int phase, int total, const std::string& desc) {
            if (token.expired())
                return;
            token.defer([this, phase, total, desc]() { on_mpc_progress(phase, total, desc); });
        });
}

void PIDCalibrationPanel::on_mpc_result(const MoonrakerAdvancedAPI::MPCResult& result) {
    mpc_result_ = result;
    lv_subject_set_int(&subj_pid_progress_, 100);
    lv_subject_copy_string(&subj_pid_progress_text_, lv_tr("Complete!"));

    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f J/K", result.block_heat_capacity);
    lv_subject_copy_string(&subj_mpc_heat_capacity_, buf);
    snprintf(buf, sizeof(buf), "%.6f K/s/K", result.sensor_responsiveness);
    lv_subject_copy_string(&subj_mpc_sensor_resp_, buf);
    snprintf(buf, sizeof(buf), "%.6f W/K", result.ambient_transfer);
    lv_subject_copy_string(&subj_mpc_ambient_transfer_, buf);
    lv_subject_copy_string(&subj_mpc_fan_transfer_, result.fan_ambient_transfer.c_str());

    const char* heater_label =
        (selected_heater_ == Heater::EXTRUDER) ? lv_tr("extruder") : lv_tr("heated bed");
    const std::string summary = fmt::format(lv_tr("MPC thermal model calibrated for {} at {}°C."),
                                            heater_label, target_temp_);
    lv_subject_copy_string(&subj_result_summary_, summary.c_str());

    spdlog::info("[PIDCal] MPC result: heat_cap={:.4f} sensor_resp={:.6f} ambient={:.6f} fan='{}'",
                 result.block_heat_capacity, result.sensor_responsiveness, result.ambient_transfer,
                 result.fan_ambient_transfer);

    set_state(State::SAVING);
    send_save_config();
}

void PIDCalibrationPanel::on_mpc_progress(int phase, int total_phases, const std::string& desc) {
    if (!has_kalico_progress_) {
        has_kalico_progress_ = true;
        spdlog::info("[PIDCal] MPC phase progress detected");
    }

    int progress = 0;
    if (total_phases > 0) {
        progress = (phase * 100) / total_phases;
    }
    if (progress > 95)
        progress = 95;
    lv_subject_set_int(&subj_pid_progress_, progress);
    lv_subject_copy_string(&subj_pid_progress_text_, desc.c_str());

    spdlog::debug("[PIDCal] MPC progress: phase={}/{} desc='{}' bar={}%", phase, total_phases, desc,
                  progress);
}

// ============================================================================
// MPC: METHOD / WATTAGE / FAN HANDLERS
// ============================================================================

void PIDCalibrationPanel::handle_method_pid_clicked() {
    if (state_ != State::IDLE)
        return;
    spdlog::debug("[PIDCal] PID method selected");
    selected_method_ = CalibMethod::PID;
    needs_migration_ = false;
    lv_subject_set_int(&subj_method_is_mpc_, 0);
    lv_subject_set_int(&subj_show_wattage_, 0);
    lv_subject_set_int(&subj_needs_migration_, 0);
    update_fan_section_visibility();
}

void PIDCalibrationPanel::handle_method_mpc_clicked() {
    if (state_ != State::IDLE)
        return;
    spdlog::debug("[PIDCal] MPC method selected");
    selected_method_ = CalibMethod::MPC;
    lv_subject_set_int(&subj_method_is_mpc_, 1);
    update_fan_section_visibility();
    // Re-detect to determine migration needs
    detect_heater_control_type();
}

void PIDCalibrationPanel::handle_wattage_up() {
    if (state_ != State::IDLE)
        return;
    if (heater_wattage_ < WATTAGE_MAX) {
        heater_wattage_ += WATTAGE_STEP;
        update_wattage_display();
        spdlog::debug("[PIDCal] Wattage: {}W", heater_wattage_);
    }
}

void PIDCalibrationPanel::handle_wattage_down() {
    if (state_ != State::IDLE)
        return;
    if (heater_wattage_ > WATTAGE_MIN) {
        heater_wattage_ -= WATTAGE_STEP;
        update_wattage_display();
        spdlog::debug("[PIDCal] Wattage: {}W", heater_wattage_);
    }
}

void PIDCalibrationPanel::handle_fan_quick_clicked() {
    if (state_ != State::IDLE)
        return;
    fan_breakpoints_ = FAN_BP_QUICK;
    lv_subject_set_int(&subj_fan_is_quick_, 1);
    lv_subject_set_int(&subj_fan_is_detailed_, 0);
    lv_subject_set_int(&subj_fan_is_thorough_, 0);
    spdlog::debug("[PIDCal] Fan breakpoints: {} (Quick)", fan_breakpoints_);
}

void PIDCalibrationPanel::handle_fan_detailed_clicked() {
    if (state_ != State::IDLE)
        return;
    fan_breakpoints_ = FAN_BP_DETAILED;
    lv_subject_set_int(&subj_fan_is_quick_, 0);
    lv_subject_set_int(&subj_fan_is_detailed_, 1);
    lv_subject_set_int(&subj_fan_is_thorough_, 0);
    spdlog::debug("[PIDCal] Fan breakpoints: {} (Detailed)", fan_breakpoints_);
}

void PIDCalibrationPanel::handle_fan_thorough_clicked() {
    if (state_ != State::IDLE)
        return;
    fan_breakpoints_ = FAN_BP_THOROUGH;
    lv_subject_set_int(&subj_fan_is_quick_, 0);
    lv_subject_set_int(&subj_fan_is_detailed_, 0);
    lv_subject_set_int(&subj_fan_is_thorough_, 1);
    spdlog::debug("[PIDCal] Fan breakpoints: {} (Thorough)", fan_breakpoints_);
}

// ============================================================================
// STATIC TRAMPOLINES (for XML event_cb)
// ============================================================================

void PIDCalibrationPanel::on_heater_extruder_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_heater_extruder_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_heater_extruder_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_heater_bed_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_heater_bed_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_heater_bed_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_temp_up(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_temp_up");
    (void)e;
    get_global_pid_cal_panel().handle_temp_up();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_temp_down(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_temp_down");
    (void)e;
    get_global_pid_cal_panel().handle_temp_down();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_start_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_start_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_start_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_abort_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_abort_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_abort_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_done_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_done_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_done_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_retry_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_retry_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_retry_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_fan_slider_changed(lv_event_t* e) {
    auto* panel = static_cast<PIDCalibrationPanel*>(lv_event_get_user_data(e));
    if (!panel)
        return;
    auto* slider = lv_event_get_target_obj(e);
    int speed = lv_slider_get_value(slider);
    panel->fan_speed_ = speed;
    panel->update_fan_slider(speed);
    spdlog::debug("[PIDCal] Fan speed set to {}%", speed);
}

// Helper: look up recommended temp from filament database
static int get_material_nozzle_temp(const char* name) {
    auto mat = filament::find_material(name);
    return mat ? mat->nozzle_recommended() : 200;
}

static int get_material_bed_temp(const char* name) {
    auto mat = filament::find_material(name);
    return mat ? mat->bed_temp : 60;
}

// Material preset handlers — index-parameterized.
//
// These were eight near-identical trampolines, one per hardcoded material
// (PLA/PETG/ABS/PA/TPU for the extruder, PLA/PETG/ABS for the bed), each
// differing only in the string it looked a temperature up with. They are now
// two handlers that read the preset SLOT from the clicked button's name and
// resolve the material through helix::presets, so the PID panel offers exactly
// the materials the user configured instead of a fixed list that disagreed with
// every other preset surface in the app.

/// Parse the preset slot from a button named "btn_preset_mN" / "btn_preset_bed_mN".
/// Returns -1 if the name is missing or malformed.
static int pid_preset_slot(lv_event_t* e) {
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const char* name = btn ? lv_obj_get_name(btn) : nullptr;
    if (!name) {
        spdlog::warn("[PIDCal] Preset button has no name; ignoring click");
        return -1;
    }
    const char* suffix = std::strrchr(name, '_');
    if (!suffix || suffix[1] != 'm' || !std::isdigit(static_cast<unsigned char>(suffix[2]))) {
        spdlog::warn("[PIDCal] Preset button '{}' has no slot suffix; ignoring click", name);
        return -1;
    }
    int slot = suffix[2] - '0';
    if (slot < 0 || slot >= helix::presets::PRESET_COUNT) {
        spdlog::warn("[PIDCal] Preset button '{}' slot {} out of range", name, slot);
        return -1;
    }
    return slot;
}

void PIDCalibrationPanel::on_pid_preset_material(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_material");
    int slot = pid_preset_slot(e);
    if (slot >= 0) {
        const std::string material = helix::presets::name(slot);
        get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp(material.c_str()),
                                                         material.c_str());
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_bed_material(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_bed_material");
    int slot = pid_preset_slot(e);
    if (slot >= 0) {
        const std::string material = helix::presets::name(slot);
        get_global_pid_cal_panel().handle_preset_clicked(get_material_bed_temp(material.c_str()),
                                                         material.c_str());
    }
    LVGL_SAFE_EVENT_CB_END();
}

// MPC method/config trampolines
void PIDCalibrationPanel::on_method_pid_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_method_pid_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_method_pid_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_method_mpc_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_method_mpc_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_method_mpc_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_wattage_up(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_wattage_up");
    (void)e;
    get_global_pid_cal_panel().handle_wattage_up();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_wattage_down(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_wattage_down");
    (void)e;
    get_global_pid_cal_panel().handle_wattage_down();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_fan_quick_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_fan_quick_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_fan_quick_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_fan_detailed_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_fan_detailed_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_fan_detailed_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_fan_thorough_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_fan_thorough_clicked");
    (void)e;
    get_global_pid_cal_panel().handle_fan_thorough_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<PIDCalibrationPanel> g_pid_cal_panel;

PIDCalibrationPanel& get_global_pid_cal_panel() {
    if (!g_pid_cal_panel) {
        g_pid_cal_panel = std::make_unique<PIDCalibrationPanel>();
        StaticPanelRegistry::instance().register_destroy("PIDCalibrationPanel",
                                                         []() { g_pid_cal_panel.reset(); });
    }
    return *g_pid_cal_panel;
}

static PIDCalibrationPanel* existing_pid_cal_panel() {
    return g_pid_cal_panel.get();
}
