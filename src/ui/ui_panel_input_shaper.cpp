// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_input_shaper.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "config.h"
#include "format_utils.h"
#include "host_identity.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "platform_capabilities.h"
#include "shaper_response.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace helix;

// Shaper overlay colors (distinct, visible on dark bg) — shared by chart and legend
static constexpr uint32_t SHAPER_OVERLAY_COLORS[] = {
    0x4FC3F7, // light blue
    0x66BB6A, // green
    0xFFA726, // orange
    0xAB47BC, // purple
    0xEF5350, // red
    0x26C6DA, // cyan
    0xFFEE58, // yellow
    0xEC407A, // pink
    0x7E57C2, // deep purple
    0x8D6E63, // brown
    0x78909C, // blue-grey
};
static constexpr size_t NUM_SHAPER_COLORS =
    sizeof(SHAPER_OVERLAY_COLORS) / sizeof(SHAPER_OVERLAY_COLORS[0]);

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<InputShaperPanel> g_input_shaper_panel;

// State subject (0=IDLE, 1=MEASURING, 2=RESULTS, 3=ERROR)
static lv_subject_t s_input_shaper_state;

// Forward declarations
static void on_input_shaper_row_clicked(lv_event_t* e);
IMoonrakerClient* get_moonraker_client();
IMoonrakerAPI* get_moonraker_api();

InputShaperPanel& get_global_input_shaper_panel() {
    if (!g_input_shaper_panel) {
        g_input_shaper_panel = std::make_unique<InputShaperPanel>();
        StaticPanelRegistry::instance().register_destroy("InputShaperPanel",
                                                         []() { g_input_shaper_panel.reset(); });
    }
    return *g_input_shaper_panel;
}

InputShaperPanel::~InputShaperPanel() {
    // Stop the analysis elapsed timer before anything it could touch goes away.
    cancel_analysis_display();

    // Share one implementation with the registry path. Normally the registry has
    // already run this and deinit_subjects_base() no-ops on subjects_initialized_;
    // this call covers the teardown that destroys the panel without it. The
    // guards' own destructors would invalidate the tokens anyway, but doing it
    // through deinit_subjects() keeps the two paths from drifting.
    deinit_subjects();

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    parent_screen_ = nullptr;

    // Guard against static destruction order fiasco (spdlog may be gone)
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[InputShaper] Destroyed");
    }
}

void init_input_shaper_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_input_shaper_row_clicked", on_input_shaper_row_clicked);
    spdlog::trace("[InputShaper] Row click callback registered");
}

/**
 * @brief Row click handler for opening input shaper from Advanced panel
 */
static void on_input_shaper_row_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[InputShaper] Input Shaping row clicked");

    auto& panel = get_global_input_shaper_panel();

    // Lazy-create the input shaper panel
    if (!panel.get_root()) {
        spdlog::debug("[InputShaper] Creating input shaper panel...");

        // Set API references before create
        IMoonrakerClient* client = get_moonraker_client();
        IMoonrakerAPI* api = get_moonraker_api();
        panel.set_api(client, api);

        lv_obj_t* screen = lv_display_get_screen_active(nullptr);
        if (!panel.create(screen)) {
            spdlog::error("[InputShaper] Failed to create input_shaper_panel");
            return;
        }
        spdlog::info("[InputShaper] Panel created");
    }

    // Show the overlay (registers with NavigationManager and pushes)
    panel.show();
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

void ui_panel_input_shaper_register_callbacks() {
    // Register event callbacks for XML
    register_xml_callbacks({
        {"input_shaper_calibrate_all_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_calibrate_all_clicked(); }},
        {"input_shaper_calibrate_x_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_calibrate_x_clicked(); }},
        {"input_shaper_calibrate_y_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_calibrate_y_clicked(); }},
        {"input_shaper_measure_noise_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_measure_noise_clicked(); }},
        {"input_shaper_cancel_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_cancel_clicked(); }},
        {"input_shaper_apply_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_apply_clicked(); }},
        {"input_shaper_close_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_close_clicked(); }},
        {"input_shaper_retry_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_retry_clicked(); }},
        {"input_shaper_save_config_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_save_config_clicked(); }},
        {"input_shaper_save_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_save_clicked(); }},
        {"input_shaper_print_test_cb",
         [](lv_event_t* /*e*/) {
             get_global_input_shaper_panel().handle_print_test_pattern_clicked();
         }},
        {"input_shaper_help_cb",
         [](lv_event_t* /*e*/) { get_global_input_shaper_panel().handle_help_clicked(); }},
        // Chip toggle callbacks for frequency response chart overlays
        {"input_shaper_chip_x_0_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(0); }},
        {"input_shaper_chip_x_1_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(1); }},
        {"input_shaper_chip_x_2_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(2); }},
        {"input_shaper_chip_x_3_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(3); }},
        {"input_shaper_chip_x_4_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(4); }},
        {"input_shaper_chip_y_0_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(0); }},
        {"input_shaper_chip_y_1_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(1); }},
        {"input_shaper_chip_y_2_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(2); }},
        {"input_shaper_chip_y_3_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(3); }},
        {"input_shaper_chip_y_4_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(4); }},
        {"input_shaper_chip_x_5_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(5); }},
        {"input_shaper_chip_x_6_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(6); }},
        {"input_shaper_chip_x_7_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(7); }},
        {"input_shaper_chip_x_8_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(8); }},
        {"input_shaper_chip_x_9_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(9); }},
        {"input_shaper_chip_x_10_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_x_clicked(10); }},
        {"input_shaper_chip_y_5_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(5); }},
        {"input_shaper_chip_y_6_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(6); }},
        {"input_shaper_chip_y_7_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(7); }},
        {"input_shaper_chip_y_8_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(8); }},
        {"input_shaper_chip_y_9_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(9); }},
        {"input_shaper_chip_y_10_cb",
         [](lv_event_t*) { get_global_input_shaper_panel().handle_chip_y_clicked(10); }},
    });

    // Initialize subjects BEFORE XML creation
    auto& panel = get_global_input_shaper_panel();
    panel.init_subjects();

    spdlog::debug("[InputShaper] Registered XML event callbacks");
}

// ============================================================================
// SUBJECT INITIALIZATION
// ============================================================================

void InputShaperPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize state subject for state machine visibility
    UI_MANAGED_SUBJECT_INT(s_input_shaper_state, 0, "input_shaper_state", subjects_);

    // Per-axis comparison table subjects
    auto init_cmp_row = [this](ComparisonRow& row, const char* prefix, size_t idx) {
        char name[48];
        snprintf(name, sizeof(name), "is_%s_cmp_%zu_type", prefix, idx);
        UI_MANAGED_SUBJECT_STRING_N(row.type, row.type_buf, CMP_TYPE_BUF, "", name, subjects_);
        snprintf(name, sizeof(name), "is_%s_cmp_%zu_freq", prefix, idx);
        UI_MANAGED_SUBJECT_STRING_N(row.freq, row.freq_buf, CMP_VALUE_BUF, "", name, subjects_);
        snprintf(name, sizeof(name), "is_%s_cmp_%zu_vib", prefix, idx);
        UI_MANAGED_SUBJECT_STRING_N(row.vib, row.vib_buf, CMP_VALUE_BUF, "", name, subjects_);
        snprintf(name, sizeof(name), "is_%s_cmp_%zu_accel", prefix, idx);
        UI_MANAGED_SUBJECT_STRING_N(row.accel, row.accel_buf, CMP_VALUE_BUF, "", name, subjects_);
    };

    for (size_t i = 0; i < MAX_SHAPERS; i++) {
        init_cmp_row(x_cmp_[i], "x", i);
        init_cmp_row(y_cmp_[i], "y", i);
    }

    // Error message subject
    UI_MANAGED_SUBJECT_STRING(is_error_message_, is_error_message_buf_,
                              "An error occurred during calibration.", "is_error_message",
                              subjects_);

    // Current config display subjects
    UI_MANAGED_SUBJECT_INT(is_shaper_configured_, 0, "is_shaper_configured", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_current_x_type_, is_current_x_type_buf_, "", "is_current_x_type",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(is_current_x_freq_, is_current_x_freq_buf_, "", "is_current_x_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(is_current_y_type_, is_current_y_type_buf_, "", "is_current_y_type",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(is_current_y_freq_, is_current_y_freq_buf_, "", "is_current_y_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(is_current_max_accel_, is_current_max_accel_buf_, "",
                              "is_current_max_accel", subjects_);

    // Measuring state labels
    UI_MANAGED_SUBJECT_STRING(is_measuring_axis_label_, is_measuring_axis_label_buf_,
                              "Calibrating...", "is_measuring_axis_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_measuring_step_label_, is_measuring_step_label_buf_, "",
                              "is_measuring_step_label", subjects_);
    UI_MANAGED_SUBJECT_INT(is_measuring_progress_, 0, "is_measuring_progress", subjects_);
    UI_MANAGED_SUBJECT_INT(is_measuring_has_progress_, 0, "is_measuring_has_progress", subjects_);

    // Per-axis result display subjects
    UI_MANAGED_SUBJECT_INT(is_results_has_x_, 0, "is_results_has_x", subjects_);
    UI_MANAGED_SUBJECT_INT(is_results_has_y_, 0, "is_results_has_y", subjects_);

    // Header button disabled state
    UI_MANAGED_SUBJECT_INT(is_calibrate_all_disabled_, 0, "is_calibrate_all_disabled", subjects_);

    // Recommended row index per axis (-1 = none highlighted)
    UI_MANAGED_SUBJECT_INT(is_x_recommended_row_, -1, "is_x_recommended_row", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_recommended_row_, -1, "is_y_recommended_row", subjects_);

    // Number of shapers per axis (controls table row visibility via bind_flag_if_le)
    UI_MANAGED_SUBJECT_INT(is_x_num_shapers_, 0, "is_x_num_shapers", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_num_shapers_, 0, "is_y_num_shapers", subjects_);

    // Number of chart chips per axis (controls chip visibility via bind_flag_if_le)
    UI_MANAGED_SUBJECT_INT(is_x_num_chips_, 0, "is_x_num_chips", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_num_chips_, 0, "is_y_num_chips", subjects_);

    UI_MANAGED_SUBJECT_STRING(is_result_x_shaper_, is_result_x_shaper_buf_, "",
                              "is_result_x_shaper", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_x_explanation_, is_result_x_explanation_buf_, "",
                              "is_result_x_explanation", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_x_vibration_, is_result_x_vibration_buf_, "",
                              "is_result_x_vibration", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_x_max_accel_, is_result_x_max_accel_buf_, "",
                              "is_result_x_max_accel", subjects_);
    UI_MANAGED_SUBJECT_INT(is_result_x_quality_, 0, "is_result_x_quality", subjects_);

    UI_MANAGED_SUBJECT_STRING(is_result_y_shaper_, is_result_y_shaper_buf_, "",
                              "is_result_y_shaper", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_y_explanation_, is_result_y_explanation_buf_, "",
                              "is_result_y_explanation", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_y_vibration_, is_result_y_vibration_buf_, "",
                              "is_result_y_vibration", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_result_y_max_accel_, is_result_y_max_accel_buf_, "",
                              "is_result_y_max_accel", subjects_);
    UI_MANAGED_SUBJECT_INT(is_result_y_quality_, 0, "is_result_y_quality", subjects_);

    // Live-before delta rows
    UI_MANAGED_SUBJECT_STRING(is_x_delta_text_, is_x_delta_buf_, "", "is_x_delta_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_y_delta_text_, is_y_delta_buf_, "", "is_y_delta_text", subjects_);
    UI_MANAGED_SUBJECT_INT(is_x_has_delta_, 0, "is_x_has_delta", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_has_delta_, 0, "is_y_has_delta", subjects_);
    UI_MANAGED_SUBJECT_STRING(is_x_verdict_text_, is_x_verdict_buf_, "", "is_x_verdict_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(is_y_verdict_text_, is_y_verdict_buf_, "", "is_y_verdict_text",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(is_x_has_verdict_, 0, "is_x_has_verdict", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_has_verdict_, 0, "is_y_has_verdict", subjects_);
    UI_MANAGED_SUBJECT_INT(is_x_fw_overwrite_warn_, 0, "is_x_fw_overwrite_warn", subjects_);

    // Frequency response chart gating
    UI_MANAGED_SUBJECT_INT(is_x_has_freq_data_, 0, "is_x_has_freq_data", subjects_);
    UI_MANAGED_SUBJECT_INT(is_y_has_freq_data_, 0, "is_y_has_freq_data", subjects_);

    // Legend shaper label subjects (one per axis, updated on chip toggle)
    UI_MANAGED_SUBJECT_STRING_N(is_x_legend_shaper_label_, is_x_legend_shaper_label_buf_,
                                CHIP_LABEL_BUF, "", "is_x_legend_shaper_label", subjects_);
    UI_MANAGED_SUBJECT_STRING_N(is_y_legend_shaper_label_, is_y_legend_shaper_label_buf_,
                                CHIP_LABEL_BUF, "", "is_y_legend_shaper_label", subjects_);

    // Chip label and active subjects
    auto init_chip = [this](ChipRow& chip, const char* axis, size_t idx) {
        char name[48];
        snprintf(name, sizeof(name), "is_%s_chip_%zu_label", axis, idx);
        UI_MANAGED_SUBJECT_STRING_N(chip.label, chip.label_buf, CHIP_LABEL_BUF, "", name,
                                    subjects_);
        snprintf(name, sizeof(name), "is_%s_chip_%zu_active", axis, idx);
        UI_MANAGED_SUBJECT_INT(chip.active, 0, name, subjects_);
    };
    for (size_t i = 0; i < MAX_SHAPERS; i++) {
        init_chip(x_chips_[i], "x", i);
        init_chip(y_chips_[i], "y", i);
    }

    subjects_initialized_ = true;

    // Join the ordered subject-shutdown pass. Without this the subjects went away
    // only when StaticPanelRegistry later destroyed the panel, so their teardown
    // ordering was decided by panel-registry destruction rather than by the one
    // registry that exists to sequence it (#1180).
    StaticSubjectRegistry::instance().register_deinit("InputShaperPanel", []() {
        if (g_input_shaper_panel) {
            g_input_shaper_panel->deinit_subjects();
        }
    });

    spdlog::debug("[InputShaper] Subjects initialized and registered");
}

void InputShaperPanel::deinit_subjects() {
    // Expire outstanding async tokens here, not only in the destructor: subjects
    // can be torn down and re-inited on a LIVE panel (shutdown registry, test
    // isolation), and a queued callback would otherwise write into a subject that
    // was deinited underneath it (#1180, #1146).
    lifetime_.invalidate();
    calibration_lifetime_.invalidate();

    // Same for the analysis elapsed timer: it writes the step label subject on
    // every tick, so it must not survive the subjects it refreshes.
    cancel_analysis_display();

    deinit_subjects_base(subjects_);
}

// ============================================================================
// CREATE
// ============================================================================

lv_obj_t* InputShaperPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[InputShaper] Panel already created");
        return overlay_root_;
    }

    parent_screen_ = parent;

    spdlog::debug("[InputShaper] Creating overlay from XML");
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "input_shaper_panel", nullptr));

    if (!overlay_root_) {
        spdlog::error("[InputShaper] Failed to create overlay from XML");
        return nullptr;
    }

    // Start hidden (push_overlay will show it)
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    setup_widgets();

    spdlog::info("[InputShaper] Overlay created successfully");
    return overlay_root_;
}

// ============================================================================
// SETUP WIDGETS (private helper)
// ============================================================================

void InputShaperPanel::setup_widgets() {
    if (!overlay_root_) {
        spdlog::error("[InputShaper] NULL overlay_root_");
        return;
    }

    // State visibility is handled via XML subject bindings
    // All display elements are now subject-bound in XML

    // Set initial state
    set_state(State::IDLE);

    // Create frequency response chart widgets inside containers
    create_chart_widgets();

    // Find legend dot widgets for programmatic color updates
    legend_x_shaper_dot_ = lv_obj_find_by_name(overlay_root_, "legend_x_shaper_dot");
    legend_y_shaper_dot_ = lv_obj_find_by_name(overlay_root_, "legend_y_shaper_dot");
    if (!legend_x_shaper_dot_ || !legend_y_shaper_dot_) {
        spdlog::warn("[InputShaper] Legend dot widget(s) not found in XML");
    }

    spdlog::debug("[InputShaper] Widget setup complete");
}

// ============================================================================
// SHOW
// ============================================================================

void InputShaperPanel::set_api(IMoonrakerClient* client, IMoonrakerAPI* api) {
    client_ = client;
    api_ = api;

    // Create calibrator with API for delegated operations
    calibrator_ = std::make_unique<helix::calibration::InputShaperCalibrator>(api_);
    spdlog::debug("[InputShaper] Calibrator created");
}

void InputShaperPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[InputShaper] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[InputShaper] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[InputShaper] Overlay shown");
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void InputShaperPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[InputShaper] on_activate()");

    // Reset to idle state
    set_state(State::IDLE);
    clear_results();
    calibrate_all_mode_ = false;

    // Query current input shaper configuration from printer
    if (api_) {
        api_->advanced().get_input_shaper_config(
            lifetime_.bg_cb(
                "InputShaperPanel::get_input_shaper_config",
                [this](const InputShaperConfig& config) { populate_current_config(config); }),
            lifetime_.bg_cb("InputShaperPanel::get_input_shaper_config_error",
                            [this](const MoonrakerError& err) {
                                spdlog::debug("[InputShaper] Could not query config: {}",
                                              err.message);
                                // Not an error - just means config not available
                                InputShaperConfig empty;
                                populate_current_config(empty);
                            }));
    }

    // Auto-start calibration for testing (env var)
    if (std::getenv("INPUT_SHAPER_AUTO_START")) {
        spdlog::info("[InputShaper] Auto-starting X calibration (INPUT_SHAPER_AUTO_START set)");
        start_with_preflight('X');
    }

    // Demo mode: inject results after on_activate() finishes its reset
    if (demo_inject_pending_) {
        demo_inject_pending_ = false;
        inject_demo_results();
    }
}

void InputShaperPanel::on_deactivate() {
    spdlog::debug("[InputShaper] on_deactivate()");

    // Stop the analysis elapsed timer even when the state check below does not
    // run; the label it refreshes is going off screen either way.
    cancel_analysis_display();

    // Cancel any in-progress calibration
    if (state_ == State::MEASURING && calibrator_) {
        spdlog::info("[InputShaper] Cancelling calibration on deactivate");
        calibration_lifetime_.invalidate(); // Discard any in-flight async callbacks
        calibrator_->cancel();
        set_state(State::IDLE);
    }

    // Dismiss the low-RAM warning modal if still open. The panel is a persistent
    // singleton, so hiding it while the modal is open would otherwise leak an
    // orphaned warning modal whose callbacks capture this panel.
    if (low_ram_warn_dialog_) {
        helix::ui::modal_hide(low_ram_warn_dialog_);
        low_ram_warn_dialog_ = nullptr;
    }

    // Call base class
    OverlayBase::on_deactivate();
}

void InputShaperPanel::cleanup() {
    spdlog::debug("[InputShaper] Cleaning up");

    // Expire all outstanding async tokens
    lifetime_.invalidate();
    calibration_lifetime_.invalidate();

    // Stop the analysis elapsed timer; StaticPanelRegistry::destroy_all() runs
    // before lv_deinit(), so teardown that skips this leaves it armed on a
    // freed panel (#1173).
    cancel_analysis_display();

    // Destroy chart widgets
    if (x_chart_.chart) {
        ui_frequency_response_chart_destroy(x_chart_.chart);
        x_chart_.chart = nullptr;
    }
    if (y_chart_.chart) {
        ui_frequency_response_chart_destroy(y_chart_.chart);
        y_chart_.chart = nullptr;
    }

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();

    // Clear references
    parent_screen_ = nullptr;
}

void InputShaperPanel::on_ui_destroyed() {
    // Destroy chart widgets (native C structs, not LVGL children)
    if (x_chart_.chart) {
        ui_frequency_response_chart_destroy(x_chart_.chart);
        x_chart_.chart = nullptr;
    }
    if (y_chart_.chart) {
        ui_frequency_response_chart_destroy(y_chart_.chart);
        y_chart_.chart = nullptr;
    }
    legend_x_shaper_dot_ = nullptr;
    legend_y_shaper_dot_ = nullptr;
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void InputShaperPanel::set_state(State new_state) {
    spdlog::debug("[InputShaper] State change: {} -> {}", static_cast<int>(state_),
                  static_cast<int>(new_state));
    if (new_state != State::MEASURING) {
        // The analysis elapsed timer only makes sense while its spinner is on
        // screen; every exit from MEASURING stops it.
        cancel_analysis_display();
    }
    state_ = new_state;

    // Update subject - XML bindings handle visibility automatically
    // State mapping: 0=IDLE, 1=MEASURING, 2=RESULTS, 3=ERROR
    lv_subject_set_int(&s_input_shaper_state, static_cast<int>(new_state));

    // Disable Calibrate All button when not idle
    lv_subject_set_int(&is_calibrate_all_disabled_, new_state != State::IDLE ? 1 : 0);
}

// ============================================================================
// CALIBRATION COMMANDS (using IMoonrakerAPI)
// ============================================================================

void InputShaperPanel::start_with_preflight(char axis) {
    if (!calibrator_) {
        on_calibration_error("Internal error: calibrator not available");
        return;
    }

    auto mem = helix::get_system_memory_info();
    if (mem.total_mb() < helix::RESONANCE_LOW_RAM_WARN_MB) {
        // Re-entry guard: a second entry while the warning modal is open is a no-op.
        if (low_ram_warn_dialog_)
            return;
        pending_calib_axis_ = axis;
        low_ram_warn_dialog_ = helix::ui::show_low_ram_resonance_warning(
            mem.total_mb(),
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[InputShaper] low_ram_confirm");
                auto* self = static_cast<InputShaperPanel*>(lv_event_get_user_data(e));
                if (!self)
                    return;
                if (self->low_ram_warn_dialog_) {
                    helix::ui::modal_hide(self->low_ram_warn_dialog_);
                    self->low_ram_warn_dialog_ = nullptr;
                }
                self->proceed_with_preflight(self->pending_calib_axis_);
                LVGL_SAFE_EVENT_CB_END();
            },
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[InputShaper] low_ram_cancel");
                auto* self = static_cast<InputShaperPanel*>(lv_event_get_user_data(e));
                if (!self)
                    return;
                if (self->low_ram_warn_dialog_) {
                    helix::ui::modal_hide(self->low_ram_warn_dialog_);
                    self->low_ram_warn_dialog_ = nullptr;
                }
                self->calibrate_all_mode_ = false; // user backed out before anything started
                LVGL_SAFE_EVENT_CB_END();
            },
            this);
        if (!low_ram_warn_dialog_) {
            // Modal failed to build — don't silently block calibration.
            proceed_with_preflight(axis);
        }
        return;
    }
    proceed_with_preflight(axis);
}

void InputShaperPanel::proceed_with_preflight(char axis) {
    current_axis_ = axis;
    last_calibrated_axis_ = axis;
    recommended_type_.clear();
    recommended_freq_ = 0.0f;

    // Show checking accelerometer status
    snprintf(is_measuring_axis_label_buf_, sizeof(is_measuring_axis_label_buf_), "%s",
             lv_tr("Checking accelerometer..."));
    lv_subject_copy_string(&is_measuring_axis_label_, is_measuring_axis_label_buf_);
    lv_subject_copy_string(&is_measuring_step_label_, "");
    lv_subject_set_int(&is_measuring_progress_, 0);
    lv_subject_set_int(&is_measuring_has_progress_, 0);

    calibration_lifetime_.invalidate();
    auto cal_tok = calibration_lifetime_.token();

    set_state(State::MEASURING);
    spdlog::info("[InputShaper] Starting pre-flight noise check before {} axis calibration", axis);

    auto tok = lifetime_.token();
    calibrator_->check_accelerometer(
        [this, tok, cal_tok](float noise_level) {
            if (tok.expired())
                return;
            if (cal_tok.expired())
                return;
            tok.defer([this, noise_level]() { on_preflight_complete(noise_level); });
        },
        [this, tok, cal_tok](const std::string& err) {
            if (tok.expired())
                return;
            if (cal_tok.expired())
                return;
            tok.defer([this, err]() { on_preflight_error(err); });
        });
}

void InputShaperPanel::on_preflight_complete(float noise_level) {
    if (state_ != State::MEASURING)
        return; // User cancelled

    spdlog::info("[InputShaper] Pre-flight passed, noise={:.4f}", noise_level);

    // Proceed to actual calibration
    start_calibration(current_axis_);
}

void InputShaperPanel::on_preflight_error(const std::string& message) {
    if (state_ != State::MEASURING)
        return;

    spdlog::error("[InputShaper] Pre-flight failed: {}", message);
    on_calibration_error("Accelerometer not responding. Check wiring and connection.");
}

void InputShaperPanel::calibrate_all() {
    calibrate_all_mode_ = true;
    x_result_ = InputShaperResult{}; // Clear stored X result
    x_saved_value_overwritten_ = false;
    start_with_preflight('X');
}

// ============================================================================
// LIVE-BEFORE CONFIG SNAPSHOT
// ============================================================================

void InputShaperPanel::snapshot_config_before() {
    has_config_before_ = false;
    config_before_ = InputShaperConfig{};
    if (!api_) {
        return;
    }
    api_->advanced().get_input_shaper_config(
        lifetime_.bg_cb("InputShaperPanel::config_before",
                        [this](const InputShaperConfig& config) {
                            config_before_ = config;
                            has_config_before_ = true;
                            spdlog::debug("[InputShaper] Live-before config: X={} @ {:.1f} Hz, "
                                          "Y={} @ {:.1f} Hz",
                                          config.shaper_type_x, config.shaper_freq_x,
                                          config.shaper_type_y, config.shaper_freq_y);
                        }),
        lifetime_.bg_cb("InputShaperPanel::config_before_error", [this](const MoonrakerError& err) {
            // Not fatal: the delta rows just stay hidden. The
            // run itself does not depend on the snapshot.
            spdlog::debug("[InputShaper] Live-before config unavailable: {}", err.message);
        }));
}

bool InputShaperPanel::get_before_axis(char axis, std::string& type, float& freq) const {
    if (!has_config_before_) {
        return false;
    }
    type = (axis == 'X') ? config_before_.shaper_type_x : config_before_.shaper_type_y;
    freq = (axis == 'X') ? config_before_.shaper_freq_x : config_before_.shaper_freq_y;
    return !type.empty() && freq > 0.0f;
}

double InputShaperPanel::before_damping_ratio(char axis) const {
    const float dr =
        (axis == 'X') ? config_before_.damping_ratio_x : config_before_.damping_ratio_y;
    // Klipper reports the ratio only when the config sets one; zero means
    // default (shaper_defs.py DEFAULT_DAMPING_RATIO).
    return dr > 0.0f ? static_cast<double>(dr) : calibration::SHAPER_DEFAULT_DAMPING_RATIO;
}

void InputShaperPanel::continue_calibrate_all_y() {
    spdlog::info("[InputShaper] Calibrate All: X complete, starting Y");
    // Don't reset calibrate_all_mode_ - still in multi-axis flow
    // Skip pre-flight for Y (accelerometer just verified for X)
    start_calibration('Y');
}

void InputShaperPanel::start_calibration(char axis) {
    if (!calibrator_) {
        spdlog::error("[InputShaper] No calibrator - cannot calibrate");
        on_calibration_error("Internal error: calibrator not available");
        return;
    }

    current_axis_ = axis;
    last_calibrated_axis_ = axis;
    calibration_lifetime_.invalidate();
    auto cal_tok = calibration_lifetime_.token();

    // Only clear results for first axis in Calibrate All, or for single-axis
    if (!calibrate_all_mode_ || axis == 'X') {
        recommended_type_.clear();
        recommended_freq_ = 0.0f;
    }

    // Snapshot the live config right before the run starts: the results cards
    // compare against what was active HERE, not against what was on screen
    // when the panel opened, and not against what SAVE_CONFIG later writes
    // (some forks overwrite the staged X result with Y's values). Only the
    // first axis of a run snapshots - the Y continuation of Calibrate All
    // must not pick up mid-run state.
    if (!(calibrate_all_mode_ && axis == 'Y')) {
        snapshot_config_before();
    }

    // Update measuring labels
    const std::string axis_label = fmt::format(lv_tr("Calibrating {} axis..."), axis);
    snprintf(is_measuring_axis_label_buf_, sizeof(is_measuring_axis_label_buf_), "%s",
             axis_label.c_str());
    lv_subject_copy_string(&is_measuring_axis_label_, is_measuring_axis_label_buf_);

    if (calibrate_all_mode_) {
        const char* step = (axis == 'X') ? "Step 1 of 2" : "Step 2 of 2";
        lv_subject_copy_string(&is_measuring_step_label_, step);
    } else {
        lv_subject_copy_string(&is_measuring_step_label_, "");
    }

    lv_subject_set_int(&is_measuring_progress_, 0);
    lv_subject_set_int(&is_measuring_has_progress_, 0);
    cancel_analysis_display(); // no elapsed label from a previous run
    set_state(State::MEASURING);
    spdlog::info("[InputShaper] Starting calibration for axis {}", axis);

    // Delegate to calibrator. lifetime_.bg_cb marshals each callback to the main
    // thread and drops it if the panel has been torn down; cal_tok additionally
    // discards callbacks from a superseded calibration run.
    calibrator_->run_calibration(
        axis,
        lifetime_.bg_cb(
            "InputShaperPanel::calibration_progress",
            [this, cal_tok](int percent, ShaperCalibrationPhase phase) {
                if (cal_tok.expired()) {
                    spdlog::debug("[InputShaper] Discarding stale progress callback");
                    return;
                }
                lv_subject_set_int(&is_measuring_progress_, percent);
                // The phase comes from the collector — the percentage cannot be
                // used to infer it, because a sweep whose range we guessed short
                // would sit at its ceiling and look like analysis had started.
                // During analysis the percent is meaningless (0 is emitted only
                // as the phase-change signal), so the bar is swapped for the
                // spinner and the label counts elapsed seconds instead.
                switch (phase) {
                case ShaperCalibrationPhase::Sweeping: {
                    lv_subject_set_int(&is_measuring_has_progress_, 1);
                    cancel_analysis_display();
                    const std::string step =
                        fmt::format(lv_tr("Measuring vibrations... {}%"), percent);
                    snprintf(is_measuring_step_label_buf_, sizeof(is_measuring_step_label_buf_),
                             "%s", step.c_str());
                    break;
                }
                case ShaperCalibrationPhase::Analyzing:
                    // The XML swaps the bar for a spinner while this is 0.
                    lv_subject_set_int(&is_measuring_has_progress_, 0);
                    begin_analysis_display();
                    break;
                case ShaperCalibrationPhase::Complete:
                    lv_subject_set_int(&is_measuring_has_progress_, 1);
                    cancel_analysis_display();
                    if (calibrate_all_mode_ && current_axis_ == 'X') {
                        snprintf(is_measuring_step_label_buf_, sizeof(is_measuring_step_label_buf_),
                                 "%s", lv_tr("X axis done, starting Y..."));
                    } else {
                        snprintf(is_measuring_step_label_buf_, sizeof(is_measuring_step_label_buf_),
                                 "%s", lv_tr("Complete"));
                    }
                    break;
                }
                lv_subject_copy_string(&is_measuring_step_label_, is_measuring_step_label_buf_);
            }),
        lifetime_.bg_cb("InputShaperPanel::calibration_result",
                        [this, cal_tok](const InputShaperResult& result) {
                            if (cal_tok.expired()) {
                                spdlog::debug("[InputShaper] Discarding stale result callback");
                                return;
                            }
                            on_calibration_result(result);
                        }),
        lifetime_.bg_cb("InputShaperPanel::calibration_error",
                        [this, cal_tok](const std::string& err) {
                            if (cal_tok.expired()) {
                                spdlog::debug("[InputShaper] Discarding stale error callback");
                                return;
                            }
                            on_calibration_error(err);
                        }));
}

// ============================================================================
// ANALYSIS-PHASE DISPLAY (spinner + elapsed-seconds label)
// ============================================================================

void InputShaperPanel::begin_analysis_display() {
    lv_subject_set_int(&is_measuring_has_progress_, 0);
    analysis_elapsed_.begin(&is_measuring_step_label_, [](uint32_t elapsed_seconds) {
        return fmt::format(lv_tr("Analyzing data... {}s"), elapsed_seconds);
    });
}

void InputShaperPanel::cancel_analysis_display() {
    analysis_elapsed_.cancel();
}

void InputShaperPanel::measure_noise() {
    if (!calibrator_) {
        spdlog::error("[InputShaper] No calibrator - cannot measure noise");
        on_calibration_error("Internal error: calibrator not available");
        return;
    }

    snprintf(is_measuring_axis_label_buf_, sizeof(is_measuring_axis_label_buf_), "%s",
             lv_tr("Measuring accelerometer noise..."));
    lv_subject_copy_string(&is_measuring_axis_label_, is_measuring_axis_label_buf_);
    calibration_lifetime_.invalidate();
    auto cal_tok = calibration_lifetime_.token();
    set_state(State::MEASURING);
    spdlog::info("[InputShaper] Starting accelerometer check via calibrator");

    calibrator_->check_accelerometer(
        lifetime_.bg_cb("InputShaperPanel::measure_noise_complete",
                        [this, cal_tok](float noise_level) {
                            if (cal_tok.expired())
                                return;
                            spdlog::debug(
                                "[InputShaper] Accelerometer check complete, noise={:.4f}",
                                noise_level);
                            char msg[64];
                            snprintf(msg, sizeof(msg), "Noise level: %.4f", noise_level);
                            ToastManager::instance().show(ToastSeverity::INFO, msg, 3000);
                            set_state(State::IDLE);
                        }),
        lifetime_.bg_cb("InputShaperPanel::measure_noise_error",
                        [this, cal_tok](const std::string& err) {
                            if (cal_tok.expired())
                                return;
                            spdlog::error("[InputShaper] Failed to measure noise: {}", err);
                            on_calibration_error(err);
                        }));
}

void InputShaperPanel::cancel_calibration() {
    spdlog::info("[InputShaper] Abort clicked, sending emergency stop + firmware restart");
    calibrate_all_mode_ = false;
    calibration_lifetime_.invalidate(); // Discard any in-flight async callbacks

    // E-stop + firmware restart: klippy comes back, so this is an expected
    // reconnect, not a fault
    helix::ui::begin_expected_klippy_restart("Firmware restarting...");

    if (calibrator_) {
        calibrator_->emergency_abort();
    }

    set_state(State::IDLE);
}

void InputShaperPanel::apply_recommendation() {
    if (!calibrator_) {
        spdlog::error("[InputShaper] Cannot apply - no calibrator");
        return;
    }

    auto tok = lifetime_.token();

    // If we have stored X result from Calibrate All, apply X first then chain Y
    if (x_result_.is_valid()) {
        spdlog::info("[InputShaper] Applying X axis shaper: {} @ {:.1f} Hz", x_result_.shaper_type,
                     x_result_.shaper_freq);

        helix::calibration::ApplyConfig x_config;
        x_config.axis = 'X';
        x_config.shaper_type = x_result_.shaper_type;
        x_config.frequency = x_result_.shaper_freq;

        calibrator_->apply_settings(
            x_config,
            [this, tok]() {
                // L081 Mechanism C: reads recommended_*_ and calls apply_y_after_x()
                // (which touches api_/lifetime_). Marshal to main.
                tok.defer("InputShaperPanel::apply_x_success", [this]() {
                    spdlog::info("[InputShaper] X axis settings applied");
                    // Chain Y apply if we have a recommendation
                    if (!recommended_type_.empty() && recommended_freq_ > 0) {
                        apply_y_after_x();
                    } else {
                        ToastManager::instance().show(
                            ToastSeverity::SUCCESS, lv_tr("Input shaper settings applied!"), 2500);
                    }
                });
            },
            [tok](const std::string& err) {
                if (tok.expired())
                    return;
                spdlog::error("[InputShaper] Failed to apply X settings: {}", err);
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Failed to apply settings"), 3000);
            });
    } else if (!recommended_type_.empty() && recommended_freq_ > 0) {
        // Single axis apply
        spdlog::info("[InputShaper] Applying {} axis shaper: {} @ {:.1f} Hz", last_calibrated_axis_,
                     recommended_type_, recommended_freq_);

        helix::calibration::ApplyConfig config;
        config.axis = last_calibrated_axis_;
        config.shaper_type = recommended_type_;
        config.frequency = recommended_freq_;

        calibrator_->apply_settings(
            config,
            [tok]() {
                if (tok.expired())
                    return;
                spdlog::info("[InputShaper] Settings applied successfully");
                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                              lv_tr("Input shaper settings applied!"), 2500);
            },
            [tok](const std::string& err) {
                if (tok.expired())
                    return;
                spdlog::error("[InputShaper] Failed to apply settings: {}", err);
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Failed to apply settings"), 3000);
            });
    } else {
        spdlog::error("[InputShaper] Cannot apply - no valid recommendation");
    }
}

void InputShaperPanel::apply_y_after_x() {
    spdlog::info("[InputShaper] Applying Y axis shaper: {} @ {:.1f} Hz", recommended_type_,
                 recommended_freq_);

    helix::calibration::ApplyConfig y_config;
    y_config.axis = 'Y';
    y_config.shaper_type = recommended_type_;
    y_config.frequency = recommended_freq_;

    auto tok = lifetime_.token();
    calibrator_->apply_settings(
        y_config,
        [this, tok]() {
            // L081 Mechanism C: reads api_, calls lifetime_.token() (member access).
            // Marshal to main.
            tok.defer("InputShaperPanel::apply_y_success", [this]() {
                spdlog::info("[InputShaper] Both axis settings applied");
                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                              lv_tr("Input shaper settings applied!"), 2500);
                // Refresh the current config display
                if (api_) {
                    auto tok2 = lifetime_.token();
                    api_->advanced().get_input_shaper_config(
                        [this, tok2](const InputShaperConfig& config) {
                            tok2.defer("InputShaperPanel::populate_after_y",
                                       [this, config]() { populate_current_config(config); });
                        },
                        [](const MoonrakerError&) {});
                }
            });
        },
        [tok](const std::string& err) {
            if (tok.expired())
                return;
            spdlog::error("[InputShaper] Failed to apply Y settings: {}", err);
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("X axis applied, but Y axis failed"), 4000);
        });
}

void InputShaperPanel::save_configuration() {
    if (!calibrator_) {
        return;
    }

    spdlog::info("[InputShaper] Saving configuration (SAVE_CONFIG)");

    // SAVE_CONFIG triggers an expected Klipper restart
    helix::ui::begin_expected_klippy_restart("Saving config... Klipper will restart.");

    auto tok = lifetime_.token();

    calibrator_->save_to_config(
        [tok]() {
            if (tok.expired())
                return;
            spdlog::info("[InputShaper] SAVE_CONFIG sent - Klipper restarting");
        },
        [tok](const std::string& err) {
            if (tok.expired())
                return;
            spdlog::error("[InputShaper] SAVE_CONFIG failed: {}", err);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Failed to save configuration"), 3000);
        });
}

// ============================================================================
// RESULT CALLBACKS (from API)
// ============================================================================

void InputShaperPanel::on_calibration_result(const InputShaperResult& result) {
    spdlog::debug(
        "[InputShaper] on_calibration_result: axis={}, calibrate_all={}, state={}, current_axis={}",
        result.axis, calibrate_all_mode_, static_cast<int>(state_), current_axis_);

    // Ignore if we're not in measuring state (user may have cancelled)
    if (state_ != State::MEASURING) {
        spdlog::debug("[InputShaper] Ignoring result - not in measuring state");
        return;
    }

    spdlog::info("[InputShaper] Calibration complete: {} @ {:.1f} Hz (vib: {:.1f}%)",
                 result.shaper_type, result.shaper_freq, result.vibrations);

    // Surface a non-blocking warning when the recommendation succeeded but the
    // frequency-response chart data couldn't be read (e.g. Klipper's /tmp CSV
    // unreadable). Without this the chart just silently disappears.
    if (result.chart_data_unavailable) {
        // The chart needs Klipper's /tmp CSV, which is only readable when
        // HelixScreen runs on the printer host. If Moonraker is remote, say so;
        // otherwise it's a transient/local read issue.
        std::string host;
        if (Config* cfg = Config::get_instance()) {
            host = cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
        }
        const bool same_host = helix::is_moonraker_on_same_host(host);
        spdlog::warn("[InputShaper] {} axis: calibration CSV unreadable, chart unavailable "
                     "(same_host={})",
                     result.axis, same_host);
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            same_host
                ? lv_tr("Calibration succeeded, but the frequency chart data couldn't be read.")
                : lv_tr("Calibration succeeded. The frequency chart is only available when "
                        "HelixScreen runs on the printer."),
            5000);
    }

    // If Calibrate All and this was X, store result and continue to Y
    if (calibrate_all_mode_ && result.axis == 'X') {
        x_result_ = result;
        continue_calibrate_all_y();
        return;
    }

    // The Y result carries the firmware-copy flag (some klippy forks overwrite
    // the staged X result with Y's values and announce it on the console).
    // Latch on ANY flagged Y result: the fork's copy fires at the end of every
    // Y-axis run, including Y-only runs where no X result exists in this
    // session - the saved config still ends up X = Y.
    if (result.axis == 'Y' && result.x_overwritten_by_firmware) {
        x_saved_value_overwritten_ = true;
        spdlog::warn("[InputShaper] Firmware overwrote the saved X result with Y's values");
    }

    // Store recommendation (from latest axis, or Y if Calibrate All)
    recommended_type_ = result.shaper_type;
    recommended_freq_ = result.shaper_freq;

    // Reset calibrate_all_mode (save before clearing for populate_axis_result)
    bool was_calibrate_all = calibrate_all_mode_;
    calibrate_all_mode_ = false;

    // Clear per-axis results and chart state for clean re-population
    clear_chart('X');
    clear_chart('Y');
    lv_subject_set_int(&is_results_has_x_, 0);
    lv_subject_set_int(&is_results_has_y_, 0);

    // Populate per-axis result cards
    if (was_calibrate_all && x_result_.is_valid()) {
        populate_axis_result('X', x_result_);
    }
    populate_axis_result(result.axis, result);

    set_state(State::RESULTS);
}

void InputShaperPanel::on_calibration_error(const std::string& message) {
    spdlog::debug(
        "[InputShaper] on_calibration_error: msg='{}', calibrate_all={}, state={}, current_axis={}",
        message, calibrate_all_mode_, static_cast<int>(state_), current_axis_);

    // Ignore if we're not in measuring state
    if (state_ != State::MEASURING) {
        spdlog::debug("[InputShaper] Ignoring error - not in measuring state");
        return;
    }

    spdlog::error("[InputShaper] Calibration error: {}", message);

    // Reset Calibrate All mode on error to prevent stale state on retry
    calibrate_all_mode_ = false;

    snprintf(is_error_message_buf_, sizeof(is_error_message_buf_), "%s", message.c_str());
    lv_subject_copy_string(&is_error_message_, is_error_message_buf_);
    set_state(State::ERROR);
}

// ============================================================================
// UI UPDATE HELPERS
// ============================================================================

void InputShaperPanel::populate_current_config(const InputShaperConfig& config) {
    lv_subject_set_int(&is_shaper_configured_, config.is_configured ? 1 : 0);

    if (config.is_configured) {
        // Uppercase X type
        std::string x_upper = config.shaper_type_x;
        for (auto& c : x_upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        snprintf(is_current_x_type_buf_, sizeof(is_current_x_type_buf_), "%s", x_upper.c_str());
        lv_subject_copy_string(&is_current_x_type_, is_current_x_type_buf_);

        // X frequency
        helix::format::format_frequency_hz(config.shaper_freq_x, is_current_x_freq_buf_,
                                           sizeof(is_current_x_freq_buf_));
        lv_subject_copy_string(&is_current_x_freq_, is_current_x_freq_buf_);

        // Uppercase Y type
        std::string y_upper = config.shaper_type_y;
        for (auto& c : y_upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        snprintf(is_current_y_type_buf_, sizeof(is_current_y_type_buf_), "%s", y_upper.c_str());
        lv_subject_copy_string(&is_current_y_type_, is_current_y_type_buf_);

        // Y frequency
        helix::format::format_frequency_hz(config.shaper_freq_y, is_current_y_freq_buf_,
                                           sizeof(is_current_y_freq_buf_));
        lv_subject_copy_string(&is_current_y_freq_, is_current_y_freq_buf_);

        // Max accel - leave empty for now (populated from results in Chunk 3)
        lv_subject_copy_string(&is_current_max_accel_, "");

        spdlog::debug("[InputShaper] Config: X={} @ {}, Y={} @ {}", is_current_x_type_buf_,
                      is_current_x_freq_buf_, is_current_y_type_buf_, is_current_y_freq_buf_);
    } else {
        lv_subject_copy_string(&is_current_x_type_, "");
        lv_subject_copy_string(&is_current_x_freq_, "");
        lv_subject_copy_string(&is_current_y_type_, "");
        lv_subject_copy_string(&is_current_y_freq_, "");
        lv_subject_copy_string(&is_current_max_accel_, "");
        spdlog::debug("[InputShaper] No shaper configured");
    }
}

void InputShaperPanel::clear_results() {
    // Clear frequency response charts
    clear_chart('X');
    clear_chart('Y');

    // Clear per-axis result cards
    lv_subject_set_int(&is_results_has_x_, 0);
    lv_subject_set_int(&is_results_has_y_, 0);
    lv_subject_set_int(&is_x_recommended_row_, -1);
    lv_subject_set_int(&is_y_recommended_row_, -1);
    lv_subject_set_int(&is_x_num_shapers_, 0);
    lv_subject_set_int(&is_y_num_shapers_, 0);

    // Clear live-before delta rows and the firmware overwrite warning
    lv_subject_copy_string(&is_x_delta_text_, "");
    lv_subject_copy_string(&is_y_delta_text_, "");
    lv_subject_set_int(&is_x_has_delta_, 0);
    lv_subject_set_int(&is_y_has_delta_, 0);
    lv_subject_copy_string(&is_x_verdict_text_, "");
    lv_subject_copy_string(&is_y_verdict_text_, "");
    lv_subject_set_int(&is_x_has_verdict_, 0);
    lv_subject_set_int(&is_y_has_verdict_, 0);
    lv_subject_set_int(&is_x_fw_overwrite_warn_, 0);
    has_config_before_ = false;
    config_before_ = InputShaperConfig{};
    x_saved_value_overwritten_ = false;
    // A stored X result is Calibrate All state; carrying it past Close would
    // make a later single-axis Apply re-send X with this session's values
    x_result_ = InputShaperResult{};

    // Clear comparison table subjects
    for (size_t i = 0; i < MAX_SHAPERS; i++) {
        lv_subject_copy_string(&x_cmp_[i].type, "");
        lv_subject_copy_string(&x_cmp_[i].freq, "");
        lv_subject_copy_string(&x_cmp_[i].vib, "");
        lv_subject_copy_string(&x_cmp_[i].accel, "");
        lv_subject_copy_string(&y_cmp_[i].type, "");
        lv_subject_copy_string(&y_cmp_[i].freq, "");
        lv_subject_copy_string(&y_cmp_[i].vib, "");
        lv_subject_copy_string(&y_cmp_[i].accel, "");
    }
}

// ============================================================================
// PER-AXIS RESULT HELPERS
// ============================================================================

const char* InputShaperPanel::get_shaper_explanation(const std::string& type) {
    // Kalico smooth shapers
    if (type == "smooth_zv")
        return "Smooth ZV — continuous filtering, minimal latency";
    if (type == "smooth_mzv")
        return "Smooth MZV — excellent balance with continuous convolution";
    if (type == "smooth_ei")
        return "Smooth EI — strong continuous vibration reduction";
    if (type == "smooth_2hump_ei")
        return "Smooth 2-Hump EI — heavy continuous smoothing";
    if (type == "smooth_zvd_ei")
        return "Smooth ZVD-EI — broadband continuous smoothing";
    if (type == "smooth_si")
        return "Smooth SI — maximum continuous vibration suppression";

    if (type == "zv")
        return "Fast but minimal smoothing — best for well-built printers";
    if (type == "mzv")
        return "Good balance of speed and vibration reduction";
    if (type == "ei")
        return "Strong vibration reduction with moderate speed impact";
    if (type == "2hump_ei")
        return "Heavy smoothing — significant vibration issues detected";
    if (type == "3hump_ei")
        return "Maximum smoothing — consider checking mechanical issues";
    if (type == "zvd")
        return "ZVD — zero vibration derivative, moderate smoothing";
    return "Vibration compensation active";
}

int InputShaperPanel::get_vibration_quality(float vibrations) {
    if (vibrations < 5.0f)
        return 0; // excellent (green)
    if (vibrations < 15.0f)
        return 1; // good (yellow)
    if (vibrations < 25.0f)
        return 2; // fair (orange)
    return 3;     // poor (red)
}

const char* InputShaperPanel::get_quality_description(float vibrations) {
    if (vibrations < 5.0f)
        return "Excellent — minimal residual vibration";
    if (vibrations < 15.0f)
        return "Good — acceptable vibration level";
    if (vibrations < 25.0f)
        return "Fair — mechanical improvements could help";
    return "Poor — check for mechanical issues";
}

void InputShaperPanel::populate_axis_result(char axis, const InputShaperResult& result) {
    // Uppercase the shaper type for display
    std::string type_upper = result.shaper_type;
    for (auto& c : type_upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Format frequency
    char freq_buf[16];
    helix::format::format_frequency_hz(result.shaper_freq, freq_buf, sizeof(freq_buf));

    if (axis == 'X') {
        lv_subject_set_int(&is_results_has_x_, 1);

        snprintf(is_result_x_shaper_buf_, sizeof(is_result_x_shaper_buf_), "Optimal: %s @ %s",
                 type_upper.c_str(), freq_buf);
        lv_subject_copy_string(&is_result_x_shaper_, is_result_x_shaper_buf_);

        snprintf(is_result_x_explanation_buf_, sizeof(is_result_x_explanation_buf_), "* %s",
                 get_shaper_explanation(result.shaper_type));
        lv_subject_copy_string(&is_result_x_explanation_, is_result_x_explanation_buf_);

        snprintf(is_result_x_vibration_buf_, sizeof(is_result_x_vibration_buf_), "%.1f%%",
                 result.vibrations);
        lv_subject_copy_string(&is_result_x_vibration_, is_result_x_vibration_buf_);

        snprintf(is_result_x_max_accel_buf_, sizeof(is_result_x_max_accel_buf_),
                 "%.0f mm/s\xC2\xB2", result.max_accel);
        lv_subject_copy_string(&is_result_x_max_accel_, is_result_x_max_accel_buf_);

        lv_subject_set_int(&is_result_x_quality_, get_vibration_quality(result.vibrations));
    } else {
        lv_subject_set_int(&is_results_has_y_, 1);

        snprintf(is_result_y_shaper_buf_, sizeof(is_result_y_shaper_buf_), "Optimal: %s @ %s",
                 type_upper.c_str(), freq_buf);
        lv_subject_copy_string(&is_result_y_shaper_, is_result_y_shaper_buf_);

        snprintf(is_result_y_explanation_buf_, sizeof(is_result_y_explanation_buf_), "* %s",
                 get_shaper_explanation(result.shaper_type));
        lv_subject_copy_string(&is_result_y_explanation_, is_result_y_explanation_buf_);

        snprintf(is_result_y_vibration_buf_, sizeof(is_result_y_vibration_buf_), "%.1f%%",
                 result.vibrations);
        lv_subject_copy_string(&is_result_y_vibration_, is_result_y_vibration_buf_);

        snprintf(is_result_y_max_accel_buf_, sizeof(is_result_y_max_accel_buf_),
                 "%.0f mm/s\xC2\xB2", result.max_accel);
        lv_subject_copy_string(&is_result_y_max_accel_, is_result_y_max_accel_buf_);

        lv_subject_set_int(&is_result_y_quality_, get_vibration_quality(result.vibrations));
    }

    // The firmware-overwrite warning lives on the X card; the flag is set when
    // the Y result carrying the copy marker arrives (Calibrate All populates X
    // just before Y, so the row is already built when this flips it visible).
    lv_subject_set_int(&is_x_fw_overwrite_warn_, x_saved_value_overwritten_ ? 1 : 0);

    // Populate comparison table subjects
    auto& cmp = (axis == 'X') ? x_cmp_ : y_cmp_;
    auto& recommended_row = (axis == 'X') ? is_x_recommended_row_ : is_y_recommended_row_;
    auto& num_shapers = (axis == 'X') ? is_x_num_shapers_ : is_y_num_shapers_;
    lv_subject_set_int(&recommended_row, -1); // Reset
    lv_subject_set_int(&num_shapers, static_cast<int>(result.all_shapers.size()));

    for (size_t i = 0; i < MAX_SHAPERS; i++) {
        if (i < result.all_shapers.size()) {
            const auto& opt = result.all_shapers[i];

            // Type with * marker for recommended
            std::string type_upper = opt.type;
            for (auto& c : type_upper)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (opt.type == result.shaper_type) {
                type_upper += " *";
                lv_subject_set_int(&recommended_row, static_cast<int>(i));
            }
            snprintf(cmp[i].type_buf, CMP_TYPE_BUF, "%s", type_upper.c_str());
            lv_subject_copy_string(&cmp[i].type, cmp[i].type_buf);

            // Frequency
            helix::format::format_frequency_hz(opt.frequency, cmp[i].freq_buf, CMP_VALUE_BUF);
            lv_subject_copy_string(&cmp[i].freq, cmp[i].freq_buf);

            // Vibration with quality description
            const char* quality = get_quality_description(opt.vibrations);
            // Truncate quality to first word only for compact display
            std::string quality_word;
            if (quality) {
                const char* dash = strstr(quality, " \xe2\x80\x94");
                if (dash) {
                    quality_word = std::string(quality, static_cast<size_t>(dash - quality));
                } else {
                    quality_word = quality;
                }
            }
            snprintf(cmp[i].vib_buf, CMP_VALUE_BUF, "%.1f%% %s", opt.vibrations,
                     quality_word.c_str());
            lv_subject_copy_string(&cmp[i].vib, cmp[i].vib_buf);

            // Max accel
            snprintf(cmp[i].accel_buf, CMP_VALUE_BUF, "%.0f", opt.max_accel);
            lv_subject_copy_string(&cmp[i].accel, cmp[i].accel_buf);
        } else {
            // Clear unused rows
            lv_subject_copy_string(&cmp[i].type, "");
            lv_subject_copy_string(&cmp[i].freq, "");
            lv_subject_copy_string(&cmp[i].vib, "");
            lv_subject_copy_string(&cmp[i].accel, "");
        }
    }

    spdlog::debug("[InputShaper] Populated {} axis comparison table with {} shapers", axis,
                  result.all_shapers.size());

    // Live-before delta rows and residual verdict
    populate_axis_delta(axis, result);

    // Populate frequency response chart if data available
    populate_chart(axis, result);
}

// ============================================================================
// LIVE-BEFORE DELTA ROWS
// ============================================================================

void InputShaperPanel::populate_axis_delta(char axis, const InputShaperResult& result) {
    auto& delta_subject = (axis == 'X') ? is_x_delta_text_ : is_y_delta_text_;
    auto& delta_buf = (axis == 'X') ? is_x_delta_buf_ : is_y_delta_buf_;
    auto& has_delta = (axis == 'X') ? is_x_has_delta_ : is_y_has_delta_;
    auto& verdict_subject = (axis == 'X') ? is_x_verdict_text_ : is_y_verdict_text_;
    auto& verdict_buf = (axis == 'X') ? is_x_verdict_buf_ : is_y_verdict_buf_;
    auto& has_verdict = (axis == 'X') ? is_x_has_verdict_ : is_y_has_verdict_;

    lv_subject_set_int(&has_delta, 0);
    lv_subject_set_int(&has_verdict, 0);
    lv_subject_copy_string(&verdict_subject, "");

    std::string old_type;
    float old_freq = 0.0f;
    if (!result.is_valid() || !get_before_axis(axis, old_type, old_freq)) {
        lv_subject_copy_string(&delta_subject, "");
        return;
    }

    // "was" value, lowercase as the firmware reports it
    char was_buf[32];
    snprintf(was_buf, sizeof(was_buf), "%s @ %.1f Hz", old_type.c_str(), old_freq);

    char new_buf[32];
    snprintf(new_buf, sizeof(new_buf), "%s @ %.1f Hz", result.shaper_type.c_str(),
             result.shaper_freq);
    snprintf(delta_buf, sizeof(delta_buf), "%s -> %s", was_buf, new_buf);
    lv_subject_copy_string(&delta_subject, delta_buf);
    lv_subject_set_int(&has_delta, 1);

    // Verdict: the old setting re-scored against the freshly measured PSD next
    // to the new fit's residual. Needs PSD bins and a computable curve for the
    // old type (unported shaper types - e.g. Kalico smooth shapers - hide the
    // row rather than show a half-verdict).
    if (result.freq_response.empty()) {
        return;
    }
    std::vector<double> bins;
    std::vector<double> psd;
    bins.reserve(result.freq_response.size());
    psd.reserve(result.freq_response.size());
    for (const auto& [f, a] : result.freq_response) {
        bins.push_back(f);
        psd.push_back(a);
    }

    const std::vector<double> old_curve = calibration::shaper_transfer_curve(
        old_type, static_cast<double>(old_freq), before_damping_ratio(axis), bins);
    const double residual_old = calibration::residual_vibration_percent(psd, old_curve);
    if (residual_old < 0.0) {
        return;
    }

    // New fit: prefer the curve the firmware fitted and wrote into the CSV -
    // it carries the fork's own values. The parser shapes it by the raw PSD
    // (values = H * psd), so divide that back out bin by bin; a zero-PSD bin
    // contributes nothing to the verdict either way.
    std::vector<double> new_curve;
    for (const auto& curve : result.shaper_curves) {
        if (curve.name == result.shaper_type && curve.values.size() == psd.size()) {
            new_curve.reserve(psd.size());
            for (size_t i = 0; i < psd.size(); i++) {
                new_curve.push_back(psd[i] > 0.0f ? static_cast<double>(curve.values[i]) / psd[i]
                                                  : 0.0);
            }
            break;
        }
    }
    if (new_curve.empty()) {
        new_curve = calibration::shaper_transfer_curve(
            result.shaper_type, static_cast<double>(result.shaper_freq),
            calibration::SHAPER_DEFAULT_DAMPING_RATIO, bins);
    }
    const double residual_new = calibration::residual_vibration_percent(psd, new_curve);
    if (residual_new < 0.0) {
        return;
    }

    char old_pct[16];
    char new_pct[16];
    snprintf(old_pct, sizeof(old_pct), "%.1f%%", residual_old);
    snprintf(new_pct, sizeof(new_pct), "%.1f%%", residual_new);
    const std::string verdict =
        fmt::format(lv_tr("Old setting on today's data: {} residual - now: {}"), old_pct, new_pct);
    snprintf(verdict_buf, sizeof(verdict_buf), "%s", verdict.c_str());
    lv_subject_copy_string(&verdict_subject, verdict_buf);
    lv_subject_set_int(&has_verdict, 1);

    spdlog::debug("[InputShaper] {} axis delta: {} -> {} (residual {} -> {})", axis, was_buf,
                  new_buf, old_pct, new_pct);
}

// ============================================================================
// FREQUENCY RESPONSE CHART
// ============================================================================

void InputShaperPanel::create_chart_widgets() {
    auto tier = helix::PlatformCapabilities::detect().tier;

    // Create X axis chart
    lv_obj_t* x_container = lv_obj_find_by_name(overlay_root_, "chart_container_x");
    if (x_container) {
        x_chart_.chart = ui_frequency_response_chart_create(x_container);
        if (x_chart_.chart) {
            ui_frequency_response_chart_configure_for_platform(x_chart_.chart, tier);
            ui_frequency_response_chart_set_freq_range(x_chart_.chart, 0.0f, 200.0f);
        }
    }

    // Create Y axis chart
    lv_obj_t* y_container = lv_obj_find_by_name(overlay_root_, "chart_container_y");
    if (y_container) {
        y_chart_.chart = ui_frequency_response_chart_create(y_container);
        if (y_chart_.chart) {
            ui_frequency_response_chart_configure_for_platform(y_chart_.chart, tier);
            ui_frequency_response_chart_set_freq_range(y_chart_.chart, 0.0f, 200.0f);
        }
    }

    spdlog::debug("[InputShaper] Chart widgets created (tier: {})",
                  helix::platform_tier_to_string(tier));
}

void InputShaperPanel::populate_chart(char axis, const InputShaperResult& result) {
    auto& chart_data = (axis == 'X') ? x_chart_ : y_chart_;
    auto& chips = (axis == 'X') ? x_chips_ : y_chips_;
    auto& has_freq_data = (axis == 'X') ? is_x_has_freq_data_ : is_y_has_freq_data_;
    auto& num_chips = (axis == 'X') ? is_x_num_chips_ : is_y_num_chips_;

    // Check if freq data available
    if (result.freq_response.empty() || !chart_data.chart) {
        lv_subject_set_int(&has_freq_data, 0);
        lv_subject_set_int(&num_chips, 0);
        return;
    }

    lv_subject_set_int(&has_freq_data, 1);

    // Store the data
    chart_data.freq_response = result.freq_response;
    chart_data.shaper_curves = result.shaper_curves;

    // Extract frequencies and amplitudes
    std::vector<float> freqs;
    std::vector<float> amps;
    freqs.reserve(result.freq_response.size());
    amps.reserve(result.freq_response.size());
    for (const auto& [f, a] : result.freq_response) {
        freqs.push_back(f);
        amps.push_back(a);
    }

    // Guard against empty data (max_element on empty range is UB / SIGSEGV)
    if (amps.empty()) {
        spdlog::warn(
            "[InputShaper] {} axis: freq_response non-empty but produced no amplitude data", axis);
        lv_subject_set_int(&has_freq_data, 0);
        lv_subject_set_int(&num_chips, 0);
        return;
    }

    // Find max amplitude for Y range
    float max_amp = *std::max_element(amps.begin(), amps.end());
    ui_frequency_response_chart_set_amplitude_range(chart_data.chart, 0.0f, max_amp * 1.1f);

    // Add raw PSD series (always visible, semi-transparent light color)
    chart_data.raw_series_id =
        ui_frequency_response_chart_add_series(chart_data.chart, "Raw PSD", lv_color_hex(0xB0B0B0));
    ui_frequency_response_chart_set_data(chart_data.chart, chart_data.raw_series_id, freqs.data(),
                                         amps.data(), freqs.size());

    // Mark peak frequency
    auto peak_it = std::max_element(amps.begin(), amps.end());
    if (peak_it != amps.end()) {
        size_t peak_idx = static_cast<size_t>(std::distance(amps.begin(), peak_it));
        ui_frequency_response_chart_mark_peak(chart_data.chart, chart_data.raw_series_id,
                                              freqs[peak_idx], *peak_it);
    }

    // Add shaper overlay series
    for (size_t i = 0; i < chart_data.shaper_curves.size() && i < MAX_SHAPERS; i++) {
        const auto& curve = chart_data.shaper_curves[i];

        // Set chip label (uppercase name)
        std::string upper_name = curve.name;
        for (auto& c : upper_name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        snprintf(chips[i].label_buf, CHIP_LABEL_BUF, "%s", upper_name.c_str());
        lv_subject_copy_string(&chips[i].label, chips[i].label_buf);

        // Add chart series (initially hidden except recommended)
        lv_color_t color = lv_color_hex(SHAPER_OVERLAY_COLORS[i % NUM_SHAPER_COLORS]);
        chart_data.shaper_series_ids[i] =
            ui_frequency_response_chart_add_series(chart_data.chart, curve.name.c_str(), color);

        // Set shaper data (use same frequency bins, shaper's filtered values)
        if (!curve.values.empty()) {
            ui_frequency_response_chart_set_data(chart_data.chart, chart_data.shaper_series_ids[i],
                                                 freqs.data(), curve.values.data(),
                                                 std::min(freqs.size(), curve.values.size()));
        }

        // Pre-select the recommended shaper, hide others
        bool is_recommended = (curve.name == result.shaper_type);
        chart_data.shaper_visible[i] = is_recommended;
        ui_frequency_response_chart_show_series(chart_data.chart, chart_data.shaper_series_ids[i],
                                                is_recommended);
        lv_subject_set_int(&chips[i].active, is_recommended ? 1 : 0);
    }

    // Clear unused chips
    for (size_t i = chart_data.shaper_curves.size(); i < MAX_SHAPERS; i++) {
        chips[i].label_buf[0] = '\0';
        lv_subject_copy_string(&chips[i].label, chips[i].label_buf);
        lv_subject_set_int(&chips[i].active, 0);
    }

    // Hide the chips with no curve behind them (the XML has a fixed MAX_SHAPERS
    // of them, and empty outlines read as broken)
    lv_subject_set_int(&num_chips,
                       static_cast<int>(std::min(chart_data.shaper_curves.size(), MAX_SHAPERS)));

    // Overlay the live-before setting as a second shaper curve so the old
    // notch is comparable against the fresh fit on the same spectrum. Drawn
    // muted (thin, translucent) so the fitted curves stay dominant; skipped
    // when the old type has no ported transfer function or no series slot is
    // free.
    if (chart_data.old_setting_series_id >= 0) {
        ui_frequency_response_chart_remove_series(chart_data.chart,
                                                  chart_data.old_setting_series_id);
        chart_data.old_setting_series_id = -1;
    }
    std::string old_type;
    float old_freq = 0.0f;
    if (get_before_axis(axis, old_type, old_freq)) {
        std::vector<double> bins(freqs.begin(), freqs.end());
        const std::vector<double> old_curve = calibration::shaper_transfer_curve(
            old_type, static_cast<double>(old_freq), before_damping_ratio(axis), bins);
        if (!old_curve.empty()) {
            std::vector<float> old_shaped;
            old_shaped.reserve(freqs.size());
            for (size_t i = 0; i < freqs.size(); i++) {
                old_shaped.push_back(amps[i] * static_cast<float>(old_curve[i]));
            }
            const int old_id = ui_frequency_response_chart_add_series(
                chart_data.chart, (old_type + " (was)").c_str(),
                theme_manager_get_color("text_muted"));
            if (old_id >= 0) {
                chart_data.old_setting_series_id = old_id;
                ui_frequency_response_chart_set_series_muted(chart_data.chart, old_id, true);
                ui_frequency_response_chart_set_data(chart_data.chart, old_id, freqs.data(),
                                                     old_shaped.data(), freqs.size());
                ui_frequency_response_chart_show_series(chart_data.chart, old_id, true);
            }
        } else {
            spdlog::debug("[InputShaper] {} axis: live-before type '{}' has no ported transfer "
                          "curve - chart overlay skipped",
                          axis, old_type);
        }
    }

    // Update legend to reflect initially selected shaper
    update_legend(axis);

    spdlog::debug("[InputShaper] Chart populated for {} axis: {} freq bins, {} shaper curves", axis,
                  freqs.size(), chart_data.shaper_curves.size());
}

void InputShaperPanel::clear_chart(char axis) {
    auto& chart_data = (axis == 'X') ? x_chart_ : y_chart_;
    auto& chips = (axis == 'X') ? x_chips_ : y_chips_;
    auto& has_freq_data = (axis == 'X') ? is_x_has_freq_data_ : is_y_has_freq_data_;
    auto& num_chips = (axis == 'X') ? is_x_num_chips_ : is_y_num_chips_;

    lv_subject_set_int(&has_freq_data, 0);
    lv_subject_set_int(&num_chips, 0);

    if (chart_data.chart) {
        ui_frequency_response_chart_clear(chart_data.chart);
        // Remove all series
        if (chart_data.raw_series_id >= 0) {
            ui_frequency_response_chart_remove_series(chart_data.chart, chart_data.raw_series_id);
            chart_data.raw_series_id = -1;
        }
        for (size_t i = 0; i < MAX_SHAPERS; i++) {
            if (chart_data.shaper_series_ids[i] >= 0) {
                ui_frequency_response_chart_remove_series(chart_data.chart,
                                                          chart_data.shaper_series_ids[i]);
                chart_data.shaper_series_ids[i] = -1;
            }
            chart_data.shaper_visible[i] = false;
        }
        if (chart_data.old_setting_series_id >= 0) {
            ui_frequency_response_chart_remove_series(chart_data.chart,
                                                      chart_data.old_setting_series_id);
            chart_data.old_setting_series_id = -1;
        }
    }

    chart_data.freq_response.clear();
    chart_data.shaper_curves.clear();

    // Clear chip labels
    for (size_t i = 0; i < MAX_SHAPERS; i++) {
        chips[i].label_buf[0] = '\0';
        lv_subject_copy_string(&chips[i].label, chips[i].label_buf);
        lv_subject_set_int(&chips[i].active, 0);
    }
}

void InputShaperPanel::toggle_shaper_overlay(char axis, int index) {
    if (index < 0 || index >= static_cast<int>(MAX_SHAPERS))
        return;

    auto& chart_data = (axis == 'X') ? x_chart_ : y_chart_;
    auto& chips = (axis == 'X') ? x_chips_ : y_chips_;

    if (chart_data.shaper_series_ids[index] < 0)
        return;

    chart_data.shaper_visible[index] = !chart_data.shaper_visible[index];
    ui_frequency_response_chart_show_series(chart_data.chart, chart_data.shaper_series_ids[index],
                                            chart_data.shaper_visible[index]);
    lv_subject_set_int(&chips[index].active, chart_data.shaper_visible[index] ? 1 : 0);

    // Update legend to reflect new active shaper
    update_legend(axis);

    spdlog::debug("[InputShaper] Toggled {} axis shaper overlay {}: {}", axis, index,
                  chart_data.shaper_visible[index]);
}

void InputShaperPanel::update_legend(char axis) {
    auto& chart_data = (axis == 'X') ? x_chart_ : y_chart_;
    auto& chips = (axis == 'X') ? x_chips_ : y_chips_;
    auto& legend_label = (axis == 'X') ? is_x_legend_shaper_label_ : is_y_legend_shaper_label_;
    auto& legend_label_buf =
        (axis == 'X') ? is_x_legend_shaper_label_buf_ : is_y_legend_shaper_label_buf_;
    lv_obj_t* legend_dot = (axis == 'X') ? legend_x_shaper_dot_ : legend_y_shaper_dot_;

    // Find the last visible shaper to display in the legend
    // Prefer the highest-index visible shaper (most recently toggled on)
    int active_idx = -1;
    for (int i = static_cast<int>(MAX_SHAPERS) - 1; i >= 0; i--) {
        if (chart_data.shaper_visible[i] && chart_data.shaper_series_ids[i] >= 0) {
            active_idx = i;
            break;
        }
    }

    if (active_idx >= 0) {
        // Copy chip label text (already uppercase) to legend label
        snprintf(legend_label_buf, CHIP_LABEL_BUF, "%s", chips[active_idx].label_buf);
        lv_subject_copy_string(&legend_label, legend_label_buf);

        // Update dot color to match the active shaper's series color
        if (legend_dot) {
            lv_color_t color = lv_color_hex(SHAPER_OVERLAY_COLORS[active_idx % NUM_SHAPER_COLORS]);
            lv_obj_set_style_bg_color(legend_dot, color, LV_PART_MAIN);
        }
    } else {
        // No shaper visible — clear legend label
        legend_label_buf[0] = '\0';
        lv_subject_copy_string(&legend_label, legend_label_buf);
    }
}

void InputShaperPanel::handle_chip_x_clicked(int index) {
    toggle_shaper_overlay('X', index);
}

void InputShaperPanel::handle_chip_y_clicked(int index) {
    toggle_shaper_overlay('Y', index);
}

// ============================================================================
// DEMO INJECTION
// ============================================================================

void InputShaperPanel::inject_demo_results() {
    spdlog::info("[InputShaper] Injecting demo results for screenshot mode");

    const char* kalico_env = std::getenv("INPUT_SHAPER_DEMO_KALICO");
    bool kalico_demo = kalico_env && std::string(kalico_env) == "1";

    // Mock shaper options: Kalico reports smooth shapers + discrete, standard Klipper reports 5
    std::vector<ShaperOption> shaper_options;
    if (kalico_demo) {
        shaper_options = {
            {"smooth_zv", 60.2f, 4.8f, 0.040f, 14000.0f},
            {"smooth_mzv", 54.4f, 1.2f, 0.085f, 5200.0f},
            {"smooth_ei", 57.0f, 0.5f, 0.095f, 5000.0f},
            {"smooth_2hump_ei", 72.4f, 0.0f, 0.065f, 9200.0f},
            {"smooth_zvd_ei", 68.0f, 0.1f, 0.070f, 8400.0f},
            {"smooth_si", 52.0f, 0.0f, 0.110f, 4800.0f},
            {"mzv", 53.8f, 1.6f, 0.130f, 4000.0f},
            {"ei", 56.2f, 0.7f, 0.120f, 4600.0f},
            {"2hump_ei", 71.8f, 0.0f, 0.076f, 8800.0f},
        };
    } else {
        shaper_options = {
            {"zv", 59.0f, 5.2f, 0.045f, 13400.0f},      {"mzv", 53.8f, 1.6f, 0.130f, 4000.0f},
            {"ei", 56.2f, 0.7f, 0.120f, 4600.0f},       {"2hump_ei", 71.8f, 0.0f, 0.076f, 8800.0f},
            {"3hump_ei", 89.6f, 0.0f, 0.076f, 8800.0f},
        };
    }

    // The recommended shaper type and its parameters for the demo
    const std::string demo_recommended = kalico_demo ? "smooth_mzv" : "mzv";

    // Find the recommended shaper's parameters from the options list
    auto rec_it = std::find_if(shaper_options.begin(), shaper_options.end(),
                               [&](const ShaperOption& o) { return o.type == demo_recommended; });
    if (rec_it == shaper_options.end()) {
        spdlog::error("[InputShaper] Demo recommended shaper '{}' not found in options",
                      demo_recommended);
        return;
    }
    const auto& rec = *rec_it;

    // Generate frequency response data matching write_mock_shaper_csv()
    auto generate_freq_data = [](char axis, const std::vector<ShaperOption>& shapers) {
        std::vector<std::pair<float, float>> freq_response;
        std::vector<ShaperResponseCurve> shaper_curves;

        for (const auto& opt : shapers) {
            ShaperResponseCurve curve;
            curve.name = opt.type;
            curve.frequency = opt.frequency;
            shaper_curves.push_back(curve);
        }

        const float peak_freq = (axis == 'X') ? 53.8f : 48.2f;
        const float peak_width = 8.0f;
        const float peak_amp = 0.02f;
        const float noise_floor = 5e-4f;

        std::mt19937 rng(42 + static_cast<unsigned>(axis));
        std::uniform_real_distribution<float> noise_dist(0.8f, 1.2f);

        // Real transfer curves per shaper (same math the firmware uses), so
        // the demo's chart overlays and residual verdict look like a real run
        std::vector<std::vector<double>> transfer(shapers.size());
        std::vector<double> bins;
        for (float freq = 5.0f; freq <= 200.0f; freq += 4.0f) {
            bins.push_back(freq);
        }
        for (size_t i = 0; i < shapers.size(); i++) {
            transfer[i] =
                calibration::shaper_transfer_curve(shapers[i].type, shapers[i].frequency,
                                                   calibration::SHAPER_DEFAULT_DAMPING_RATIO, bins);
        }

        for (size_t bin = 0; bin < bins.size(); bin++) {
            const float freq = static_cast<float>(bins[bin]);
            float df = freq - peak_freq;
            float resonance = peak_amp / (1.0f + (df * df) / (peak_width * peak_width));
            float base_psd = noise_floor * noise_dist(rng) + resonance;

            if (freq > 120.0f) {
                base_psd *= std::exp(-(freq - 120.0f) / 60.0f);
            }

            float psd_main = base_psd;
            float psd_cross = base_psd * 0.15f * noise_dist(rng);
            float psd_z = base_psd * 0.08f * noise_dist(rng);
            float psd_xyz = psd_main + psd_cross + psd_z;

            freq_response.push_back({freq, psd_xyz});

            for (size_t i = 0; i < shapers.size(); i++) {
                // Unported shaper types (Kalico smooth shapers in the demo set)
                // degrade to a flat passband, never an empty series
                const double h = (bin < transfer[i].size()) ? transfer[i][bin] : 1.0;
                shaper_curves[i].values.push_back(psd_xyz * static_cast<float>(h));
            }
        }

        return std::make_pair(freq_response, shaper_curves);
    };

    // Build per-axis result using shared recommended shaper parameters
    auto build_result = [&](char axis) {
        InputShaperResult result;
        result.axis = axis;
        result.shaper_type = rec.type;
        result.shaper_freq = rec.frequency;
        result.max_accel = rec.max_accel;
        result.smoothing = rec.smoothing;
        result.vibrations = rec.vibrations;
        result.all_shapers = shaper_options;
        auto [freq, curves] = generate_freq_data(axis, shaper_options);
        result.freq_response = std::move(freq);
        result.shaper_curves = std::move(curves);
        return result;
    };

    auto x_result = build_result('X');
    auto y_result = build_result('Y');

    // Store recommendation for Apply button
    recommended_type_ = rec.type;
    recommended_freq_ = rec.frequency;
    x_result_ = x_result;

    // Demo live-before config so the delta rows, the residual verdict, and the
    // muted "was" chart overlay appear in screenshot/demo mode
    config_before_ = InputShaperConfig{};
    config_before_.is_configured = true;
    config_before_.shaper_type_x = "ei";
    config_before_.shaper_freq_x = 51.2f;
    config_before_.shaper_type_y = "ei";
    config_before_.shaper_freq_y = 44.0f;
    has_config_before_ = true;

    // Populate both axes (uses existing private methods)
    lv_subject_set_int(&is_results_has_x_, 0);
    lv_subject_set_int(&is_results_has_y_, 0);

    populate_axis_result('X', x_result);
    populate_axis_result('Y', y_result);

    set_state(State::RESULTS);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void InputShaperPanel::handle_calibrate_all_clicked() {
    if (state_ != State::IDLE)
        return;
    spdlog::debug("[InputShaper] Calibrate All clicked");
    calibrate_all();
}

void InputShaperPanel::handle_calibrate_x_clicked() {
    if (state_ != State::IDLE)
        return;
    spdlog::debug("[InputShaper] Calibrate X clicked");
    calibrate_all_mode_ = false;
    start_with_preflight('X');
}

void InputShaperPanel::handle_calibrate_y_clicked() {
    if (state_ != State::IDLE)
        return;
    spdlog::debug("[InputShaper] Calibrate Y clicked");
    calibrate_all_mode_ = false;
    start_with_preflight('Y');
}

void InputShaperPanel::handle_measure_noise_clicked() {
    if (state_ != State::IDLE) {
        return;
    }
    spdlog::debug("[InputShaper] Measure Noise clicked");
    measure_noise();
}

void InputShaperPanel::handle_cancel_clicked() {
    spdlog::debug("[InputShaper] Cancel clicked");
    cancel_calibration();
}

void InputShaperPanel::handle_apply_clicked() {
    spdlog::debug("[InputShaper] Apply clicked");
    apply_recommendation();
}

void InputShaperPanel::handle_close_clicked() {
    spdlog::debug("[InputShaper] Close clicked");
    clear_results();
    set_state(State::IDLE);
    NavigationManager::instance().go_back();
}

void InputShaperPanel::handle_retry_clicked() {
    spdlog::debug("[InputShaper] Retry clicked");
    calibrate_all_mode_ = false;
    start_with_preflight(current_axis_);
}

void InputShaperPanel::handle_save_config_clicked() {
    spdlog::debug("[InputShaper] Save Config clicked");
    clear_results();
    set_state(State::IDLE);
    NavigationManager::instance().go_back();
    save_configuration();
}

void InputShaperPanel::handle_save_clicked() {
    spdlog::debug("[InputShaper] Save clicked — applying and saving to config");
    apply_recommendation();
    clear_results();
    set_state(State::IDLE);
    NavigationManager::instance().go_back();
    save_configuration();
}

void InputShaperPanel::handle_print_test_pattern_clicked() {
    if (!api_) {
        spdlog::warn("[InputShaper] Cannot print test: API not set");
        return;
    }

    // TUNING_TOWER enables acceleration ramping during print
    // This allows user to visually compare ringing at different accelerations
    const std::string tuning_tower_cmd =
        "TUNING_TOWER COMMAND=SET_VELOCITY_LIMIT PARAMETER=ACCEL START=1500 FACTOR=500 BAND=5";

    spdlog::info("[InputShaper] Enabling tuning tower for test print");

    auto tok = lifetime_.token();

    api_->execute_gcode(
        tuning_tower_cmd,
        [tok]() {
            if (tok.expired())
                return;
            spdlog::info("[InputShaper] Tuning tower enabled - start a print to test calibration");
            ToastManager::instance().show(
                ToastSeverity::INFO, lv_tr("Tuning tower enabled - start a print to test"), 3000);
        },
        [tok](const MoonrakerError& err) {
            if (tok.expired())
                return;
            spdlog::error("[InputShaper] Failed to enable tuning tower: {}", err.message);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Failed to enable tuning tower"), 3000);
        });
}

void InputShaperPanel::handle_help_clicked() {
    spdlog::debug("[InputShaper] Help clicked - showing help modal");

    // Detailed help text explaining Input Shaper calibration
    static const char* help_message =
        "Input Shaper reduces ringing and ghosting artifacts caused by "
        "printer vibrations during fast movements.\n\n"

        "REQUIREMENTS:\n"
        "• ADXL345 accelerometer connected to your toolhead\n"
        "• [resonance_tester] section configured in printer.cfg\n"
        "• [input_shaper] section in printer.cfg (can be empty initially)\n\n"

        "HOW TO USE:\n"
        "1. Tap 'Measure Noise' first to verify accelerometer is working\n"
        "2. Tap 'Calibrate X' to measure X-axis resonance (~1-2 min)\n"
        "3. Tap 'Calibrate Y' to measure Y-axis resonance (~1-2 min)\n"
        "4. Review results and tap 'Apply' to use recommended settings\n"
        "5. Optionally 'Save Config' to make permanent (restarts Klipper)\n\n"

        "SHAPER TYPES:\n"
        "• ZV - Lowest smoothing, good for low vibration printers\n"
        "• MZV - Balanced choice, recommended for most printers\n"
        "• EI - More aggressive, better vibration reduction\n"
        "• 2HUMP_EI / 3HUMP_EI - Maximum reduction, more smoothing\n\n"

        "Lower vibration % is better. Lower smoothing preserves detail.";

    helix::ui::modal_show_alert(lv_tr("Input Shaper Help"), help_message, ModalSeverity::Info,
                                lv_tr("Got it"));
}
