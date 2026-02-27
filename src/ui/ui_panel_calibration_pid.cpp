// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_calibration_pid.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_temp_control.h"
#include "ui_update_queue.h"

#include "filament_database.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <lvgl.h>
#include <memory>

// ============================================================================
// STATIC SUBJECT
// ============================================================================

// State subject (0=IDLE, 1=CALIBRATING, 2=SAVING, 3=COMPLETE, 4=ERROR)
static lv_subject_t s_pid_cal_state;
static bool s_callbacks_registered = false;

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

    spdlog::trace("[PIDCal] Instance created");
}

PIDCalibrationPanel::~PIDCalibrationPanel() {
    // Applying [L011]: No mutex in destructors
    // Applying [L041]: deinit_subjects() as first line in destructor
    deinit_subjects();

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

    subjects_initialized_ = true;

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
            // Material preset callbacks
            {"on_pid_preset_pla", on_pid_preset_pla},
            {"on_pid_preset_petg", on_pid_preset_petg},
            {"on_pid_preset_abs", on_pid_preset_abs},
            {"on_pid_preset_pa", on_pid_preset_pa},
            {"on_pid_preset_tpu", on_pid_preset_tpu},
            {"on_pid_preset_bed_pla", on_pid_preset_bed_pla},
            {"on_pid_preset_bed_petg", on_pid_preset_bed_petg},
            {"on_pid_preset_bed_abs", on_pid_preset_bed_abs},
        });
        s_callbacks_registered = true;
    }

    spdlog::debug("[PIDCal] Subjects and callbacks registered");
}

void PIDCalibrationPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Deinitialize all subjects via SubjectManager (Applying [L041])
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

    // Fan speed slider
    fan_slider_ = lv_obj_find_by_name(overlay_root_, "fan_speed_slider");
    fan_speed_label_ = lv_obj_find_by_name(overlay_root_, "fan_speed_label");
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

    // Fetch current PID values now (while no gcode traffic) for delta display later
    fetch_old_pid_values();

    // Demo mode: inject results after on_activate() finishes its reset
    if (demo_inject_pending_) {
        demo_inject_pending_ = false;
        inject_demo_results();
    }
}

void PIDCalibrationPanel::on_deactivate() {
    spdlog::debug("[PIDCal] on_deactivate()");

    // Stop fallback timer
    stop_fallback_progress_timer();

    // Teardown graph before deactivating
    teardown_pid_graph();

    // Turn off fan if it was running
    turn_off_fan();

    // If calibration is in progress, abort it
    if (state_ == State::CALIBRATING) {
        spdlog::info("[PIDCal] Aborting calibration on deactivate");
        if (api_) {
            api_->execute_gcode("TURN_OFF_HEATERS", nullptr, nullptr);
        }
    }

    // Call base class
    OverlayBase::on_deactivate();
}

void PIDCalibrationPanel::cleanup() {
    spdlog::debug("[PIDCal] Cleaning up");

    // Stop fallback timer before cleanup
    stop_fallback_progress_timer();

    // Teardown graph before cleanup
    teardown_pid_graph();

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Clear slider references
    fan_slider_ = nullptr;
    fan_speed_label_ = nullptr;

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
    // State mapping: 0=IDLE, 1=CALIBRATING, 2=SAVING, 3=COMPLETE, 4=ERROR
    lv_subject_set_int(&s_pid_cal_state, static_cast<int>(new_state));
    // Disable Start button in header when not idle
    lv_subject_set_int(&subj_cal_not_idle_, new_state != State::IDLE ? 1 : 0);

    // Setup graph when entering CALIBRATING state
    if (new_state == State::CALIBRATING) {
        setup_pid_graph();
        // Reset progress
        pid_estimated_total_ = 3;
        has_kalico_progress_ = false;
        lv_subject_set_int(&subj_pid_progress_, 0);
        lv_subject_copy_string(&subj_pid_progress_text_, lv_tr("Starting..."));
        start_fallback_progress_timer();
    } else {
        stop_fallback_progress_timer();
    }
}

// ============================================================================
// UI UPDATES
// ============================================================================

void PIDCalibrationPanel::update_fan_slider(int speed) {
    if (fan_slider_)
        lv_slider_set_value(fan_slider_, speed, LV_ANIM_OFF);
    if (fan_speed_label_) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", speed);
        lv_label_set_text(fan_speed_label_, buf);
    }
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
    const char* hint = "Select a material or adjust temperature";
    lv_subject_copy_string(&subj_temp_hint_, hint);
}

// ============================================================================
// TEMPERATURE GRAPH
// ============================================================================

void PIDCalibrationPanel::set_temp_control_panel(TempControlPanel* tcp) {
    temp_control_panel_ = tcp;
    spdlog::trace("[{}] TempControlPanel set", get_name());
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

        // Register with TempControlPanel for live updates
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

    // Unregister from TempControlPanel first
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
        spdlog::error("[PIDCal] No MoonrakerAPI");
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
    const char* label =
        (selected_heater_ == Heater::EXTRUDER) ? "Extruder PID Tuning" : "Heated Bed PID Tuning";
    lv_subject_copy_string(&subj_calibrating_heater_, label);

    spdlog::info("[PIDCal] Starting PID calibration: {} at {}°C", heater_name, target_temp_);

    api_->advanced().start_pid_calibrate(
        heater_name, target_temp_,
        [this](float kp, float ki, float kd) {
            // Callback from background thread - marshal to UI thread
            helix::ui::queue_update([this, kp, ki, kd]() {
                if (cleanup_called())
                    return;
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
        [this](const MoonrakerError& err) {
            std::string msg = err.message;
            helix::ui::queue_update([this, msg]() {
                if (cleanup_called())
                    return;
                if (state_ != State::CALIBRATING) {
                    spdlog::info("[PIDCal] Ignoring PID error (state={}, user likely aborted)",
                                 static_cast<int>(state_));
                    return;
                }
                turn_off_fan();
                on_calibration_result(false, 0, 0, 0, msg);
            });
        },
        [this](int sample, float tolerance) {
            helix::ui::queue_update([this, sample, tolerance]() {
                if (cleanup_called())
                    return;
                on_pid_progress(sample, tolerance);
            });
        });
}

void PIDCalibrationPanel::send_save_config() {
    if (!api_)
        return;

    // Suppress recovery modal — SAVE_CONFIG triggers an expected Klipper restart
    EmergencyStopOverlay::instance().suppress_recovery_dialog(15000);

    spdlog::info("[PIDCal] Sending SAVE_CONFIG");
    api_->advanced().save_config(
        [this]() {
            helix::ui::queue_update([this]() {
                if (cleanup_called())
                    return;
                if (state_ == State::SAVING) {
                    set_state(State::COMPLETE);
                }
            });
        },
        [this](const MoonrakerError& err) {
            std::string msg = err.message;
            helix::ui::queue_update([this, msg]() {
                if (cleanup_called())
                    return;
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

    api_->advanced().get_heater_pid_values(
        heater_name,
        [this](float kp, float ki, float kd) {
            old_kp_ = kp;
            old_ki_ = ki;
            old_kd_ = kd;
            has_old_values_ = true;
            spdlog::debug("[PIDCal] Got old PID values: Kp={:.3f} Ki={:.3f} Kd={:.3f}", kp, ki, kd);
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
    fetch_old_pid_values();
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
    fetch_old_pid_values();
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
    spdlog::debug("[PIDCal] Start clicked");
    set_state(State::CALIBRATING);
    send_pid_calibrate();
}

void PIDCalibrationPanel::handle_abort_clicked() {
    spdlog::info("[PIDCal] Abort clicked, sending emergency stop + firmware restart");

    // Suppress recovery modal — E-stop + restart triggers expected reconnect
    EmergencyStopOverlay::instance().suppress_recovery_dialog(15000);

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
        lv_subject_set_int(&subj_pid_progress_, 100);
        lv_subject_copy_string(&subj_pid_progress_text_, lv_tr("Complete!"));

        // Store results
        result_kp_ = kp;
        result_ki_ = ki;
        result_kd_ = kd;

        // Format values with delta if old values are available
        auto format_pid_value = [this](char* buf, size_t buf_size, float new_val, float old_val) {
            if (has_old_values_ && old_val > 0.001f) {
                float pct = ((new_val - old_val) / old_val) * 100.0f;
                snprintf(buf, buf_size, "%.3f (%+.0f%%)", new_val, pct);
            } else {
                snprintf(buf, buf_size, "%.3f", new_val);
            }
        };

        spdlog::debug("[PIDCal] on_calibration_result: has_old_values_={} old_kp_={:.3f}",
                      has_old_values_, old_kp_);

        char val_buf[32];
        format_pid_value(val_buf, sizeof(val_buf), kp, old_kp_);
        lv_subject_copy_string(&subj_pid_kp_, val_buf);

        format_pid_value(val_buf, sizeof(val_buf), ki, old_ki_);
        lv_subject_copy_string(&subj_pid_ki_, val_buf);

        format_pid_value(val_buf, sizeof(val_buf), kd, old_kd_);
        lv_subject_copy_string(&subj_pid_kd_, val_buf);

        // Set human-readable result summary
        const char* heater_label =
            (selected_heater_ == Heater::EXTRUDER) ? "extruder" : "heated bed";
        char summary[128];
        snprintf(summary, sizeof(summary), "Temperature control optimized for %s at %d°C.",
                 heater_label, target_temp_);
        lv_subject_copy_string(&subj_result_summary_, summary);

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

    // Format values with delta percentages (same pattern as on_calibration_result)
    auto format_pid_value = [this](char* buf, size_t buf_size, float new_val, float old_val) {
        if (has_old_values_ && old_val > 0.001f) {
            float pct = ((new_val - old_val) / old_val) * 100.0f;
            snprintf(buf, buf_size, "%.3f (%+.0f%%)", new_val, pct);
        } else {
            snprintf(buf, buf_size, "%.3f", new_val);
        }
    };

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
    // First sample callback: switch from fallback to Kalico progress mode
    if (!has_kalico_progress_) {
        has_kalico_progress_ = true;
        stop_fallback_progress_timer();
        spdlog::info("[PIDCal] Kalico sample progress detected, switching to precise mode");
    }

    // Dynamically adjust estimated total
    if (sample >= pid_estimated_total_) {
        pid_estimated_total_ = sample + 1;
    }

    // Calculate progress percentage, cap at 95% (100% only on completion)
    int progress = (sample * 100) / pid_estimated_total_;
    if (progress > 95)
        progress = 95;

    lv_subject_set_int(&subj_pid_progress_, progress);

    // Update progress text
    char buf[32];
    snprintf(buf, sizeof(buf), "Sample %d/%d", sample, pid_estimated_total_);
    lv_subject_copy_string(&subj_pid_progress_text_, buf);

    spdlog::debug("[PIDCal] Progress: sample={}/{} tolerance={:.3f} bar={}%", sample,
                  pid_estimated_total_, tolerance, progress);
}

// ============================================================================
// FALLBACK PROGRESS TIMER (for standard Klipper without sample callbacks)
// ============================================================================

void PIDCalibrationPanel::start_fallback_progress_timer() {
    stop_fallback_progress_timer();
    fallback_cycle_ = 0;

    // Tick every 15 seconds — PID calibration takes ~3-10 minutes
    uint32_t tick_ms = (selected_heater_ == Heater::EXTRUDER) ? 13500 : 15000;
    progress_fallback_timer_ = lv_timer_create(on_fallback_progress_tick, tick_ms, this);

    // Fire once immediately after a short delay to show "Heating to target..."
    lv_timer_t* initial = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PIDCalibrationPanel*>(lv_timer_get_user_data(t));
            if (!self->has_kalico_progress_ && self->state_ == State::CALIBRATING) {
                lv_subject_set_int(&self->subj_pid_progress_, 5);
                lv_subject_copy_string(&self->subj_pid_progress_text_,
                                       lv_tr("Heating to target..."));
            }
            lv_timer_delete(t);
        },
        3000, this);
    lv_timer_set_repeat_count(initial, 1);
}

void PIDCalibrationPanel::stop_fallback_progress_timer() {
    if (progress_fallback_timer_) {
        lv_timer_delete(progress_fallback_timer_);
        progress_fallback_timer_ = nullptr;
    }
}

void PIDCalibrationPanel::on_fallback_progress_tick(lv_timer_t* timer) {
    auto* self = static_cast<PIDCalibrationPanel*>(lv_timer_get_user_data(timer));
    if (self->has_kalico_progress_ || self->state_ != State::CALIBRATING)
        return;

    self->fallback_cycle_++;

    // Slowly advance progress bar: asymptotic approach to 90%
    // Each tick adds less: 15, 25, 33, 40, 46, 51, 55, ...
    int progress = 90 - (90 * 100) / (100 + self->fallback_cycle_ * 30);
    if (progress > 90)
        progress = 90;
    lv_subject_set_int(&self->subj_pid_progress_, progress);

    // Cycle through helpful messages
    const char* messages[] = {
        "Oscillating around target...",
        "Measuring thermal response...",
        "Tuning control parameters...",
        "Refining stability...",
    };
    int msg_idx = (self->fallback_cycle_ - 1) % 4;
    lv_subject_copy_string(&self->subj_pid_progress_text_, messages[msg_idx]);

    spdlog::debug("[PIDCal] Fallback progress: cycle={} bar={}%", self->fallback_cycle_, progress);
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

// Material preset trampolines (extruder) — temps from filament database
void PIDCalibrationPanel::on_pid_preset_pla(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_pla");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp("PLA"), "PLA");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_petg(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_petg");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp("PETG"), "PETG");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_abs(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_abs");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp("ABS"), "ABS");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_pa(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_pa");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp("PA"), "PA");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_tpu(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_tpu");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_nozzle_temp("TPU"), "TPU");
    LVGL_SAFE_EVENT_CB_END();
}

// Material preset trampolines (bed) — temps from filament database
void PIDCalibrationPanel::on_pid_preset_bed_pla(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_bed_pla");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_bed_temp("PLA"), "PLA");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_bed_petg(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_bed_petg");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_bed_temp("PETG"), "PETG");
    LVGL_SAFE_EVENT_CB_END();
}

void PIDCalibrationPanel::on_pid_preset_bed_abs(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PIDCal] on_pid_preset_bed_abs");
    (void)e;
    get_global_pid_cal_panel().handle_preset_clicked(get_material_bed_temp("ABS"), "ABS");
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

void destroy_pid_cal_panel() {
    g_pid_cal_panel.reset();
}
