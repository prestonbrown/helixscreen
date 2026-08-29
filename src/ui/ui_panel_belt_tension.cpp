// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_belt_tension.h"

#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<BeltTensionPanel> g_belt_tension_panel;

// State subject (0=START, 1=PROGRESS, 2=RESULTS, 3=STROBE, 4=ERROR)
static lv_subject_t s_belt_tension_state;

// Forward declarations
static void on_belt_tension_row_clicked(lv_event_t* e);

BeltTensionPanel& get_global_belt_tension_panel() {
    if (!g_belt_tension_panel) {
        g_belt_tension_panel = std::make_unique<BeltTensionPanel>();
        StaticPanelRegistry::instance().register_destroy("BeltTensionPanel",
                                                         []() { g_belt_tension_panel.reset(); });
    }
    return *g_belt_tension_panel;
}

BeltTensionPanel::~BeltTensionPanel() {
    // lifetime_ destructor auto-invalidates all outstanding tokens

    // Deinitialize subjects to disconnect observers before destruction
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    parent_screen_ = nullptr;

    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[BeltTension] Destroyed");
    }
}

void init_belt_tension_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_belt_tension_row_clicked", on_belt_tension_row_clicked);
    spdlog::trace("[BeltTension] Row click callback registered");
}

/**
 * @brief Row click handler for opening belt tension from Advanced panel
 */
static void on_belt_tension_row_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[BeltTension] Belt Tension row clicked");

    auto& panel = get_global_belt_tension_panel();

    // Lazy-create the panel
    if (!panel.get_root()) {
        spdlog::debug("[BeltTension] Creating belt tension panel...");

        // Set API references before create
        auto* client = get_moonraker_client();
        IMoonrakerAPI* api = get_moonraker_api();
        panel.set_api(client, api);

        lv_obj_t* screen = lv_display_get_screen_active(nullptr);
        if (!panel.create(screen)) {
            spdlog::error("[BeltTension] Failed to create panel_belt_tension");
            return;
        }
        spdlog::info("[BeltTension] Panel created");
    }

    // Show the overlay
    panel.show();
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

void ui_panel_belt_tension_register_callbacks() {
    register_xml_callbacks({
        {"belt_tension_start_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_start_clicked(); }},
        {"belt_tension_cancel_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_cancel_clicked(); }},
        {"belt_tension_strobe_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_strobe_clicked(); }},
        {"belt_tension_strobe_freq_up_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_strobe_freq_up(); }},
        {"belt_tension_strobe_freq_down_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_strobe_freq_down(); }},
        {"belt_tension_lock_a_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_lock_a_clicked(); }},
        {"belt_tension_lock_b_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_lock_b_clicked(); }},
        {"belt_tension_back_to_results_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_back_to_results_clicked();
         }},
        {"belt_tension_retry_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_retry_clicked(); }},
        {"belt_tension_help_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_alert(
                 lv_tr("Belt Tension Check"),
                 lv_tr("Uneven belt tension causes print artifacts like layer shifts, "
                       "VFAs (vertical fine artifacts), and ringing.\n\n"
                       "This tool vibrates each belt path and measures the resonant "
                       "frequency \u2014 matched frequencies mean balanced tension.\n\n"
                       "For CoreXY, Path A and B should be within a few Hz of each other."),
                 ModalSeverity::Info, lv_tr("Got it"));
         }},
        {"belt_tension_results_help_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_alert(
                 lv_tr("Understanding Results"),
                 lv_tr("Frequency Delta: Difference between Path A and B. "
                       "Ideally under 5 Hz; over 15 Hz needs adjustment.\n\n"
                       "Path Similarity: How closely the vibration profiles match. "
                       "Above 90% is excellent; below 70% suggests uneven tension."),
                 ModalSeverity::Info, lv_tr("Got it"));
         }},
        {"belt_tension_strobe_help_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_alert(
                 lv_tr("Visual Fine-Tuning"),
                 lv_tr("The motor vibrates the belt while a strobe light flashes at the "
                       "same frequency. When the belt appears to stand still (frozen), "
                       "you have found the resonant frequency.\n\n"
                       "Use the frequency buttons to sweep through values until the belt "
                       "looks stationary, then lock the reading."),
                 ModalSeverity::Info, lv_tr("Got it"));
         }},
    });

    // Initialize subjects BEFORE XML creation
    auto& panel = get_global_belt_tension_panel();
    panel.init_subjects();

    spdlog::debug("[BeltTension] Registered XML event callbacks");
}

// ============================================================================
// SUBJECT INITIALIZATION
// ============================================================================

void BeltTensionPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // View state subject for state machine visibility
    UI_MANAGED_SUBJECT_INT(s_belt_tension_state, 0, "belt_tension_state", subjects_);

    // Start screen subjects
    UI_MANAGED_SUBJECT_STRING(hw_kinematics_subject_, hw_kinematics_buf_, lv_tr("Detecting..."),
                              "bt_hw_kinematics", subjects_);
    UI_MANAGED_SUBJECT_STRING(hw_adxl_subject_, hw_adxl_buf_, lv_tr("Detecting..."), "bt_hw_adxl",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(hw_led_subject_, hw_led_buf_, lv_tr("Detecting..."), "bt_hw_led",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(target_freq_subject_, target_freq_buf_, "110 Hz", "bt_target_freq",
                              subjects_);

    // Progress subjects
    UI_MANAGED_SUBJECT_INT(progress_subject_, 0, "bt_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(progress_label_subject_, progress_label_buf_, lv_tr("Preparing..."),
                              "bt_progress_label", subjects_);

    // Result subjects
    UI_MANAGED_SUBJECT_STRING(result_a_freq_subject_, result_a_freq_buf_, "--", "bt_result_a_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(result_a_status_subject_, result_a_status_buf_, "",
                              "bt_result_a_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(result_b_freq_subject_, result_b_freq_buf_, "--", "bt_result_b_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(result_b_status_subject_, result_b_status_buf_, "",
                              "bt_result_b_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(result_delta_subject_, result_delta_buf_, "", "bt_result_delta",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(result_similarity_subject_, result_similarity_buf_, "",
                              "bt_result_similarity", subjects_);
    UI_MANAGED_SUBJECT_STRING(result_recommendation_subject_, result_recommendation_buf_, "",
                              "bt_result_recommendation", subjects_);
    UI_MANAGED_SUBJECT_INT(has_results_subject_, 0, "bt_has_results", subjects_);
    UI_MANAGED_SUBJECT_INT(has_strobe_subject_, 0, "bt_has_strobe", subjects_);

    // Strobe subjects
    UI_MANAGED_SUBJECT_STRING(strobe_freq_subject_, strobe_freq_buf_, "100.0 Hz", "bt_strobe_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(strobe_instruction_subject_, strobe_instruction_buf_,
                              lv_tr("Adjust belt tension until the belt appears stationary"),
                              "bt_strobe_instruction", subjects_);

    // Error subject
    UI_MANAGED_SUBJECT_STRING(error_message_subject_, error_message_buf_,
                              lv_tr("An error occurred during measurement."), "bt_error_message",
                              subjects_);

    subjects_initialized_ = true;

    // Register cleanup for shutdown safety
    StaticSubjectRegistry::instance().register_deinit("BeltTensionPanel", []() {
        if (g_belt_tension_panel) {
            g_belt_tension_panel->deinit_subjects();
        }
    });

    spdlog::debug("[BeltTension] Subjects initialized and registered");
}

void BeltTensionPanel::deinit_subjects() {
    // Expire outstanding async tokens here, not only in cleanup()/on_deactivate():
    // subjects can be torn down and re-inited on a LIVE panel (shutdown registry,
    // test isolation), and a queued callback would otherwise write into a subject
    // that was deinited underneath it (prestonbrown/helixscreen#1146).
    lifetime_.invalidate();

    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }
}

// ============================================================================
// CREATE
// ============================================================================

lv_obj_t* BeltTensionPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[BeltTension] Panel already created");
        return overlay_root_;
    }

    parent_screen_ = parent;

    spdlog::debug("[BeltTension] Creating overlay from XML");
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "panel_belt_tension", nullptr));

    if (!overlay_root_) {
        spdlog::error("[BeltTension] Failed to create overlay from XML");
        return nullptr;
    }

    // Start hidden (push_overlay will show it)
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Set initial state
    set_view_state(ViewState::START);

    spdlog::info("[BeltTension] Overlay created successfully");
    return overlay_root_;
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void BeltTensionPanel::set_view_state(ViewState state) {
    spdlog::debug("[BeltTension] View state change: {} -> {}",
                  lv_subject_get_int(&s_belt_tension_state), static_cast<int>(state));

    // Update subject - XML bindings handle visibility automatically
    lv_subject_set_int(&s_belt_tension_state, static_cast<int>(state));

    // Show restart action button in header only when results are displayed
    if (overlay_root_) {
        lv_obj_t* action_btn = lv_obj_find_by_name(overlay_root_, "action_button");
        if (action_btn) {
            if (state == ViewState::RESULTS) {
                lv_obj_remove_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ============================================================================
// SHOW / LIFECYCLE
// ============================================================================

void BeltTensionPanel::set_api(helix::IMoonrakerClient* client, IMoonrakerAPI* api) {
    client_ = client;
    api_ = api;

    // Create calibrator with API
    calibrator_ = std::make_unique<helix::calibration::BeltTensionCalibrator>(api_);
    spdlog::debug("[BeltTension] Calibrator created");
}

void BeltTensionPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[BeltTension] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[BeltTension] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[BeltTension] Overlay shown");
}

void BeltTensionPanel::on_activate() {
    OverlayBase::on_activate();

    spdlog::debug("[BeltTension] on_activate()");

    // Reset to start state
    set_view_state(ViewState::START);

    // Reset subjects to defaults
    lv_subject_set_int(&progress_subject_, 0);
    lv_subject_set_int(&has_results_subject_, 0);
    lv_subject_set_int(&has_strobe_subject_, 0);

    // Detect hardware capabilities
    if (calibrator_) {
        calibrator_->detect_hardware(
            lifetime_.bg_cb("BeltTensionPanel::detect_hardware",
                            [this](const helix::calibration::BeltTensionHardware& hw) {
                                on_hardware_detected(hw);
                            }),
            lifetime_.bg_cb("BeltTensionPanel::detect_hw_error", [this](const std::string& msg) {
                spdlog::warn("[BeltTension] Hardware detection failed: {}", msg);
                // Show defaults, user can still try
                snprintf(hw_kinematics_buf_, sizeof(hw_kinematics_buf_), "%s", lv_tr("Unknown"));
                lv_subject_notify(&hw_kinematics_subject_);
                snprintf(hw_adxl_buf_, sizeof(hw_adxl_buf_), "%s", lv_tr("Not detected"));
                lv_subject_notify(&hw_adxl_subject_);
                snprintf(hw_led_buf_, sizeof(hw_led_buf_), "%s", lv_tr("Not detected"));
                lv_subject_notify(&hw_led_subject_);
            }));
    }
}

void BeltTensionPanel::on_deactivate() {
    spdlog::debug("[BeltTension] on_deactivate()");

    // Cancel any in-progress calibration
    auto state = static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state));
    if (state == ViewState::PROGRESS && calibrator_) {
        spdlog::info("[BeltTension] Cancelling measurement on deactivate");
        calibrator_->reset();
        set_view_state(ViewState::START);
    }

    OverlayBase::on_deactivate();
}

void BeltTensionPanel::cleanup() {
    spdlog::debug("[BeltTension] Cleaning up");

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // Unregister from NavigationManager
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    OverlayBase::cleanup();
}

void BeltTensionPanel::on_ui_destroyed() {
    // Destroy chart if created
    if (chart_) {
        ui_frequency_response_chart_destroy(chart_);
        chart_ = nullptr;
    }
    chart_series_a_ = -1;
    chart_series_b_ = -1;
}

// ============================================================================
// HARDWARE DETECTION CALLBACK
// ============================================================================

void BeltTensionPanel::on_hardware_detected(const helix::calibration::BeltTensionHardware& hw) {
    detected_hw_ = hw;

    // Update kinematics display
    const char* kin_label = lv_tr("Unknown");
    switch (hw.kinematics) {
    case helix::calibration::KinematicsType::COREXY:
        kin_label = "CoreXY";
        break;
    case helix::calibration::KinematicsType::CARTESIAN:
        kin_label = "Cartesian";
        break;
    default:
        kin_label = hw.kinematics_name.empty() ? lv_tr("Unknown") : hw.kinematics_name.c_str();
        break;
    }
    snprintf(hw_kinematics_buf_, sizeof(hw_kinematics_buf_), "%s", kin_label);
    lv_subject_notify(&hw_kinematics_subject_);

    // Update ADXL status
    snprintf(hw_adxl_buf_, sizeof(hw_adxl_buf_), "%s",
             hw.has_adxl ? lv_tr("Connected (auto-sweep)") : lv_tr("Not found (strobe only)"));
    lv_subject_notify(&hw_adxl_subject_);

    // Update LED status
    if (hw.has_pwm_led) {
        snprintf(hw_led_buf_, sizeof(hw_led_buf_), lv_tr("Available (%s)"), hw.pwm_led_pin.c_str());
    } else {
        snprintf(hw_led_buf_, sizeof(hw_led_buf_), "%s", lv_tr("Not found (use phone app)"));
    }
    lv_subject_notify(&hw_led_subject_);

    // Enable strobe button if LED available
    lv_subject_set_int(&has_strobe_subject_, hw.has_pwm_led ? 1 : 0);

    spdlog::info("[BeltTension] Hardware: {} ADXL={} LED={}", kin_label, hw.has_adxl,
                 hw.has_pwm_led);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void BeltTensionPanel::handle_start_clicked() {
    spdlog::info("[BeltTension] Start clicked");

    if (!calibrator_) {
        on_error(lv_tr("Calibrator not initialized. Try reopening the panel."));
        return;
    }

    set_view_state(ViewState::PROGRESS);
    lv_subject_set_int(&progress_subject_, 0);
    snprintf(progress_label_buf_, sizeof(progress_label_buf_), "%s",
             lv_tr("Starting measurement..."));
    lv_subject_notify(&progress_label_subject_);

    calibrator_->run_auto_sweep(
        // Progress callback
        lifetime_.bg_cb("BeltTensionPanel::sweep_progress",
                        [this](int percent) {
                            lv_subject_set_int(&progress_subject_, percent);

                            if (percent < 50) {
                                snprintf(progress_label_buf_, sizeof(progress_label_buf_),
                                         lv_tr("Measuring Path A... %d%%"), percent * 2);
                            } else {
                                snprintf(progress_label_buf_, sizeof(progress_label_buf_),
                                         lv_tr("Measuring Path B... %d%%"), (percent - 50) * 2);
                            }
                            lv_subject_notify(&progress_label_subject_);
                        }),
        // Complete callback
        lifetime_.bg_cb("BeltTensionPanel::sweep_complete",
                        [this](const helix::calibration::BeltTensionResult& result) {
                            on_sweep_complete(result);
                        }),
        // Error callback
        lifetime_.bg_cb("BeltTensionPanel::sweep_error",
                        [this](const std::string& msg) { on_error(msg); }));
}

void BeltTensionPanel::handle_cancel_clicked() {
    spdlog::info("[BeltTension] Cancel clicked");

    if (calibrator_) {
        calibrator_->reset();
    }
    set_view_state(ViewState::START);
}

void BeltTensionPanel::handle_strobe_clicked() {
    spdlog::info("[BeltTension] Strobe mode clicked");

    if (!detected_hw_.has_pwm_led) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("No PWM LED detected for strobe mode"));
        return;
    }

    // Set strobe frequency to the average of the two measured frequencies
    if (last_result_.is_complete()) {
        current_strobe_freq_ =
            (last_result_.path_a.peak_frequency + last_result_.path_b.peak_frequency) / 2.0f;
    } else {
        current_strobe_freq_ = last_result_.target_frequency;
    }

    update_strobe_display();
    set_view_state(ViewState::STROBE);
}

void BeltTensionPanel::handle_strobe_freq_up() {
    current_strobe_freq_ += 0.5f;
    update_strobe_display();
    spdlog::debug("[BeltTension] Strobe freq up: {:.1f} Hz", current_strobe_freq_);
}

void BeltTensionPanel::handle_strobe_freq_down() {
    if (current_strobe_freq_ > 1.0f) {
        current_strobe_freq_ -= 0.5f;
    }
    update_strobe_display();
    spdlog::debug("[BeltTension] Strobe freq down: {:.1f} Hz", current_strobe_freq_);
}

void BeltTensionPanel::handle_lock_a_clicked() {
    spdlog::info("[BeltTension] Lock A clicked (strobe placeholder)");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Belt A frequency locked"));
}

void BeltTensionPanel::handle_lock_b_clicked() {
    spdlog::info("[BeltTension] Lock B clicked (strobe placeholder)");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Belt B frequency locked"));
}

void BeltTensionPanel::handle_back_to_results_clicked() {
    spdlog::debug("[BeltTension] Back to results");
    set_view_state(ViewState::RESULTS);
}

void BeltTensionPanel::handle_retry_clicked() {
    spdlog::info("[BeltTension] Retry clicked");
    set_view_state(ViewState::START);
}

// ============================================================================
// RESULT CALLBACKS
// ============================================================================

void BeltTensionPanel::on_sweep_complete(const helix::calibration::BeltTensionResult& result) {
    spdlog::info("[BeltTension] Sweep complete: A={:.1f}Hz B={:.1f}Hz delta={:.1f}Hz sim={:.0f}%",
                 result.path_a.peak_frequency, result.path_b.peak_frequency, result.frequency_delta,
                 result.similarity_percent);

    last_result_ = result;
    populate_results(result);
    set_view_state(ViewState::RESULTS);
}

void BeltTensionPanel::on_error(const std::string& message) {
    spdlog::error("[BeltTension] Error: {}", message);

    snprintf(error_message_buf_, sizeof(error_message_buf_), "%s", message.c_str());
    lv_subject_notify(&error_message_subject_);
    set_view_state(ViewState::ERROR);
}

void BeltTensionPanel::populate_results(const helix::calibration::BeltTensionResult& result) {
    // Path A frequency and status
    snprintf(result_a_freq_buf_, sizeof(result_a_freq_buf_), "%.1f Hz",
             result.path_a.peak_frequency);
    lv_subject_notify(&result_a_freq_subject_);

    snprintf(result_a_status_buf_, sizeof(result_a_status_buf_), "%s",
             helix::calibration::belt_status_to_string(result.path_a.status));
    lv_subject_notify(&result_a_status_subject_);

    // Path B frequency and status
    snprintf(result_b_freq_buf_, sizeof(result_b_freq_buf_), "%.1f Hz",
             result.path_b.peak_frequency);
    lv_subject_notify(&result_b_freq_subject_);

    snprintf(result_b_status_buf_, sizeof(result_b_status_buf_), "%s",
             helix::calibration::belt_status_to_string(result.path_b.status));
    lv_subject_notify(&result_b_status_subject_);

    // Delta
    snprintf(result_delta_buf_, sizeof(result_delta_buf_), lv_tr("%.1f Hz difference"),
             result.frequency_delta);
    lv_subject_notify(&result_delta_subject_);

    // Similarity
    snprintf(result_similarity_buf_, sizeof(result_similarity_buf_), "%.0f%%",
             result.similarity_percent);
    lv_subject_notify(&result_similarity_subject_);

    // Recommendation
    std::string rec = result.recommendation();
    snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_), "%s", rec.c_str());
    lv_subject_notify(&result_recommendation_subject_);

    // Mark that we have results
    lv_subject_set_int(&has_results_subject_, 1);
}

void BeltTensionPanel::update_strobe_display() {
    snprintf(strobe_freq_buf_, sizeof(strobe_freq_buf_), "%.1f Hz", current_strobe_freq_);
    lv_subject_notify(&strobe_freq_subject_);

    // Update instruction text based on hardware capabilities
    bool has_led = calibrator_ && calibrator_->get_hardware().has_pwm_led;
    if (has_led) {
        snprintf(strobe_instruction_buf_, sizeof(strobe_instruction_buf_), "%s",
                 lv_tr("Watch the belt under the strobe LED. "
                       "Adjust frequency until the belt appears stationary — "
                       "that is the resonant frequency."));
    } else {
        snprintf(strobe_instruction_buf_, sizeof(strobe_instruction_buf_),
                 lv_tr("Set a phone strobe app to %.1f Hz and aim at the belt. "
                       "Adjust until the belt appears stationary.\n\n"
                       "Recommended apps:\n"
                       "  Android: Strobily, Strobe Light\n"
                       "  iOS: Strobe Light Tachometer, myStroboscope"),
                 current_strobe_freq_);
    }
    lv_subject_notify(&strobe_instruction_subject_);
}
