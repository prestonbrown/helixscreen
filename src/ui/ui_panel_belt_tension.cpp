// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_belt_tension.h"

#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_timer_guard.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "belt_stream_client.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "memory_utils.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "resonance_console.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace helix;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

// State subject (0=START, 1=RUNNING, 2=RESULTS, 3=ERROR)
static lv_subject_t s_belt_tension_state;

BeltTensionPanel::~BeltTensionPanel() {
    // lifetime_'s destructor auto-invalidates all outstanding tokens.

    stall_guard_.end();

    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    klippy_observer_.reset();
    gate_observers_wired_ = false;

    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clear widget pointers (owned by LVGL)
    overlay_root_ = nullptr;
    chart_host_running_ = nullptr;
    chart_host_results_ = nullptr;
}

void init_belt_tension_row_handler() {
    register_xml_callbacks({
        {"on_belt_tension_row_clicked",
         [](lv_event_t*) {
             spdlog::debug("[BeltTension] Belt Tension row clicked");
             auto& panel = get_global_belt_tension_panel();
             panel.set_api(get_moonraker_client(), get_moonraker_api());
             panel.show(lv_display_get_screen_active(nullptr));
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
        {"belt_tension_retest_a_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_retest_clicked(calibration::BeltPath::PATH_A);
         }},
        {"belt_tension_retest_b_cb",
         [](lv_event_t* /*e*/) {
             get_global_belt_tension_panel().handle_retest_clicked(calibration::BeltPath::PATH_B);
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

    // Results
    UI_MANAGED_SUBJECT_STRING(note_a_subject_, note_a_buf_, "", "bt_note_a", subjects_);
    UI_MANAGED_SUBJECT_STRING(note_b_subject_, note_b_buf_, "", "bt_note_b", subjects_);
    UI_MANAGED_SUBJECT_INT(verdict_subject_, 0, "bt_verdict", subjects_);
    UI_MANAGED_SUBJECT_STRING(verdict_text_subject_, verdict_text_buf_, "", "bt_verdict_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(similarity_subject_, similarity_buf_, "--", "bt_similarity",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(facts_subject_, facts_buf_, "", "bt_facts", subjects_);
    UI_MANAGED_SUBJECT_STRING(unpaired_subject_, unpaired_buf_, "", "bt_unpaired", subjects_);
    UI_MANAGED_SUBJECT_INT(has_unpaired_subject_, 0, "bt_has_unpaired", subjects_);
    UI_MANAGED_SUBJECT_INT(chart_available_subject_, 0, "bt_chart_available", subjects_);

    // Error
    UI_MANAGED_SUBJECT_STRING(error_message_subject_, error_message_buf_, "", "bt_error_message",
                              subjects_);

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "BeltTensionPanel", []() { get_global_belt_tension_panel().deinit_subjects(); });
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

    set_view_state(ViewState::START);
    refresh_notes();

    // Re-evaluate the gate on every entry, and probe co-location once here
    // rather than on each gate refresh - the gate recomputes on every subject
    // change and a connect() syscall per change would be waste.
    ensure_gate_observers();
    refresh_gate();
    // theme_changed is a file-static theme global, deinited only after LVGL is gone.
    theme_observer_ = helix::ui::observe<int>(
        theme_manager_get_changed_subject(), this,
        [](BeltTensionPanel* self, int) { self->apply_path_colors(); }, subject_never_freed());
    probe_klippy_socket();
    query_hw_facts();

    // Detect hardware capabilities
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

    // An idle screen is not a walk-away. A sweep means nobody is touching the
    // screen, so the screensaver lands mid-measurement; the run has to survive
    // it.
    if (reason == DeactivateReason::Suspended) {
        return;
    }
    theme_observer_.reset();

    // A rebuild frees the whole widget tree without firing on_ui_destroyed(),
    // and the chart's object is a child of a host inside that tree. Drop it
    // here while the tree is still alive; create() re-caches the hosts and the
    // next run rebuilds the chart lazily.
    if (reason == DeactivateReason::Rebuild) {
        on_ui_destroyed();
    }

    // Abandon an in-progress run. The printer keeps sweeping (stopping it is
    // what Stop is for), but this panel stops listening and returns to START.
    if (static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::RUNNING) {
        spdlog::info("[BeltTension] Cancelling measurement on deactivate");
    }
    cancel_run();
    set_view_state(ViewState::START);
}

void BeltTensionPanel::cleanup() {
    spdlog::debug("[BeltTension] Cleaning up");

    cancel_run();

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // ObserverGuard::reset(), never release() (#579)
    accel_observer_.reset();
    print_active_observer_.reset();
    connected_observer_.reset();
    klippy_observer_.reset();
    theme_observer_.reset();
    gate_observers_wired_ = false;

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
}

// ============================================================================
// STATE MANAGEMENT
// ============================================================================

void BeltTensionPanel::set_view_state(ViewState state) {
    spdlog::debug("[BeltTension] View state change: {} -> {}",
                  lv_subject_get_int(&s_belt_tension_state), static_cast<int>(state));

    // Update subject - XML bindings handle visibility automatically
    lv_subject_set_int(&s_belt_tension_state, static_cast<int>(state));

    if (state == ViewState::RESULTS) {
        if (chart_ && chart_host_results_) {
            lv_obj_set_parent(ui_frequency_response_chart_get_obj(chart_), chart_host_results_);
        }
    } else if (state == ViewState::RUNNING) {
        if (chart_ && chart_host_running_) {
            lv_obj_set_parent(ui_frequency_response_chart_get_obj(chart_), chart_host_running_);
        }
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

    // The XML binding already disables this button while the gate is shut. This
    // is the same check on the action itself, so a stale binding or a
    // programmatic click cannot get past it.
    refresh_gate();
    if (lv_subject_get_int(&can_start_subject_) == 0) {
        spdlog::warn("[BeltTension] Start refused: {}",
                     lv_subject_get_string(&gate_message_subject_));
        return;
    }

    run_after_ram_check([this]() {
        begin_run({helix::calibration::BeltPath::PATH_A, helix::calibration::BeltPath::PATH_B},
                  /*clear_previous=*/true);
    });
}

void BeltTensionPanel::handle_retest_clicked(helix::calibration::BeltPath path) {
    spdlog::info("[BeltTension] Re-test clicked for path {}",
                 helix::calibration::BeltTensionCalibrator::output_name(path));

    run_after_ram_check([this, path]() {
        const int idx = path == helix::calibration::BeltPath::PATH_A ? 0 : 1;
        auto& run = runs_[idx];

        // The curve being replaced becomes the ghost this run compares against,
        // and the similarity the last comparison showed becomes the note's "was".
        run.previous = std::move(run.curve);
        run.curve.clear();
        run.has_previous = run.has;
        run.has = false;
        was_similarity_percent_ = similarity_percent_;

        begin_run({path}, /*clear_previous=*/false);
    });
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
    // analysis can run out of memory and leave Klipper stuck.
    if (low_ram_dialog_) {
        return; // the warning is already up
    }
    helix::ui::ConfirmOptions opts;
    opts.owner_token = lifetime_.token();
    helix::ui::show_low_ram_resonance_warning(mem.total_mb(), &low_ram_dialog_, go, opts);
    if (!low_ram_dialog_) {
        go(); // the modal failed to build; do not silently block the check
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
                                 bool clear_previous) {
    if (queue.empty()) {
        return;
    }

    if (clear_previous) {
        for (auto& run : runs_) {
            run.previous.clear();
            run.has_previous = false;
        }
        was_similarity_percent_ = 0.0f;
    }

    // Markers belong to the comparison being replaced.
    if (chart_) {
        for (int idx = 0; idx < 2; ++idx) {
            ui_frequency_response_chart_set_markers(chart_, series_[idx], nullptr, 0);
        }
    }

    queue_ = queue;
    run_queue_total_ = queue_.size();
    run_started_ms_ = lv_tick_get();
    run_active_ = true;

    chart_to_running_host();
    refresh_notes();
    set_view_state(ViewState::RUNNING);
    start_elapsed_timer();
    start_next_measurement();
}

void BeltTensionPanel::start_next_measurement() {
    if (queue_.empty()) {
        finish_run();
        return;
    }

    const auto path = queue_.front();
    const int idx = path == helix::calibration::BeltPath::PATH_A ? 0 : 1;
    lv_subject_set_int(&running_path_subject_, idx);
    // A single-path run is a re-test: the path already has a result.
    const char* title =
        run_queue_total_ == 1 ? lv_tr("Re-measuring Path {}") : lv_tr("Measuring Path {}");
    lv_subject_copy_string(&run_title_subject_,
                           fmt::format(fmt::runtime(title), static_cast<char>('A' + idx)).c_str());
    refresh_run_detail();
    refresh_notes();

    // One guard spans the whole queue: every progress line re-arms it, so it
    // measures silence, not sweep length.
    stall_guard_.begin(STALL_TIMEOUT_MS, [this]() { on_stall(); });

    if (!calibrator_) {
        on_error(lv_tr("No printer connection"));
        return;
    }

    calibrator_->measure_path(
        path, [this](int percent, float freq_hz) { on_sweep_progress(percent, freq_hz); },
        [this, path](helix::calibration::BeltCurve curve) {
            on_sweep_complete(path, std::move(curve));
        },
        [this](const std::string& message) { on_sweep_error(message); });
}

void BeltTensionPanel::on_sweep_progress(int /*percent*/, float freq_hz) {
    if (!run_active_) {
        return; // a cancelled run's transcript is stale the moment it lands
    }
    // Re-arm on every line: the guard fires only on true silence.
    stall_guard_.end();
    stall_guard_.begin(STALL_TIMEOUT_MS, [this]() { on_stall(); });

    if (chart_) {
        ui_frequency_response_chart_set_cursor(chart_, freq_hz, path_color(queue_.front()));
    }
    refresh_run_detail();
}

void BeltTensionPanel::on_sweep_complete(helix::calibration::BeltPath path,
                                         helix::calibration::BeltCurve curve) {
    if (!run_active_) {
        return; // a cancelled run's transcript is stale the moment it lands
    }
    stall_guard_.end();

    auto& run = runs_[path == helix::calibration::BeltPath::PATH_A ? 0 : 1];
    run.curve = std::move(curve);
    run.has = true;
    run.measured_at_ms = lv_tick_get();

    if (chart_) {
        ui_frequency_response_chart_clear_cursor(chart_);
    }

    if (!queue_.empty() && queue_.front() == path) {
        queue_.erase(queue_.begin());
    }

    refresh_notes();

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
    spdlog::warn("[BeltTension] Stall guard fired: no progress for {} ms", STALL_TIMEOUT_MS);
    on_error(lv_tr("Klipper stopped reporting progress. Its analysis can run out of memory on "
                   "small printers and leave Klipper stuck, where a firmware restart cannot "
                   "reach it. Power-cycle the printer (or restart Klipper over SSH), then try "
                   "again."));
}

void BeltTensionPanel::finish_run() {
    elapsed_timer_.reset();

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

    populate_results(cmp);
    set_view_state(ViewState::RESULTS);
    spdlog::info("[BeltTension] Results: similarity={:.0f}% verdict={} pairs={} unpaired={}/{}",
                 cmp.similarity_percent, static_cast<int>(cmp.verdict), cmp.peaks.pairs.size(),
                 cmp.peaks.unpaired_a.size(), cmp.peaks.unpaired_b.size());
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

    // Paired peaks, strongest first: "Peaks 35/36 · 133/132 Hz" (A/B).
    const auto& pairs = cmp.peaks.pairs;
    std::string pair_list;
    for (size_t i = 0; i < pairs.size() && i < MAX_LISTED_PEAKS; ++i) {
        pair_list += fmt::format("{}{:.0f}/{:.0f}", i == 0 ? "" : " · ", pairs[i].a.freq_hz,
                                 pairs[i].b.freq_hz);
    }
    const std::string facts = pair_list.empty()
                                  ? std::string(lv_tr("No shared peaks"))
                                  : fmt::format(fmt::runtime(lv_tr("Peaks {} Hz")), pair_list);
    snprintf(facts_buf_, sizeof(facts_buf_), "%s", facts.c_str());
    lv_subject_copy_string(&facts_subject_, facts_buf_);

    // Unpaired peaks, the strongest few per path, in frequency order:
    // "Only on A: 120, 129 Hz".
    const auto listed = [](std::vector<helix::calibration::BeltPeak> peaks) {
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto& l, const auto& r) { return l.amplitude > r.amplitude; });
        if (peaks.size() > MAX_LISTED_PEAKS) {
            peaks.resize(MAX_LISTED_PEAKS);
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
    const auto only_a = listed(cmp.peaks.unpaired_a);
    const auto only_b = listed(cmp.peaks.unpaired_b);
    std::string unpaired;
    if (!only_a.empty()) {
        unpaired = fmt::format(fmt::runtime(lv_tr("Only on A: {} Hz")), join_freqs(only_a));
    }
    if (!only_b.empty()) {
        unpaired += (unpaired.empty() ? "" : " · ") +
                    fmt::format(fmt::runtime(lv_tr("Only on B: {} Hz")), join_freqs(only_b));
    }
    snprintf(unpaired_buf_, sizeof(unpaired_buf_), "%s", unpaired.c_str());
    lv_subject_copy_string(&unpaired_subject_, unpaired_buf_);
    lv_subject_set_int(&has_unpaired_subject_, unpaired.empty() ? 0 : 1);

    push_chart_markers(cmp);
    refresh_notes();
}

void BeltTensionPanel::push_chart_markers(const helix::calibration::BeltComparison& cmp) {
    if (!chart_) {
        return;
    }
    // Numbered dots on the listed pairs (the same number on both curves),
    // hollow rings on the listed unpaired peaks.
    std::vector<FrChartMarker> markers[2];
    const auto& pairs = cmp.peaks.pairs;
    for (size_t i = 0; i < pairs.size() && i < MAX_LISTED_PEAKS; ++i) {
        const int number = static_cast<int>(i) + 1;
        markers[0].push_back({pairs[i].a.freq_hz, number});
        markers[1].push_back({pairs[i].b.freq_hz, number});
    }
    const auto add_unpaired = [](std::vector<helix::calibration::BeltPeak> peaks,
                                 std::vector<FrChartMarker>& out) {
        std::sort(peaks.begin(), peaks.end(),
                  [](const auto& l, const auto& r) { return l.amplitude > r.amplitude; });
        for (size_t i = 0; i < peaks.size() && i < MAX_LISTED_PEAKS; ++i) {
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
    if (calibrator_) {
        calibrator_->cancel();
    }
    queue_.clear();
    stall_guard_.end();
    elapsed_timer_.reset();
    if (chart_) {
        ui_frequency_response_chart_clear_cursor(chart_);
    }
}

void BeltTensionPanel::back_to_start() {
    cancel_run();
    refresh_gate();
    set_view_state(ViewState::START);
}

// ============================================================================
// SUBJECT REFRESH
// ============================================================================

void BeltTensionPanel::refresh_notes() {
    const bool running =
        static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::RUNNING;

    for (int idx = 0; idx < 2; ++idx) {
        const auto& run = runs_[idx];
        auto& note_subject = idx == 0 ? note_a_subject_ : note_b_subject_;
        auto& note_buf = idx == 0 ? note_a_buf_ : note_b_buf_;

        std::string note = "--";
        if (running && !queue_.empty() &&
            (queue_.front() == (idx == 0 ? helix::calibration::BeltPath::PATH_A
                                         : helix::calibration::BeltPath::PATH_B))) {
            note = lv_tr("sweeping");
        } else if (run.has) {
            const uint32_t age_s = (lv_tick_get() - run.measured_at_ms) / 1000;
            note = age_s < 60 ? lv_tr("just now")
                              : fmt::format(fmt::runtime(lv_tr("{} min ago")), age_s / 60);
        }
        if (run.has_previous) {
            note += fmt::format(fmt::runtime(lv_tr(" · was {:.0f}%")), was_similarity_percent_);
        }

        snprintf(note_buf, idx == 0 ? sizeof(note_a_buf_) : sizeof(note_b_buf_), "%s",
                 note.c_str());
        lv_subject_copy_string(&note_subject, note_buf);
    }
    push_chart_data();
}

void BeltTensionPanel::refresh_run_detail() {
    if (run_queue_total_ > 1) {
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
    } else {
        lv_subject_copy_string(&gate_message_subject_,
                               lv_tr(helix::calibration::belt_gate_message(gate)));
    }
    spdlog::debug("[BeltTension] gate = {}", helix::calibration::belt_gate_message(gate));

    // A precondition that fails mid-measurement ends the measurement. The case
    // that makes this real is a print starting while the user is measuring: the
    // toolhead is now doing two jobs at once, and every later reading is
    // garbage from both.
    if (gate != helix::calibration::BeltGate::OK &&
        static_cast<ViewState>(lv_subject_get_int(&s_belt_tension_state)) == ViewState::RUNNING) {
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

void BeltTensionPanel::query_hw_facts() {
    if (!client_) {
        return;
    }

    helix::calibration::query_resonance_tester_config(
        *client_,
        lifetime_.bg_cb(
            "BeltTension::resonance_cfg", [this](helix::calibration::ResonanceTesterConfig cfg) {
                sweep_cfg_ = cfg;
                push_chart_data();
                // Both paths sweep once each, hence the 2x.
                const int minutes =
                    std::max(1, static_cast<int>(std::ceil(2.0f * cfg.sweep_seconds() / 60.0f)));
                snprintf(hw_sweep_buf_, sizeof(hw_sweep_buf_), "%s",
                         fmt::format(lv_tr("{:.0f}-{:.0f} Hz · about {} min"), cfg.min_freq,
                                     cfg.max_freq, minutes)
                             .c_str());
                lv_subject_copy_string(&hw_sweep_subject_, hw_sweep_buf_);
            }));
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

    for (int idx = 0; idx < 2; ++idx) {
        const char letter = 'A' + idx;
        series_[idx] = ui_frequency_response_chart_add_series(
            chart_, fmt::format(lv_tr("Path {}"), letter).c_str(),
            path_color(idx == 0 ? helix::calibration::BeltPath::PATH_A
                                : helix::calibration::BeltPath::PATH_B));
        ui_frequency_response_chart_set_series_style(chart_, series_[idx], {3, true, true});

        ghost_series_[idx] = ui_frequency_response_chart_add_series(
            chart_, fmt::format(lv_tr("Path {} before"), letter).c_str(),
            path_color(idx == 0 ? helix::calibration::BeltPath::PATH_A
                                : helix::calibration::BeltPath::PATH_B));
        ui_frequency_response_chart_set_series_muted(chart_, ghost_series_[idx], true);
        ui_frequency_response_chart_show_series(chart_, ghost_series_[idx], false);
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
        const lv_color_t color = path_color(idx == 0 ? helix::calibration::BeltPath::PATH_A
                                                     : helix::calibration::BeltPath::PATH_B);
        ui_frequency_response_chart_set_series_color(chart_, series_[idx], color);
        ui_frequency_response_chart_set_series_color(chart_, ghost_series_[idx], color);
    }
}

void BeltTensionPanel::push_chart_data() {
    if (!chart_) {
        return;
    }
    const float lo = sweep_cfg_.min_freq;
    const float hi = sweep_cfg_.max_freq;
    const auto in_band = [lo, hi](const helix::calibration::BeltCurve& curve) {
        helix::calibration::BeltCurve out;
        for (const auto& bin : curve) {
            if (bin.first >= lo && bin.first <= hi) {
                out.push_back(bin);
            }
        }
        return out;
    };

    // The chart shows the sweep band only (bins past the ceiling carry no
    // excitation) as a percentage of the tallest in-band point of any curve on
    // screen, so the two paths share one scale and the axis reads 0-100%.
    helix::calibration::BeltCurve current[2], previous[2];
    float tallest = 0.0f;
    for (int idx = 0; idx < 2; ++idx) {
        if (runs_[idx].has) {
            current[idx] = in_band(runs_[idx].curve);
        }
        if (runs_[idx].has_previous) {
            previous[idx] = in_band(runs_[idx].previous);
        }
        for (const auto* curve : {&current[idx], &previous[idx]}) {
            for (const auto& bin : *curve) {
                tallest = std::max(tallest, bin.second);
            }
        }
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

    const auto push = [this, tallest](int series_id, const helix::calibration::BeltCurve& curve) {
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
    };
    for (int idx = 0; idx < 2; ++idx) {
        push(series_[idx], current[idx]);
        push(ghost_series_[idx], previous[idx]);
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
    lv_subject_set_int(&chart_available_subject_, 0);
}

void BeltTensionPanel::chart_to_running_host() {
    ensure_chart();
    if (chart_ && chart_host_running_ &&
        lv_obj_get_parent(ui_frequency_response_chart_get_obj(chart_)) != chart_host_running_) {
        lv_obj_set_parent(ui_frequency_response_chart_get_obj(chart_), chart_host_running_);
    }
}
