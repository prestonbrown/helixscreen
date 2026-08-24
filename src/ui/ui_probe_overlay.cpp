// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_probe_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "moonraker_advanced_api.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "probe_sensor_types.h"
#include "static_panel_registry.h"
#include "toolhead_homing.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

using json = nlohmann::json;

using namespace helix;
using helix::sensors::probe_type_to_display_string;
using helix::sensors::ProbeSensorManager;
using helix::sensors::ProbeSensorType;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<ProbeOverlay> g_probe_overlay;

// Forward declarations
static void on_probe_row_clicked(lv_event_t* e);
IMoonrakerAPI* get_moonraker_api();
IMoonrakerClient* get_moonraker_client();

ProbeOverlay& get_global_probe_overlay() {
    if (!g_probe_overlay) {
        g_probe_overlay = std::make_unique<ProbeOverlay>();
        StaticPanelRegistry::instance().register_destroy("ProbeOverlay",
                                                         []() { g_probe_overlay.reset(); });
    }
    return *g_probe_overlay;
}

ProbeOverlay::~ProbeOverlay() {
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    overlay_root_ = nullptr;
    parent_screen_ = nullptr;

    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[Probe] Destroyed");
    }
}

void init_probe_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_probe_row_clicked", on_probe_row_clicked);
    spdlog::trace("[Probe] Row click callback registered");
}

static void on_probe_row_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Probe] Probe row clicked");

    auto& overlay = get_global_probe_overlay();

    // Lazy-create the probe overlay
    if (!overlay.get_root()) {
        spdlog::debug("[Probe] Creating probe overlay...");

        IMoonrakerAPI* api = get_moonraker_api();
        overlay.set_api(api);

        lv_obj_t* screen = lv_display_get_screen_active(nullptr);
        if (!overlay.create(screen)) {
            spdlog::error("[Probe] Failed to create probe_overlay");
            return;
        }
        spdlog::info("[Probe] Overlay created");
    }

    overlay.show();
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

// Helper to send a GCode command via MoonrakerClient
static void send_probe_gcode(const char* gcode, const char* label) {
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::error("[Probe] No client for {} command", label);
        return;
    }
    spdlog::debug("[Probe] Sending {}: {}", label, gcode);
    client->gcode_script(gcode);
}

void ui_probe_overlay_register_callbacks() {
    register_xml_callbacks({
        // Universal probe actions
        {"on_probe_accuracy",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_probe_accuracy(); }},
        {"on_zoffset_cal",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_zoffset_cal(); }},
        {"on_bed_mesh", [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_bed_mesh(); }},

        // BLTouch controls
        {"on_bltouch_deploy",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=pin_down", "BLTouch Deploy");
         }},
        {"on_bltouch_stow",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=pin_up", "BLTouch Stow");
         }},
        {"on_bltouch_reset",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=reset", "BLTouch Reset");
         }},
        {"on_bltouch_selftest",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=self_test", "BLTouch Self-Test");
         }},
        {"on_bltouch_output_5v",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("SET_BLTOUCH OUTPUT_MODE=5V", "BLTouch Output 5V");
         }},
        {"on_bltouch_output_od",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("SET_BLTOUCH OUTPUT_MODE=OD", "BLTouch Output OD");
         }},

        // Cartographer controls
        {"on_carto_touch_cal",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("CARTOGRAPHER_TOUCH_CALIBRATE", "Cartographer Touch Calibrate");
             ToastManager::instance().show(ToastSeverity::INFO,
                                           lv_tr("Cartographer touch calibrate sent"));
         }},
        {"on_carto_scan_cal",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("CARTOGRAPHER_SCAN_CALIBRATE", "Cartographer Scan Calibrate");
             ToastManager::instance().show(
                 ToastSeverity::INFO,
                 lv_tr("Cartographer scan calibrate sent — use Z-Offset panel to adjust"));
         }},

        // Beacon controls
        {"on_beacon_calibrate",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BEACON_CALIBRATE", "Beacon Calibrate");
             ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Beacon calibrate sent"));
         }},
        {"on_beacon_auto_cal",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BEACON_AUTO_CALIBRATE", "Beacon Auto-Calibrate");
             ToastManager::instance().show(ToastSeverity::INFO,
                                           lv_tr("Beacon auto-calibrate sent"));
         }},

        // Klicky controls
        {"on_klicky_deploy",
         [](lv_event_t* /*e*/) { send_probe_gcode("ATTACH_PROBE", "Klicky Deploy"); }},
        {"on_klicky_dock",
         [](lv_event_t* /*e*/) { send_probe_gcode("DOCK_PROBE", "Klicky Dock"); }},

        // Eddy current probe controls (BTT Eddy, Mellow Fly Eddy, etc.)
        {"on_eddy_calibrate",
         [](lv_event_t* /*e*/) {
             auto& mgr = ProbeSensorManager::instance();
             for (const auto& s : mgr.get_sensors()) {
                 if (s.type == ProbeSensorType::EDDY_CURRENT) {
                     std::string cmd = "PROBE_EDDY_CURRENT_CALIBRATE CHIP=" + s.sensor_name;
                     send_probe_gcode(cmd.c_str(), "Eddy Current Calibrate");
                     return;
                 }
             }
             spdlog::warn("[Probe] No eddy current sensor found for calibrate");
         }},
        {"on_eddy_drive_current",
         [](lv_event_t* /*e*/) {
             auto& mgr = ProbeSensorManager::instance();
             for (const auto& s : mgr.get_sensors()) {
                 if (s.type == ProbeSensorType::EDDY_CURRENT) {
                     std::string cmd = "LDC_CALIBRATE_DRIVE_CURRENT CHIP=" + s.sensor_name;
                     send_probe_gcode(cmd.c_str(), "Eddy Drive Current Cal");
                     return;
                 }
             }
             spdlog::warn("[Probe] No eddy current sensor found for drive current cal");
         }},

        // Config edit callbacks
        {"on_probe_cfg_x_offset",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit(
                 "x_offset", "X Offset", "Horizontal offset from nozzle to probe");
         }},
        {"on_probe_cfg_y_offset",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit("y_offset", "Y Offset",
                                                           "Vertical offset from nozzle to probe");
         }},
        {"on_probe_cfg_samples",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit("samples", "Samples",
                                                           "Number of probe samples per point");
         }},
        {"on_probe_cfg_speed",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit("speed", "Probe Speed",
                                                           "Speed (mm/s) during probing moves");
         }},
        {"on_probe_cfg_retract",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit(
                 "sample_retract_dist", "Retract Distance",
                 "Distance (mm) to retract between samples");
         }},
        {"on_probe_cfg_tolerance",
         [](lv_event_t* /*e*/) {
             get_global_probe_overlay().handle_config_edit(
                 "samples_tolerance", "Samples Tolerance",
                 "Maximum allowed deviation between samples (mm)");
         }},

        // Probe accuracy modal callbacks
        {"on_probe_accuracy_close",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_accuracy_close(); }},
        {"on_probe_accuracy_estop",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_accuracy_estop(); }},

        // Config edit modal buttons
        {"on_probe_config_save",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_config_save(); }},
        {"on_probe_config_cancel",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_config_cancel(); }},
    });

    spdlog::trace("[Probe] Event callbacks registered");
}

// ============================================================================
// OVERLAY LIFECYCLE
// ============================================================================

void ProbeOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Display subjects
    UI_MANAGED_SUBJECT_STRING(probe_display_name_, probe_display_name_buf_, "",
                              "probe_display_name", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_type_label_, probe_type_label_buf_, "", "probe_type_label",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_z_offset_display_, probe_z_offset_display_buf_, "--",
                              "probe_z_offset_display", subjects_);

    // Overlay state (0=normal)
    UI_MANAGED_SUBJECT_INT(probe_overlay_state_, 0, "probe_overlay_state", subjects_);

    // Cartographer subjects
    UI_MANAGED_SUBJECT_STRING(probe_carto_coil_temp_, probe_carto_coil_temp_buf_, "--",
                              "probe_carto_coil_temp", subjects_);

    // Beacon subjects
    UI_MANAGED_SUBJECT_STRING(probe_beacon_temp_, probe_beacon_temp_buf_, "--", "probe_beacon_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_beacon_temp_comp_status_, probe_beacon_temp_comp_status_buf_,
                              "--", "probe_beacon_temp_comp_status", subjects_);

    // Klicky detection subject
    UI_MANAGED_SUBJECT_INT(probe_is_klicky_, 0, "probe_is_klicky", subjects_);

    // Config display subjects
    UI_MANAGED_SUBJECT_INT(probe_config_loaded_, 0, "probe_config_loaded", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_x_offset_, probe_cfg_x_offset_buf_, "--",
                              "probe_cfg_x_offset", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_y_offset_, probe_cfg_y_offset_buf_, "--",
                              "probe_cfg_y_offset", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_samples_, probe_cfg_samples_buf_, "--", "probe_cfg_samples",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_speed_, probe_cfg_speed_buf_, "--", "probe_cfg_speed",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_retract_dist_, probe_cfg_retract_dist_buf_, "--",
                              "probe_cfg_retract_dist", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_tolerance_, probe_cfg_tolerance_buf_, "--",
                              "probe_cfg_tolerance", subjects_);

    // Config edit modal subjects
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_title_, probe_config_edit_title_buf_, "",
                              "probe_config_edit_title", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_desc_, probe_config_edit_desc_buf_, "",
                              "probe_config_edit_desc", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_current_, probe_config_edit_current_buf_, "",
                              "probe_config_edit_current", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_value_, probe_config_edit_value_buf_, "",
                              "probe_config_edit_value", subjects_);

    // Probe accuracy state machine + progress
    UI_MANAGED_SUBJECT_INT(probe_acc_state_, 0, "probe_acc_state", subjects_);
    UI_MANAGED_SUBJECT_INT(probe_acc_progress_, 0, "probe_acc_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_progress_text_, probe_acc_progress_text_buf_,
                              lv_tr("Preparing..."), "probe_acc_progress_text", subjects_);

    // Probe accuracy results modal subjects
    UI_MANAGED_SUBJECT_STRING(probe_acc_maximum_, probe_acc_maximum_buf_, "--", "probe_acc_maximum",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_minimum_, probe_acc_minimum_buf_, "--", "probe_acc_minimum",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_range_, probe_acc_range_buf_, "--", "probe_acc_range",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_average_, probe_acc_average_buf_, "--", "probe_acc_average",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_median_, probe_acc_median_buf_, "--", "probe_acc_median",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_stddev_, probe_acc_stddev_buf_, "--", "probe_acc_stddev",
                              subjects_);

    // Quality assessment + error message
    UI_MANAGED_SUBJECT_INT(probe_acc_quality_, 1, "probe_acc_quality", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_quality_text_, probe_acc_quality_text_buf_, "",
                              "probe_acc_quality_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_acc_error_msg_, probe_acc_error_msg_buf_, "",
                              "probe_acc_error_msg", subjects_);

    subjects_initialized_ = true;
    spdlog::trace("[Probe] Subjects initialized");
}

lv_obj_t* ProbeOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[Probe] Overlay already created");
        return overlay_root_;
    }

    parent_screen_ = parent;

    // Ensure subjects are initialized before XML creation
    if (!subjects_initialized_) {
        init_subjects();
    }

    spdlog::debug("[Probe] Creating overlay from XML");
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "probe_overlay", nullptr));

    if (!overlay_root_) {
        spdlog::error("[Probe] Failed to create overlay from XML");
        return nullptr;
    }

    // Start hidden (push_overlay will show it)
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Cache type panel container for later swapping
    type_panel_container_ = lv_obj_find_by_name(overlay_root_, "probe_type_panel");

    spdlog::info("[Probe] Overlay created successfully");
    return overlay_root_;
}

void ProbeOverlay::show() {
    if (!overlay_root_) {
        spdlog::error("[Probe] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[Probe] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[Probe] Overlay shown");
}

void ProbeOverlay::on_activate() {
    spdlog::debug("[Probe] Activated");

    // Update display subjects from current probe state
    update_display_subjects();

    // Load type-specific panel
    load_type_panel();

    // Load config values from Klipper
    load_config_values();
}

void ProbeOverlay::on_deactivate() {
    spdlog::debug("[Probe] Deactivated");
}

void ProbeOverlay::cleanup() {
    spdlog::trace("[Probe] Cleanup");
}

void ProbeOverlay::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

// ============================================================================
// DISPLAY SUBJECTS
// ============================================================================

void ProbeOverlay::update_display_subjects() {
    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();

    if (sensors.empty()) {
        snprintf(probe_display_name_buf_, sizeof(probe_display_name_buf_), "No Probe Detected");
        lv_subject_copy_string(&probe_display_name_, probe_display_name_buf_);
        lv_subject_copy_string(&probe_type_label_, "");
        snprintf(probe_z_offset_display_buf_, sizeof(probe_z_offset_display_buf_), "--");
        lv_subject_copy_string(&probe_z_offset_display_, probe_z_offset_display_buf_);
        return;
    }

    // Use first sensor (primary probe)
    const auto& sensor = sensors[0];
    std::string display_name = probe_type_to_display_string(sensor.type);
    snprintf(probe_display_name_buf_, sizeof(probe_display_name_buf_), "%s", display_name.c_str());
    lv_subject_copy_string(&probe_display_name_, probe_display_name_buf_);

    // Type description label
    const char* type_label = "Standard Probe";
    switch (sensor.type) {
    case ProbeSensorType::CARTOGRAPHER:
        type_label = "Cartographer 3D Scanner";
        break;
    case ProbeSensorType::BEACON:
        type_label = "Eddy Current Probe";
        break;
    case ProbeSensorType::BLTOUCH:
        type_label = "Servo-Actuated Touch Probe";
        break;
    case ProbeSensorType::TAP:
        type_label = "Nozzle Contact Probe";
        break;
    case ProbeSensorType::KLICKY:
        type_label = "Magnetic Dock Probe";
        break;
    case ProbeSensorType::EDDY_CURRENT:
        type_label = "Eddy Current Probe";
        break;
    case ProbeSensorType::SMART_EFFECTOR:
        type_label = "Piezo Contact Probe";
        break;
    case ProbeSensorType::PRTOUCH_V2:
        type_label = "Creality Pressure Probe";
        break;
    case ProbeSensorType::LOADCELL:
        type_label = "Bed Load Cell Probe";
        break;
    case ProbeSensorType::STANDARD:
    default:
        break;
    }
    snprintf(probe_type_label_buf_, sizeof(probe_type_label_buf_), "%s", type_label);
    lv_subject_copy_string(&probe_type_label_, probe_type_label_buf_);

    // Z offset display
    float z_offset = mgr.get_z_offset();
    snprintf(probe_z_offset_display_buf_, sizeof(probe_z_offset_display_buf_), "%.3fmm", z_offset);
    lv_subject_copy_string(&probe_z_offset_display_, probe_z_offset_display_buf_);

    // Set Klicky detection flag for generic panel visibility binding
    lv_subject_set_int(&probe_is_klicky_, sensor.type == ProbeSensorType::KLICKY ? 1 : 0);

    // Cartographer coil temperature (placeholder until live query)
    if (sensor.type == ProbeSensorType::CARTOGRAPHER) {
        snprintf(probe_carto_coil_temp_buf_, sizeof(probe_carto_coil_temp_buf_), "--");
        lv_subject_copy_string(&probe_carto_coil_temp_, probe_carto_coil_temp_buf_);
    }

    // Beacon sensor temperature (placeholder until live query)
    if (sensor.type == ProbeSensorType::BEACON) {
        snprintf(probe_beacon_temp_buf_, sizeof(probe_beacon_temp_buf_), "--");
        lv_subject_copy_string(&probe_beacon_temp_, probe_beacon_temp_buf_);
        snprintf(probe_beacon_temp_comp_status_buf_, sizeof(probe_beacon_temp_comp_status_buf_),
                 "Unknown");
        lv_subject_copy_string(&probe_beacon_temp_comp_status_, probe_beacon_temp_comp_status_buf_);
    }
}

void ProbeOverlay::set_accuracy_error(const std::string& msg) {
    snprintf(probe_acc_error_msg_buf_, sizeof(probe_acc_error_msg_buf_), "%s", msg.c_str());
    lv_subject_copy_string(&probe_acc_error_msg_, probe_acc_error_msg_buf_);
    lv_subject_set_int(&probe_acc_state_, 3); // ERROR
}

// ============================================================================
// TYPE-SPECIFIC PANEL LOADING
// ============================================================================

void ProbeOverlay::load_type_panel() {
    if (!type_panel_container_) {
        spdlog::warn("[Probe] Type panel container not found");
        return;
    }

    // Clear existing type panel children
    lv_obj_clean(type_panel_container_);

    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();

    if (sensors.empty()) {
        spdlog::debug("[Probe] No sensors, skipping type panel load");
        return;
    }

    const auto& sensor = sensors[0];
    const char* component = nullptr;

    switch (sensor.type) {
    case ProbeSensorType::BLTOUCH:
        component = "probe_bltouch_panel";
        break;
    case ProbeSensorType::CARTOGRAPHER:
        component = "probe_cartographer_panel";
        break;
    case ProbeSensorType::BEACON:
        component = "probe_beacon_panel";
        break;
    case ProbeSensorType::EDDY_CURRENT:
        component = "probe_eddy_panel";
        break;
    default:
        component = "probe_generic_panel";
        break;
    }

    spdlog::debug("[Probe] Loading type panel: {}", component);
    auto* panel = static_cast<lv_obj_t*>(lv_xml_create(type_panel_container_, component, nullptr));
    if (!panel) {
        spdlog::warn("[Probe] Failed to create type panel: {}", component);
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void ProbeOverlay::handle_probe_accuracy() {
    spdlog::debug("[Probe] Probe accuracy test requested");

    if (!api_) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("No printer connection"));
        return;
    }

    // Check homing state — PROBE_ACCURACY requires all axes homed
    PrinterState& ps = get_printer_state();
    const bool all_homed = helix::toolhead_is_homed(ps);

    // PROBE_ACCURACY defaults to 10 samples (not the [probe] config's samples= which is for
    // regular probing). We pass SAMPLES= explicitly so progress tracking matches.
    static constexpr int PROBE_ACCURACY_SAMPLES = 10;

    std::string gcode;
    if (!all_homed) {
        // Diagnostic-only re-fetch — all_homed above already decided the branch.
        const char* homed_dbg = lv_subject_get_string(ps.get_homed_axes_subject());
        spdlog::info("[Probe] Axes not homed (homed_axes='{}'), homing first",
                     homed_dbg ? homed_dbg : "");
        gcode = "G28\n";
    }

    // After homing, the toolhead may be at Z max in a corner. Traveling the full Z range at
    // probe speed (often 2mm/s) is painfully slow, and probing in the corner isn't ideal.
    // Move to center at a reasonable height first at travel speed.
    // Pre-position: move to bed center at a safe Z height before probing.
    // Without this, after G28 the toolhead is at Z max in a corner and PROBE_ACCURACY
    // descends the full range at probe speed (often 2mm/s) — painfully slow.
    const auto& bv = api_->hardware().build_volume();
    if (bv.z_max > 0.0f && bv.x_max > bv.x_min && bv.y_max > bv.y_min) {
        float center_x = (bv.x_min + bv.x_max) / 2.0f;
        float center_y = (bv.y_min + bv.y_max) / 2.0f;
        float safe_z = 30.0f;
        gcode += fmt::format("G1 Z{:.1f} F3000\n", safe_z);
        gcode += fmt::format("G1 X{:.1f} Y{:.1f} F6000\n", center_x, center_y);
        spdlog::info("[Probe] Pre-positioning to center ({:.0f},{:.0f}) Z{:.1f} before accuracy "
                     "test (z_max={:.0f})",
                     center_x, center_y, safe_z, bv.z_max);
    }

    gcode += fmt::format("PROBE_ACCURACY SAMPLES={}", PROBE_ACCURACY_SAMPLES);

    // Reset progress state
    probe_acc_sample_count_ = 0;
    probe_acc_total_samples_ = PROBE_ACCURACY_SAMPLES;

    // Set initial state and show modal
    lv_subject_set_int(&probe_acc_state_, 1); // PROBING
    lv_subject_set_int(&probe_acc_progress_, 0);
    snprintf(probe_acc_progress_text_buf_, sizeof(probe_acc_progress_text_buf_), "%s",
             all_homed ? lv_tr("Preparing...") : lv_tr("Homing first..."));
    lv_subject_copy_string(&probe_acc_progress_text_, probe_acc_progress_text_buf_);

    accuracy_modal_ = Modal::show("probe_accuracy_modal");
    if (!accuracy_modal_) {
        spdlog::error("[Probe] Failed to show accuracy modal");
        lv_subject_set_int(&probe_acc_state_, 0);
        return;
    }

    // Unregister any stale handler from a previous run
    if (!probe_acc_handler_name_.empty()) {
        api_->unregister_method_callback("notify_gcode_response", probe_acc_handler_name_);
    }

    // Subscribe to gcode responses for progress + results
    static int s_handler_id = 0;
    probe_acc_handler_name_ = "probe_accuracy_" + std::to_string(++s_handler_id);

    api_->register_method_callback(
        "notify_gcode_response", probe_acc_handler_name_,
        [handler_name = probe_acc_handler_name_, api = api_](const json& msg) {
            if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
                !msg["params"][0].is_string()) {
                return;
            }
            const std::string& line = msg["params"][0].get_ref<const std::string&>();

            // Check for individual probe sample: "probe at X,Y is z=Z"
            auto z_pos = line.find(" is z=");
            if (z_pos != std::string::npos && line.find("probe at ") != std::string::npos) {
                std::string z_val = line.substr(z_pos + 6); // After " is z="
                // L081_FREEZE_OK: handler is registered when the user taps Probe
                // Accuracy (post-startup); samples are a high-frequency stream where
                // a dropped progress tick is harmless.
                helix::ui::queue_update([z_val]() {
                    auto& overlay = get_global_probe_overlay();
                    overlay.probe_acc_sample_count_++;
                    int current = overlay.probe_acc_sample_count_;
                    int total = overlay.probe_acc_total_samples_;

                    // Update progress bar
                    int pct = total > 0 ? (current * 100 / total) : 0;
                    if (pct > 100)
                        pct = 100;
                    lv_subject_set_int(&overlay.probe_acc_progress_, pct);

                    // Update progress text: "Sample 3 of 10: z=1.2341"
                    snprintf(overlay.probe_acc_progress_text_buf_,
                             sizeof(overlay.probe_acc_progress_text_buf_), "%s %d %s %d: z=%s",
                             lv_tr("Sample"), current, lv_tr("of"), total, z_val.c_str());
                    lv_subject_copy_string(&overlay.probe_acc_progress_text_,
                                           overlay.probe_acc_progress_text_buf_);
                });
                return;
            }

            // Check for final results line
            if (line.find("probe accuracy results:") != std::string::npos) {
                spdlog::info("[Probe] {}", line);
                api->unregister_method_callback("notify_gcode_response", handler_name);

                std::string results = line;
                helix::ui::queue_update(
                    [results]() { get_global_probe_overlay().show_accuracy_results(results); });
                return;
            }

            // Check for errors
            if (line.rfind("!! ", 0) == 0 || line.rfind("Error:", 0) == 0 ||
                line.find("error:") != std::string::npos) {
                spdlog::error("[Probe] PROBE_ACCURACY error: {}", line);
                api->unregister_method_callback("notify_gcode_response", handler_name);

                std::string error_msg = line;
                helix::ui::queue_update(
                    [error_msg]() { get_global_probe_overlay().set_accuracy_error(error_msg); });
            }
        });

    api_->execute_gcode(
        gcode,
        [handler_name = probe_acc_handler_name_]() {
            spdlog::info("[Probe] PROBE_ACCURACY command completed");
        },
        [api = api_, handler_name = probe_acc_handler_name_](const MoonrakerError& err) {
            spdlog::error("[Probe] PROBE_ACCURACY failed: {}", err.user_message());
            api->unregister_method_callback("notify_gcode_response", handler_name);
            std::string msg = err.user_message();
            helix::ui::queue_update(
                [msg]() { get_global_probe_overlay().set_accuracy_error(msg); });
        },
        MoonrakerAdvancedAPI::PROBING_TIMEOUT_MS);
}

void ProbeOverlay::show_accuracy_results(const std::string& results_line) {
    // Parse: "probe accuracy results: maximum 1.234, minimum 1.230, range 0.004,
    //         average 1.232, median 1.232, standard deviation 0.001"
    auto extract_value = [&](const std::string& key) -> std::string {
        auto pos = results_line.find(key);
        if (pos == std::string::npos)
            return "--";
        pos += key.length();
        while (pos < results_line.size() && results_line[pos] == ' ')
            pos++;
        auto end = results_line.find(',', pos);
        if (end == std::string::npos)
            end = results_line.size();
        return results_line.substr(pos, end - pos);
    };

    auto set_subject = [](lv_subject_t* subj, char* buf, size_t buf_size, const std::string& val) {
        snprintf(buf, buf_size, "%s mm", val.c_str());
        lv_subject_copy_string(subj, buf);
    };

    set_subject(&probe_acc_maximum_, probe_acc_maximum_buf_, sizeof(probe_acc_maximum_buf_),
                extract_value("maximum "));
    set_subject(&probe_acc_minimum_, probe_acc_minimum_buf_, sizeof(probe_acc_minimum_buf_),
                extract_value("minimum "));
    set_subject(&probe_acc_range_, probe_acc_range_buf_, sizeof(probe_acc_range_buf_),
                extract_value("range "));
    set_subject(&probe_acc_average_, probe_acc_average_buf_, sizeof(probe_acc_average_buf_),
                extract_value("average "));
    set_subject(&probe_acc_median_, probe_acc_median_buf_, sizeof(probe_acc_median_buf_),
                extract_value("median "));
    set_subject(&probe_acc_stddev_, probe_acc_stddev_buf_, sizeof(probe_acc_stddev_buf_),
                extract_value("standard deviation "));

    // Assess quality based on range value
    std::string range_str = extract_value("range ");
    double range_val = 0.0;
    try {
        range_val = std::stod(range_str);
    } catch (...) {
        range_val = 1.0; // Unknown = treat as poor
    }

    // quality: 1 = good (range < 0.05mm), 0 = poor
    // < 0.01 = excellent (Cartographer, Beacon, good BLTouch)
    // 0.01-0.05 = good (BLTouch, Klicky, prtouch)
    // > 0.05 = poor (check probe mount, nozzle, or bed)
    bool good = range_val < 0.05;
    const char* quality_text = nullptr;
    if (range_val < 0.01) {
        quality_text = lv_tr("Excellent Accuracy");
    } else if (range_val < 0.05) {
        quality_text = lv_tr("Good Accuracy");
    } else {
        quality_text = lv_tr("Poor Accuracy");
    }
    lv_subject_set_int(&probe_acc_quality_, good ? 1 : 0);
    snprintf(probe_acc_quality_text_buf_, sizeof(probe_acc_quality_text_buf_), "%s", quality_text);
    lv_subject_copy_string(&probe_acc_quality_text_, probe_acc_quality_text_buf_);

    // Transition to RESULTS state
    lv_subject_set_int(&probe_acc_progress_, 100);
    lv_subject_set_int(&probe_acc_state_, 2); // RESULTS
}

void ProbeOverlay::handle_accuracy_close() {
    lv_subject_set_int(&probe_acc_state_, 0); // IDLE
    if (accuracy_modal_) {
        Modal::hide(accuracy_modal_);
        accuracy_modal_ = nullptr;
    }
}

void ProbeOverlay::handle_accuracy_estop() {
    spdlog::warn("[Probe] Emergency stop requested during probe accuracy test");

    // Unregister the response handler
    if (api_ && !probe_acc_handler_name_.empty()) {
        api_->unregister_method_callback("notify_gcode_response", probe_acc_handler_name_);
        probe_acc_handler_name_.clear();
    }

    // Send emergency stop
    if (api_) {
        api_->execute_gcode("M112", nullptr, nullptr);
    }

    // Close the modal
    handle_accuracy_close();
}

void ProbeOverlay::handle_zoffset_cal() {
    spdlog::debug("[Probe] Z-Offset calibration requested");

#if defined(HELIX_PLATFORM_ESP32)
    // Secondary entry to Z-Offset calibration (excluded, null-vtable stub on v1).
    helix::ui::show_feature_unavailable_toast();
    return;
#endif

    auto& overlay = get_global_zoffset_cal_panel();

    // Lazy-create z-offset overlay
    if (!overlay.get_root()) {
        overlay.init_subjects();
        overlay.set_api(get_moonraker_api());
        overlay.create(lv_display_get_screen_active(nullptr));
    }

    overlay.show();
}

void ProbeOverlay::handle_bed_mesh() {
    spdlog::debug("[Probe] Bed mesh requested");

#if defined(HELIX_PLATFORM_ESP32)
    // Secondary entry to bed-mesh calibration (excluded, null-vtable stub on v1).
    helix::ui::show_feature_unavailable_toast();
    return;
#endif

    auto& panel = get_global_bed_mesh_panel();

    // Lazy-create bed mesh overlay
    if (!panel.get_root()) {
        if (!panel.are_subjects_initialized()) {
            panel.init_subjects();
        }
        panel.register_callbacks();
        auto* root = panel.create(lv_display_get_screen_active(nullptr));
        if (root) {
            NavigationManager::instance().register_overlay_instance(root, &panel);
        }
    }

    if (panel.get_root()) {
        NavigationManager::instance().push_overlay(panel.get_root());
    }
}

// ============================================================================
// CONFIG VALUE LOADING
// ============================================================================

std::string ProbeOverlay::get_probe_config_section() const {
    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();
    if (sensors.empty())
        return "probe";

    switch (sensors[0].type) {
    case ProbeSensorType::BLTOUCH:
        return "bltouch";
    case ProbeSensorType::SMART_EFFECTOR:
        return "smart_effector";
    case ProbeSensorType::PRTOUCH_V2:
        return "prtouch_v2";
    default:
        return "probe";
    }
}

void ProbeOverlay::load_config_values() {
    if (!api_) {
        spdlog::debug("[Probe] No API, skipping config load");
        return;
    }

    probe_section_ = get_probe_config_section();
    spdlog::debug("[Probe] Loading config values for [{}]", probe_section_);

    api_->query_configfile(
        [this](const json& config) {
            // query_configfile returns the full config object
            // Section names in the config JSON are lowercased
            if (!config.contains(probe_section_)) {
                spdlog::debug("[Probe] Section [{}] not found in config", probe_section_);
                return;
            }

            const auto& section = config[probe_section_];

            // Helper to extract string value and copy to subject buffer
            auto set_cfg = [](const json& sec, const char* key, char* buf, size_t buf_size,
                              lv_subject_t* subject) {
                if (sec.contains(key)) {
                    std::string val = sec[key].get<std::string>();
                    snprintf(buf, buf_size, "%s", val.c_str());
                } else {
                    snprintf(buf, buf_size, "default");
                }
                lv_subject_copy_string(subject, buf);
            };

            lifetime_.defer("ProbeOverlay::load_probe_config", [this, section, set_cfg]() {
                set_cfg(section, "x_offset", probe_cfg_x_offset_buf_,
                        sizeof(probe_cfg_x_offset_buf_), &probe_cfg_x_offset_);
                set_cfg(section, "y_offset", probe_cfg_y_offset_buf_,
                        sizeof(probe_cfg_y_offset_buf_), &probe_cfg_y_offset_);
                set_cfg(section, "samples", probe_cfg_samples_buf_, sizeof(probe_cfg_samples_buf_),
                        &probe_cfg_samples_);
                set_cfg(section, "speed", probe_cfg_speed_buf_, sizeof(probe_cfg_speed_buf_),
                        &probe_cfg_speed_);
                set_cfg(section, "sample_retract_dist", probe_cfg_retract_dist_buf_,
                        sizeof(probe_cfg_retract_dist_buf_), &probe_cfg_retract_dist_);
                set_cfg(section, "samples_tolerance", probe_cfg_tolerance_buf_,
                        sizeof(probe_cfg_tolerance_buf_), &probe_cfg_tolerance_);

                lv_subject_set_int(&probe_config_loaded_, 1);
                spdlog::debug("[Probe] Config values loaded for [{}]", probe_section_);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Probe] Failed to query configfile: {}", err.message);
        });
}

// ============================================================================
// CONFIG EDITING
// ============================================================================

void ProbeOverlay::handle_config_edit(const std::string& field_key, const std::string& title,
                                      const std::string& description) {
    spdlog::debug("[Probe] Config edit requested: {}", field_key);

    editing_field_key_ = field_key;

    // Set modal subjects
    snprintf(probe_config_edit_title_buf_, sizeof(probe_config_edit_title_buf_), "Edit %s",
             title.c_str());
    lv_subject_copy_string(&probe_config_edit_title_, probe_config_edit_title_buf_);

    snprintf(probe_config_edit_desc_buf_, sizeof(probe_config_edit_desc_buf_), "%s",
             description.c_str());
    lv_subject_copy_string(&probe_config_edit_desc_, probe_config_edit_desc_buf_);

    // Get current value from the corresponding display subject
    const char* current_val = "--";
    if (field_key == "x_offset")
        current_val = probe_cfg_x_offset_buf_;
    else if (field_key == "y_offset")
        current_val = probe_cfg_y_offset_buf_;
    else if (field_key == "samples")
        current_val = probe_cfg_samples_buf_;
    else if (field_key == "speed")
        current_val = probe_cfg_speed_buf_;
    else if (field_key == "sample_retract_dist")
        current_val = probe_cfg_retract_dist_buf_;
    else if (field_key == "samples_tolerance")
        current_val = probe_cfg_tolerance_buf_;

    snprintf(probe_config_edit_current_buf_, sizeof(probe_config_edit_current_buf_), "%s",
             current_val);
    lv_subject_copy_string(&probe_config_edit_current_, probe_config_edit_current_buf_);

    // Pre-fill edit value with current
    snprintf(probe_config_edit_value_buf_, sizeof(probe_config_edit_value_buf_), "%s", current_val);
    lv_subject_copy_string(&probe_config_edit_value_, probe_config_edit_value_buf_);

    // Show the modal
    edit_modal_ = Modal::show("probe_config_edit_modal");
    if (!edit_modal_) {
        spdlog::error("[Probe] Failed to show config edit modal");
    }
}

void ProbeOverlay::handle_config_save() {
    if (editing_field_key_.empty()) {
        spdlog::warn("[Probe] No field being edited");
        return;
    }

    // Read the input value from the modal
    if (edit_modal_) {
        auto* input = lv_obj_find_by_name(edit_modal_, "probe_config_input");
        if (input) {
            const char* text = lv_textarea_get_text(input);
            if (text && text[0] != '\0') {
                snprintf(probe_config_edit_value_buf_, sizeof(probe_config_edit_value_buf_), "%s",
                         text);
            }
        }
    }

    std::string new_value = probe_config_edit_value_buf_;
    std::string field = editing_field_key_;
    std::string section = probe_section_;

    spdlog::info("[Probe] Saving config: [{}] {} = {}", section, field, new_value);

    // Close the edit modal
    if (edit_modal_) {
        Modal::hide(edit_modal_);
        edit_modal_ = nullptr;
    }

    if (!api_) {
        spdlog::error("[Probe] No API for config edit");
        return;
    }

    // Use the safe edit flow: backup -> edit -> firmware restart -> monitor -> revert on failure
    config_editor_.load_config_files(
        *api_,
        [this, section, field,
         new_value](std::map<std::string, helix::system::SectionLocation> /*section_map*/) {
            auto token = lifetime_.token();
            config_editor_.safe_edit_value(
                *api_, section, field, new_value,
                [this, token]() {
                    spdlog::info("[Probe] Config edit saved successfully");
                    if (token.expired())
                        return;
                    token.defer([this]() {
                        // Reload config values to reflect the change
                        load_config_values();
                    });
                },
                [](const std::string& err) {
                    spdlog::error("[Probe] Config edit failed: {}", err);
                    // TODO: Show error to user via modal
                });
        },
        [](const std::string& err) {
            spdlog::error("[Probe] Failed to load config files for edit: {}", err);
        });
}

void ProbeOverlay::handle_config_cancel() {
    spdlog::debug("[Probe] Config edit cancelled");
    editing_field_key_.clear();

    if (edit_modal_) {
        Modal::hide(edit_modal_);
        edit_modal_ = nullptr;
    }
}
