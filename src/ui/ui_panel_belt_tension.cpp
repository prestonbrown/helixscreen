// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_belt_tension.h"

#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "app_globals.h"
#include "belt_dsp_probe.h"
#include "belt_gating.h"
#include "belt_listen_session.h"
#include "belt_live_data.h"
#include "belt_stream_client.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "observer_factory.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "toolhead_homing.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<BeltTensionPanel> g_belt_tension_panel;

// State subject (0=START, 1=POSITION, 2=LISTEN, 3=COMPARE, 4=ERROR)
static lv_subject_t s_belt_tension_state;

namespace {
/// Voron's documented frequency for a correctly tensioned TARGET_SPAN_MM span,
/// and the band either side of it that still counts as correct. Same pair as
/// BeltTensionResult's defaults; named here because populate_comparison() works
/// from two medians rather than from a BeltTensionResult. Only used when the
/// model has a measured span offset - without one the span is unknown and an
/// absolute target means nothing.
constexpr float TARGET_FREQUENCY_HZ = 110.0f;
constexpr float TARGET_TOLERANCE_HZ = 10.0f;
} // namespace

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

    // Before anything else: a live stream is a background thread calling
    // on_batch_bg() on members that are about to be destroyed.
    stop_listening();

    // Backstop for a BeltTrace observer that outlives deinit_subjects() (e.g.
    // torn down after StaticPanelRegistry::destroy_all() but before this
    // object's own subjects field is destroyed) - same reasoning as
    // PrinterState::~PrinterState().
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }

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
        {"belt_tension_retry_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_retry_clicked(); }},
        {"belt_tension_listen_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_position_confirmed(); }},
        {"belt_tension_next_belt_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_advance_clicked(); }},
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
                       "Anything under 2 Hz is below what this measurement can "
                       "resolve, so it counts as matched.\n\n"
                       "Match: How close the two belts are, as a percentage of "
                       "belt A's frequency. Above 95% is excellent; below 90% "
                       "is worth adjusting."),
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
    UI_MANAGED_SUBJECT_STRING(current_belt_subject_, current_belt_buf_, lv_tr("Belt A"),
                              "bt_current_belt", subjects_);

    // Live meter subjects
    UI_MANAGED_SUBJECT_STRING(live_freq_subject_, live_freq_buf_, "--", "bt_live_freq", subjects_);
    UI_MANAGED_SUBJECT_STRING(median_freq_subject_, median_freq_buf_, "", "bt_median_freq",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(pluck_count_subject_, pluck_count_buf_, "0 / 5", "bt_pluck_count",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(hint_subject_, hint_buf_, lv_tr("Hold still"), "bt_hint", subjects_);
    UI_MANAGED_SUBJECT_INT(committed_subject_, 0, "bt_committed", subjects_);
    UI_MANAGED_SUBJECT_INT(match_percent_subject_, 0, "bt_match_percent", subjects_);
    UI_MANAGED_SUBJECT_INT(live_tick_subject_, 0, "bt_live_tick", subjects_);
    UI_MANAGED_SUBJECT_STRING(reference_freq_subject_, reference_freq_buf_, "--",
                              "bt_reference_freq", subjects_);
    UI_MANAGED_SUBJECT_INT(has_reference_subject_, 0, "bt_has_reference", subjects_);
    UI_MANAGED_SUBJECT_STRING(advance_label_subject_, advance_label_buf_, lv_tr("Next belt"),
                              "bt_advance_label", subjects_);

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
    // A live stream keeps deferring publish_live_values() at 10 Hz. Tearing the
    // subjects out from under it would leave those callbacks writing into
    // deinited subjects; the generation bump below drops them, but there is no
    // reason to keep producing them either.
    stop_listening();

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

    // Signal death of every subject below BEFORE it is torn down, so a
    // BeltTrace observer still holding a copy of the old token sees it is
    // gone and skips lv_observer_remove() on the observer node
    // subjects_.deinit_all() is about to free (#705). Install a fresh live
    // token rather than clearing the member - an empty token reads as "dead"
    // in ObserverGuard::reset() and would make every observer registered
    // after this point skip its removal too.
    if (subjects_lifetime_) {
        *subjects_lifetime_ = false;
    }
    subjects_lifetime_ = std::make_shared<bool>(true);

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

    // A precondition that fails mid-measurement ends the measurement. The case
    // that makes this real is a print starting while the user is plucking: the
    // toolhead moves, every later reading is garbage, and leaving the meter
    // running would present that garbage as a result.
    if (gate != helix::calibration::BeltGate::OK &&
        static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::LISTEN) {
        spdlog::warn("[BeltTension] Gate closed mid-measurement: {}",
                     helix::calibration::belt_gate_message(gate));
        stop_listening();
        on_error(lv_tr(helix::calibration::belt_gate_message(gate)));
    }
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

    // Fall back to the span implied by wherever the gantry is now. The search
    // window the session builds around it only has to bracket the real
    // frequency, so a stale Y is far better than pretending we parked.
    listen_span_mm_ = helix::calibration::TARGET_SPAN_MM;
    if (offset.has_value()) {
        // position_y is a whole-mm int subject. A millimetre of rounding moves
        // the search window by well under a Hz, so int is enough here.
        const float span =
            static_cast<float>(lv_subject_get_int(get_printer_state().get_position_y_subject())) +
            *offset;
        if (span > 0.0f) {
            listen_span_mm_ = span;
        }
    }

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
                                    // The park landed, so the span is the one we
                                    // asked for rather than one inferred from a
                                    // position that may be stale.
                                    listen_span_mm_ = helix::calibration::TARGET_SPAN_MM;
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
    lv_subject_set_int(&has_results_subject_, 0);
    reference_hz_ = 0.0f;
    belt_a_hz_ = 0.0f;
    belt_b_hz_ = 0.0f;
    listening_belt_ = 'A';
    reset_live_subjects();
    lv_subject_set_int(&has_reference_subject_, 0);
    lv_subject_copy_string(&reference_freq_subject_, "--");
    lv_subject_copy_string(&advance_label_subject_, lv_tr("Next belt"));

    // Re-evaluate the gate on every entry, and probe co-location once here
    // rather than on each gate refresh - the gate recomputes on every subject
    // change and a connect() syscall per change would be waste.
    ensure_gate_observers();
    refresh_gate();
    probe_klippy_socket();
    query_accel_chip();

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
    stop_listening();

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

    stop_listening();

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

    reference_hz_ = 0.0f;
    belt_a_hz_ = 0.0f;
    belt_b_hz_ = 0.0f;
    lv_subject_set_int(&has_reference_subject_, 0);
    start_listening('A');
}

void BeltTensionPanel::handle_cancel_clicked() {
    spdlog::info("[BeltTension] Cancel clicked");

    stop_listening();
    if (calibrator_) {
        calibrator_->reset();
    }
    set_view_state(ViewState::START);
}

void BeltTensionPanel::handle_retry_clicked() {
    spdlog::info("[BeltTension] Retry clicked");
    stop_listening();
    set_view_state(ViewState::START);
}

void BeltTensionPanel::handle_advance_clicked() {
    if (listening_belt_ == 'A') {
        handle_next_belt_clicked();
    } else {
        handle_compare_clicked();
    }
}

void BeltTensionPanel::handle_next_belt_clicked() {
    float median = 0.0f;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        if (session_) {
            median = session_->median_hz();
        }
    }
    if (median <= 0.0f) {
        spdlog::warn("[BeltTension] Next belt refused: belt A has no median yet");
        return;
    }

    spdlog::info("[BeltTension] Belt A committed at {:.2f} Hz, moving to belt B", median);
    belt_a_hz_ = median;
    reference_hz_ = median;

    stop_listening();

    snprintf(reference_freq_buf_, sizeof(reference_freq_buf_), "%.0f Hz",
             static_cast<double>(median));
    lv_subject_notify(&reference_freq_subject_);
    lv_subject_set_int(&has_reference_subject_, 1);

    start_listening('B');
}

void BeltTensionPanel::handle_compare_clicked() {
    float median = 0.0f;
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        if (session_) {
            median = session_->median_hz();
        }
    }
    if (median <= 0.0f) {
        spdlog::warn("[BeltTension] Compare refused: belt B has no median yet");
        return;
    }

    spdlog::info("[BeltTension] Belt B committed at {:.2f} Hz", median);
    belt_b_hz_ = median;

    stop_listening();
    populate_comparison(belt_a_hz_, belt_b_hz_);
    set_view_state(ViewState::COMPARE);
}

// ============================================================================
// RESULT CALLBACKS
// ============================================================================

void BeltTensionPanel::on_error(const std::string& message) {
    spdlog::error("[BeltTension] Error: {}", message);

    snprintf(error_message_buf_, sizeof(error_message_buf_), "%s", message.c_str());
    lv_subject_notify(&error_message_subject_);
    set_view_state(ViewState::ERROR);
}

void BeltTensionPanel::populate_comparison(float a_hz, float b_hz) {
    const bool have_target = lv_subject_get_int(&has_target_subject_) != 0;
    const float delta = std::fabs(a_hz - b_hz);
    const bool matched = helix::calibration::belt_frequencies_match(a_hz, b_hz);

    // Whole Hz, never a decimal: see BELT_RESOLUTION_HZ.
    snprintf(result_a_freq_buf_, sizeof(result_a_freq_buf_), "%.0f Hz", static_cast<double>(a_hz));
    lv_subject_notify(&result_a_freq_subject_);
    snprintf(result_b_freq_buf_, sizeof(result_b_freq_buf_), "%.0f Hz", static_cast<double>(b_hz));
    lv_subject_notify(&result_b_freq_subject_);

    // An absolute GOOD/WARNING/BAD verdict only means something when the span
    // is known, because the target frequency is a property of the span. With
    // no measured span offset for this model the panel does matching only, and
    // an absolute verdict would be an invention.
    const char* a_status = "";
    const char* b_status = "";
    if (have_target) {
        a_status =
            helix::calibration::belt_status_to_string(helix::calibration::evaluate_belt_status(
                a_hz, TARGET_FREQUENCY_HZ, TARGET_TOLERANCE_HZ));
        b_status =
            helix::calibration::belt_status_to_string(helix::calibration::evaluate_belt_status(
                b_hz, TARGET_FREQUENCY_HZ, TARGET_TOLERANCE_HZ));
    }
    snprintf(result_a_status_buf_, sizeof(result_a_status_buf_), "%s", a_status);
    lv_subject_notify(&result_a_status_subject_);
    snprintf(result_b_status_buf_, sizeof(result_b_status_buf_), "%s", b_status);
    lv_subject_notify(&result_b_status_subject_);

    if (matched) {
        snprintf(result_delta_buf_, sizeof(result_delta_buf_), "%s",
                 lv_tr("Within measurement resolution"));
    } else {
        snprintf(result_delta_buf_, sizeof(result_delta_buf_), lv_tr("%.0f Hz difference"),
                 static_cast<double>(delta));
    }
    lv_subject_notify(&result_delta_subject_);

    const float match = helix::calibration::belt_match_percent(a_hz, b_hz);
    snprintf(result_similarity_buf_, sizeof(result_similarity_buf_), "%.0f%%",
             static_cast<double>(match));
    lv_subject_notify(&result_similarity_subject_);
    lv_subject_set_int(&match_percent_subject_, static_cast<int>(std::lround(match)));

    // Matching alone is not advice. Two belts can match each other perfectly and
    // both be far off the target, and "loosen the tighter one" is actively wrong
    // when both are already below it - it moves the machine further from where it
    // should be. So whenever the span is known, the target drives the wording and
    // matching is the secondary concern. Only a printer with no measured span
    // offset falls back to pure matching, because there the target is unknown
    // rather than merely unmet.
    const float need_a = TARGET_FREQUENCY_HZ - a_hz; // positive means "tighten"
    const float need_b = TARGET_FREQUENCY_HZ - b_hz;
    const bool a_in_band = std::fabs(need_a) <= TARGET_TOLERANCE_HZ;
    const bool b_in_band = std::fabs(need_b) <= TARGET_TOLERANCE_HZ;
    const char* looser = need_a > need_b ? lv_tr("A") : lv_tr("B");

    if (!have_target) {
        if (matched) {
            snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_), "%s",
                     lv_tr("Both belts read the same to within what this measurement can "
                           "resolve. Nothing to adjust."));
        } else if (a_hz > b_hz) {
            snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                     lv_tr("Belt A (front right) is tighter by %.0f Hz. Tighten belt B, on the "
                           "front left, or loosen belt A."),
                     static_cast<double>(delta));
        } else {
            snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                     lv_tr("Belt B (front left) is tighter by %.0f Hz. Tighten belt A, on the "
                           "front right, or loosen belt B."),
                     static_cast<double>(delta));
        }
    } else if (a_in_band && b_in_band && matched) {
        snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                 lv_tr("Both belts are on the %.0f Hz target and match each other. "
                       "Nothing to adjust."),
                 static_cast<double>(TARGET_FREQUENCY_HZ));
    } else if (need_a > 0.0f && need_b > 0.0f) {
        snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                 lv_tr("Both belts are below the %.0f Hz target - A by %.0f Hz, B by %.0f Hz. "
                       "Tighten both, %s more."),
                 static_cast<double>(TARGET_FREQUENCY_HZ), static_cast<double>(need_a),
                 static_cast<double>(need_b), looser);
    } else if (need_a < 0.0f && need_b < 0.0f) {
        snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                 lv_tr("Both belts are above the %.0f Hz target - A by %.0f Hz, B by %.0f Hz. "
                       "Loosen both."),
                 static_cast<double>(TARGET_FREQUENCY_HZ), static_cast<double>(-need_a),
                 static_cast<double>(-need_b));
    } else {
        // One side of the target each, so they cannot be brought together by
        // moving only one belt.
        snprintf(result_recommendation_buf_, sizeof(result_recommendation_buf_),
                 lv_tr("Target is %.0f Hz. Tighten belt %s and loosen belt %s."),
                 static_cast<double>(TARGET_FREQUENCY_HZ), need_a > 0.0f ? "A" : "B",
                 need_a > 0.0f ? "B" : "A");
    }
    lv_subject_notify(&result_recommendation_subject_);

    lv_subject_set_int(&has_results_subject_, 1);

    spdlog::info("[BeltTension] Compare: A={:.2f} Hz B={:.2f} Hz delta={:.2f} Hz match={:.0f}%",
                 a_hz, b_hz, delta, match);
}

// ============================================================================
// LIVE MEASUREMENT
// ============================================================================

void BeltTensionPanel::query_accel_chip() {
    // Fall back before asking, so the panel is never left with no sensor name
    // if the query fails or the config has no resonance_tester section.
    const auto sensors = helix::sensors::AccelSensorManager::instance().get_sensors();
    if (!sensors.empty()) {
        sensor_name_ = sensors.front().klipper_name;
    }

    if (!api_) {
        return;
    }

    api_->query_configfile(
        lifetime_.bg_cb("BeltTension::accel_chip",
                        [this](const json& config) {
                            if (!config.is_object() || !config.contains("resonance_tester") ||
                                !config["resonance_tester"].is_object()) {
                                spdlog::debug(
                                    "[BeltTension] No resonance_tester section; sensor stays '{}'",
                                    sensor_name_);
                                return;
                            }
                            const json& rt = config["resonance_tester"];
                            // accel_chip is the single-sensor form; accel_chip_x/_y is the
                            // per-axis form. Either names a config section, and both belts
                            // are measured from the same toolhead sensor, so the X one is
                            // as good as the Y one.
                            for (const char* key : {"accel_chip", "accel_chip_x"}) {
                                if (rt.contains(key) && rt[key].is_string()) {
                                    std::string chip = rt[key].get<std::string>();
                                    if (!chip.empty()) {
                                        sensor_name_ = std::move(chip);
                                        break;
                                    }
                                }
                            }
                            spdlog::info("[BeltTension] Accelerometer section '{}'", sensor_name_);
                        }),
        lifetime_.bg_cb("BeltTension::accel_chip_err", [this](const MoonrakerError& err) {
            spdlog::debug("[BeltTension] configfile query failed ({}); sensor stays '{}'",
                          err.message, sensor_name_);
        }));
}

void BeltTensionPanel::reset_live_subjects() {
    lv_subject_copy_string(&current_belt_subject_,
                           listening_belt_ == 'B' ? lv_tr("Belt B") : lv_tr("Belt A"));
    lv_subject_copy_string(&live_freq_subject_, "--");
    lv_subject_copy_string(&median_freq_subject_, "");
    snprintf(pluck_count_buf_, sizeof(pluck_count_buf_), "0 / %zu",
             helix::calibration::PluckAggregator::COMMIT_AFTER);
    lv_subject_notify(&pluck_count_subject_);
    lv_subject_copy_string(&hint_subject_, lv_tr("Hold still"));
    lv_subject_set_int(&committed_subject_, 0);
    lv_subject_set_int(&match_percent_subject_, 0);
    helix::calibration::BeltLiveData::instance().clear();
}

void BeltTensionPanel::start_listening(char belt) {
    stop_listening();

    listening_belt_ = belt;
    reset_live_subjects();
    lv_subject_copy_string(&advance_label_subject_,
                           belt == 'B' ? lv_tr("Compare") : lv_tr("Next belt"));
    set_view_state(ViewState::LISTEN);

    if (klippy_socket_path_.empty() || sensor_name_.empty()) {
        on_error(lv_tr("No accelerometer stream available. Check that Klipper is running "
                       "and an accelerometer is configured, then retry."));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        session_.reset();
        noise_prefix_.clear();
        had_reject_ = false;
        last_event_tp_ = std::chrono::steady_clock::now();
    }

    if (!stream_) {
        stream_ = std::make_unique<helix::calibration::BeltStreamClient>();
    }

    spdlog::info("[BeltTension] Listening on belt {} via '{}' at {} (span {:.0f} mm)", belt,
                 sensor_name_, klippy_socket_path_, listen_span_mm_);

    // on_batch is deliberately NOT wrapped in lifetime_.bg_cb: bg_cb defers the
    // whole body to the main thread, which would put a 2048-point pitch
    // estimate on the LVGL thread ten times a second. The DSP stays here, and
    // lifetime safety comes from stop_listening() joining the loop thread on
    // every exit path plus stream_ being the first member destroyed.
    const bool ok = stream_->start(
        klippy_socket_path_, sensor_name_,
        [this, tok = lifetime_.token()](const helix::calibration::AccelBatch& batch) {
            on_batch_bg(batch, tok);
        },
        lifetime_.bg_cb("BeltTension::stream_error",
                        [this](const std::string& msg) { on_stream_error(msg); }));

    if (!ok) {
        on_error(lv_tr("Could not open Klipper's accelerometer stream. Check that Klipper "
                       "is running, then retry."));
    }
}

void BeltTensionPanel::stop_listening() {
    // Close the socket and join the loop thread FIRST. After this returns no
    // batch callback can be in flight, so clearing the session below cannot
    // pull the buffer out from under a running DSP pass.
    if (stream_) {
        stream_->stop();
    }

    std::lock_guard<std::mutex> lock(listen_mutex_);
    session_.reset();
    noise_prefix_.clear();
    noise_prefix_.shrink_to_fit();
    had_reject_ = false;
}

void BeltTensionPanel::on_batch_bg(const helix::calibration::AccelBatch& batch,
                                   const helix::LifetimeToken& tok) {
    LiveSnapshot snap;

    {
        std::lock_guard<std::mutex> lock(listen_mutex_);

        if (!session_) {
            // Noise-floor phase. Nothing is published, so the "Hold still"
            // hint set by start_listening() stays up for its duration.
            noise_prefix_.insert(noise_prefix_.end(), batch.samples.begin(), batch.samples.end());
            if (noise_prefix_.size() < NOISE_FLOOR_SAMPLES) {
                return;
            }

            // The session needs the measured rate, not the configured one, and
            // the stream only knows it once samples have arrived - which is
            // exactly now. Build the session here rather than in
            // start_listening() for that reason.
            float rate = stream_ ? stream_->sample_rate_hz() : 0.0f;
            if (rate <= 0.0f) {
                rate = 3200.0f;
            }
            session_ =
                std::make_unique<helix::calibration::BeltListenSession>(listen_span_mm_, rate);
            const bool learned = session_->learn_noise_floor(noise_prefix_);
            spdlog::info("[BeltTension] Noise floor learned={} from {} samples at {:.0f} Hz",
                         learned, noise_prefix_.size(), rate);
            noise_prefix_.clear();
            noise_prefix_.shrink_to_fit();
            last_event_tp_ = std::chrono::steady_clock::now();
            return;
        }

        // The DSP runs here, on the loop thread. Only finished numbers cross.
        const auto event = session_->push(batch);
        const auto now = std::chrono::steady_clock::now();

        if (event) {
            last_event_tp_ = now;
            if (event->accepted) {
                snap.last_hz = event->frequency_hz;
                snap.spectrum = session_->last_spectrum();
                // A good pluck answers the "too soft" prompt, so retire it now
                // rather than letting it sit out its REJECT_HINT_MS window. It
                // would otherwise still be telling the user to pluck harder
                // while the accepted count ticks up in front of them.
                had_reject_ = false;
            } else {
                had_reject_ = true;
                last_reject_tp_ = now;
            }
        }

        snap.accepted = session_->accepted_count();
        snap.median_hz = session_->median_hz();
        snap.committed = session_->committed();
        snap.window = session_->window();
        snap.ms_since_event = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_event_tp_).count());
        if (had_reject_) {
            snap.ms_since_reject = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reject_tp_)
                    .count());
        }
    }

    // Not lifetime_.defer(): that one is main-thread only. LifetimeToken::defer
    // holds its own shared_ptr to the generation counter, so it is safe to call
    // from the loop thread and it never reads `this` to decide whether to run.
    tok.defer("BeltTension::batch_ui",
              [this, snap = std::move(snap)]() { publish_live_values(snap); });
}

void BeltTensionPanel::publish_live_values(const LiveSnapshot& snap) {
    if (snap.last_hz > 0.0f) {
        snprintf(live_freq_buf_, sizeof(live_freq_buf_), "%.0f Hz",
                 static_cast<double>(snap.last_hz));
        lv_subject_notify(&live_freq_subject_);
    } else if (snap.median_hz > 0.0f) {
        snprintf(live_freq_buf_, sizeof(live_freq_buf_), "%.0f Hz",
                 static_cast<double>(snap.median_hz));
        lv_subject_notify(&live_freq_subject_);
    }

    if (snap.median_hz > 0.0f) {
        snprintf(median_freq_buf_, sizeof(median_freq_buf_), lv_tr("Median %.0f Hz"),
                 static_cast<double>(snap.median_hz));
    } else {
        median_freq_buf_[0] = '\0';
    }
    lv_subject_notify(&median_freq_subject_);

    snprintf(pluck_count_buf_, sizeof(pluck_count_buf_), "%zu / %zu", snap.accepted,
             helix::calibration::PluckAggregator::COMMIT_AFTER);
    lv_subject_notify(&pluck_count_subject_);

    lv_subject_set_int(&committed_subject_, snap.committed ? 1 : 0);

    if (reference_hz_ > 0.0f && snap.median_hz > 0.0f) {
        const float match = helix::calibration::belt_match_percent(reference_hz_, snap.median_hz);
        lv_subject_set_int(&match_percent_subject_, static_cast<int>(std::lround(match)));
    }

    // Hint priority: a recent rejection is the most actionable thing we can
    // say, then a long silence, then the neutral "we are listening".
    const char* hint = nullptr;
    if (snap.ms_since_reject < helix::calibration::REJECT_HINT_MS) {
        hint = lv_tr("Too soft - pluck harder");
    } else if (helix::calibration::belt_should_show_idle_hint(snap.ms_since_event)) {
        // Front-left is belt B, front-right is belt A (design spec, confirmed
        // against photographs of the machine).
        hint = listening_belt_ == 'B' ? lv_tr("Pluck the front belt on the left")
                                      : lv_tr("Pluck the front belt on the right");
    } else {
        hint = lv_tr("Listening");
    }
    lv_subject_copy_string(&hint_subject_, hint);

    helix::calibration::BeltLiveData::instance().set_waveform(snap.window);
    // Only a freshly accepted pluck carries a new spectrum (see
    // BeltListenSession::last_spectrum()) - an empty snap.spectrum here means
    // "nothing new," and the strip is left holding whatever it last drew.
    if (!snap.spectrum.empty()) {
        helix::calibration::BeltLiveData::instance().set_spectrum(snap.spectrum);
    }

    // Drives BeltTrace's redraw. Bumped every publish, not only when the
    // spectrum changes, because the waveform trace has fresh data every
    // batch even between plucks.
    lv_subject_set_int(&live_tick_subject_, lv_subject_get_int(&live_tick_subject_) + 1);
}

void BeltTensionPanel::on_stream_error(const std::string& message) {
    spdlog::error("[BeltTension] Stream failed: {}", message);

    // A dead stream must not leave the last good frequency on screen: the
    // number would keep reading as live while nothing is being measured.
    stop_listening();

    char buf[256];
    snprintf(buf, sizeof(buf),
             lv_tr("The accelerometer stream stopped: %s. Check that "
                   "Klipper is running, then retry."),
             message.c_str());
    on_error(buf);
}
