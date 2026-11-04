// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_belt_tension.h"

#include "ui_belt_gap_gauge.h"
#include "ui_belt_path_names.h"
#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_timer_guard.h"

#include "app_globals.h"
#include "belt_stream_client.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "memory_utils.h"
#include "observe_language.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "resonance_console.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace helix;

namespace {

using helix::calibration::BeltPath;

int path_index(BeltPath path) {
    return path == BeltPath::PATH_A ? 0 : 1;
}

BeltPath path_at(int index) {
    return index == 0 ? BeltPath::PATH_A : BeltPath::PATH_B;
}

/// The path's letter, for logs. Screens name a path through helix::ui::belt_path_name().
char path_letter(BeltPath path) {
    return path == BeltPath::PATH_A ? 'A' : 'B';
}

/// "Path A" / "Left belt": labels and the start of a sentence.
std::string path_title(BeltPath path) {
    return helix::ui::belt_path_name(path).title;
}

/// "Path A" / "the left belt": inside a sentence.
std::string path_phrase(BeltPath path) {
    return helix::ui::belt_path_name(path).phrase;
}

BeltPath other_path(BeltPath path) {
    return path == BeltPath::PATH_A ? BeltPath::PATH_B : BeltPath::PATH_A;
}

/// A curve's tallest bin; 0 when empty.
float tallest_bin(const helix::calibration::BeltCurve& curve) {
    float tallest = 0.0f;
    for (const auto& bin : curve) {
        tallest = std::max(tallest, bin.second);
    }
    return tallest;
}

/// Why the settling laps cannot run, for the ERROR view.
const char* settle_refusal_message(helix::calibration::BeltSettleRefusal refusal) {
    using helix::calibration::BeltSettleRefusal;
    switch (refusal) {
    case BeltSettleRefusal::NO_AXIS_LIMITS:
        return lv_tr("The printer has not reported its axis limits yet.");
    case BeltSettleRefusal::NO_PROBE_POINT:
        return lv_tr("[resonance_tester] has no probe_points, so the toolhead has no safe height "
                     "for the settling laps.");
    case BeltSettleRefusal::NO_VELOCITY:
        return lv_tr("The printer has not reported its maximum velocity yet.");
    case BeltSettleRefusal::SPAN_TOO_SMALL:
    case BeltSettleRefusal::NONE:
        break;
    }
    return lv_tr("The bed is too small for settling laps.");
}

/// "0 turns", "¼ turn", "1¼ turns".
std::string format_turn_amount(float turns) {
    const std::string amount = helix::calibration::format_belt_turns(turns);
    const bool singular = std::abs(turns) <= 1.0f && amount != "0";
    return fmt::format(fmt::runtime(singular ? lv_tr("{} turn") : lv_tr("{} turns")), amount);
}

} // namespace

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

// State subject (0=START, 1=RUNNING, 2=RESULTS, 3=ERROR, 4=TUNING)
static lv_subject_t s_belt_tension_state;

BeltTensionPanel::~BeltTensionPanel() {
    // lifetime_'s destructor auto-invalidates all outstanding tokens.

    stall_guard_.end();
    // The stream's thread may not outlive the panel; no LVGL this late.
    preview_.stop();

    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    klippy_observer_.reset();
    max_velocity_observer_.reset();
    belt_layout_observer_.reset();
    language_observer_.reset();
    gate_observers_wired_ = false;

    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    chart_host_running_ = nullptr;
    chart_host_results_ = nullptr;
    chart_host_tuning_ = nullptr;
    chart_host_wide_ = nullptr;
}

namespace helix::ui {

void open_belt_tension_panel(lv_obj_t* parent_screen) {
    auto& panel = get_global_belt_tension_panel();
    panel.set_api(get_moonraker_client(), get_moonraker_api());
    panel.show(parent_screen);
}

} // namespace helix::ui

void init_belt_tension_row_handler() {
    register_xml_callbacks({
        {"on_belt_tension_row_clicked",
         [](lv_event_t*) {
             spdlog::debug("[BeltTension] Belt Tension row clicked");
             helix::ui::open_belt_tension_panel(lv_display_get_screen_active(nullptr));
         }},
    });
    spdlog::trace("[BeltTension] Row click callback registered");
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

void ui_panel_belt_tension_register_callbacks() {
    register_xml_callbacks({
        {"belt_tension_start_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_start_clicked(); }},
        {"belt_tension_stop_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_stop_clicked(); }},
        {"belt_tension_retry_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_retry_clicked(); }},
        {"belt_tension_retest_slot_cb",
         [](lv_event_t* e) {
             const char* slot = static_cast<const char*>(lv_event_get_user_data(e));
             get_global_belt_tension_panel().handle_retest_slot_clicked(slot && slot[0] == '1' ? 1
                                                                                               : 0);
         }},
        {"belt_tension_retest_menu_cb",
         [](lv_event_t* e) {
             get_global_belt_tension_panel().handle_retest_menu_clicked(
                 static_cast<lv_obj_t*>(lv_event_get_current_target(e)));
         }},
        {"belt_tension_names_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_names_clicked(); }},
        {"belt_tension_identify_a_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_identify_clicked(calibration::BeltPath::PATH_A);
         }},
        {"belt_tension_identify_b_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_identify_clicked(calibration::BeltPath::PATH_B);
         }},
        {"belt_tension_tune_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_clicked(); }},
        {"belt_tension_tune_measure_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_measure_clicked(); }},
        {"belt_tension_tune_done_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_done_clicked(); }},
        {"belt_tension_tune_swap_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_swap_clicked(); }},
        {"belt_tension_tune_amount_dec_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_amount_step(-1); }},
        {"belt_tension_tune_amount_inc_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_amount_step(+1); }},
        {"belt_tension_tune_next_zone_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_tune_next_zone_clicked();
         }},
        {"belt_tension_tune_confirm_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_confirm_clicked(); }},
        {"belt_tension_tune_keep_cb",
         [](lv_event_t* /*e*/) { get_global_belt_tension_panel().handle_tune_keep_clicked(); }},
        {"belt_tension_tune_calibrate_anyway_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_tune_calibrate_anyway_clicked();
         }},
        {"belt_tension_tune_why_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_alert(
                 lv_tr("Why the curves differ"),
                 lv_tr("Matched belts give curves of the same shape. When the tallest peak on "
                       "one path has no partner on the other, something besides tension is "
                       "shaping the response:\n\n"
                       "- a worn, frayed or mis-tracking belt\n"
                       "- a loose or slipping pulley grub screw\n"
                       "- an idler or bearing that binds\n"
                       "- debris in the teeth, or a belt rubbing a flange\n"
                       "- a loose accelerometer mount\n\n"
                       "Tuning still follows the zone that moved with the turn, but check "
                       "these before chasing a perfect match."),
                 ModalSeverity::Info, lv_tr("Got it"));
         }},
        {"belt_tension_help_cb",
         [](lv_event_t* /*e*/) {
             helix::ui::modal_alert(
                 lv_tr("Belt Tension Check"),
                 lv_tr("This check runs Klipper's resonance test along each belt "
                       "path and compares the two responses. Balanced belts give "
                       "curves of the same shape, with peaks at the same "
                       "frequencies.\n\n"
                       "Each sweep takes a few minutes and moves the toolhead; keep "
                       "clear of the printer while it runs."),
                 ModalSeverity::Info, lv_tr("Got it"));
         }},
    });
    helix::ui::BeltRetestMenu::register_callbacks();
    helix::ui::BeltNamesModal::register_callbacks();

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

    // Gate subjects - START's action is bound to these, not to a hidden menu row
    UI_MANAGED_SUBJECT_INT(can_start_subject_, 0, "bt_can_start", subjects_);
    UI_MANAGED_SUBJECT_STRING(gate_message_subject_, gate_message_buf_, "", "bt_gate_message",
                              subjects_);

    // Hardware summary
    UI_MANAGED_SUBJECT_STRING(hw_kinematics_subject_, hw_kinematics_buf_, lv_tr("Detecting..."),
                              "bt_hw_kinematics", subjects_);
    UI_MANAGED_SUBJECT_STRING(hw_accel_subject_, hw_accel_buf_, lv_tr("Detecting..."),
                              "bt_hw_accel", subjects_);
    UI_MANAGED_SUBJECT_STRING(hw_sweep_subject_, hw_sweep_buf_, "-", "bt_hw_sweep", subjects_);

    // Running state
    UI_MANAGED_SUBJECT_STRING(run_title_subject_, run_title_buf_, "", "bt_run_title", subjects_);
    UI_MANAGED_SUBJECT_STRING(run_detail_subject_, run_detail_buf_, "", "bt_run_detail", subjects_);
    UI_MANAGED_SUBJECT_INT(running_path_subject_, 0, "bt_running_path", subjects_);

    // Path names
    UI_MANAGED_SUBJECT_INT(identify_state_subject_, 0, "bt_identify_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(identify_status_subject_, identify_status_buf_, "",
                              "bt_identify_status", subjects_);

    // Results
    UI_MANAGED_SUBJECT_STRING(slot_note_subject_[0], slot_note_buf_[0], "", "bt_slot_0_note",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(slot_note_subject_[1], slot_note_buf_[1], "", "bt_slot_1_note",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(slot_name_subject_[0], slot_name_buf_[0], "", "bt_slot_0_name",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(slot_name_subject_[1], slot_name_buf_[1], "", "bt_slot_1_name",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(slots_swapped_subject_, 0, "bt_slots_swapped", subjects_);
    UI_MANAGED_SUBJECT_INT(verdict_subject_, 0, "bt_verdict", subjects_);
    UI_MANAGED_SUBJECT_STRING(verdict_text_subject_, verdict_text_buf_, "", "bt_verdict_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(similarity_subject_, similarity_buf_, "--", "bt_similarity",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(similarity_was_subject_, similarity_was_buf_, "", "bt_similarity_was",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(has_similarity_was_subject_, 0, "bt_has_similarity_was", subjects_);
    UI_MANAGED_SUBJECT_STRING(facts_subject_, facts_buf_, "", "bt_facts", subjects_);
    UI_MANAGED_SUBJECT_STRING(unpaired_subject_, unpaired_buf_, "", "bt_unpaired", subjects_);
    UI_MANAGED_SUBJECT_INT(has_unpaired_subject_, 0, "bt_has_unpaired", subjects_);
    UI_MANAGED_SUBJECT_INT(chart_available_subject_, 0, "bt_chart_available", subjects_);

    // Error
    UI_MANAGED_SUBJECT_STRING(error_message_subject_, error_message_buf_, "", "bt_error_message",
                              subjects_);

    // Tuning loop
    UI_MANAGED_SUBJECT_STRING(tune_cta_subject_, tune_cta_buf_, "", "bt_tune_cta", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_phase_subject_, 0, "bt_tune_phase", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_kind_subject_, 0, "bt_tune_kind", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_dir_subject_, 0, "bt_tune_dir", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_path_subject_, 0, "bt_tune_path", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_severity_subject_, 0, "bt_tune_severity", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_baseline_subject_, 0, "bt_tune_baseline", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_has_zone_subject_, 0, "bt_tune_has_zone", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_has_next_zone_subject_, 0, "bt_tune_has_next_zone", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_shape_warning_subject_, 0, "bt_tune_shape_warning", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_instruction_subject_, tune_instruction_buf_, "",
                              "bt_tune_instruction", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_amount_subject_, tune_amount_buf_, "", "bt_tune_amount",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_gap_subject_, tune_gap_buf_, "", "bt_tune_gap", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_gap_caption_subject_, tune_gap_caption_buf_, "",
                              "bt_tune_gap_caption", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_round_subject_, tune_round_buf_, "", "bt_tune_round", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_zone_subject_, tune_zone_buf_, "", "bt_tune_zone", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_rate_subject_, tune_rate_buf_, "", "bt_tune_rate", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_last_subject_, tune_last_buf_, "", "bt_tune_last", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_has_last_subject_, 0, "bt_tune_has_last", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_hint_subject_, tune_hint_buf_, "", "bt_tune_hint", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_power_hint_subject_, tune_power_hint_buf_, "",
                              "bt_tune_power_hint", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_has_power_hint_subject_, 0, "bt_tune_has_power_hint", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_has_gap_subject_, 0, "bt_tune_has_gap", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_unrepeatable_subject_, 0, "bt_tune_unrepeatable", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_can_swap_subject_, 0, "bt_tune_can_swap", subjects_);
    UI_MANAGED_SUBJECT_INT(tune_range_known_subject_, 0, "bt_tune_range_known", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_measure_short_label_subject_, tune_measure_short_label_buf_, "",
                              "bt_tune_measure_short_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_tensioner_subject_, tune_tensioner_buf_, "", "bt_tune_tensioner",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_measure_label_subject_, tune_measure_label_buf_, "",
                              "bt_tune_measure_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_confirm_label_subject_, tune_confirm_label_buf_, "",
                              "bt_tune_confirm_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(tune_next_zone_label_subject_, tune_next_zone_label_buf_, "",
                              "bt_tune_next_zone_label", subjects_);

    subjects_initialized_ = true;
    refresh_names();

    // Subject deinit runs after StaticPanelRegistry::destroy_all() has freed the
    // panel (its destructor deinits these subjects), so the hook peeks rather
    // than building a replacement panel during shutdown.
    StaticSubjectRegistry::instance().register_deinit("BeltTensionPanel", []() {
        if (auto* panel = helix::lazy_global_if_exists<BeltTensionPanel>()) {
            panel->deinit_subjects();
        }
    });
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
    klippy_observer_.reset();
    max_velocity_observer_.reset();
    belt_layout_observer_.reset();
    language_observer_.reset();
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
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // The chart is created imperatively inside these hosts; cache them once
    // per view build, the same contract create() gives overlay_root_.
    chart_host_running_ = helix::ui::find_required(overlay_root_, "chart_host_running", get_name());
    chart_host_results_ = helix::ui::find_required(overlay_root_, "chart_host_results", get_name());
    chart_host_tuning_ = helix::ui::find_required(overlay_root_, "chart_host_tuning", get_name());
    chart_host_wide_ = helix::ui::find_required(overlay_root_, "chart_host_wide", get_name());

    ensure_gate_observers();
    refresh_gate();

    // Set initial state
    set_view_state(ViewState::START);
    return overlay_root_;
}

// ============================================================================
// SHOW / LIFECYCLE
// ============================================================================

void BeltTensionPanel::set_api(helix::IMoonrakerClient* client, IMoonrakerAPI* api) {
    client_ = client;
    api_ = api;

    calibrator_ = std::make_unique<helix::calibration::BeltTensionCalibrator>(api_);
    spdlog::debug("[BeltTension] Calibrator created");
}

void BeltTensionPanel::set_render_tier_for_test(helix::PlatformTier tier,
                                                bool supports_animations) {
    tier_override_ = tier;
    tier_animations_ = supports_animations;
}

void BeltTensionPanel::set_memory_for_test(const helix::MemoryInfo& mem) {
    mem_override_ = mem;
}

void BeltTensionPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[BeltTension] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[BeltTension] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    helix::nav::register_overlay(overlay_root_, this);

    // Push onto navigation stack
    helix::nav::push_overlay(overlay_root_);

    spdlog::info("[BeltTension] Overlay shown");
}

void BeltTensionPanel::on_activate() {
    OverlayBase::on_activate();

    spdlog::debug("[BeltTension] on_activate()");

    if (suspended_) {
        // A wake from the screensaver: the user was at the tensioner, or a
        // sweep was running. Keep the view, the run and the tuning session.
        // The suspension expired every pending async answer, so re-ask what
        // can be re-asked; never mid-sweep, where re-detecting hardware would
        // overwrite the calibrator's MEASURING state.
        suspended_ = false;
        ensure_gate_observers();
        if (settle_query_pending_) {
            start_settle();
        }
        if (!measuring()) {
            probe_klippy_socket();
            query_hw_facts();
            if (detection_pending_) {
                start_hardware_detection();
            }
        }
        refresh_gate();
        refresh_notes();
        push_chart_data();
        refresh_tune();
        return;
    }

    set_view_state(ViewState::START);
    refresh_notes();
    push_chart_data();

    // Re-evaluate the gate on every entry, and probe co-location once here
    // rather than on each gate refresh - the gate recomputes on every subject
    // change and a connect() syscall per change would be waste.
    ensure_gate_observers();
    refresh_gate();
    // theme_changed is a file-static theme global, deinited only after LVGL is gone.
    theme_observer_ = helix::ui::observe<int>(
        theme_manager_get_changed_subject(), this,
        [](BeltTensionPanel* self, int) { self->apply_path_colors(); }, subject_never_freed());
    // A rotation can cross the ultrawide line under a live view. ui_is_ultrawide
    // is a theme global, deinited only after LVGL is gone.
    if (lv_subject_t* ultrawide = lv_xml_get_subject(nullptr, "ui_is_ultrawide")) {
        ultrawide_observer_ = helix::ui::observe<int>(
            ultrawide, this,
            [](BeltTensionPanel* self, int) {
                self->place_chart(
                    static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)));
            },
            subject_never_freed());
    }
    // The names follow the active printer's saved layout, and the language.
    auto& settings = SettingsManager::instance();
    if (settings.are_subjects_initialized()) {
        belt_layout_observer_ = helix::ui::observe<int>(
            settings.subject_belt_layout(), this,
            [](BeltTensionPanel* self, int) { self->refresh_names(); },
            settings.get_subjects_lifetime());
    }
    language_observer_ = helix::ui::observe_language_change(
        this, [](BeltTensionPanel* self) { self->refresh_names(); });
    refresh_names();
    probe_klippy_socket();
    query_hw_facts();
    start_hardware_detection();
}

void BeltTensionPanel::start_hardware_detection() {
    if (calibrator_) {
        detection_pending_ = true;
        calibrator_->detect_hardware(
            lifetime_.bg_cb("BeltTensionPanel::detect_hardware",
                            [this](const helix::calibration::BeltTensionHardware& hw) {
                                on_hardware_detected(hw);
                            }),
            lifetime_.bg_cb("BeltTensionPanel::detect_hw_error", [this](const std::string& msg) {
                spdlog::warn("[BeltTension] Hardware detection failed: {}", msg);
                snprintf(hw_kinematics_buf_, sizeof(hw_kinematics_buf_), "%s", lv_tr("Unknown"));
                lv_subject_notify(&hw_kinematics_subject_);
                snprintf(hw_accel_buf_, sizeof(hw_accel_buf_), "%s", lv_tr("Not detected"));
                lv_subject_notify(&hw_accel_subject_);
                // detected_hw_ is now known-bad, so the gate must be recomputed
                // against it rather than left on a stale pass.
                detected_hw_ = {};
                detection_pending_ = false;
                refresh_gate();
            }));
    }
}

void BeltTensionPanel::on_deactivating(DeactivateReason reason) {
    spdlog::debug("[BeltTension] on_deactivating({})", deactivate_reason_name(reason));

    // The menu hangs off the screen, not the panel: left up, a tap on it would
    // start a sweep with no belt UI on screen.
    hide_retest_menu();
    // The dialog's answer is owned by lifetime_, which every deactivation
    // expires: left up, Continue would close it and start nothing.
    hide_low_ram_dialog();
    // Left up, Identify would buzz a motor with no belt UI on screen.
    hide_names_dialog();

    // An idle screen is not a walk-away. A sweep means nobody is touching the
    // screen, so the screensaver lands mid-measurement; the run has to survive
    // it.
    if (reason == DeactivateReason::Suspended) {
        suspended_ = true;
        return;
    }
    suspended_ = false;
    theme_observer_.reset();
    ultrawide_observer_.reset();
    belt_layout_observer_.reset();
    language_observer_.reset();

    // A rebuild frees the whole widget tree without firing on_ui_destroyed(),
    // and the chart's object is a child of a host inside that tree. Drop it
    // here while the tree is still alive; create() re-caches the hosts and the
    // next run rebuilds the chart lazily.
    if (reason == DeactivateReason::Rebuild) {
        on_ui_destroyed();
    }

    // Abandon an in-progress run. The printer keeps sweeping (stopping it is
    // what Stop is for), but this panel stops listening and returns to START.
    if (measuring()) {
        spdlog::info("[BeltTension] Cancelling measurement on deactivate");
    }
    cancel_run();
    end_tuning();
    set_view_state(ViewState::START);
}

void BeltTensionPanel::cleanup() {
    spdlog::debug("[BeltTension] Cleaning up");

    cancel_run();
    clear_previews();
    hide_retest_menu();
    note_age_timer_.reset();

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // ObserverGuard::reset(), never release() (#579)
    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    klippy_observer_.reset();
    max_velocity_observer_.reset();
    theme_observer_.reset();
    ultrawide_observer_.reset();
    belt_layout_observer_.reset();
    language_observer_.reset();
    gate_observers_wired_ = false;
    hide_names_dialog();

    destroy_chart();

    // Unregister from NavigationManager
    if (overlay_root_) {
        helix::nav::unregister_overlay(overlay_root_);
    }

    OverlayBase::cleanup();
}

void BeltTensionPanel::on_ui_destroyed() {
    destroy_chart();
    chart_host_running_ = nullptr;
    chart_host_results_ = nullptr;
    chart_host_tuning_ = nullptr;
    chart_host_wide_ = nullptr;
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void BeltTensionPanel::set_view_state(ViewState state) {
    spdlog::debug("[BeltTension] View state change: {} -> {}",
                  lv_subject_get_int(&s_belt_tension_state), static_cast<int>(state));

    // Update subject - XML bindings handle visibility automatically
    lv_subject_set_int(&s_belt_tension_state, static_cast<int>(state));

    // RESULTS keeps its notes' ages current; nothing else shows an age.
    if (state == ViewState::RESULTS) {
        refresh_notes(); // "done" belongs to RUNNING; RESULTS shows ages
        start_note_age_timer();
    } else {
        note_age_timer_.reset();
    }

    place_chart(state);
}

lv_obj_t* BeltTensionPanel::chart_host_for(ViewState state) const {
    lv_obj_t* own = nullptr;
    switch (state) {
    case ViewState::RUNNING:
        own = chart_host_running_;
        break;
    case ViewState::RESULTS:
        own = chart_host_results_;
        break;
    case ViewState::TUNING:
        own = chart_host_tuning_;
        break;
    default:
        return nullptr;
    }
    // On ultrawide the XML hides every state's own host and shows the wide
    // one beside the sidebar; the chart is the one child XML cannot move.
    lv_subject_t* ultrawide = lv_xml_get_subject(nullptr, "ui_is_ultrawide");
    if (ultrawide && lv_subject_get_int(ultrawide) == 1 && chart_host_wide_) {
        return chart_host_wide_;
    }
    return own;
}

void BeltTensionPanel::place_chart(ViewState state) {
    lv_obj_t* host = chart_host_for(state);
    if (!chart_ || !host) {
        return;
    }
    lv_obj_t* obj = ui_frequency_response_chart_get_obj(chart_);
    if (lv_obj_get_parent(obj) != host) {
        lv_obj_set_parent(obj, host);
    }
}

// ============================================================================
// HARDWARE DETECTION CALLBACK
// ============================================================================

void BeltTensionPanel::on_hardware_detected(const helix::calibration::BeltTensionHardware& hw) {
    detected_hw_ = hw;
    detection_pending_ = false;

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

    snprintf(hw_accel_buf_, sizeof(hw_accel_buf_), "%s",
             hw.has_adxl ? lv_tr("Connected") : lv_tr("Not detected"));
    lv_subject_notify(&hw_accel_subject_);

    spdlog::info("[BeltTension] Hardware: {} ADXL={}", kin_label, hw.has_adxl);

    // Kinematics feeds BeltGateInputs::is_corexy, so the gate is only truthful
    // once detection has landed.
    refresh_gate();
}

void BeltTensionPanel::on_error(const std::string& message) {
    spdlog::warn("[BeltTension] Error: {}", message);
    cancel_run();
    snprintf(error_message_buf_, sizeof(error_message_buf_), "%s", message.c_str());
    lv_subject_copy_string(&error_message_subject_, error_message_buf_);
    set_view_state(ViewState::ERROR);
}

// ============================================================================
// RUN ORCHESTRATION
// ============================================================================

void BeltTensionPanel::handle_start_clicked() {
    spdlog::info("[BeltTension] Start clicked");

    if (!gate_allows_sweep("Start")) {
        return;
    }

    run_after_ram_check([this]() {
        end_tuning();
        begin_run({helix::calibration::BeltPath::PATH_A, helix::calibration::BeltPath::PATH_B},
                  /*clear_previous=*/true);
    });
}

void BeltTensionPanel::handle_retest_clicked(helix::calibration::BeltPath path) {
    spdlog::info("[BeltTension] Re-test clicked for path {}",
                 helix::calibration::BeltTensionCalibrator::output_name(path));
    if (!gate_allows_sweep("Re-test")) {
        return;
    }

    run_after_ram_check([this, path]() {
        const int idx = path_index(path);
        auto& run = runs_[idx];

        // The curve being replaced becomes the ghost this run compares against,
        // and the similarity the last comparison showed becomes bt_similarity_was.
        run.previous = std::move(run.curve);
        run.curve.clear();
        run.in_plane.clear();
        run.has_previous = run.has;
        run.has = false;
        was_similarity_percent_ = similarity_percent_;

        begin_run({path}, /*clear_previous=*/false);
    });
}

void BeltTensionPanel::handle_retest_slot_clicked(int slot) {
    handle_retest_clicked(helix::ui::belt_path_in_slot(slot));
}

void BeltTensionPanel::handle_retest_menu_clicked(lv_obj_t* anchor) {
    if (!anchor || retest_menu_.is_visible()) {
        return;
    }
    retest_menu_.set_action_callback([this](int action, int /*item*/) {
        if (action == helix::ui::BeltRetestMenu::kActionSlot0 ||
            action == helix::ui::BeltRetestMenu::kActionSlot1) {
            handle_retest_slot_clicked(action);
        }
    });
    retest_menu_.show_below_widget(lv_obj_get_screen(anchor), anchor);
}

void BeltTensionPanel::hide_low_ram_dialog() {
    if (low_ram_dialog_) {
        helix::ui::modal_hide(low_ram_dialog_);
        low_ram_dialog_ = nullptr;
    }
}

void BeltTensionPanel::hide_retest_menu() {
    retest_menu_.set_action_callback(nullptr);
    retest_menu_.hide();
}

// ============================================================================
// WHICH BELT IS WHICH
// ============================================================================

void BeltTensionPanel::handle_names_clicked() {
    if (names_modal_.is_visible()) {
        return;
    }
    set_identify_status(0, "");
    names_modal_.show(lv_screen_active());
}

void BeltTensionPanel::hide_names_dialog() {
    if (names_modal_.is_visible()) {
        names_modal_.hide();
    }
}

void BeltTensionPanel::set_identify_status(int state, const std::string& text) {
    if (!subjects_initialized_) {
        return;
    }
    snprintf(identify_status_buf_, sizeof(identify_status_buf_), "%s", text.c_str());
    lv_subject_copy_string(&identify_status_subject_, identify_status_buf_);
    lv_subject_set_int(&identify_state_subject_, state);
}

void BeltTensionPanel::handle_identify_clicked(helix::calibration::BeltPath path) {
    spdlog::info("[BeltTension] Identify clicked for path {}", path_letter(path));
    // Start's gate covers a buzz too: the calibrator reads busy while a sweep
    // or another buzz runs, which closes it.
    if (measuring() || !gate_allows_sweep("Identify") || !calibrator_) {
        return;
    }
    set_identify_status(
        1, fmt::format(fmt::runtime(lv_tr("Buzzing the motor of {}")), path_phrase(path)));
    calibrator_->buzz_path(
        path,
        [this]() {
            set_identify_status(0, "");
            refresh_gate();
        },
        [this](const std::string& message) {
            set_identify_status(2,
                                fmt::format(fmt::runtime(lv_tr("Identify failed: {}")), message));
            refresh_gate();
        });
    refresh_gate();
}

void BeltTensionPanel::refresh_names() {
    if (!subjects_initialized_) {
        return;
    }
    for (int slot = 0; slot < 2; ++slot) {
        lv_subject_copy_string(
            &slot_name_subject_[slot],
            helix::ui::belt_path_name(helix::ui::belt_path_in_slot(slot)).title.c_str());
    }
    lv_subject_set_int(&slots_swapped_subject_,
                       helix::ui::belt_path_in_slot(0) == BeltPath::PATH_B ? 1 : 0);
    refresh_notes();

    if (last_cmp_.valid) {
        refresh_peak_lines(last_cmp_);
    }
    refresh_run_title();
    refresh_tune();
    if (overlay_root_) {
        // The sketch is drawn, not bound: it asks belt_path_name() as it draws.
        if (lv_obj_t* sketch = lv_obj_find_by_name(overlay_root_, "bt_sketch")) {
            lv_obj_invalidate(sketch);
        }
    }
}

bool BeltTensionPanel::gate_allows_sweep(const char* action) {
    // The XML bindings already disable every sweep button while the gate is
    // shut. This is the same check on the action itself, so a stale binding or
    // a programmatic click cannot get past it.
    refresh_gate();
    if (lv_subject_get_int(&can_start_subject_) == 0) {
        spdlog::warn("[BeltTension] {} refused: {}", action,
                     lv_subject_get_string(&gate_message_subject_));
        return false;
    }
    return true;
}

void BeltTensionPanel::run_after_ram_check(std::function<void()> go) {
    const helix::MemoryInfo mem = mem_override_.value_or(helix::get_system_memory_info());
    const auto verdict = helix::resonance_memory_check(mem);
    if (verdict == helix::ResonanceMemory::OK) {
        go();
        return;
    }
    if (verdict == helix::ResonanceMemory::REFUSE) {
        helix::ui::show_resonance_memory_refusal(mem.headroom_mb());
        return;
    }
    // Klipper analyses the sweep on this same host, and on a small board that
    // analysis can run out of memory and leave Klipper stuck. One answer
    // covers a tuning session's rounds.
    if (tune_ && tune_low_ram_accepted_) {
        go();
        return;
    }
    if (low_ram_dialog_) {
        return; // the warning is already up
    }
    helix::ui::ConfirmOptions opts;
    opts.owner_token = lifetime_.token();
    // The gate can close while the warning is up (a print starts from another
    // client), so Continue re-checks it before anything changes.
    auto accepted = [this, go = std::move(go)]() {
        if (!gate_allows_sweep("Continue")) {
            return;
        }
        if (tune_) {
            tune_low_ram_accepted_ = true;
        }
        go();
    };
    helix::ui::show_low_ram_resonance_warning(mem.total_mb(), &low_ram_dialog_, accepted, opts);
    if (!low_ram_dialog_) {
        accepted(); // the modal failed to build; do not silently block the check
    }
}

void BeltTensionPanel::handle_stop_clicked() {
    spdlog::info("[BeltTension] Stop clicked");

    helix::ui::ConfirmOptions opts;
    // Nothing is held across the dialog: the sweep keeps running whether the
    // user confirms or not, and both exits leave the panel as it was.
    opts.on_dismiss = [] {};
    helix::ui::modal_confirm(
        lv_tr("Stop the check?"),
        lv_tr("This is an emergency stop: the printer halts and Klipper restarts."),
        ModalSeverity::Warning, lv_tr("Stop"),
        [this]() {
            // A short tuning round can end while the dialog is up; an
            // emergency stop then would restart an idle printer.
            if (!measuring()) {
                spdlog::info("[BeltTension] Stop confirmed after the sweep ended; nothing to stop");
                return;
            }
            spdlog::warn("[BeltTension] Emergency abort confirmed");
            if (calibrator_) {
                calibrator_->emergency_abort();
            }
            back_to_start();
        },
        opts);
}

void BeltTensionPanel::handle_retry_clicked() {
    spdlog::info("[BeltTension] Retry clicked");
    // A failed calibrator stays in its ERROR state and would keep Start
    // disabled forever; reset it so the gate can open again.
    if (calibrator_) {
        calibrator_->reset();
    }
    back_to_start();
}

void BeltTensionPanel::begin_run(const std::vector<helix::calibration::BeltPath>& queue,
                                 bool clear_previous, RunKind kind, bool settle_first) {
    if (queue.empty()) {
        return;
    }
    // Every sweep starts here, so every path into one passes the gate,
    // including a start deferred behind a dialog.
    if (!gate_allows_sweep(kind == RunKind::TUNE_ROUND    ? "Tuning round"
                           : kind == RunKind::CALIBRATION ? "Calibration"
                                                          : "Sweep")) {
        return;
    }
    // The session is the one source of a window: a round sweeps its zone,
    // everything else the printer's whole range.
    if (kind == RunKind::TUNE_ROUND && (!tune_ || !tune_->sweep_window())) {
        return;
    }
    run_kind_ = kind;
    run_window_ = kind == RunKind::TUNE_ROUND ? tune_->sweep_window() : std::nullopt;
    tune_facts_suffix_.clear();

    if (clear_previous) {
        for (auto& run : runs_) {
            run.previous.clear();
            run.has_previous = false;
        }
        was_similarity_percent_ = 0.0f;
    }

    // Markers belong to the comparison being replaced.
    if (chart_ && kind == RunKind::CHECK) {
        for (int idx = 0; idx < 2; ++idx) {
            ui_frequency_response_chart_set_markers(chart_, series_[idx], nullptr, 0);
        }
    }

    queue_ = queue;
    run_queue_total_ = queue_.size();
    run_started_ms_ = lv_tick_get();
    run_active_ = true;

    if (kind == RunKind::CHECK) {
        chart_to_running_host();
        set_view_state(ViewState::RUNNING);
    } else {
        set_tune_phase(TunePhase::SWEEPING);
    }
    refresh_notes();
    push_chart_data();
    start_elapsed_timer();
    if (settle_first) {
        start_settle();
    } else {
        settling_ = false;
        start_next_measurement();
    }
}

void BeltTensionPanel::start_settle() {
    settling_ = true;
    const uint32_t generation = ++settle_generation_;
    lv_subject_set_int(&running_path_subject_, 2); // neither path's colour
    lv_subject_copy_string(&run_title_subject_, lv_tr("Settling belts"));
    refresh_run_detail();
    refresh_notes();

    if (!calibrator_) {
        on_error(lv_tr("No printer connection"));
        return;
    }

    // The laps run at the probe point's height, so the probe point is re-read
    // rather than trusted from a query that may not have answered yet. The
    // stall guard stays off: the script's own timeout covers the laps.
    auto go = [this, generation]() {
        if (!run_active_ || !settling_ || generation != settle_generation_) {
            return;
        }
        settle_query_pending_ = false;
        if (!calibrator_) {
            on_error(lv_tr("No printer connection"));
            return;
        }
        calibrator_->settle(
            settle_inputs(), [this]() { on_settle_done(); },
            [this](helix::calibration::BeltSettleRefusal refusal) { on_settle_refused(refusal); },
            [this](const std::string& message) { on_sweep_error(message); });
    };
    if (!client_) {
        go();
        return;
    }
    settle_query_pending_ = true;
    helix::calibration::query_resonance_tester_config(
        *client_, lifetime_.bg_cb("BeltTension::settle_resonance_cfg",
                                  [this, go](helix::calibration::ResonanceTesterConfig cfg) {
                                      // A failed query answers with defaults; keep
                                      // what the printer said before.
                                      if (cfg.from_printer) {
                                          set_sweep_config(cfg);
                                      }
                                      go();
                                  }));
}

void BeltTensionPanel::on_settle_done() {
    if (!run_active_ || !settling_) {
        return;
    }
    settling_ = false;
    start_next_measurement();
}

void BeltTensionPanel::on_settle_refused(helix::calibration::BeltSettleRefusal refusal) {
    if (!run_active_) {
        return;
    }
    on_error(settle_refusal_message(refusal));
}

helix::calibration::BeltSettleInputs BeltTensionPanel::settle_inputs() const {
    auto& ps = get_printer_state();
    auto& motion = ps.motion_state();
    const AxisBounds machine = motion.get_axis_bounds();
    const AxisBounds gcode = motion.get_gcode_axis_bounds();

    helix::calibration::BeltSettleInputs in;
    in.x_min = machine.x_min;
    in.x_max = machine.x_max;
    in.y_min = machine.y_min;
    in.y_max = machine.y_max;
    in.has_xy_limits = machine.has_x && machine.has_y;
    in.probe_point = sweep_cfg_.probe_point;
    // machine = gcode + homing_origin
    in.origin_x = machine.x_min - gcode.x_min;
    in.origin_y = machine.y_min - gcode.y_min;
    in.origin_z = machine.z_min - gcode.z_min;
    in.max_velocity_mm_s =
        static_cast<float>(lv_subject_get_int(motion.get_max_velocity_subject()));
    const auto& tools = ToolState::instance();
    in.tool_count = tools.has_multiple_toolheads() ? tools.tool_count() : 1;
    return in;
}

float BeltTensionPanel::settle_seconds() const {
    const auto plan = helix::calibration::plan_belt_settle(settle_inputs());
    return plan.refusal == helix::calibration::BeltSettleRefusal::NONE ? plan.seconds : 0.0f;
}

void BeltTensionPanel::start_next_measurement() {
    if (queue_.empty()) {
        finish_run();
        return;
    }

    const auto path = queue_.front();
    const int idx = path_index(path);
    lv_subject_set_int(&running_path_subject_, idx);
    refresh_run_title();
    refresh_run_detail();
    refresh_notes();

    // One guard spans the whole queue: every progress line re-arms it, so it
    // measures silence, not sweep length.
    stall_guard_.begin(kStallTimeoutMs, [this]() { on_stall(); });

    if (!calibrator_) {
        on_error(lv_tr("No printer connection"));
        return;
    }

    // Armed before the sweep is sent, so its first console line starts the stream.
    arm_preview(path);
    calibrator_->measure_path(
        path, [this](int percent, float freq_hz) { on_sweep_progress(percent, freq_hz); },
        [this, path](helix::calibration::BeltResponse response) {
            on_sweep_complete(path, std::move(response));
        },
        [this](const std::string& message) { on_sweep_error(message); }, run_window_);
}

void BeltTensionPanel::on_sweep_progress(int /*percent*/, float freq_hz) {
    if (!run_active_) {
        return; // a cancelled run's transcript is stale the moment it lands
    }
    // Re-arm on every line: the guard fires only on true silence.
    stall_guard_.end();
    stall_guard_.begin(kStallTimeoutMs, [this]() { on_stall(); });

    preview_.note_console_frequency(freq_hz);
    if (chart_) {
        ui_frequency_response_chart_set_cursor(chart_, freq_hz, path_color(queue_.front()));
    }
    refresh_run_detail();
}

void BeltTensionPanel::on_sweep_complete(helix::calibration::BeltPath path,
                                         helix::calibration::BeltResponse response) {
    if (!run_active_) {
        return; // a cancelled run's transcript is stale the moment it lands
    }
    stall_guard_.end();
    preview_.stop();

    const int idx = path_index(path);
    if (run_kind_ == RunKind::CHECK) {
        // The CSV curve replaces the preview now. A tuning or calibration run
        // draws its curves only once the run is in, so the preview holds until then.
        preview_live_[idx] = false;
        auto& run = runs_[idx];
        run.curve = std::move(response.xyz);
        run.in_plane = std::move(response.in_plane);
        run.has = true;
        run.measured_at_ms = lv_tick_get();
    } else {
        tune_curves_[idx] = std::move(response);
    }

    if (chart_) {
        ui_frequency_response_chart_clear_cursor(chart_);
    }

    if (!queue_.empty() && queue_.front() == path) {
        queue_.erase(queue_.begin());
    }

    // A baseline round sweeps the pair twice with no turn between: the first
    // pair is the session's first round, and the second sweeps whatever
    // window that round left (it may have widened the zone).
    if (run_kind_ == RunKind::TUNE_ROUND && tune_ && queue_.size() == 2 && run_queue_total_ == 4) {
        add_tune_round();
        run_window_ = tune_->sweep_window();
    }

    refresh_notes();
    push_chart_data();

    if (queue_.empty()) {
        finish_run();
    } else {
        start_next_measurement();
    }
}

void BeltTensionPanel::on_sweep_error(const std::string& message) {
    if (!run_active_) {
        return;
    }
    on_error(message);
}

void BeltTensionPanel::on_stall() {
    spdlog::warn("[BeltTension] Stall guard fired: no progress for {} ms", kStallTimeoutMs);
    on_error(lv_tr("Klipper stopped reporting progress. Its analysis can run out of memory on "
                   "small printers and leave Klipper stuck, where a firmware restart cannot "
                   "reach it. Power-cycle the printer (or restart Klipper over SSH), then try "
                   "again."));
}

void BeltTensionPanel::finish_run() {
    elapsed_timer_.reset();
    if (run_kind_ == RunKind::TUNE_ROUND) {
        finish_tune_round();
        return;
    }
    if (run_kind_ == RunKind::CALIBRATION) {
        finish_calibration();
        return;
    }

    auto& a = runs_[0];
    auto& b = runs_[1];
    if (!a.has || !b.has) {
        // Cannot happen through the queue logic, but a half-cleared runs_ table
        // must not reach the comparison math.
        on_error(lv_tr("Measurement incomplete. Try again."));
        return;
    }

    const auto cmp = helix::calibration::compare_belt_paths(a.curve, b.curve, sweep_cfg_.min_freq,
                                                            sweep_cfg_.max_freq);
    if (!cmp.valid) {
        on_error(lv_tr("Not enough frequency data from the sweeps. Try again."));
        return;
    }

    last_cmp_ = cmp;
    populate_results(cmp);
    set_view_state(ViewState::RESULTS);
    spdlog::info("[BeltTension] Results: similarity={:.0f}% verdict={} pairs={} unpaired={}/{}",
                 cmp.similarity_percent, static_cast<int>(cmp.verdict), cmp.peaks.pairs.size(),
                 cmp.peaks.unpaired_a.size(), cmp.peaks.unpaired_b.size());
    // A gate subject that changed mid-sweep read the calibrator as busy.
    refresh_gate();
}

void BeltTensionPanel::populate_results(const helix::calibration::BeltComparison& cmp) {
    lv_subject_set_int(&verdict_subject_, static_cast<int>(cmp.verdict));
    const char* verdict_label = lv_tr("Poor match");
    switch (cmp.verdict) {
    case helix::calibration::BeltVerdict::MATCHED:
        verdict_label = lv_tr("Good match");
        break;
    case helix::calibration::BeltVerdict::CLOSE:
        verdict_label = lv_tr("Fair match");
        break;
    case helix::calibration::BeltVerdict::ADJUST:
        break;
    }
    snprintf(verdict_text_buf_, sizeof(verdict_text_buf_), "%s", verdict_label);
    lv_subject_copy_string(&verdict_text_subject_, verdict_text_buf_);

    snprintf(similarity_buf_, sizeof(similarity_buf_), "%.0f%%",
             static_cast<double>(cmp.similarity_percent));
    lv_subject_copy_string(&similarity_subject_, similarity_buf_);
    similarity_percent_ = cmp.similarity_percent;

    refresh_peak_lines(cmp);

    lv_subject_copy_string(&tune_cta_subject_,
                           cmp.verdict == helix::calibration::BeltVerdict::MATCHED
                               ? lv_tr("Fine-tune")
                               : lv_tr("Tune Belts"));

    push_chart_markers(cmp);
    refresh_notes();
    refresh_similarity_was();
    push_chart_data();
}

void BeltTensionPanel::refresh_peak_lines(const helix::calibration::BeltComparison& cmp) {
    // Paired peaks, strongest first, each pair in header-slot order:
    // "Peaks 35/36 · 133/132 Hz".
    const bool b_first = helix::ui::belt_path_in_slot(0) == BeltPath::PATH_B;
    const auto& pairs = cmp.peaks.pairs;
    std::string pair_list;
    for (size_t i = 0; i < pairs.size() && i < kMaxListedPeaks; ++i) {
        const float left = b_first ? pairs[i].b.freq_hz : pairs[i].a.freq_hz;
        const float right = b_first ? pairs[i].a.freq_hz : pairs[i].b.freq_hz;
        pair_list += fmt::format("{}{:.0f}/{:.0f}", i == 0 ? "" : " · ", left, right);
    }
    const std::string facts =
        (pair_list.empty() ? std::string(lv_tr("No shared peaks"))
                           : fmt::format(fmt::runtime(lv_tr("Peaks {} Hz")), pair_list)) +
        tune_facts_suffix_;
    snprintf(facts_buf_, sizeof(facts_buf_), "%s", facts.c_str());
    lv_subject_copy_string(&facts_subject_, facts_buf_);

    // Unpaired peaks, the strongest few per path, in frequency order:
    // "Only on Path A: 120, 129 Hz".
    const auto listed = [](std::vector<helix::calibration::BeltPeak> peaks) {
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto& l, const auto& r) { return l.amplitude > r.amplitude; });
        if (peaks.size() > kMaxListedPeaks) {
            peaks.resize(kMaxListedPeaks);
        }
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto& l, const auto& r) { return l.freq_hz < r.freq_hz; });
        return peaks;
    };
    const auto join_freqs = [](const std::vector<helix::calibration::BeltPeak>& peaks) {
        std::string out;
        for (size_t i = 0; i < peaks.size(); ++i) {
            out += fmt::format("{}{:.0f}", i == 0 ? "" : ", ", peaks[i].freq_hz);
        }
        return out;
    };
    // In header-slot order, so the line reads in the same order as the headers.
    std::string unpaired;
    for (int slot = 0; slot < 2; ++slot) {
        const BeltPath path = helix::ui::belt_path_in_slot(slot);
        const auto only =
            listed(path == BeltPath::PATH_A ? cmp.peaks.unpaired_a : cmp.peaks.unpaired_b);
        if (only.empty()) {
            continue;
        }
        unpaired += (unpaired.empty() ? "" : " · ") +
                    fmt::format(fmt::runtime(lv_tr("Only on {0}: {1} Hz")), path_phrase(path),
                                join_freqs(only));
    }
    snprintf(unpaired_buf_, sizeof(unpaired_buf_), "%s", unpaired.c_str());
    lv_subject_copy_string(&unpaired_subject_, unpaired_buf_);
    lv_subject_set_int(&has_unpaired_subject_, unpaired.empty() ? 0 : 1);
}

void BeltTensionPanel::push_chart_markers(const helix::calibration::BeltComparison& cmp) {
    if (!chart_) {
        return;
    }
    // Numbered dots on the listed pairs (the same number on both curves),
    // hollow rings on the listed unpaired peaks.
    std::vector<FrChartMarker> markers[2];
    const auto& pairs = cmp.peaks.pairs;
    for (size_t i = 0; i < pairs.size() && i < kMaxListedPeaks; ++i) {
        const int number = static_cast<int>(i) + 1;
        markers[0].push_back({pairs[i].a.freq_hz, number});
        markers[1].push_back({pairs[i].b.freq_hz, number});
    }
    const auto add_unpaired = [](std::vector<helix::calibration::BeltPeak> peaks,
                                 std::vector<FrChartMarker>& out) {
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto& l, const auto& r) { return l.amplitude > r.amplitude; });
        for (size_t i = 0; i < peaks.size() && i < kMaxListedPeaks; ++i) {
            out.push_back({peaks[i].freq_hz, 0});
        }
    };
    add_unpaired(cmp.peaks.unpaired_a, markers[0]);
    add_unpaired(cmp.peaks.unpaired_b, markers[1]);
    for (int idx = 0; idx < 2; ++idx) {
        ui_frequency_response_chart_set_markers(chart_, series_[idx], markers[idx].data(),
                                                markers[idx].size());
    }
}

void BeltTensionPanel::cancel_run() {
    run_active_ = false;
    settling_ = false;
    settle_query_pending_ = false;
    ++settle_generation_;
    run_window_.reset();
    run_kind_ = RunKind::CHECK;
    if (calibrator_) {
        calibrator_->cancel();
    }
    queue_.clear();
    stall_guard_.end();
    elapsed_timer_.reset();
    clear_previews();
    if (chart_) {
        ui_frequency_response_chart_clear_cursor(chart_);
    }
}

void BeltTensionPanel::back_to_start() {
    cancel_run();
    end_tuning();
    refresh_gate();
    set_view_state(ViewState::START);
}

bool BeltTensionPanel::measuring() const {
    const auto state = static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state));
    return state == ViewState::RUNNING ||
           (state == ViewState::TUNING && tune_phase_ == TunePhase::SWEEPING);
}

// ============================================================================
// SUBJECT REFRESH
// ============================================================================

void BeltTensionPanel::refresh_notes() {
    const bool running =
        static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::RUNNING;

    for (int slot = 0; slot < 2; ++slot) {
        const int idx = path_index(helix::ui::belt_path_in_slot(slot));
        const auto& run = runs_[idx];

        std::string note = "--";
        if (running && !settling_ && !queue_.empty() && queue_.front() == path_at(idx)) {
            note = lv_tr("sweeping");
        } else if (running && run.has && run.measured_at_ms >= run_started_ms_) {
            note = lv_tr("done");
        } else if (run.has) {
            const uint32_t age_s = (lv_tick_get() - run.measured_at_ms) / 1000;
            note = age_s < 60 ? lv_tr("just now")
                              : fmt::format(fmt::runtime(lv_tr("{} min ago")), age_s / 60);
        }

        snprintf(slot_note_buf_[slot], sizeof(slot_note_buf_[slot]), "%s", note.c_str());
        lv_subject_copy_string(&slot_note_subject_[slot], slot_note_buf_[slot]);
    }
}

void BeltTensionPanel::refresh_similarity_was() {
    const bool replaced = runs_[0].has_previous || runs_[1].has_previous;
    std::string was;
    if (replaced) {
        was = fmt::format(fmt::runtime(lv_tr("was {:.0f}%")), was_similarity_percent_);
    }
    snprintf(similarity_was_buf_, sizeof(similarity_was_buf_), "%s", was.c_str());
    lv_subject_copy_string(&similarity_was_subject_, similarity_was_buf_);
    lv_subject_set_int(&has_similarity_was_subject_, was.empty() ? 0 : 1);
}

void BeltTensionPanel::start_note_age_timer() {
    note_age_timer_.reset(lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<BeltTensionPanel*>(lv_timer_get_user_data(t));
            self->refresh_notes();
        },
        kNoteAgeRefreshMs, this));
}

void BeltTensionPanel::refresh_run_title() {
    if (!run_active_ || settling_ || queue_.empty()) {
        return; // the laps own the title while they run
    }
    // A single-path run is a re-test: the path already has a result.
    const char* title = run_queue_total_ == 1 ? lv_tr("Re-measuring {}") : lv_tr("Measuring {}");
    lv_subject_copy_string(&run_title_subject_,
                           fmt::format(fmt::runtime(title), path_phrase(queue_.front())).c_str());
}

void BeltTensionPanel::refresh_run_detail() {
    if (run_queue_total_ > 1 && !settling_) {
        snprintf(run_detail_buf_, sizeof(run_detail_buf_), "%zu of %zu · %u:%02u",
                 run_queue_total_ - queue_.size() + 1, run_queue_total_,
                 (lv_tick_get() - run_started_ms_) / 1000 / 60,
                 (lv_tick_get() - run_started_ms_) / 1000 % 60);
    } else {
        snprintf(run_detail_buf_, sizeof(run_detail_buf_), "%s",
                 fmt::format(lv_tr("{}:{:02} elapsed"),
                             (lv_tick_get() - run_started_ms_) / 1000 / 60,
                             (lv_tick_get() - run_started_ms_) / 1000 % 60)
                     .c_str());
    }
    lv_subject_copy_string(&run_detail_subject_, run_detail_buf_);
}

void BeltTensionPanel::start_elapsed_timer() {
    elapsed_timer_.reset(lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<BeltTensionPanel*>(lv_timer_get_user_data(t));
            self->refresh_run_detail();
        },
        1000, this));
}

// ============================================================================
// TUNING LOOP
// ============================================================================

void BeltTensionPanel::handle_tune_clicked() {
    if (!last_cmp_.valid || !runs_[0].has || !runs_[1].has || measuring()) {
        spdlog::warn("[BeltTension] Tune refused: no comparison to tune from");
        return;
    }
    // A narrow sweep sends FREQ_END, which Klipper takes up to 300 Hz whatever
    // the printer's max_freq says; Klipper's defaults are no stand-in for it.
    if (!sweep_cfg_.from_printer) {
        spdlog::warn("[BeltTension] Tune refused: the printer has not reported its sweep range");
        return;
    }
    const auto kind =
        PrinterDetector::get_belt_tensioner(get_printer_state().profile_state().printer_type());
    tune_.emplace(helix::calibration::BeltResponse{runs_[0].curve, runs_[0].in_plane},
                  helix::calibration::BeltResponse{runs_[1].curve, runs_[1].in_plane}, last_cmp_,
                  sweep_cfg_.min_freq, sweep_cfg_.max_freq, kind);
    tune_shape_differs_ = helix::calibration::tallest_peak_unpaired(last_cmp_);
    tune_round0_similarity_ = similarity_percent_;
    tune_has_curves_ = false;
    tune_adj_history_.clear();
    tune_last_outcome_.reset();
    tune_last_calibration_.reset();
    reset_tune_turns();
    reset_tune_gauge_range();
    spdlog::info("[BeltTension] Tuning: kind={} stage={} adjusting {}", static_cast<int>(kind),
                 static_cast<int>(tune_->stage()), path_letter(tune_->adjusted_path()));

    tune_phase_ = tune_phase_for_stage();
    lv_subject_set_int(&tune_phase_subject_, static_cast<int>(tune_phase_));
    set_view_state(ViewState::TUNING);
    refresh_tune();
    push_chart_data();
    // A spring tensioner has its zone already: its baseline sweeps at once.
    if (tune_->needs_baseline()) {
        start_tune_round();
    }
}

BeltTensionPanel::TunePhase BeltTensionPanel::tune_phase_for_stage() const {
    using helix::calibration::BeltTuneStage;
    if (!tune_) {
        return TunePhase::ADJUST;
    }
    switch (tune_->stage()) {
    case BeltTuneStage::ALREADY_MATCHED:
        return TunePhase::ALREADY_MATCHED;
    case BeltTuneStage::CALIBRATE:
        return TunePhase::CALIBRATE;
    case BeltTuneStage::NO_ZONE:
        return TunePhase::MATCHED;
    case BeltTuneStage::BASELINE:
    case BeltTuneStage::TUNE:
    case BeltTuneStage::UNREPEATABLE:
        break;
    }
    return tune_->zone_matched() ? TunePhase::MATCHED : TunePhase::ADJUST;
}

void BeltTensionPanel::handle_tune_measure_clicked() {
    if (!tune_ || measuring()) {
        return;
    }
    if (tune_phase_ == TunePhase::CALIBRATE) {
        if (!gate_allows_sweep("Measure")) {
            return;
        }
        run_after_ram_check([this]() {
            if (!tune_ || tune_phase_ != TunePhase::CALIBRATE) {
                return;
            }
            begin_run({BeltPath::PATH_A, BeltPath::PATH_B}, /*clear_previous=*/false,
                      RunKind::CALIBRATION);
        });
        return;
    }
    if (!tune_->zone() || tune_phase_ != TunePhase::ADJUST) {
        return;
    }
    if (!gate_allows_sweep("Re-measure")) {
        return;
    }
    if (tune_->tensioner() == helix::calibration::BeltTensionerKind::SCREW &&
        !tune_->needs_baseline()) {
        tune_->record_turns(tune_turns_);
    }
    start_tune_round();
}

void BeltTensionPanel::handle_tune_done_clicked() {
    if (measuring()) {
        return;
    }
    end_tuning();
    set_view_state(ViewState::RESULTS);
    push_chart_markers(last_cmp_);
    push_chart_data();
}

void BeltTensionPanel::handle_tune_calibrate_anyway_clicked() {
    if (!tune_ || measuring() || tune_phase_ != TunePhase::ALREADY_MATCHED) {
        return;
    }
    tune_->calibrate_anyway();
    reset_tune_turns();
    reset_tune_gauge_range();
    set_tune_phase(tune_phase_for_stage());
    push_chart_data();
    if (tune_->needs_baseline()) {
        start_tune_round();
    }
}

void BeltTensionPanel::handle_tune_swap_clicked() {
    if (!tune_ || (tune_phase_ != TunePhase::ADJUST && tune_phase_ != TunePhase::CALIBRATE)) {
        return;
    }
    if (!tune_->swap_adjusted_path()) {
        return;
    }
    tune_adj_history_.clear();
    tune_last_outcome_.reset();
    tune_last_calibration_.reset();
    reset_tune_turns();
    refresh_tune();
    push_chart_data();
}

void BeltTensionPanel::handle_tune_amount_step(int direction) {
    if (!tune_ || tune_->tensioner() != helix::calibration::BeltTensionerKind::SCREW) {
        return;
    }
    namespace bt = helix::calibration::belt_tune;
    tune_turns_ = std::clamp(tune_turns_ + (direction > 0 ? bt::kTurnStep : -bt::kTurnStep), 0.0f,
                             bt::kTurnMax);
    refresh_tune();
}

void BeltTensionPanel::handle_tune_next_zone_clicked() {
    if (!tune_ || measuring() || !gate_allows_sweep("Next zone")) {
        return;
    }
    if (!tune_->next_zone()) {
        refresh_tune();
        return;
    }
    // The previous zone's curves were swept over its own window.
    tune_has_curves_ = false;
    tune_adj_history_.clear();
    tune_last_outcome_.reset();
    tune_last_calibration_.reset();
    reset_tune_turns();
    // The earlier zone's turns moved this zone's peaks: measure before advising.
    set_tune_phase(TunePhase::ADJUST);
    push_chart_data();
    start_tune_round();
}

void BeltTensionPanel::handle_tune_confirm_clicked() {
    if (!tune_ || measuring() || !gate_allows_sweep("Confirm")) {
        return;
    }
    const auto& t = *tune_;
    // A zone that matched at its baseline took no tuning round: no count to report.
    const int rounds = t.total_rounds();
    std::string summary =
        rounds == 0 ? std::string()
                    : fmt::format(fmt::runtime(rounds == 1 ? lv_tr(" · Tuned in {} round")
                                                           : lv_tr(" · Tuned in {} rounds")),
                                  rounds);
    if (t.tensioner() == helix::calibration::BeltTensionerKind::SCREW) {
        for (const auto path : {BeltPath::PATH_A, BeltPath::PATH_B}) {
            const float turns = t.total_turns(path);
            if (std::abs(turns) < helix::calibration::belt_tune::kTurnStep / 2.0f) {
                continue;
            }
            summary += fmt::format(fmt::runtime(turns > 0.0f ? lv_tr(" · {} tightened about {}")
                                                             : lv_tr(" · {} loosened about {}")),
                                   path_title(path), format_turn_amount(turns));
        }
    }
    const float round0 = tune_round0_similarity_;

    run_after_ram_check([this, summary, round0]() {
        // bt_similarity_was is measured against the comparison tuning started from.
        for (auto& run : runs_) {
            run.previous = run.curve;
            run.has_previous = run.has;
        }
        was_similarity_percent_ = round0;
        end_tuning();
        begin_run({BeltPath::PATH_A, BeltPath::PATH_B}, /*clear_previous=*/false);
        tune_facts_suffix_ = summary;
    });
}

void BeltTensionPanel::handle_tune_keep_clicked() {
    if (!tune_ || !tune_->zone() || measuring()) {
        return;
    }
    reset_tune_turns();
    set_tune_phase(TunePhase::ADJUST);
}

int BeltTensionPanel::tune_round_pairs() const {
    using helix::calibration::BeltTuneStage;
    return tune_ && (tune_->stage() == BeltTuneStage::BASELINE ||
                     tune_->stage() == BeltTuneStage::UNREPEATABLE)
               ? 2
               : 1;
}

void BeltTensionPanel::start_tune_round(bool settle_first) {
    if (!tune_ || !tune_->zone()) {
        return;
    }
    run_after_ram_check([this, settle_first]() {
        if (!tune_ || !tune_->zone()) {
            return;
        }
        const int adj = path_index(tune_->adjusted_path());
        const helix::calibration::BeltCurve shown = helix::calibration::tuning_curve(
            tune_has_curves_
                ? tune_curves_[adj]
                : helix::calibration::BeltResponse{runs_[adj].curve, runs_[adj].in_plane});
        if (!shown.empty()) {
            tune_adj_history_.push_back(shown);
            if (tune_adj_history_.size() > 2) {
                tune_adj_history_.erase(tune_adj_history_.begin());
            }
        }
        std::vector<BeltPath> queue;
        for (int i = 0; i < tune_round_pairs(); ++i) {
            queue.push_back(BeltPath::PATH_A);
            queue.push_back(BeltPath::PATH_B);
        }
        begin_run(queue, /*clear_previous=*/false, RunKind::TUNE_ROUND, settle_first);
    });
}

const helix::calibration::BeltTuneRound& BeltTensionPanel::add_tune_round() {
    const float gap_before = tune_->gap_hz();
    tune_last_direction_ = tune_->advice().direction;
    const auto& round = tune_->add_round(tune_curves_[0], tune_curves_[1]);
    tune_has_curves_ = true;
    tune_last_outcome_ = round.outcome;
    tune_last_move_hz_ = std::abs(tune_->gap_hz() - gap_before);
    spdlog::info("[BeltTension] Tune round {}: gap {:+.2f} Hz, outcome {}", tune_->total_rounds(),
                 tune_->gap_hz(), static_cast<int>(round.outcome));
    return round;
}

void BeltTensionPanel::finish_tune_round() {
    clear_previews();
    run_window_.reset();
    run_kind_ = RunKind::CHECK;
    if (!tune_) {
        return;
    }
    const bool baseline = tune_round_pairs() == 2;
    add_tune_round();
    if (baseline && tune_->stage() == helix::calibration::BeltTuneStage::TUNE) {
        reset_tune_gauge_range();
    }
    reset_tune_turns();
    set_tune_phase(tune_phase_for_stage());
    push_chart_data();
    // A gate subject that changed mid-sweep read the calibrator as busy.
    refresh_gate();
}

void BeltTensionPanel::finish_calibration() {
    clear_previews();
    run_window_.reset();
    run_kind_ = RunKind::CHECK;
    if (!tune_) {
        return;
    }
    const auto result = tune_->add_calibration_sweep(tune_curves_[0], tune_curves_[1]);
    tune_has_curves_ = true;
    spdlog::info("[BeltTension] Calibration step: moved {:+.2f} Hz over {} turns, registered={} "
                 "gave_up={}",
                 result.move_hz, result.turns, result.registered, result.gave_up);
    reset_tune_turns();
    if (result.registered) {
        // The zone is entered from these very sweeps: its baseline follows at
        // once, with no laps, since no belt changed in between.
        tune_last_calibration_.reset();
        tune_last_outcome_.reset();
        tune_adj_history_.clear();
        reset_tune_gauge_range();
        set_tune_phase(TunePhase::ADJUST);
        start_tune_round(/*settle_first=*/false);
        return;
    }
    tune_last_calibration_ = result;
    set_tune_phase(tune_phase_for_stage());
    push_chart_data();
    refresh_gate();
}

void BeltTensionPanel::set_tune_phase(TunePhase phase) {
    tune_phase_ = phase;
    lv_subject_set_int(&tune_phase_subject_, static_cast<int>(phase));
    refresh_tune();
}

void BeltTensionPanel::reset_tune_turns() {
    tune_turns_ = 0.0f;
}

void BeltTensionPanel::reset_tune_gauge_range() {
    const float floor_hz = 2.0f * helix::calibration::belt_tune::kGaugeWarnHz;
    tune_gauge_range_hz_ = tune_ ? std::max(floor_hz, 1.25f * std::abs(tune_->gap_hz())) : floor_hz;
}

void BeltTensionPanel::end_tuning() {
    clear_previews();
    tune_.reset();
    tune_low_ram_accepted_ = false;
    tune_has_curves_ = false;
    tune_curves_[0] = {};
    tune_curves_[1] = {};
    tune_adj_history_.clear();
    tune_last_outcome_.reset();
    tune_last_calibration_.reset();
    tune_phase_ = TunePhase::ADJUST;
    if (subjects_initialized_) {
        lv_subject_set_int(&tune_phase_subject_, static_cast<int>(tune_phase_));
    }
}

int BeltTensionPanel::round_minutes(const helix::calibration::BeltSweepWindow& window,
                                    int pairs) const {
    if (sweep_cfg_.hz_per_sec <= 0.0f) {
        return 1;
    }
    // Both paths sweep the window once per pair, after the laps.
    const float seconds = 2.0f * static_cast<float>(pairs) *
                              (window.freq_end_hz - window.freq_start_hz) / sweep_cfg_.hz_per_sec +
                          settle_seconds();
    return std::max(1, static_cast<int>(std::lround(seconds / 60.0f)));
}

int BeltTensionPanel::full_check_minutes() const {
    // Both paths sweep once each, hence the 2x, after the laps.
    return std::max(1, static_cast<int>(std::ceil(
                           (2.0f * sweep_cfg_.sweep_seconds() + settle_seconds()) / 60.0f)));
}

void BeltTensionPanel::refresh_tune() {
    if (!tune_ || !subjects_initialized_) {
        return;
    }
    using helix::calibration::BeltTensionerKind;
    using helix::calibration::BeltTuneStage;
    using helix::calibration::BeltTurnDirection;
    const auto& t = *tune_;
    const BeltPath adj = t.adjusted_path();
    const std::string adj_name = path_phrase(adj);
    const auto& zone = t.zone();
    const auto stage = t.stage();
    const bool baseline = t.needs_baseline();
    const bool calibrating = stage == BeltTuneStage::CALIBRATE;
    const bool unrepeatable = stage == BeltTuneStage::UNREPEATABLE;
    // A gap is on screen wherever one was measured; the turn advice only in
    // CALIBRATE and TUNE. UNREPEATABLE, NO_ZONE and ALREADY_MATCHED advise nothing.
    const bool has_gap = stage == BeltTuneStage::ALREADY_MATCHED || calibrating || unrepeatable ||
                         stage == BeltTuneStage::TUNE;
    const bool advises = calibrating || stage == BeltTuneStage::TUNE;
    const bool screw = t.tensioner() == BeltTensionerKind::SCREW;
    const auto advice = t.advice();
    const float gap = t.gap_hz();
    const float tolerance = t.tolerance_hz();

    lv_subject_set_int(&tune_kind_subject_, screw                                        ? 1
                                            : t.tensioner() == BeltTensionerKind::SPRING ? 2
                                                                                         : 0);
    lv_subject_set_int(&tune_path_subject_, path_index(adj));
    lv_subject_set_int(&tune_dir_subject_,
                       advises && advice.direction == BeltTurnDirection::LOOSEN ? 1 : 0);
    lv_subject_set_int(&tune_baseline_subject_, baseline ? 1 : 0);
    lv_subject_set_int(&tune_unrepeatable_subject_, unrepeatable ? 1 : 0);
    lv_subject_set_int(&tune_has_zone_subject_, zone ? 1 : 0);
    lv_subject_set_int(&tune_has_gap_subject_, has_gap ? 1 : 0);
    lv_subject_set_int(&tune_has_next_zone_subject_, t.peek_next_zone() ? 1 : 0);
    lv_subject_set_int(&tune_can_swap_subject_, t.can_swap_adjusted_path() ? 1 : 0);
    lv_subject_set_int(&tune_shape_warning_subject_,
                       unrepeatable || t.show_mechanical_hint() ? 1 : 0);
    lv_subject_set_int(&tune_severity_subject_,
                       static_cast<int>(helix::ui::belt_gap_severity(gap, tolerance)));

    std::string instruction;
    if (stage == BeltTuneStage::NO_ZONE) {
        instruction = lv_tr("Nothing to tune here");
    } else if (stage == BeltTuneStage::ALREADY_MATCHED) {
        instruction = lv_tr("Belts already match");
    } else if (calibrating) {
        const bool loosen = advice.direction == BeltTurnDirection::LOOSEN;
        instruction = fmt::format(fmt::runtime(screw ? (loosen ? lv_tr("Loosen {0} one full turn")
                                                               : lv_tr("Tighten {0} one full turn"))
                                                     : (loosen ? lv_tr("Loosen {0} noticeably")
                                                               : lv_tr("Tighten {0} noticeably"))),
                                  adj_name);
    } else if (tune_phase_ == TunePhase::MATCHED) {
        instruction = fmt::format(fmt::runtime(lv_tr("Matched at {:.1f} Hz")), t.adjusted_hz());
    } else if (baseline) {
        instruction = lv_tr("Measure this zone first");
    } else if (unrepeatable) {
        instruction = lv_tr("Readings don't repeat");
    } else if (advice.spring_reseat) {
        instruction = fmt::format(
            fmt::runtime(lv_tr("Release {}'s tensioner, let the spring settle, lock it")),
            adj_name);
    } else if (advice.turns) {
        // The suggestion lives here; the stepper records what was turned.
        instruction = fmt::format(fmt::runtime(advice.direction == BeltTurnDirection::TIGHTEN
                                                   ? lv_tr("Tighten {0} {1}")
                                                   : lv_tr("Loosen {0} {1}")),
                                  adj_name, format_turn_amount(*advice.turns));
    } else {
        instruction = fmt::format(fmt::runtime(advice.direction == BeltTurnDirection::TIGHTEN
                                                   ? lv_tr("Tighten {}")
                                                   : lv_tr("Loosen {}")),
                                  adj_name);
    }
    lv_subject_copy_string(&tune_instruction_subject_, instruction.c_str());

    std::string amount;
    if (screw && stage == BeltTuneStage::TUNE) {
        amount = tune_turns_ > 0.0f ? format_turn_amount(tune_turns_) : lv_tr("Not turned");
    }
    lv_subject_copy_string(&tune_amount_subject_, amount.c_str());
    lv_subject_copy_string(&tune_gap_subject_,
                           has_gap ? fmt::format("{:.1f} Hz", std::abs(gap)).c_str() : "--");

    std::string caption;
    if (has_gap) {
        caption = std::abs(gap) <= tolerance
                      ? std::string(lv_tr("within one bin"))
                      : fmt::format(fmt::runtime(gap < 0.0f ? lv_tr("{} is looser")
                                                            : lv_tr("{} is tighter")),
                                    path_title(adj));
    }
    lv_subject_copy_string(&tune_gap_caption_subject_, caption.c_str());

    const int rounds = t.total_rounds();
    lv_subject_copy_string(
        &tune_round_subject_,
        fmt::format(fmt::runtime(lv_tr("Round {}")),
                    tune_phase_ == TunePhase::MATCHED ? std::max(1, rounds) : rounds + 1)
            .c_str());
    lv_subject_copy_string(&tune_zone_subject_,
                           zone ? fmt::format(fmt::runtime(lv_tr("Zone {:.0f}-{:.0f} Hz")),
                                              zone->window.freq_start_hz, zone->window.freq_end_hz)
                                      .c_str()
                                : "");
    const auto rate = t.hz_per_turn();
    lv_subject_copy_string(
        &tune_rate_subject_,
        screw && rate ? fmt::format(fmt::runtime(lv_tr("~{:.1f} Hz / turn")), *rate).c_str() : "");
    const std::string last = tune_last_message();
    lv_subject_copy_string(&tune_last_subject_, last.c_str());
    lv_subject_set_int(&tune_has_last_subject_, last.empty() ? 0 : 1);
    lv_subject_copy_string(
        &tune_hint_subject_,
        unrepeatable ? lv_tr("These readings aren't repeatable, so tuning can't be steered. Check "
                             "the mechanics first.")
        : tune_shape_differs_
            ? lv_tr("Curves differ in shape. Check the mechanics before chasing a perfect match.")
            : lv_tr("The gap is not closing. Something besides tension may be shaping it."));

    const auto power = t.power_hint();
    std::string power_line;
    if (power) {
        power_line = fmt::format(
            fmt::runtime(lv_tr("{0} resonates much more than {1} at {2:.0f} Hz. That points at a "
                               "mechanical difference (pulley grub screws, idler bearings, belt "
                               "tracking), not tension.")),
            path_title(power->stronger), path_phrase(other_path(power->stronger)), power->freq_hz);
    }
    snprintf(tune_power_hint_buf_, sizeof(tune_power_hint_buf_), "%s", power_line.c_str());
    lv_subject_copy_string(&tune_power_hint_subject_, tune_power_hint_buf_);
    lv_subject_set_int(&tune_has_power_hint_subject_, power ? 1 : 0);

    lv_subject_copy_string(
        &tune_tensioner_subject_,
        fmt::format(fmt::runtime(lv_tr("Tensioner: {}")), path_title(adj)).c_str());

    const auto swept = t.sweep_window();
    lv_subject_copy_string(
        &tune_measure_label_subject_,
        calibrating
            ? fmt::format(fmt::runtime(lv_tr("Measure (~{} min)")), full_check_minutes()).c_str()
            : fmt::format(fmt::runtime(lv_tr("Re-measure (~{} min)")),
                          swept ? round_minutes(*swept, tune_round_pairs()) : 1)
                  .c_str());
    lv_subject_copy_string(&tune_measure_short_label_subject_,
                           calibrating ? lv_tr("Measure") : lv_tr("Re-measure"));
    // Next zone sweeps at once: its baseline pair over the next zone's sweep window.
    const auto next = t.peek_next_zone();
    lv_subject_copy_string(&tune_next_zone_label_subject_,
                           next ? fmt::format(fmt::runtime(lv_tr("Next Zone (~{} min)")),
                                              round_minutes(helix::calibration::zone_sweep_window(
                                                                next->window, sweep_cfg_.min_freq,
                                                                sweep_cfg_.max_freq),
                                                            2))
                                      .c_str()
                                : "");
    lv_subject_copy_string(
        &tune_confirm_label_subject_,
        fmt::format(fmt::runtime(lv_tr("Confirm (full, ~{} min)")), full_check_minutes()).c_str());

    if (has_gap && overlay_root_) {
        // The hero's gauge, and the one-line hero's thin bar on the smallest screens.
        for (const char* name : {"bt_tune_gauge", "bt_tune_line_gauge"}) {
            if (lv_obj_t* gauge = lv_obj_find_by_name(overlay_root_, name)) {
                helix::ui::ui_belt_gap_gauge_set(gauge, gap, t.previous_gap_hz(), tolerance,
                                                 tune_gauge_range_hz_);
            }
        }
    }
}

std::string BeltTensionPanel::tune_last_message() const {
    if (!tune_) {
        return {};
    }
    using helix::calibration::BeltTensionerKind;
    using helix::calibration::BeltTuneOutcome;
    using helix::calibration::BeltTuneStage;
    const auto& t = *tune_;
    const std::string adj_name = path_phrase(t.adjusted_path());
    const std::string ref_name = path_phrase(other_path(t.adjusted_path()));
    const bool screw = t.tensioner() == BeltTensionerKind::SCREW;

    if (t.stage() == BeltTuneStage::ALREADY_MATCHED) {
        return fmt::format(fmt::runtime(lv_tr("Within {0:.1f} Hz of each other at {1:.0f} Hz")),
                           std::abs(t.gap_hz()), t.adjusted_hz());
    }
    if (tune_last_calibration_) {
        const auto& cal = *tune_last_calibration_;
        if (cal.gave_up) {
            return screw ? fmt::format(fmt::runtime(lv_tr("Nothing followed {1} turns of {0}. Turn "
                                                          "{0} back {1} turns, then check the "
                                                          "mechanics.")),
                                       adj_name, static_cast<int>(std::lround(cal.turns)))
                         : fmt::format(fmt::runtime(lv_tr("Nothing followed the turns on {0}. "
                                                          "Undo them, then check the mechanics.")),
                                       adj_name);
        }
        const bool loosen = t.advice().direction == helix::calibration::BeltTurnDirection::LOOSEN;
        const char* line =
            screw ? (loosen ? lv_tr("That turn didn't register yet. Loosen {0} one more full turn.")
                            : lv_tr("That turn didn't register yet. Tighten {0} one more full "
                                    "turn."))
                  : (loosen ? lv_tr("That turn didn't register yet. Loosen {0} a little more.")
                            : lv_tr("That turn didn't register yet. Tighten {0} a little more."));
        return fmt::format(fmt::runtime(line), adj_name);
    }
    if (!tune_last_outcome_) {
        return {};
    }
    const bool no_turn = !t.rounds().empty() && t.rounds().back().no_turn;
    switch (*tune_last_outcome_) {
    case BeltTuneOutcome::CLOSER:
    case BeltTuneOutcome::OVERSHOT:
    case BeltTuneOutcome::WRONG_WAY:
    case BeltTuneOutcome::NO_MOVEMENT:
        // A round with no turn reports the gap's move, not a verdict on a turn.
        if (no_turn) {
            return fmt::format(fmt::runtime(lv_tr("No turn: the gap moved {:.1f} Hz")),
                               tune_last_move_hz_);
        }
        break;
    default:
        break;
    }
    switch (*tune_last_outcome_) {
    case BeltTuneOutcome::CLOSER: {
        const auto prev = t.previous_gap_hz();
        const float closer = prev ? std::abs(*prev) - std::abs(t.gap_hz()) : 0.0f;
        return fmt::format(fmt::runtime(lv_tr("last turn: {:.1f} Hz closer")), closer);
    }
    case BeltTuneOutcome::OVERSHOT:
        return fmt::format(fmt::runtime(lv_tr("Went past {} by {:.1f} Hz. Back it off a little.")),
                           ref_name, std::abs(t.gap_hz()));
    case BeltTuneOutcome::WRONG_WAY:
        return fmt::format(
            fmt::runtime(tune_last_direction_ == helix::calibration::BeltTurnDirection::TIGHTEN
                             ? lv_tr("The gap grew after tightening {0}. Check you turned "
                                     "{0}'s tensioner.")
                             : lv_tr("The gap grew after loosening {0}. Check you turned "
                                     "{0}'s tensioner.")),
            adj_name);
    case BeltTuneOutcome::NO_MOVEMENT:
        return fmt::format(
            fmt::runtime(lv_tr("The gap didn't close (moved {:.1f} Hz). Try a bigger turn.")),
            tune_last_move_hz_);
    case BeltTuneOutcome::NO_PEAK:
        if (!t.rounds().empty() && t.rounds().back().reference_lost) {
            return fmt::format(
                fmt::runtime(lv_tr("The peak of {0} left the zone. Check that only {1}'s tensioner "
                                   "was turned. Not sure which is which? Tap the pencil to "
                                   "identify the belts.")),
                ref_name, adj_name);
        }
        return lv_tr("No clear peak in this zone");
    case BeltTuneOutcome::OUT_OF_WINDOW:
        return fmt::format(
            fmt::runtime(lv_tr("The peak of {} moved out of the zone. Widening it.")), adj_name);
    case BeltTuneOutcome::IMPLAUSIBLE:
        return lv_tr("That reading jumped too far to trust. Re-measure without turning.");
    case BeltTuneOutcome::MATCHED:
    case BeltTuneOutcome::BASELINE:
    case BeltTuneOutcome::UNREPEATABLE:
        break;
    }
    return {};
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
    // "Connected" has to mean commands will actually run: Moonraker up but
    // klippy down (an emergency stop, a crash) refuses gcode, so a sweep
    // started then would stall until the guard fired.
    in.connected = lv_subject_get_int(ps.network_state().get_nav_buttons_enabled_subject()) != 0 &&
                   lv_subject_get_int(ps.network_state().get_klippy_state_subject()) ==
                       static_cast<int>(KlippyState::READY);
    in.has_accelerometer = accel_subj && lv_subject_get_int(accel_subj) != 0;
    in.is_corexy = detected_hw_.kinematics == helix::calibration::KinematicsType::COREXY;
    in.detecting = detection_pending_ &&
                   detected_hw_.kinematics == helix::calibration::KinematicsType::UNKNOWN;
    in.klippy_socket_reachable = klippy_socket_reachable_;
    in.print_active = lv_subject_get_int(ps.print_state().get_print_active_subject()) != 0;

    const auto gate = helix::calibration::evaluate_belt_gate(in);
    const bool calibrator_idle =
        !calibrator_ ||
        calibrator_->get_state() == helix::calibration::BeltTensionCalibrator::State::IDLE;
    lv_subject_set_int(&can_start_subject_,
                       gate == helix::calibration::BeltGate::OK && calibrator_idle ? 1 : 0);
    if (gate == helix::calibration::BeltGate::NOT_COREXY && !detected_hw_.kinematics_name.empty()) {
        // Name what the printer is, so the user can tell a wrong machine from
        // a missing detection.
        lv_subject_copy_string(&gate_message_subject_,
                               fmt::format(lv_tr("Belt Tension needs a CoreXY printer. "
                                                 "This one is {}."),
                                           detected_hw_.kinematics_name)
                                   .c_str());
    } else if (gate == helix::calibration::BeltGate::OK && calibrator_ &&
               calibrator_->get_state() ==
                   helix::calibration::BeltTensionCalibrator::State::BUZZING) {
        lv_subject_copy_string(&gate_message_subject_, lv_tr("Wait for the motor to stop"));
    } else {
        lv_subject_copy_string(&gate_message_subject_,
                               lv_tr(helix::calibration::belt_gate_message(gate)));
    }
    spdlog::debug("[BeltTension] gate = {}", helix::calibration::belt_gate_message(gate));

    // A precondition that fails mid-measurement ends the measurement. The case
    // that makes this real is a print starting while the user is measuring: the
    // toolhead is now doing two jobs at once, and every later reading is
    // garbage from both.
    if (gate != helix::calibration::BeltGate::OK && measuring()) {
        spdlog::warn("[BeltTension] Gate closed mid-measurement: {}",
                     helix::calibration::belt_gate_message(gate));
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

    accel_observer_ = helix::ui::observe<int>(
        accel_subj, this, [](BeltTensionPanel* self, int) { self->refresh_gate(); },
        ps.get_subjects_lifetime());
    print_active_observer_ = helix::ui::observe<int>(
        ps.print_state().get_print_active_subject(), this,
        [](BeltTensionPanel* self, int) { self->refresh_gate(); }, ps.get_subjects_lifetime());
    connected_observer_ = helix::ui::observe<int>(
        ps.network_state().get_nav_buttons_enabled_subject(), this,
        [](BeltTensionPanel* self, int) { self->refresh_gate(); }, ps.get_subjects_lifetime());
    klippy_observer_ = helix::ui::observe<int>(
        ps.network_state().get_klippy_state_subject(), this,
        [](BeltTensionPanel* self, int) { self->refresh_gate(); }, ps.get_subjects_lifetime());
    // The laps' share of the duration line needs max_velocity and the axis
    // limits, which can land after the [resonance_tester] answer. Both arrive
    // in one toolhead frame and max_velocity is applied first, so the refresh
    // waits for the frame to finish.
    max_velocity_observer_ = helix::ui::observe<int>(
        ps.motion_state().get_max_velocity_subject(), this,
        [](BeltTensionPanel* self, int) {
            self->lifetime_.defer("BeltTension::hw_sweep", [self]() { self->refresh_hw_sweep(); });
        },
        ps.get_subjects_lifetime());

    gate_observers_wired_ = true;
    spdlog::debug("[BeltTension] Gate observers attached");
}

void BeltTensionPanel::probe_klippy_socket() {
    if (!api_) {
        klippy_socket_reachable_ = false;
        refresh_gate();
        return;
    }

    helix::calibration::query_klippy_uds_address(
        *api_, lifetime_.bg_cb("BeltTension::server_config", [this](std::string path) {
            klippy_socket_path_ = std::move(path);
            klippy_socket_reachable_ =
                !klippy_socket_path_.empty() &&
                helix::calibration::BeltStreamClient::socket_reachable(klippy_socket_path_);
            spdlog::debug("[BeltTension] klippy uds '{}' reachable={}", klippy_socket_path_,
                          klippy_socket_reachable_);
            refresh_gate();
        }));
}

void BeltTensionPanel::query_hw_facts() {
    if (!client_) {
        return;
    }

    helix::calibration::query_resonance_tester_config(
        *client_, lifetime_.bg_cb("BeltTension::resonance_cfg",
                                  [this](helix::calibration::ResonanceTesterConfig cfg) {
                                      // A failed query answers with defaults, which
                                      // never replace a range the printer reported.
                                      if (cfg.from_printer || !sweep_cfg_.from_printer) {
                                          set_sweep_config(cfg);
                                      }
                                      hw_sweep_known_ = true;
                                      push_chart_data();
                                      refresh_hw_sweep();
                                  }));
}

void BeltTensionPanel::set_sweep_config(const helix::calibration::ResonanceTesterConfig& cfg) {
    sweep_cfg_ = cfg;
    if (subjects_initialized_) {
        lv_subject_set_int(&tune_range_known_subject_, cfg.from_printer ? 1 : 0);
    }
}

void BeltTensionPanel::refresh_hw_sweep() {
    if (!hw_sweep_known_ || !subjects_initialized_) {
        return;
    }
    snprintf(hw_sweep_buf_, sizeof(hw_sweep_buf_), "%s",
             fmt::format(lv_tr("{:.0f}-{:.0f} Hz · about {} min"), sweep_cfg_.min_freq,
                         sweep_cfg_.max_freq, full_check_minutes())
                 .c_str());
    lv_subject_copy_string(&hw_sweep_subject_, hw_sweep_buf_);
}

// ============================================================================
// CHART
// ============================================================================

lv_color_t BeltTensionPanel::path_color(helix::calibration::BeltPath path) {
    return theme_manager_get_color(path == helix::calibration::BeltPath::PATH_A ? "belt_path_a"
                                                                                : "belt_path_b");
}

ui_frequency_response_chart_t* BeltTensionPanel::ensure_chart() {
    const auto tier = tier_override_.value_or(PlatformCapabilities::detect().tier);
    if (tier == helix::PlatformTier::EMBEDDED || chart_) {
        return chart_;
    }
    if (!chart_host_running_) {
        return nullptr;
    }

    chart_ = ui_frequency_response_chart_create(chart_host_running_);
    if (!chart_) {
        return nullptr;
    }
    ui_frequency_response_chart_configure_for_platform(chart_, tier, tier_animations_);
    // Relative response: both curves share one 0-100% scale (push_chart_data).
    ui_frequency_response_chart_set_y_labels_visible(chart_, true);
    ui_frequency_response_chart_set_y_labels_percent(chart_, true);

    // Series names appear only in the chart's logs, so they keep the path letter.
    for (int idx = 0; idx < 2; ++idx) {
        const char letter = 'A' + idx;
        series_[idx] = ui_frequency_response_chart_add_series(
            chart_, fmt::format("Path {}", letter).c_str(), path_color(path_at(idx)));
        ui_frequency_response_chart_set_series_style(chart_, series_[idx], kFrMeasuredCurveStyle);

        // The preview stands in for the path's own curve, so it carries its name.
        preview_series_[idx] = ui_frequency_response_chart_add_series(
            chart_, fmt::format("Path {} preview", letter).c_str(), path_color(path_at(idx)));
        ui_frequency_response_chart_set_series_style(chart_, preview_series_[idx],
                                                     kFrMeasuredCurveStyle);
        ui_frequency_response_chart_show_series(chart_, preview_series_[idx], false);

        ghost_series_[idx] = ui_frequency_response_chart_add_series(
            chart_, fmt::format("Path {} before", letter).c_str(), path_color(path_at(idx)));
        ui_frequency_response_chart_set_series_muted(chart_, ghost_series_[idx], true);
        ui_frequency_response_chart_show_series(chart_, ghost_series_[idx], false);

        for (int& id : tune_ghost_series_[idx]) {
            id = ui_frequency_response_chart_add_series(
                chart_, fmt::format("Path {} earlier round", letter).c_str(),
                path_color(path_at(idx)));
            ui_frequency_response_chart_set_series_muted(chart_, id, true);
            ui_frequency_response_chart_show_series(chart_, id, false);
        }
    }

    push_chart_data();
    lv_subject_set_int(&chart_available_subject_, 1);
    spdlog::debug("[BeltTension] Chart created (tier {})", helix::platform_tier_to_string(tier));
    return chart_;
}

void BeltTensionPanel::apply_path_colors() {
    if (!chart_) {
        return;
    }
    for (int idx = 0; idx < 2; ++idx) {
        const lv_color_t color = path_color(path_at(idx));
        ui_frequency_response_chart_set_series_color(chart_, series_[idx], color);
        ui_frequency_response_chart_set_series_color(chart_, ghost_series_[idx], color);
        ui_frequency_response_chart_set_series_color(chart_, preview_series_[idx], color);
        for (const int id : tune_ghost_series_[idx]) {
            ui_frequency_response_chart_set_series_color(chart_, id, color);
        }
    }
}

void BeltTensionPanel::push_chart_data() {
    if (!chart_) {
        return;
    }
    if (tune_ &&
        static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::TUNING) {
        push_tune_chart_data();
        return;
    }
    for (const auto& ids : tune_ghost_series_) {
        for (const int id : ids) {
            ui_frequency_response_chart_show_series(chart_, id, false);
        }
    }
    const float lo = sweep_cfg_.min_freq;
    const float hi = sweep_cfg_.max_freq;
    const auto in_band = [lo, hi](const helix::calibration::BeltCurve& curve) {
        return helix::calibration::curve_in_band(curve, lo, hi);
    };

    // The chart shows the sweep band only (bins past the ceiling carry no
    // excitation) as a percentage: the two current curves share the scale of
    // the taller, so their heights compare, and each ghost has its own, so a
    // quieter earlier sweep still shows its shape.
    helix::calibration::BeltCurve current[2], previous[2];
    float tallest = 0.0f;
    for (int idx = 0; idx < 2; ++idx) {
        if (runs_[idx].has) {
            current[idx] = in_band(runs_[idx].curve);
        }
        if (runs_[idx].has_previous) {
            previous[idx] = in_band(runs_[idx].previous);
        }
        tallest = std::max(tallest, tallest_bin(current[idx]));
    }

    // Points are placed by index across the plot, so the axis must span
    // exactly the data's first and last bin; with no data yet the printer's
    // band keeps the running cursor honest.
    float f_lo = lo;
    float f_hi = hi;
    for (const auto* curve : {&current[0], &current[1], &previous[0], &previous[1]}) {
        if (!curve->empty()) {
            f_lo = curve->front().first;
            f_hi = curve->back().first;
            break;
        }
    }
    ui_frequency_response_chart_set_freq_range(chart_, f_lo, f_hi);
    ui_frequency_response_chart_set_amplitude_range(chart_, 0.0f, 100.0f);

    for (int idx = 0; idx < 2; ++idx) {
        push_series_data(series_[idx], current[idx], tallest);
        push_series_data(ghost_series_[idx], previous[idx], tallest_bin(previous[idx]));
    }
    refresh_preview_series();
}

void BeltTensionPanel::push_series_data(int series_id, const helix::calibration::BeltCurve& curve,
                                        float tallest) {
    if (curve.empty() || tallest <= 0.0f) {
        ui_frequency_response_chart_show_series(chart_, series_id, false);
        return;
    }
    std::vector<float> freqs, amps;
    freqs.reserve(curve.size());
    amps.reserve(curve.size());
    for (const auto& [f, a] : curve) {
        freqs.push_back(f);
        amps.push_back(100.0f * a / tallest);
    }
    ui_frequency_response_chart_set_data(chart_, series_id, freqs.data(), amps.data(),
                                         freqs.size());
    ui_frequency_response_chart_show_series(chart_, series_id, true);
}

void BeltTensionPanel::push_tune_chart_data() {
    // The in-plane curves tuning reads: over the band until a zone is known,
    // then over the zone (the analysed bins, not the wider sweep).
    const auto& zone = tune_->zone();
    const float lo = zone ? zone->window.freq_start_hz : sweep_cfg_.min_freq;
    const float hi = zone ? zone->window.freq_end_hz : sweep_cfg_.max_freq;
    const int adj = path_index(tune_->adjusted_path());

    helix::calibration::BeltCurve current[2];
    float tallest = 0.0f;
    for (int idx = 0; idx < 2; ++idx) {
        const helix::calibration::BeltResponse round0{runs_[idx].curve, runs_[idx].in_plane};
        current[idx] = helix::calibration::curve_in_band(
            helix::calibration::tuning_curve(tune_has_curves_ ? tune_curves_[idx] : round0), lo,
            hi);
        tallest = std::max(tallest, tallest_bin(current[idx]));
    }
    // Earlier rounds are ghosts on their own scale, as on RESULTS.
    std::vector<helix::calibration::BeltCurve> earlier;
    for (const auto& curve : tune_adj_history_) {
        earlier.push_back(helix::calibration::curve_in_band(curve, lo, hi));
    }

    // Points are placed by index, so the axis spans the data's own bins.
    float f_lo = lo;
    float f_hi = hi;
    for (const auto& curve : current) {
        if (!curve.empty()) {
            f_lo = curve.front().first;
            f_hi = curve.back().first;
            break;
        }
    }
    ui_frequency_response_chart_set_freq_range(chart_, f_lo, f_hi);
    ui_frequency_response_chart_set_amplitude_range(chart_, 0.0f, 100.0f);

    for (int idx = 0; idx < 2; ++idx) {
        push_series_data(series_[idx], current[idx], tallest);
        ui_frequency_response_chart_show_series(chart_, ghost_series_[idx], false);
        for (size_t k = 0; k < 2; ++k) {
            const int id = tune_ghost_series_[idx][k];
            if (idx == adj && k < earlier.size()) {
                push_series_data(id, earlier[k], tallest_bin(earlier[k]));
            } else {
                ui_frequency_response_chart_show_series(chart_, id, false);
            }
        }
    }

    // Hollow rings at the centroids the session steers by; none once nothing is left to tune.
    const bool marked = tune_->stage() != helix::calibration::BeltTuneStage::NO_ZONE;
    const FrChartMarker ref_mark{tune_->reference_hz(), 0};
    const FrChartMarker adj_mark{tune_->adjusted_hz(), 0};
    ui_frequency_response_chart_set_markers(chart_, series_[1 - adj], marked ? &ref_mark : nullptr,
                                            marked ? 1 : 0);
    ui_frequency_response_chart_set_markers(chart_, series_[adj], marked ? &adj_mark : nullptr,
                                            marked ? 1 : 0);
    refresh_preview_series();
}

void BeltTensionPanel::arm_preview(helix::calibration::BeltPath path) {
    if (!chart_) {
        return;
    }
    const auto target = helix::calibration::live_sweep_target(
        {klippy_socket_path_, klippy_socket_reachable_, sweep_cfg_},
        helix::calibration::SweepAxis::DIAGONAL, run_window_);
    if (!target) {
        spdlog::debug("[BeltTension] No live preview for path {}: socket '{}' reachable={}",
                      path_letter(path), klippy_socket_path_, klippy_socket_reachable_);
        return;
    }
    preview_.arm(*target, [this, path](const std::vector<helix::calibration::SweepPoint>& points) {
        on_preview_points(path, points);
    });
}

void BeltTensionPanel::on_preview_points(
    helix::calibration::BeltPath path, const std::vector<helix::calibration::SweepPoint>& points) {
    const int idx = path_index(path);
    preview_points_[idx] = points;
    if (std::any_of(points.begin(), points.end(),
                    [](const helix::calibration::SweepPoint& p) { return p.clean; })) {
        preview_live_[idx] = true;
    }
    refresh_preview_series();
}

void BeltTensionPanel::refresh_preview_series() {
    if (!chart_) {
        return;
    }
    const auto [lo, hi] = ui_frequency_response_chart_get_freq_range(chart_);
    // The column the curve that replaces the preview draws: a full check's
    // psd_xyz, the tuning loop's in-plane sum.
    const auto column = run_kind_ == RunKind::CHECK ? helix::calibration::SweepColumn::XYZ
                                                    : helix::calibration::SweepColumn::IN_PLANE;
    for (int idx = 0; idx < 2; ++idx) {
        if (!preview_live_[idx]) {
            ui_frequency_response_chart_show_series(chart_, preview_series_[idx], false);
            continue;
        }
        const auto laid = helix::calibration::layout_preview(preview_points_[idx], column, lo, hi);
        helix::calibration::BeltCurve curve;
        curve.reserve(laid.freqs.size());
        for (size_t i = 0; i < laid.freqs.size(); ++i) {
            curve.emplace_back(laid.freqs[i], laid.amps[i]);
        }
        // Each preview reads 0-100% of its own tallest point.
        push_series_data(preview_series_[idx], curve, laid.max_amp);
        if (laid.max_amp > 0.0f) {
            ui_frequency_response_chart_show_series(chart_, series_[idx], false);
        }
    }
}

void BeltTensionPanel::clear_previews() {
    preview_.stop();
    for (int idx = 0; idx < 2; ++idx) {
        preview_points_[idx].clear();
        preview_live_[idx] = false;
        if (chart_) {
            ui_frequency_response_chart_show_series(chart_, preview_series_[idx], false);
        }
    }
}

void BeltTensionPanel::destroy_chart() {
    if (!chart_) {
        return;
    }
    ui_frequency_response_chart_destroy(chart_);
    chart_ = nullptr;
    series_[0] = series_[1] = -1;
    ghost_series_[0] = ghost_series_[1] = -1;
    preview_series_[0] = preview_series_[1] = -1;
    for (auto& ids : tune_ghost_series_) {
        ids[0] = ids[1] = -1;
    }
    lv_subject_set_int(&chart_available_subject_, 0);
}

void BeltTensionPanel::chart_to_running_host() {
    ensure_chart();
    place_chart(ViewState::RUNNING);
}
