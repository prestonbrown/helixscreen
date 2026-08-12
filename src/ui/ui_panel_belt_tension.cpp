// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_belt_tension.h"

#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "belt_dsp_probe.h"
#include "belt_gating.h"
#include "belt_stream_client.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "toolhead_homing.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<BeltTensionPanel> g_belt_tension_panel;

// State subject (0=START, 1=POSITION, 2=LISTEN, 3=COMPARE, 4=ERROR)
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

    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    gate_observers_wired_ = false;

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
        MoonrakerAPI* api = get_moonraker_api();
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
        {"belt_tension_retry_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_retry_clicked(); }},
        {"belt_tension_listen_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_position_confirmed(); }},
        {"belt_tension_help_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_show_alert(
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
             helix::ui::modal_show_alert(
                 lv_tr("Understanding Results"),
                 lv_tr("Frequency Delta: Difference between Path A and B. "
                       "Ideally under 5 Hz; over 15 Hz needs adjustment.\n\n"
                       "Path Similarity: How closely the vibration profiles match. "
                       "Above 90% is excellent; below 70% suggests uneven tension."),
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
    UI_MANAGED_SUBJECT_STRING(target_freq_subject_, target_freq_buf_, "110 Hz", "bt_target_freq",
                              subjects_);

    // Gate subjects
    UI_MANAGED_SUBJECT_INT(can_start_subject_, 0, "bt_can_start", subjects_);
    UI_MANAGED_SUBJECT_STRING(
        gate_message_subject_, gate_message_buf_,
        lv_tr(helix::calibration::belt_gate_message(helix::calibration::BeltGate::NOT_CONNECTED)),
        "bt_gate_message", subjects_);

    // Positioning subjects
    UI_MANAGED_SUBJECT_INT(has_target_subject_, 0, "bt_has_target", subjects_);
    UI_MANAGED_SUBJECT_STRING(park_status_subject_, park_status_buf_, lv_tr("Preparing..."),
                              "bt_park_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(current_belt_subject_, current_belt_buf_, "A", "bt_current_belt",
                              subjects_);

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

    // Drop the gate observers before the subjects they watch can go: they are
    // re-attached on the next create()/on_activate().
    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    gate_observers_wired_ = false;

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

    ensure_gate_observers();
    refresh_gate();

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
            if (state == ViewState::COMPARE) {
                lv_obj_remove_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(action_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ============================================================================
// PRECONDITION GATE
// ============================================================================

void BeltTensionPanel::refresh_gate() {
    auto& ps = get_printer_state();

    // printer_has_accelerometer lives on PrinterCapabilitiesState and is not
    // re-exported on PrinterState, so reach it through the XML subject registry
    // the way WizardInputShaperStep::has_accelerometer() does. It can be absent
    // before discovery has run; absent means "no accelerometer", never "yes".
    lv_subject_t* accel_subj = lv_xml_get_subject(nullptr, "printer_has_accelerometer");

    helix::calibration::BeltGateInputs in;
    in.connected = lv_subject_get_int(ps.get_nav_buttons_enabled_subject()) != 0;
    in.has_accelerometer = accel_subj && lv_subject_get_int(accel_subj) != 0;
    in.is_corexy = detected_hw_.kinematics == helix::calibration::KinematicsType::COREXY;
    in.klippy_socket_reachable = klippy_socket_reachable_;
    in.dsp_capable = helix::calibration::cached_dsp_probe().capable;
    in.print_active = lv_subject_get_int(ps.get_print_active_subject()) != 0;

    const auto gate = helix::calibration::evaluate_belt_gate(in);
    lv_subject_set_int(&can_start_subject_, gate == helix::calibration::BeltGate::OK ? 1 : 0);
    lv_subject_copy_string(&gate_message_subject_,
                           lv_tr(helix::calibration::belt_gate_message(gate)));
    spdlog::debug("[BeltTension] gate = {}", helix::calibration::belt_gate_message(gate));
}

void BeltTensionPanel::ensure_gate_observers() {
    if (gate_observers_wired_) {
        return;
    }

    auto& ps = get_printer_state();

    // Same registry lookup as refresh_gate(). Skip the observer when the
    // subject is absent rather than handing null to the factory - the next
    // activation retries, because gate_observers_wired_ stays false.
    lv_subject_t* accel_subj = lv_xml_get_subject(nullptr, "printer_has_accelerometer");
    if (!accel_subj) {
        spdlog::debug("[BeltTension] printer_has_accelerometer not registered yet");
        return;
    }

    accel_observer_ = helix::ui::observe_int_sync<BeltTensionPanel>(
        accel_subj, this, [](BeltTensionPanel* self, int) { self->refresh_gate(); },
        ps.get_subjects_lifetime());
    print_active_observer_ = helix::ui::observe_int_sync<BeltTensionPanel>(
        ps.get_print_active_subject(), this,
        [](BeltTensionPanel* self, int) { self->refresh_gate(); }, ps.get_subjects_lifetime());
    connected_observer_ = helix::ui::observe_int_sync<BeltTensionPanel>(
        ps.get_nav_buttons_enabled_subject(), this,
        [](BeltTensionPanel* self, int) { self->refresh_gate(); }, ps.get_subjects_lifetime());

    gate_observers_wired_ = true;
    spdlog::debug("[BeltTension] Gate observers attached");
}

void BeltTensionPanel::probe_klippy_socket() {
    if (!api_) {
        klippy_socket_reachable_ = false;
        refresh_gate();
        return;
    }

    api_->rest().get_server_config(
        lifetime_.bg_cb(
            "BeltTension::server_config",
            [this](const RestResponse& resp) {
                // Moonraker wraps its payload in "result"; older builds return
                // the object bare.
                klippy_socket_path_.clear();
                if (resp.data.is_object()) {
                    const json& root =
                        resp.data.contains("result") ? resp.data["result"] : resp.data;
                    if (root.is_object() && root.contains("config") && root["config"].is_object()) {
                        const json& cfg = root["config"];
                        if (cfg.contains("server") && cfg["server"].is_object()) {
                            const json& srv = cfg["server"];
                            if (srv.contains("klippy_uds_address") &&
                                srv["klippy_uds_address"].is_string()) {
                                klippy_socket_path_ = srv["klippy_uds_address"].get<std::string>();
                            }
                        }
                    }
                }
                klippy_socket_reachable_ =
                    !klippy_socket_path_.empty() &&
                    helix::calibration::BeltStreamClient::socket_reachable(klippy_socket_path_);
                spdlog::debug("[BeltTension] klippy uds '{}' reachable={}", klippy_socket_path_,
                              klippy_socket_reachable_);
                refresh_gate();
            }),
        lifetime_.bg_cb("BeltTension::server_config_err", [this](const MoonrakerError& err) {
            spdlog::debug("[BeltTension] /server/config failed: {}", err.message);
            klippy_socket_reachable_ = false;
            refresh_gate();
        }));
}

std::optional<float> BeltTensionPanel::span_offset_for_current_printer() const {
    const double mm =
        PrinterDetector::get_belt_span_offset_mm(get_printer_state().get_printer_type());
    // Negative means the model has no measured offset. Do not guess one: a 10 mm
    // error moves the 110 Hz target by about 7 Hz. Returning nullopt makes the
    // panel fall back to span-independent A-vs-B matching, which stays correct.
    return mm >= 0.0 ? std::optional<float>(static_cast<float>(mm)) : std::nullopt;
}

void BeltTensionPanel::handle_park_gantry() {
    const auto offset = span_offset_for_current_printer();
    lv_subject_set_int(&has_target_subject_, offset.has_value() ? 1 : 0);

    const auto bounds = get_printer_state().get_axis_bounds();
    const auto target =
        helix::calibration::park_y_for_span(helix::calibration::TARGET_SPAN_MM, offset, bounds);

    if (!target.valid) {
        // Matching is span-independent, so the feature still works - we just
        // cannot show an absolute target or park for the user.
        lv_subject_copy_string(&park_status_subject_,
                               lv_tr("Position the gantry yourself, then continue"));
        return;
    }

    if (!api_) {
        lv_subject_copy_string(&park_status_subject_,
                               lv_tr("Position the gantry yourself, then continue"));
        return;
    }

    lv_subject_copy_string(&park_status_subject_, lv_tr("Moving gantry"));

    // Only Y moves. On a CoreXY the free span runs front idler to rear along
    // each side rail, so gantry Y sets it; the toolhead's X position changes
    // neither span.
    helix::ensure_homed_then(
        api_, lifetime_,
        [this, y = static_cast<double>(target.y_mm)]() {
            api_->motion().move_to_position(
                'Y', y, PARK_FEEDRATE_MM_MIN,
                lifetime_.bg_cb("BeltTension::parked",
                                [this]() {
                                    lv_subject_copy_string(&park_status_subject_,
                                                           lv_tr("Ready to pluck"));
                                }),
                lifetime_.bg_cb("BeltTension::park_failed",
                                [this](const MoonrakerError& e) { on_error(e.message); }));
        },
        lifetime_.bg_cb("BeltTension::home_failed",
                        [this](const MoonrakerError& e) { on_error(e.message); }));
}

// ============================================================================
// SHOW / LIFECYCLE
// ============================================================================

void BeltTensionPanel::set_api(helix::MoonrakerClient* client, MoonrakerAPI* api) {
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
    lv_subject_set_int(&has_results_subject_, 0);
    lv_subject_copy_string(&current_belt_subject_, "A");

    // Re-evaluate the gate on every entry, and probe co-location once here
    // rather than on each gate refresh - the gate recomputes on every subject
    // change and a connect() syscall per change would be waste.
    ensure_gate_observers();
    refresh_gate();
    probe_klippy_socket();

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
                // detected_hw_ is now known-bad, so the gate must be recomputed
                // against it rather than left on a stale pass.
                detected_hw_ = {};
                refresh_gate();
            }));
    }
}

void BeltTensionPanel::on_deactivate() {
    spdlog::debug("[BeltTension] on_deactivate()");

    // Abandon an in-progress run. POSITION may have a park move outstanding and
    // LISTEN is the live meter; both must not survive the panel going away.
    auto state = static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state));
    if (state == ViewState::POSITION || state == ViewState::LISTEN) {
        spdlog::info("[BeltTension] Cancelling measurement on deactivate");
        if (calibrator_) {
            calibrator_->reset();
        }
        set_view_state(ViewState::START);
    }

    OverlayBase::on_deactivate();
}

void BeltTensionPanel::cleanup() {
    spdlog::debug("[BeltTension] Cleaning up");

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // ObserverGuard::reset(), never release() (#579)
    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    gate_observers_wired_ = false;

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
             hw.has_adxl ? lv_tr("Connected (auto-sweep)") : lv_tr("Not detected"));
    lv_subject_notify(&hw_adxl_subject_);

    spdlog::info("[BeltTension] Hardware: {} ADXL={}", kin_label, hw.has_adxl);

    // Kinematics feeds BeltGateInputs::is_corexy, so the gate is only truthful
    // once detection has landed.
    refresh_gate();
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void BeltTensionPanel::handle_start_clicked() {
    spdlog::info("[BeltTension] Start clicked");

    // The XML binding already disables this button while the gate is shut. This
    // is the same check on the action itself, so a stale binding or a
    // programmatic click cannot get past it.
    refresh_gate();
    if (lv_subject_get_int(&can_start_subject_) == 0) {
        spdlog::warn("[BeltTension] Start refused: {}",
                     lv_subject_get_string(&gate_message_subject_));
        return;
    }

    set_view_state(ViewState::POSITION);
    handle_park_gantry();
}

void BeltTensionPanel::handle_position_confirmed() {
    spdlog::info("[BeltTension] Position confirmed, listening");

    lv_subject_copy_string(&current_belt_subject_, "A");
    set_view_state(ViewState::LISTEN);
}

void BeltTensionPanel::handle_cancel_clicked() {
    spdlog::info("[BeltTension] Cancel clicked");

    if (calibrator_) {
        calibrator_->reset();
    }
    set_view_state(ViewState::START);
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
    set_view_state(ViewState::COMPARE);
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
