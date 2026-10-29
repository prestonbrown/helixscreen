// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_collector.h"

#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "action_prompt_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "print_start_analyzer.h"
#include "print_start_phase_label.h"
#include "printer_detector.h"
#include "text_io.h"
#include "thermal_rate_model.h"
#include "unit_conversions.h"
#include "wait_line_reader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>

using namespace helix;

using json = nlohmann::json;

namespace {
/// Strip trailing "..." / spaces from a status message so the displayed
/// "(N/M)" suffix doesn't read "Pre-scanning Bed... (3)". Returns the
/// trimmed string by value; if input is empty, returns empty.
std::string trim_trailing_ellipsis(const std::string& s) {
    auto end = s.size();
    while (end > 0) {
        char c = s[end - 1];
        if (c == '.' || c == ' ' || c == '\t') {
            --end;
        } else {
            break;
        }
    }
    return s.substr(0, end);
}

bool is_before_mesh(helix::PrintStartPhase phase) {
    return phase != helix::PrintStartPhase::BED_MESH && phase != helix::PrintStartPhase::PURGING &&
           phase != helix::PrintStartPhase::COMPLETE;
}

} // namespace

bool PrintStartCollector::heater_climbed(int temp, int target, HeaterHighWater& mark) {
    constexpr int kClimbDecidegrees = 10;
    if (target <= 0 || mark.high < 0 || target != mark.target) {
        mark.high = temp;
        mark.target = target;
        return false;
    }
    if (temp < mark.high + kClimbDecidegrees) {
        return false;
    }
    mark.high = temp;
    // At or past the target the heater has arrived; the at-target test owns it.
    return temp < target;
}

// ============================================================================
// STATIC PATTERN DEFINITIONS
// ============================================================================

// Pattern to detect PRINT_START macro invocation
const helix::Regex
    PrintStartCollector::print_start_pattern_(R"(PRINT_START|START_PRINT|_PRINT_START)",
                                              helix::Regex::ICase);

// Pattern to detect print start completion (first layer indicator)
// Includes HELIX:READY for our custom macro integration
const helix::Regex PrintStartCollector::completion_pattern_(
    R"(SET_PRINT_STATS_INFO\s+CURRENT_LAYER=|LAYER:?\s*1\b|;LAYER:1|First layer|HELIX:READY)",
    helix::Regex::ICase);

// Pattern to detect RESPOND-based print start completion (authoritative signal)
// Matches messages containing "print" + "start"/"started"/"starting" adjacent in either word order
const helix::Regex PrintStartCollector::respond_completion_pattern_(
    R"(\bprint\b\W+\b(start|started|starting)\b|\b(start|started|starting)\b\W+\bprint\b)",
    helix::Regex::ICase);

bool PrintStartCollector::is_respond_completion(const std::string& line) {
    // A host prompt's lines are UI for a dialog, so a "Start print" button is
    // the printer asking whether to start, not reporting that it did.
    if (auto action = ActionPromptManager::parse_action_line(line);
        action && action->command.rfind("prompt_", 0) == 0) {
        return false;
    }
    return helix::regex_search(line, respond_completion_pattern_);
}

// One "<id>:<temp> /<target>" pair per heater with a gcode id, space separated.
// The M105 reply carries an "ok " prefix and does not match.
const helix::Regex PrintStartCollector::heater_wait_report_pattern_(
    R"(\s*\w+:-?\d+(\.\d+)? /-?\d+(\.\d+)?(\s+\w+:-?\d+(\.\d+)? /-?\d+(\.\d+)?)*\s*)");

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintStartCollector::PrintStartCollector(IMoonrakerClient& client, PrinterState& state)
    : client_(client), state_(state) {
    spdlog::debug("[PrintStartCollector] Constructed");
}

PrintStartCollector::~PrintStartCollector() {
    // Don't call stop() here - it uses client_ and state_ references which may
    // already be destroyed during static destruction order. Callers should
    // explicitly call stop() before letting the shared_ptr go out of scope.
    // Just mark as inactive to prevent any pending callbacks from running.
    active_.store(false);
    registered_.store(false);

    // Safe to delete timer here (doesn't use client_ or state_)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }
}

// ============================================================================
// PUBLIC METHODS
// ============================================================================

void PrintStartCollector::start() {
    if (active_.load()) {
        spdlog::debug("[PrintStartCollector] Already active, ignoring start()");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        run_generation_.fetch_add(1);
        reset_run_locked();
        leveling_points_ = state_.get_discovery().leveling_probe_points();
        // Snapshot stale subject values so fallbacks only trigger on real changes
        baseline_layer_ =
            lv_subject_get_int(state_.print_state().get_print_layer_current_subject());
        baseline_progress_ = lv_subject_get_int(state_.print_state().get_print_progress_subject());
    }
    fallbacks_enabled_.store(false); // Will be enabled after initial window
    spdlog::info("[PrintStartCollector] Baselines: layer={}, progress={}", baseline_layer_,
                 baseline_progress_);
    helix::MemoryMonitor::log_now("print_start_collector_start", spdlog::level::debug);

    // Capture start temperatures for heating fraction calculation
    start_ext_temp_ = helix::ui::temperature::deci_to_degrees(
        lv_subject_get_int(state_.temperature_state().get_active_extruder_temp_subject()));
    start_bed_temp_ = helix::ui::temperature::deci_to_degrees(
        lv_subject_get_int(state_.temperature_state().get_bed_temp_subject()));
    priced_heating_seconds_.reset();
    cached_ext_temp_.store(start_ext_temp_, std::memory_order_relaxed);
    cached_bed_temp_.store(start_bed_temp_, std::memory_order_relaxed);
    frame_ext_temp_.store(start_ext_temp_, std::memory_order_relaxed);
    frame_bed_temp_.store(start_bed_temp_, std::memory_order_relaxed);
    cached_ext_target_.store(helix::ui::temperature::deci_to_degrees(lv_subject_get_int(
                                 state_.temperature_state().get_active_extruder_target_subject())),
                             std::memory_order_relaxed);
    cached_bed_target_.store(helix::ui::temperature::deci_to_degrees(lv_subject_get_int(
                                 state_.temperature_state().get_bed_target_subject())),
                             std::memory_order_relaxed);
    frame_ext_target_.store(cached_ext_target_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    frame_bed_target_.store(cached_bed_target_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    frame_ext_target_seen_.store(false, std::memory_order_relaxed);
    frame_bed_target_seen_.store(false, std::memory_order_relaxed);
    // Reset thermal rate models with current temperatures
    {
        auto& mgr = ThermalRateManager::instance();
        mgr.get_model("extruder").reset(static_cast<float>(start_ext_temp_));
        mgr.get_model("heater_bed").reset(static_cast<float>(start_bed_temp_));
    }

    // Load prediction history from config
    load_prediction_history();

    // Compute duration-proportional weights from predictions + thermal model
    compute_predicted_weights();

    // Ensure we have a profile for pattern matching
    if (!profile_) {
        // Try printer-specific profile first (printer type may not have been known at init time)
        std::string printer_type = state_.profile_state().printer_type();
        if (!printer_type.empty()) {
            std::string profile_name = PrinterDetector::get_print_start_profile(printer_type);
            if (!profile_name.empty()) {
                profile_ = PrintStartProfile::load(profile_name);
                spdlog::info("[PrintStartCollector] Loaded profile '{}' for '{}'", profile_name,
                             printer_type);
            }
        }
        if (!profile_) {
            profile_ = PrintStartProfile::load_default();
        }
    }

    auto self = shared_from_this();

    // Register gcode response handler if not already registered (stays registered
    // permanently — on_gcode_response() returns early when !active_)
    if (handler_name_.empty()) {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "print_start_collector_" + std::to_string(++s_collector_id);
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
    }

    // Register for printer status updates (fallback for printers with KAMP/custom macros)
    // This watches for _START_PRINT.print_started, START_PRINT.preparation_done, etc.
    macro_subscription_id_ = client_.register_notify_update([self](const json& notification) {
        if (!self->active_.load())
            return;

        if (!notification.contains("params") || !notification["params"].is_array() ||
            notification["params"].empty()) {
            return;
        }

        const auto& status = notification["params"][0];

        // Profile-declared phase object: a status object whose field carries
        // structured phase state, mapped through the profile's state patterns.
        // No-op unless the profile declares one.
        self->handle_phase_object_status(status);

        // Profile-declared status-signal rules: physical phase inference over
        // status frames, edge-triggered. No-op unless the profile declares any.
        self->handle_status_signals(status);

        // A start sequence written into the file: the line the print stands
        // on is the step running. No-op unless the profile says so.
        self->handle_start_block_position(status);

        // Check _START_PRINT.print_started (AD5M KAMP macro)
        if (status.contains("gcode_macro _START_PRINT")) {
            const auto& macro = status["gcode_macro _START_PRINT"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check START_PRINT.preparation_done
        if (status.contains("gcode_macro START_PRINT")) {
            const auto& macro = status["gcode_macro START_PRINT"];
            if (macro.contains("preparation_done") && macro["preparation_done"].is_boolean() &&
                macro["preparation_done"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: preparation_done=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check _HELIX_STATE.print_started (our custom macro)
        if (status.contains("gcode_macro _HELIX_STATE")) {
            const auto& macro = status["gcode_macro _HELIX_STATE"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Helix macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }
    });

    registered_.store(true);
    active_.store(true);

    // Set initial state
    state_.print_state().set_print_start_state(PrintStartPhase::INITIALIZING,
                                               lv_tr("Preparing Print..."), 0);

    // Create LVGL timer for periodic elapsed + ETA updates (runs on main thread)
    {
        eta_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* collector = static_cast<PrintStartCollector*>(lv_timer_get_user_data(timer));
                if (collector) {
                    collector->update_eta_display();
                    collector->check_purge_shape();
                    collector->check_fallback_completion();
                    collector->flush_deferred_start_block_step();
                }
            },
            kEtaUpdateIntervalMs, this);
        spdlog::debug("[PrintStartCollector] ETA timer created ({}ms interval)",
                      kEtaUpdateIntervalMs);
    }
    query_phase_object();

    // Run first update immediately
    update_eta_display();

    spdlog::debug("[PrintStartCollector] Started monitoring (handler: {})", handler_name_);
}

void PrintStartCollector::stop() {
    // Mark inactive first to stop callbacks from processing
    bool was_active = active_.exchange(false);
    bool was_registered = registered_.exchange(false);

    // Keep gcode response handler registered permanently — it checks active_ and
    // returns early when not collecting. This avoids the race where early responses
    // from START_PRINT are lost because the handler isn't registered yet.

    // Unregister macro subscription using atomic exchange to prevent double-unsubscribe
    SubscriptionId sub_id = macro_subscription_id_.exchange(0);
    if (sub_id != 0) {
        client_.unsubscribe_notify_update(sub_id);
        spdlog::debug("[PrintStartCollector] Unsubscribed from status updates");
    }

    fallbacks_enabled_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (start_block_cancel_) {
            start_block_cancel_->store(true);
            start_block_cancel_.reset();
        }
    }

    // Delete ETA timer (must be on main thread, but stop() is always called from main thread)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }

    if (was_active) {
        state_.print_state().clear_print_start_time_left();
        state_.print_state().reset_print_start_state();
        spdlog::debug("[PrintStartCollector] Stopped monitoring");
    }
}

void PrintStartCollector::detach() {
    stop();
    if (!handler_name_.empty()) {
        client_.unregister_method_callback("notify_gcode_response", handler_name_);
        handler_name_.clear();
    }
}

void PrintStartCollector::note_host_side_pre_start(const std::string& dispatched_block) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        host_pre_start_echo_lines_.clear();
        size_t begin = 0;
        while (begin <= dispatched_block.size()) {
            const size_t newline = dispatched_block.find('\n', begin);
            const std::string line(helix::text_io::trim(dispatched_block.substr(
                begin, newline == std::string::npos ? std::string::npos : newline - begin)));
            if (!line.empty()) {
                host_pre_start_echo_lines_.push_back(line);
            }
            if (newline == std::string::npos) {
                break;
            }
            begin = newline + 1;
        }
        if (window_ == helix::PreprintWindow::HostPreStart) {
            return; // already declared
        }
        window_ = helix::PreprintWindow::HostPreStart;
    }
    spdlog::info("[PrintStartCollector] Host-side pre-start block is part of this window - "
                 "re-filtering prediction history");
    // start() has already loaded the printer-edge bucket, so re-run the load
    // now that we know which population this run belongs to. Cheap, and it
    // happens once, milliseconds after arming.
    load_prediction_history();
}

void PrintStartCollector::note_phase_overrides(helix::PreprintPhaseOverrides overrides) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase_overrides_ = std::move(overrides);
        // The total moves with the toggles, in either direction; the anchor
        // would clamp a larger one back to the old countdown.
        last_remaining_ = 0;
    }
    compute_predicted_weights();
}

void PrintStartCollector::reset_run_locked() {
    printing_state_start_ = helix::sim::SimulatedClock::now();
    last_signal_time_ = printing_state_start_;
    last_inferred_activity_time_ = printing_state_start_;
    hold_until_ = helix::sim::SimulatedClock::time_point::min();
    held_for_ = {};
    // Assume the narrower window until something says otherwise.
    window_ = helix::PreprintWindow::PrinterEdge;
    host_pre_start_echo_lines_.clear();
    phase_overrides_.reset();
    detected_phases_.clear();
    phases_left_.clear();
    current_phase_ = PrintStartPhase::INITIALIZING;
    print_start_detected_ = false;
    max_sequential_progress_ = 0;
    phase_time_spent_.clear();
    phase_since_ = printing_state_start_;
    start_block_.set_steps({});
    start_block_requested_ = false;
    file_position_.reset();
    frame_filename_.clear();
    deferred_step_.reset();
    console_phase_line_.clear();
    if (start_block_cancel_) {
        start_block_cancel_->store(true);
        start_block_cancel_.reset();
    }
    console_phase_time_ = helix::sim::SimulatedClock::time_point::min();
    mesh_probe_current_ = 0;
    mesh_probe_total_ = 0;
    mesh_adapted_total_ = 0;
    mesh_points_.reset();
    mesh_first_probe_time_ = {};
    mesh_last_probe_time_ = {};
    mesh_seconds_per_probe_ = 0.0f;
    mesh_extrapolation_armed_ = false;
    mesh_entry_ext_target_ = 0;
    mesh_counts_probes_ = true;
    pre_mesh_points_.reset();
    pre_mesh_last_probe_time_ = {};
    current_mesh_message_.clear();
    current_message_.clear();
    heater_wait_report_time_ = helix::sim::SimulatedClock::time_point::min();
    heater_wait_shown_ = false;
    wait_heater_arrived_at_ = helix::sim::SimulatedClock::time_point::min();
    heaters_ready_wait_since_ = helix::sim::SimulatedClock::time_point::min();
    forget_wait_line_locked();
    pre_wait_phase_ = PrintStartPhase::IDLE;
    pre_wait_message_.clear();
    last_phase_object_state_.clear();
    phase_object_state_cache_.reset();
    phase_object_context_cache_.reset();
    live_object_phase_ = PrintStartPhase::IDLE;
    held_status_signal_rules_.clear();
    bed_mesh_present_ = false;
    temps_ready_time_ = {};
    silent_progression_idx_ = 0;
    real_signal_seen_.store(false, std::memory_order_relaxed);
    // Arm the layer-1 completion edge fresh for this print. The current
    // subject value may be a stale positive carried over from the previous
    // print (reset_for_new_print() runs async, after start()), so we do NOT
    // seed layer_zero_seen_ from it — only a freshly observed current_layer<1
    // (post-reset) latches it. See MoonrakerManager::should_complete_preprint().
    layer_zero_seen_.store(false, std::memory_order_relaxed);
    first_layer_observed_.store(-1, std::memory_order_relaxed);
    layer_advanced_.store(false, std::memory_order_relaxed);
    last_remaining_ = 0;
    fallback_completion_ = false;
    // A mark left from the last print would count its climb since then as activity.
    bed_climb_ = {};
    ext_climb_ = {};
    // Position inference starts with a clean slate and a fresh sample clock
    position_classifier_.reset();
    last_position_activity_ = helix::PositionActivity::NONE;
    leveling_descents_.reset();
    purge_flow_.reset();
    owning_step_ = {};
    position_clock_start_ = printing_state_start_;
}

void PrintStartCollector::reset() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reset_run_locked();
    }
    fallbacks_enabled_.store(false);

    if (active_.load()) {
        state_.print_state().set_print_start_state(PrintStartPhase::INITIALIZING,
                                                   lv_tr("Preparing Print..."), 0);
    }

    spdlog::debug("[PrintStartCollector] Reset state");
}

void PrintStartCollector::enable_fallbacks() {
    if (!active_.load())
        return;

    fallbacks_enabled_.store(true);
    spdlog::debug("[PrintStartCollector] Fallback detection enabled");

    // Don't immediately check fallback conditions here.
    // set_print_start_state() defers reset_for_new_print() via async invoke,
    // so stale subject data (layer count, progress) from the previous print
    // hasn't been cleared yet. Let fallback checks be triggered naturally by
    // incoming data updates (observer callbacks on layer/progress subjects).
}

void PrintStartCollector::complete_from_external_signal(const char* source) {
    if (!active_.load()) {
        return;
    }
    spdlog::info("[PrintStartCollector] External completion signal: {}", source);
    update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
}

void PrintStartCollector::note_bed_mesh_presence(bool present) {
    if (!active_.load()) {
        return;
    }
    bool meshing_begins = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const bool was_present = bed_mesh_present_;
        bed_mesh_present_ = present;
        // The present→absent edge while CLEANING: the macro has finished its
        // wipe and moved into the probing work. Clears arriving before
        // CLEANING are the rough G28's own mesh clear and say nothing about
        // which step is running.
        meshing_begins = was_present && !present && current_phase_ == PrintStartPhase::CLEANING;
        spdlog::debug("[PrintStartCollector] bed-mesh presence: present={} was={} phase={} "
                      "fires={}",
                      present, was_present, static_cast<int>(current_phase_), meshing_begins);
    }
    if (meshing_begins) {
        spdlog::info("[PrintStartCollector] bed mesh cleared during cleaning - meshing begins");
        enter_bed_mesh_with_buffer(lv_tr("Bed Meshing..."));
    }
}

void PrintStartCollector::note_mesh_bounds(float x_min, float x_max, float y_min, float y_max) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    position_classifier_.set_bounds(x_min, x_max, y_min, y_max);
}

void PrintStartCollector::note_position_sample(float x_mm, float y_mm, float z_mm) {
    if (!active_.load()) {
        return;
    }
    helix::PositionActivity activity = helix::PositionActivity::NONE;
    helix::PrintStartPhase phase;
    std::optional<helix::PrintStartPhase> leveling;
    bool classified = false;
    check_purge_shape();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
        if (!leveling_points_.empty() && is_before_mesh(phase)) {
            leveling = leveling_descents_.note_position(leveling_points_, x_mm, y_mm, z_mm);
        }
        classified = profile_ && profile_->position_signals();
    }
    if (leveling) {
        apply_leveling_phase(*leveling);
    }
    if (!classified) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto now = helix::sim::SimulatedClock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - position_clock_start_)
                .count();
        position_classifier_.note_position(x_mm, y_mm, z_mm, static_cast<uint64_t>(ms));
        activity = position_classifier_.activity();
        phase = current_phase_;
        if (activity == last_position_activity_) {
            return; // same verdict — nothing to publish
        }
        last_position_activity_ = activity;
        spdlog::debug("[PrintStartCollector] position activity: {} (x={:.1f} y={:.1f} z={:.1f})",
                      static_cast<int>(activity), x_mm, y_mm, z_mm);
    }

    // Map the verdict onto the display. Message-only refinements keep the
    // phase (and its progress weight) untouched; only the sweep march
    // promotes the phase. Real gcode_response signals always outrank these —
    // the classifier's verdicts only matter while the console is quiet.
    // Probing verdicts publish during ANY pre-mesh collection phase: K1-class
    // firmware interleaves heating with rough-G28 centre probes, and the
    // bed-mesh flap may already have promoted the phase by corner-tour time
    // (the label is then moot, not wrong).
    const bool before_mesh = is_before_mesh(phase);
    switch (activity) {
    case helix::PositionActivity::CENTER_PROBE:
        if (before_mesh) {
            spdlog::info("[PrintStartCollector] position: centre Z probes → \"Probing Z...\"");
            update_phase(phase, lv_tr("Probing Z..."));
        }
        break;
    case helix::PositionActivity::CORNER_PROBE:
        if (before_mesh) {
            spdlog::info("[PrintStartCollector] position: corner tour → \"Checking Bed Mesh...\"");
            update_phase(phase, lv_tr("Checking Bed Mesh..."));
        }
        break;
    case helix::PositionActivity::RASTER:
        if (before_mesh) {
            spdlog::info("[PrintStartCollector] position: sweep march → bed mesh");
            enter_bed_mesh_with_buffer(lv_tr("Bed Meshing..."));
        }
        break;
    case helix::PositionActivity::WIPE:
    case helix::PositionActivity::NONE:
        break; // wipe already has its console marker; silence needs no label
    }
}

void PrintStartCollector::check_purge_shape() {
    if (!active_.load()) {
        return;
    }
    auto& motion = state_.motion_state();
    const double x =
        helix::units::from_centimm(lv_subject_get_int(motion.get_position_x_subject()));
    const double y =
        helix::units::from_centimm(lv_subject_get_int(motion.get_position_y_subject()));
    const double z =
        helix::units::from_centimm(lv_subject_get_int(motion.get_position_z_subject()));
    const bool forward = lv_subject_get_int(motion.get_live_extruder_velocity_subject()) > 0;

    helix::PurgeEvidence evidence;
    auto& ams = AmsState::instance();
    evidence.ams_present = ams.is_available();
    if (evidence.ams_present) {
        evidence.action =
            static_cast<helix::AmsAction>(lv_subject_get_int(ams.get_ams_action_subject()));
        evidence.filament_loaded = lv_subject_get_int(ams.get_filament_loaded_subject()) != 0;
    }
    evidence.heaters_at_target = at_target(frame_ext_temp_.load(std::memory_order_relaxed),
                                           frame_ext_target_.load(std::memory_order_relaxed)) &&
                                 at_target(frame_bed_temp_.load(std::memory_order_relaxed),
                                           frame_bed_target_.load(std::memory_order_relaxed));

    PrintStartPhase phase;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            helix::sim::SimulatedClock::now() - position_clock_start_)
                            .count();
        evidence.stationary_flow =
            purge_flow_.note_sample(static_cast<uint64_t>(ms), forward, x, y, z);
        if (owning_step_.judge(PrintStartPhase::PURGING) == helix::OwningStep::Verdict::Absorb) {
            return; // the running step's own flow is not logged as a purge
        }
    }
    if (phase == PrintStartPhase::PURGING || phase == PrintStartPhase::COMPLETE ||
        !helix::is_purge(evidence)) {
        return;
    }
    spdlog::info("[PrintStartCollector] stationary purge (action={} loaded={} flow={}) at "
                 "x={:.1f} y={:.1f} z={:.1f}",
                 static_cast<int>(evidence.action), evidence.filament_loaded,
                 evidence.stationary_flow, x, y, z);
    PrintStartProfile::MatchResult match{
        PrintStartPhase::PURGING, helix::print_start_phase_signal_message(PrintStartPhase::PURGING),
        0};
    apply_profile_match(match, /*marks_real_signal=*/false);
}

void PrintStartCollector::check_fallback_completion() {
    if (!active_.load() || !fallbacks_enabled_.load()) {
        return;
    }

    // Check current phase under lock
    helix::sim::SimulatedClock::time_point start_time;
    PrintStartPhase current;
    bool print_start_was_detected;
    float predicted_total;
    helix::sim::SimulatedClock::duration held_for;
    bool wait_shown;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Already complete - nothing to do
        if (current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        current = current_phase_;
        wait_shown = heater_wait_shown_;
        print_start_was_detected = print_start_detected_;
        start_time = printing_state_start_;
        predicted_total = predicted_total_seconds_;
        held_for = held_for_;
    }

    // Get temperature data for proactive and completion fallback checks
    int ext_temp =
        lv_subject_get_int(state_.temperature_state().get_active_extruder_temp_subject());
    int ext_target =
        lv_subject_get_int(state_.temperature_state().get_active_extruder_target_subject());
    int bed_temp = lv_subject_get_int(state_.temperature_state().get_bed_temp_subject());
    int bed_target = lv_subject_get_int(state_.temperature_state().get_bed_target_subject());

    // Cache for thread-safe access from calculate_progress_locked()
    cached_ext_temp_.store(helix::ui::temperature::deci_to_degrees(ext_temp),
                           std::memory_order_relaxed);
    cached_ext_target_.store(helix::ui::temperature::deci_to_degrees(ext_target),
                             std::memory_order_relaxed);
    cached_bed_temp_.store(helix::ui::temperature::deci_to_degrees(bed_temp),
                           std::memory_order_relaxed);
    cached_bed_target_.store(helix::ui::temperature::deci_to_degrees(bed_target),
                             std::memory_order_relaxed);
    // Klipper sends a target only when it changes, so one set just before
    // start() reaches the subjects after the seed and never comes again as a
    // frame. Until a frame names a target, the subject's is the live one.
    if (!frame_ext_target_seen_.load(std::memory_order_relaxed)) {
        frame_ext_target_.store(cached_ext_target_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
    }
    if (!frame_bed_target_seen_.load(std::memory_order_relaxed)) {
        frame_bed_target_.store(cached_bed_target_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
    }

    // A heater still climbing toward its target is the printer working even
    // when the console is silent: M190 and M109 print nothing a profile can
    // match, and they spend their last degree or two inside the at-target band.
    const bool bed_climbed = heater_climbed(bed_temp, bed_target, bed_climb_);
    const bool ext_climbed = heater_climbed(ext_temp, ext_target, ext_climb_);
    if (bed_climbed || ext_climbed) {
        note_inferred_activity();
    }

    // The ceiling this pre-print is measured against: kAbsoluteMaxTimeout,
    // stretched for a long prediction. The backstop, a multiple of it, runs
    // before any branch below can return, so nothing the heaters or the
    // console do holds Preparing open forever. Time a declared hold covered is
    // the printer doing what it said it would: the ceiling leaves it out, and
    // the backstop leaves out at most one ceiling of it.
    std::chrono::seconds ceiling = kAbsoluteMaxTimeout;
    if (predicted_total > 0) {
        ceiling = std::max(ceiling, std::chrono::seconds(static_cast<int>(predicted_total *
                                                                          kAbsoluteTimeoutMargin)));
    }
    const auto elapsed = helix::sim::SimulatedClock::now() - start_time;
    const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const auto unheld = elapsed - held_for;
    const auto backstop_unheld =
        elapsed - std::min(held_for, helix::sim::SimulatedClock::duration(ceiling));
    if (backstop_unheld > ceiling * kBackstopCeilingMultiple) {
        spdlog::warn("[PrintStartCollector] Fallback: backstop ({} sec, ceiling={}s)", elapsed_sec,
                     ceiling.count());
        fallback_completion_ = true;
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Recompute predicted weights when heater targets increase from 0.
    // This handles macros that heat bed first, then issue M109 later —
    // at start() time the nozzle target is 0, so HEATING_NOZZLE gets no weight.
    // Also recompute on a substantial RISE (e.g. staged heating: probe temp,
    // then print temp), but never on decreases — temporary M104 S0 (nozzle
    // cooldown for probe cleaning) must not remove the heating phase and
    // cause progress regression. A heater's first measured rate recomputes
    // too: a bed heating at 5.6 s/C against a 1 s/C size guess otherwise
    // counts down a sixth of its real wait.
    {
        int new_ext = helix::ui::temperature::deci_to_degrees(ext_target);
        int new_bed = helix::ui::temperature::deci_to_degrees(bed_target);
        constexpr int kTargetRiseRecomputeDegrees = 15;
        bool ext_newly_set = (weights_ext_target_ == 0 && new_ext > 0);
        bool bed_newly_set = (weights_bed_target_ == 0 && new_bed > 0);
        bool ext_rose = new_ext > weights_ext_target_ + kTargetRiseRecomputeDegrees;
        bool bed_rose = new_bed > weights_bed_target_ + kTargetRiseRecomputeDegrees;
        auto& rates = ThermalRateManager::instance();
        const bool ext_first_rate =
            !weights_ext_measured_ && rates.get_model("extruder").measured_rate().has_value();
        const bool bed_first_rate =
            !weights_bed_measured_ && rates.get_model("heater_bed").measured_rate().has_value();
        if (ext_newly_set || bed_newly_set || ext_rose || bed_rose || ext_first_rate ||
            bed_first_rate) {
            spdlog::info("[PrintStartCollector] Heater targets or rates changed "
                         "(ext: {}→{}°C, bed: {}→{}°C, first measured rate ext={} bed={}), "
                         "recomputing weights",
                         weights_ext_target_, new_ext, weights_bed_target_, new_bed, ext_first_rate,
                         bed_first_rate);
            compute_predicted_weights();
            // The ETA's inputs changed, not just its noise: release the
            // monotonic anchor so the next publish can report the corrected
            // estimate instead of clamping it back to the target-less
            // provisional one (observed frozen at 215s for a 369s prep).
            last_remaining_ = 0;
        }
    }

    // Temps are in decidegrees (value * 10), targets may be 0 if not set
    bool bed_heating = bed_target > 0 && bed_temp < bed_target - kTempToleranceDecidegrees;
    bool nozzle_heating = ext_target > 0 && ext_temp < ext_target - kTempToleranceDecidegrees;
    bool temps_ready = !bed_heating && !nozzle_heating;

    // =========================================================================
    // PROACTIVE DETECTION: Track preparation phases from temperature state.
    // This is a FALLBACK for printers whose firmware emits sparse/no
    // gcode_response signals (e.g. K1C) — it advances heating phases from
    // temperature alone. It is GATED OFF the moment any real firmware signal is
    // observed (real_signal_seen_): on signal-rich firmwares like the Snapmaker
    // U1 the bed/nozzle sit below target for almost all of preparation while the
    // firmware narrates Homing → PRINT_BED_DETECTING → PRINT_SWITCH_CHECKING →
    // DETECT_PLATE → ... via action codes, so an ungated proactive detector
    // fires "Heating Bed"/"Heating
    // Nozzle" on nearly every tick and stomps the real, ordered phases. Once the
    // firmware is talking it is authoritative; proactive must stay silent.
    bool is_temp_detected_phase = !real_signal_seen_.load(std::memory_order_relaxed) &&
                                  current != PrintStartPhase::COMPLETE &&
                                  current != PrintStartPhase::PURGING;
    if (is_temp_detected_phase) {
        // If the toolhead is actively mid-home (G28 underway — some but not all
        // axes homed), show "Homing" rather than mislabeling the concurrent bed
        // warm-up as "Heating Bed". We require a NON-empty, partial homed_axes so
        // that a bed-first-heat macro (homed_axes still "" before G28 starts)
        // correctly shows "Heating Bed", and so the proactive heating unit tests
        // (which don't set homed_axes) are unaffected. Heaters play no part: a
        // G28 run cold, before either target is set, is homing all the same,
        // and an already-homed printer ("xyz") never enters it.
        const char* homed = lv_subject_get_string(state_.motion_state().get_homed_axes_subject());
        bool fully_homed = homed != nullptr && strchr(homed, 'x') != nullptr &&
                           strchr(homed, 'y') != nullptr && strchr(homed, 'z') != nullptr;
        // Enter HOMING when axes are partially homed (G28 mid-flight). STAY in
        // HOMING until fully homed even if homed_axes momentarily reads "":
        // Klipper clears homed_axes to "" as each axis re-homes, which would
        // otherwise flicker the label Homing→Heating Bed→Homing. The
        // current==HOMING latch absorbs that blip. It does NOT affect
        // bed-first-heat printers — they sit in INITIALIZING with homed_axes ""
        // and never ENTER homing from an empty string alone (the partial-axes
        // entry condition gates that).
        bool homing_active = homed != nullptr && !fully_homed &&
                             (homed[0] != '\0' || current == PrintStartPhase::HOMING);
        if (homing_active) {
            if (current != PrintStartPhase::HOMING) {
                spdlog::info("[PrintStartCollector] Proactive: homing (axes='{}')", homed);
                update_phase(PrintStartPhase::HOMING, lv_tr("Homing..."));
            }
            return; // homing in progress — don't let the heating branches override
        }
        if (bed_heating && (current != PrintStartPhase::HEATING_BED)) {
            // Bed still heating — show bed phase
            spdlog::info("[PrintStartCollector] Proactive: bed heating ({}/{})",
                         helix::ui::temperature::deci_to_degrees(bed_temp),
                         helix::ui::temperature::deci_to_degrees(bed_target));
            update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
            return;
        }
        if (nozzle_heating && !bed_heating && current != PrintStartPhase::HEATING_NOZZLE) {
            // Bed done (or no bed target), nozzle still heating
            spdlog::info("[PrintStartCollector] Proactive: nozzle heating ({}/{})",
                         helix::ui::temperature::deci_to_degrees(ext_temp),
                         helix::ui::temperature::deci_to_degrees(ext_target));
            update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
            return;
        }
        if ((bed_heating || nozzle_heating) && current == PrintStartPhase::IDLE) {
            // First detection — heaters just started ramping
            spdlog::info("[PrintStartCollector] Proactive: initializing (heaters ramping)");
            update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            return;
        }
    }

    // Heating-phase heater correction: a latched firmware heating signal
    // (e.g. K2's M109) can leave us in HEATING_NOZZLE while the bed is the
    // real long pole. Re-derive the shown heater from live temps, bed-first.
    // Scoped to when we're ALREADY in a heating phase, so it can never
    // relabel a firmware's ordered non-heating phase (e.g. Snapmaker U1
    // PRINT_BED_DETECTING) as a heating phase. A heater wait's own label is
    // resolved by the wait block below instead.
    if (!wait_shown &&
        (current == PrintStartPhase::HEATING_BED || current == PrintStartPhase::HEATING_NOZZLE)) {
        PrintStartPhase resolved = bed_heating      ? PrintStartPhase::HEATING_BED
                                   : nozzle_heating ? PrintStartPhase::HEATING_NOZZLE
                                                    : current;
        if (resolved != current) {
            spdlog::info("[PrintStartCollector] Heating correction: phase {} -> {} "
                         "(bed heating={}, nozzle heating={})",
                         static_cast<int>(current), static_cast<int>(resolved), bed_heating,
                         nozzle_heating);
            // relabel_heating_phase re-checks current_phase_ under the lock, so a
            // concurrent bg gcode signal that advanced past heating between the
            // `current` snapshot above and here is never regressed.
            relabel_heating_phase(resolved);
        }
    } else if (current == PrintStartPhase::BED_MESH && ext_climbed && nozzle_heating &&
               !bed_heating) {
        // A stored mesh loads in under a second, and a macro that parks and
        // purges without a word announces nothing between the mesh and the
        // print-temperature M109. relabel_heating_phase() confirms the mesh is
        // idle and the target rose after it began.
        relabel_heating_phase(PrintStartPhase::HEATING_NOZZLE);
    }

    // A heater wait blocks the whole queue, so while one reports, the heater
    // still short of its target is what the printer is doing, whatever phase
    // the firmware announced last. Nozzle first: M109 waits are short and the
    // bed is usually still warming behind them, while a nozzle above its
    // target (a cool-down) is not short of it. "Short" is the 2C at-target
    // test, since M109 keeps blocking until the nozzle settles within a
    // degree or so. The chamber counts only once both heaters are at target.
    // Before the first real signal the proactive detection above owns the
    // heaters.
    // What this tick read the wait as holding on; the chamber-wait flag
    // follows it, so the flag drops the tick the chamber stops being why.
    PrintStartPhase tick_waited_on = PrintStartPhase::IDLE;
    if (real_signal_seen_.load(std::memory_order_relaxed)) {
        bool waiting;
        bool nozzle_short;
        bool bed_short;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            waiting =
                heater_wait_report_time_ + kHeaterWaitReportGap > helix::sim::SimulatedClock::now();
            nozzle_short = !heating_target_reached_locked(PrintStartPhase::HEATING_NOZZLE);
            bed_short = !heating_target_reached_locked(PrintStartPhase::HEATING_BED);
        }
        if (!waiting) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                forget_wait_line_locked();
            }
            end_heater_wait();
        } else {
            request_wait_line();
            const int chamber_temp =
                lv_subject_get_int(state_.temperature_state().get_chamber_temp_subject());
            const int chamber_target =
                lv_subject_get_int(state_.temperature_state().get_chamber_target_subject());
            const auto& temps = state_.temperature_state();
            const bool heaters_ready = !nozzle_short && !bed_short;
            const bool chamber_heating = heaters_ready && chamber_target > 0 &&
                                         chamber_temp < chamber_target - kTempToleranceDecidegrees;
            // The chamber device reporting its own wait (M191) settles it.
            const bool device_wait = heaters_ready && temps.chamber_wait_active();
            // A wait that goes on with the nozzle and bed ready while the
            // chamber heater refused its target is a wait on that chamber: a
            // TEMPERATURE_WAIT MINIMUM names no heater target. A printer with
            // no chamber heater soaks on its sensor, and a refusal is what
            // tells this wait from any other (a probe coil's, say). M109 can
            // hold a few seconds past arrival while it settles, so this
            // reading needs the wait to outlast that first.
            const bool heater_refused =
                !temps.chamber_heater_name().empty() &&
                temps.chamber_command_rejection().kind != chamber::CommandRejection::None;
            const std::string& chamber_heater = temps.chamber_heater_name();
            bool chamber_unheated = false;
            std::optional<PrintStartPhase> named;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                const auto now = helix::sim::SimulatedClock::now();
                if (!heaters_ready) {
                    heaters_ready_wait_since_ = helix::sim::SimulatedClock::time_point::min();
                } else if (heaters_ready_wait_since_ ==
                           helix::sim::SimulatedClock::time_point::min()) {
                    heaters_ready_wait_since_ = now;
                }
                const bool settled =
                    heaters_ready &&
                    heaters_ready_wait_since_ != helix::sim::SimulatedClock::time_point::min() &&
                    now - heaters_ready_wait_since_ >= kChamberWaitSettle;
                chamber_unheated =
                    settled && chamber_target <= 0 && !chamber_heater.empty() && heater_refused;
                if (wait_line_read_) {
                    named = heater_waited_on(wait_line_, chamber_heater);
                }
            }
            // The command at the print position names the heater when it is
            // one of the waits; otherwise the heater still short of its
            // target is the best guess.
            const PrintStartPhase guessed = nozzle_short ? PrintStartPhase::HEATING_NOZZLE
                                            : bed_short  ? PrintStartPhase::HEATING_BED
                                            : device_wait || chamber_heating || chamber_unheated
                                                ? PrintStartPhase::SOAKING
                                                : PrintStartPhase::IDLE;
            const PrintStartPhase waited_on = named.value_or(guessed);
            tick_waited_on = waited_on;
            // The report that shows the waited-on heater arriving is the
            // wait's last, so moving the label to another heater on a guess
            // takes a report after the tick that saw it arrive: the next wait.
            // The waiting command needs no such proof.
            bool may_relabel = true;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (named || !heater_wait_shown_ || waited_on == current_phase_) {
                    wait_heater_arrived_at_ = helix::sim::SimulatedClock::time_point::min();
                } else if (wait_heater_arrived_at_ ==
                           helix::sim::SimulatedClock::time_point::min()) {
                    wait_heater_arrived_at_ = helix::sim::SimulatedClock::now();
                    may_relabel = false;
                } else {
                    may_relabel = heater_wait_report_time_ > wait_heater_arrived_at_;
                }
            }
            if (waited_on != PrintStartPhase::IDLE && may_relabel) {
                relabel_heating_phase(
                    waited_on, /*heater_wait=*/true,
                    waited_on == PrintStartPhase::SOAKING
                        ? chamber_wait_message(chamber_temp,
                                               device_wait && temps.chamber_wait_target_c() > 0
                                                   ? static_cast<int>(std::lround(
                                                         temps.chamber_wait_target_c() * 10))
                                                   : chamber_target)
                        : std::string());
            }
        }
    }
    {
        bool chamber_wait;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            chamber_wait = heater_wait_shown_ && current_phase_ == PrintStartPhase::SOAKING &&
                           tick_waited_on == PrintStartPhase::SOAKING;
        }
        state_.print_state().set_print_start_chamber_wait(chamber_wait);
    }

    // =========================================================================
    // SILENT-PHASE PROGRESSION: time-based phase advancement for firmwares
    // that run cleaning/purge as silent macros (no gcode echo between
    // temps-ready and first layer). Driven by the profile's
    // silent_progression entries — fires only when (a) we have a profile
    // with entries, (b) temps are ready, (c) the elapsed-since-ready
    // threshold has been crossed, and (d) the target phase would *advance*
    // (current_phase_ < entry.phase). The fourth guard means a real gcode
    // signal that already moved us past the silent phase wins — the silent
    // fallback never regresses.
    // =========================================================================
    if (profile_ && !profile_->silent_progression().empty()) {
        if (temps_ready) {
            if (temps_ready_time_.time_since_epoch().count() == 0) {
                temps_ready_time_ = helix::sim::SimulatedClock::now();
                spdlog::debug("[PrintStartCollector] Temps ready — silent progression armed");
            }
            const auto& entries = profile_->silent_progression();
            auto since_ready = std::chrono::duration_cast<std::chrono::seconds>(
                                   helix::sim::SimulatedClock::now() - temps_ready_time_)
                                   .count();
            while (silent_progression_idx_ < entries.size()) {
                const auto& entry = entries[silent_progression_idx_];
                if (since_ready < entry.after_temps_ready_seconds) {
                    break;
                }
                ++silent_progression_idx_;
                PrintStartPhase cur;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    cur = current_phase_;
                }
                if (static_cast<int>(cur) >= static_cast<int>(entry.phase)) {
                    spdlog::debug(
                        "[PrintStartCollector] Silent progression skip {} (already at {})",
                        static_cast<int>(entry.phase), static_cast<int>(cur));
                    continue;
                }
                spdlog::info("[PrintStartCollector] Silent progression: +{}s → '{}' (phase={})",
                             since_ready, entry.message, static_cast<int>(entry.phase));
                update_phase(entry.phase, entry.message.c_str());
            }
        }
    }

    // =========================================================================
    // COMPLETION DETECTION: Detect when PRINT_START is done
    // =========================================================================

    // Suppress timeout while mesh probing is actively progressing.
    // During mesh, the nozzle target may be 0 (macro cleared it for cooldown),
    // making the temperature check unreliable. Active probing = we're making real progress.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (mesh_probing_locked()) {
            return; // Active probing — don't timeout
        }
    }

    // Fallback 3: timeouts.
    //
    // ext_target == 0 during macro execution means the nozzle heat command (M109)
    // hasn't been issued yet — NOT that no nozzle heating is needed. Bed-first
    // macros (common on AD5M with ABS) heat bed, then home + mesh, then heat
    // nozzle. Treating ext_target=0 as "nozzle satisfied" causes premature timeout.
    //
    // Temps count only at target, the same 2C test that completes a heating
    // phase: a bed at 90% of its target can be minutes short of it. An unknown
    // nozzle target (ext_target=0) cannot be confirmed, so it never counts.
    //
    // A heater far above its target is cooling toward it (a nozzle cooled for
    // probing), which a wait blocks on as surely as a climb.
    const bool nozzle_target_set = ext_target > 0;
    const bool bed_cooling = bed_target > 0 && bed_temp > bed_target + kTempToleranceDecidegrees;
    const bool nozzle_cooling = ext_target > 0 && ext_temp > ext_target + kTempToleranceDecidegrees;
    bool temps_at_target;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        temps_at_target = nozzle_target_set && !nozzle_cooling && !bed_cooling &&
                          heating_target_reached_locked(PrintStartPhase::HEATING_NOZZLE) &&
                          heating_target_reached_locked(PrintStartPhase::HEATING_BED);
    }
    const bool in_live_step = in_live_preprint_step();

    // A pre-print that is still narrating itself is not stuck, however long it
    // runs, and a timeout that ends it saves no phase timings, so a deadline
    // set by a short prediction could not grow. Every timeout therefore waits
    // for kPreprintQuietTimeout without activity as well as for the clock. The
    // ceiling ignores temperatures and inferred activity, since a heater that
    // never settles must not hold Preparing open, but still waits that long
    // without a matched line or probe line. A pre-print step the printer
    // reports live holds the deadline timeouts until it ends: a silent cool-down
    // inside one outlasts the quiet gate.
    const auto now = helix::sim::SimulatedClock::now();
    helix::sim::SimulatedClock::duration quiet_for;
    helix::sim::SimulatedClock::duration signal_quiet_for;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto last_signal = std::max(last_signal_time_, hold_until_);
        signal_quiet_for = now - last_signal;
        quiet_for = now - std::max(last_signal, last_inferred_activity_time_);
    }
    const bool quiet = quiet_for >= kPreprintQuietTimeout;
    const auto quiet_sec = std::chrono::duration_cast<std::chrono::seconds>(quiet_for).count();

    // A prediction sets the deadline; kFallbackTimeout stands in without one.
    const std::chrono::seconds deadline =
        predicted_total > 0
            ? std::chrono::seconds(static_cast<int>(predicted_total * kAdaptiveTimeoutMargin))
            : kFallbackTimeout;
    if (elapsed > deadline && temps_at_target && quiet && !in_live_step) {
        spdlog::info("[PrintStartCollector] Fallback: timeout ({} sec, predicted={:.0f}s, "
                     "quiet={}s)",
                     elapsed_sec, predicted_total, quiet_sec);
        fallback_completion_ = true;
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    if (unheld > ceiling && signal_quiet_for >= kPreprintQuietTimeout) {
        spdlog::warn("[PrintStartCollector] Fallback: ceiling ({} sec, ceiling={}s, "
                     "nozzle_target_set={})",
                     elapsed_sec, ceiling.count(), nozzle_target_set);
        fallback_completion_ = true;
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

bool PrintStartCollector::is_own_pre_start_echo_locked(const std::string& line) const {
    const std::string trimmed(helix::text_io::trim(line));
    if (trimmed.empty()) {
        return false;
    }
    for (const auto& dispatched : host_pre_start_echo_lines_) {
        // Equality covers the per-line echo; containment covers a console
        // that repeats the whole block (or decorates the line around it).
        if (trimmed == dispatched || trimmed.find(dispatched) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void PrintStartCollector::on_gcode_response(const json& msg) {
    if (!active_.load()) {
        return;
    }

    // Parse notify_gcode_response format: {"method": "...", "params": ["line"]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    // I4: Type-check params[0] before extracting string reference
    if (!msg["params"][0].is_string()) {
        return;
    }
    const std::string& line = msg["params"][0].get_ref<const std::string&>();

    // Skip empty lines and common noise
    if (line.empty() || line == "ok") {
        return;
    }

    spdlog::trace("[PrintStartCollector] G-code: {}", line);

    // Klipper echoes dispatched gcode back through the console. A line that
    // reproduces the host-side pre-start block is our own text being repeated
    // before the printer has run any of it, so nothing in it — pattern match,
    // marker, or plugin signal — may be read as the printer narrating work.
    // Unrelated lines arriving in the same window are still honoured.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (is_own_pre_start_echo_locked(line)) {
            spdlog::debug("[PrintStartCollector] Ignoring echo of dispatched pre-start gcode: {}",
                          line);
            return;
        }
    }

    // A heater wait reports once a second for as long as it blocks. The
    // fallback tick reads the stamp; the wait itself is the printer working.
    if (is_heater_wait_report(line)) {
        bool wait_starts;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto now = helix::sim::SimulatedClock::now();
            wait_starts = heater_wait_report_time_ + kHeaterWaitReportGap <= now;
            heater_wait_report_time_ = now;
        }
        note_inferred_activity();
        // Read the waiting command now rather than at the next tick, which
        // would otherwise label the wait's first seconds from a guess.
        if (wait_starts && real_signal_seen_.load(std::memory_order_relaxed)) {
            auto self = shared_from_this();
            helix::ui::queue_update("PrintStartCollector::wait_started",
                                    [self]() { self->request_wait_line(); });
        }
    }

    // Check for HELIX:PHASE signals (highest priority - definitive signals from plugin/macros)
    if (check_helix_phase_signal(line)) {
        return; // Signal handled
    }

    // Profile tag patterns: lines a firmware prints to announce a step
    // outright, often with progress no heuristic could infer. They live in
    // the profile, so another printer's coincidental vocabulary never matches.
    if (profile_ && check_tag_signal(line)) {
        return; // Signal handled
    }

    // Check for RESPOND-based print start completion (authoritative signal)
    // Many users end PRINT_START macros with RESPOND MSG="Print started!" or similar.
    // This fires before Moonraker's state transition and is a definitive end-of-preprint signal.
    if (is_respond_completion(line)) {
        spdlog::info("[PrintStartCollector] RESPOND completion detected: {}", line);
        real_signal_seen_.store(true, std::memory_order_relaxed);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Check profile signal formats (priority 2, after HELIX:PHASE)
    if (profile_) {
        PrintStartProfile::MatchResult match;
        if (profile_->try_match_signal(line, match)) {
            real_signal_seen_.store(true, std::memory_order_relaxed);
            note_signal();
            // Profile messages are English tags — resolve through the pack.
            match.message = lv_tr(match.message.c_str());
            apply_console_phase(line, [&] {
                if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
                    update_phase(match.phase, match.message, match.progress);
                } else {
                    update_phase(match.phase, match.message.c_str());
                }
            });
            return;
        }
    }

    // Check for PRINT_START marker (once per session)
    bool should_set_initializing = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!print_start_detected_ && is_print_start_marker(line)) {
            print_start_detected_ = true;
            should_set_initializing = true;
        }
    }
    if (should_set_initializing) {
        real_signal_seen_.store(true, std::memory_order_relaxed);
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Starting Print..."));
        spdlog::info("[PrintStartCollector] PRINT_START detected");
        return;
    }

    // Stock Klipper's bed_mesh.py emits "// Adapted probe count: (N,M)" once
    // when adaptive meshing reduces the grid (slicer-passed MESH_MIN/MESH_MAX
    // or ADAPTIVE=1). Fires before the first probe — perfect timing to
    // populate the live denominator. NOT emitted by Snapmaker U1's firmware
    // fork; they get an empty `mesh_probe_total_` and we show "(N)" without
    // total via adaptive_meshing=true in the profile.
    if (auto adapt_total = helix::parse_adapted_probe_count(line)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mesh_adapted_total_ = *adapt_total;
        mesh_probe_total_ = *adapt_total;
        spdlog::info("[PrintStartCollector] Adapted probe count from gcode: {}", *adapt_total);
    }

    // Any probe line proves the pre-print is still working, whether or not we
    // have entered BED_MESH yet. Mesh probing is the longest stretch that
    // matches no profile pattern, so without this the quiet gate would expire
    // mid-sweep on exactly the printers that need it most.
    if (helix::is_probe_result_line(line) || helix::parse_probe_progress(line)) {
        note_signal();
    }

    // Check for bed mesh probe progress (sub-phase tracking within BED_MESH).
    // Also detects BED_MESH phase entry from probe lines on printers that don't
    // emit a separate BED_MESH_CALIBRATE command line before probing starts.
    {
        bool in_mesh = false;
        bool in_leveling = false;
        std::optional<PrintStartPhase> leveling;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            in_mesh = (current_phase_ == PrintStartPhase::BED_MESH);
            // QGL and Z_TILT also fire "probe at X,Y is z=Z" lines via Klipper's
            // shared probe routine; a Voron 2.4 QGL touches 4 corner pads with
            // `samples: 3`. Those must neither enter BED_MESH nor count
            // against its probe total. With the configured leveling points
            // known, a probe at one of them is leveling and anything else is
            // mesh, whatever the current phase. Without them, every probe
            // line while QGL/Z_TILT is current is taken as leveling.
            if (leveling_points_.empty()) {
                in_leveling = (current_phase_ == PrintStartPhase::QGL ||
                               current_phase_ == PrintStartPhase::Z_TILT);
            } else if (is_before_mesh(current_phase_)) {
                if (auto pos = helix::parse_probe_position(line)) {
                    leveling = helix::leveling_phase_at(leveling_points_, pos->x, pos->y);
                }
            }
        }
        if (leveling) {
            apply_leveling_phase(*leveling);
            return; // leveling output, not mesh data or a profile pattern
        }

        bool is_probe_line =
            !in_mesh && !in_leveling &&
            (helix::parse_probe_progress(line) || helix::is_probe_result_line(line));

        // If we see a probe line but aren't in BED_MESH yet, buffer probes
        // before entering the phase. Some printers (e.g. AD5M Klipper mod)
        // emit 1-2 PROBE commands for nozzle wipe before mesh calibration.
        // Require kMeshProbeEntryThreshold distinct probe POINTS to confirm
        // this is actual mesh probing, not an isolated operation.
        if (is_probe_line) {
            bool should_enter = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto now = helix::sim::SimulatedClock::now();

                // Gap reset for pre-mesh buffer
                if (pre_mesh_points_.points() > 0 &&
                    (now - pre_mesh_last_probe_time_) > kMeshProbeGapReset) {
                    spdlog::debug("[PrintStartCollector] Pre-mesh probe gap reset "
                                  "({}s since last probe)",
                                  std::chrono::duration_cast<std::chrono::seconds>(
                                      now - pre_mesh_last_probe_time_)
                                      .count());
                    pre_mesh_points_.reset();
                }

                pre_mesh_points_.feed(line);
                pre_mesh_last_probe_time_ = now;

                if (pre_mesh_points_.points() >= kMeshProbeEntryThreshold) {
                    should_enter = true;
                } else {
                    spdlog::debug("[PrintStartCollector] Pre-mesh probe point {}/{} (buffering)",
                                  pre_mesh_points_.points(), kMeshProbeEntryThreshold);
                }
            }

            if (should_enter) {
                enter_bed_mesh_with_buffer(lv_tr("Bed Mesh..."));
                in_mesh = true;
                return;
            }
            // Below threshold — don't enter BED_MESH yet, skip probe counting
            return;
        }

        bool counting = true;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            counting = mesh_counts_probes_;
        }
        if (in_mesh && !counting &&
            (helix::parse_probe_progress(line) || helix::is_probe_result_line(line))) {
            return; // a check of a stored mesh: activity, not points to count
        }

        if (in_mesh) {
            if (auto pp = helix::parse_probe_progress(line)) {
                // "Probing point X/Y" — firmware provides current and total
                int progress;
                char msg_buf[64];
                std::string label;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    auto now = helix::sim::SimulatedClock::now();

                    // Gap reset: if >30s since last probe, earlier probes were
                    // from a different operation (e.g. nozzle wipe)
                    if (mesh_probe_current_ > 0 &&
                        (now - mesh_last_probe_time_) > kMeshProbeGapReset) {
                        spdlog::info("[PrintStartCollector] Mesh probe gap reset "
                                     "({}s since last probe)",
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         now - mesh_last_probe_time_)
                                         .count());
                        mesh_probe_current_ = 0;
                        mesh_seconds_per_probe_ = 0.0f;
                        mesh_extrapolation_armed_ = false;
                    }

                    if (mesh_probe_current_ == 0) {
                        mesh_first_probe_time_ = now;
                    } else if (pp->current > 1) {
                        float elapsed = static_cast<float>(
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                now - mesh_first_probe_time_)
                                                .count()) /
                                        1000.0f;
                        mesh_seconds_per_probe_ = elapsed / static_cast<float>(pp->current - 1);
                    }
                    mesh_last_probe_time_ = now;

                    mesh_probe_current_ = pp->current;
                    mesh_probe_total_ = pp->total;
                    progress = calculate_progress_locked();
                    label = current_mesh_message_.empty()
                                ? lv_tr("Bed Mesh")
                                : trim_trailing_ellipsis(current_mesh_message_);
                }
                spdlog::debug("[PrintStartCollector] Mesh probe {}/{} ({:.1f}s/probe)", pp->current,
                              pp->total, mesh_seconds_per_probe_);
                snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", label.c_str(), pp->current,
                         pp->total);
                state_.print_state().set_print_start_state(PrintStartPhase::BED_MESH, msg_buf,
                                                           progress);
                return; // Handled — skip check_phase_patterns
            }

            if (helix::is_probe_result_line(line)) {
                // "probe at X,Y is z=Z" fallback — no total from firmware.
                // Dedupe by (x,y): Klipper's `samples: N` config emits N consecutive
                // probes at the same position; we count unique points, not samples.
                int count;
                int total;
                int progress;
                bool is_new_point = false;
                std::string label;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    auto now = helix::sim::SimulatedClock::now();

                    // Gap reset: if >30s since last probe, earlier probes were
                    // from a different operation (e.g. nozzle wipe on AD5M)
                    if (mesh_points_.points() > 0 &&
                        (now - mesh_last_probe_time_) > kMeshProbeGapReset) {
                        spdlog::info("[PrintStartCollector] Mesh probe gap reset "
                                     "({}s since last probe)",
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         now - mesh_last_probe_time_)
                                         .count());
                        mesh_points_.reset();
                        mesh_probe_current_ = 0;
                        mesh_seconds_per_probe_ = 0.0f;
                        mesh_extrapolation_armed_ = false;
                    }

                    // Position dedupe (and the sample-divisor fallback for lines
                    // whose coordinates don't parse) lives in ProbePointCounter,
                    // shared with the bed mesh panel's live probe readout.
                    const int before = mesh_points_.points();
                    mesh_points_.feed(line);
                    is_new_point = (mesh_points_.points() != before);

                    if (is_new_point) {
                        if (mesh_points_.points() == 1) {
                            mesh_first_probe_time_ = now;
                        } else {
                            float elapsed =
                                static_cast<float>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - mesh_first_probe_time_)
                                        .count()) /
                                1000.0f;
                            mesh_seconds_per_probe_ =
                                elapsed / static_cast<float>(mesh_points_.points() - 1);
                        }
                    }
                    mesh_last_probe_time_ = now;

                    mesh_probe_current_ = mesh_points_.points();
                    count = mesh_probe_current_;
                    total = mesh_probe_total_;
                    progress = calculate_progress_locked();
                    label = current_mesh_message_.empty()
                                ? lv_tr("Bed Mesh")
                                : trim_trailing_ellipsis(current_mesh_message_);
                }
                if (is_new_point) {
                    spdlog::debug("[PrintStartCollector] Mesh point #{}/{} ({:.1f}s/point)", count,
                                  total, mesh_seconds_per_probe_);
                    char msg_buf[64];
                    if (total > 0) {
                        snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", label.c_str(), count,
                                 total);
                    } else {
                        snprintf(msg_buf, sizeof(msg_buf), "%s (%d)", label.c_str(), count);
                    }
                    state_.print_state().set_print_start_state(PrintStartPhase::BED_MESH, msg_buf,
                                                               progress);
                }

                // The line is consumed as mesh data whether or not it added a
                // point — a repeat sample of the same point is still mesh
                // output. It must not reach check_phase_patterns: profiles
                // whose BED_MESH pattern matches "probe at" (creality_k1)
                // would re-announce the phase with the profile's label, which
                // is treated as a sub-phase change and wipes the probe
                // counters — including the denominator learned on entry —
                // mid-sweep. Mirrors the "Probing point" branch above.
                return;
            }
        }
    }

    // Check phase patterns
    check_phase_patterns(line);
}

void PrintStartCollector::note_signal(std::chrono::seconds hold) {
    std::chrono::seconds extended{0};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto now = helix::sim::SimulatedClock::now();
        last_signal_time_ = now;
        if (hold <= std::chrono::seconds::zero()) {
            return;
        }
        // The console line and its display_status copy announce the same hold
        // milliseconds apart; only time beyond the standing hold extends it.
        const auto until = now + hold;
        const auto from = std::max(hold_until_, now);
        if (until - from < std::chrono::seconds(1)) {
            return;
        }
        held_for_ += until - from;
        hold_until_ = until;
        extended = std::chrono::duration_cast<std::chrono::seconds>(until - from);
    }
    spdlog::info("[PrintStartCollector] Printer announced {}s of silent pre-print work",
                 extended.count());
}

void PrintStartCollector::note_inferred_activity() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_inferred_activity_time_ = helix::sim::SimulatedClock::now();
}

void PrintStartCollector::check_phase_patterns(const std::string& line) {
    if (!profile_) {
        return;
    }

    PrintStartProfile::MatchResult match;
    if (profile_->try_match_pattern(line, match)) {
        apply_console_phase(line, [&] { apply_profile_match(match); });
    }
}

void PrintStartCollector::apply_console_phase(const std::string& line,
                                              const std::function<void()>& apply) {
    PrintStartPhase phase_before;
    std::string message_before;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase_before = current_phase_;
        message_before = current_message_;
    }
    apply();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (current_phase_ != phase_before || current_message_ != message_before) {
        console_phase_time_ = helix::sim::SimulatedClock::now();
        console_phase_line_ = line;
        deferred_step_.reset();
    }
}

void PrintStartCollector::apply_profile_match(const PrintStartProfile::MatchResult& match,
                                              bool marks_real_signal, bool live_state) {
    marks_real_signal = marks_real_signal && match.narrates;
    if (marks_real_signal) {
        real_signal_seen_.store(true, std::memory_order_relaxed);
        note_signal(std::chrono::seconds(match.hold_seconds));
    } else {
        note_inferred_activity();
    }
    if (absorbed_by_owning_step(match.phase)) {
        return;
    }
    // match.message arrives already translated: the profile matchers
    // resolve the template through the loaded pack before substituting
    // $1 capture groups.
    // Update when this is a NEW phase, OR when the phase was already detected
    // and this line names the same or a later one with a DIFFERENT message.
    // Firmwares narrate several distinct steps through one phase enum (the
    // U1 routes "Homing axes"/"Probing Z" through HOMING and "Inspecting
    // bed"/"Detecting plate"/"Bed mesh" through BED_MESH), so without the
    // message refinement every step after the enum's first detection keeps
    // the first label. Narration may refine; inference (a status-signal
    // rule, marks_real_signal=false) may not relabel a phase the printer
    // already named — except within BED_MESH, where an inferred rule that
    // re-fires relabels the sub-phase (a restarted sweep). The refinement is
    // bounded: it may not name a phase earlier than the one on display, so a
    // late repeat of an old pattern cannot drag the phase backwards.
    // maybe_reset_for_mesh_subphase_locked() (inside update_phase) resets
    // the probe counter on a BED_MESH message change so the "(n)" count
    // restarts.
    bool should_update = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!match.owns.empty()) {
            owning_step_ = {match.phase, match.owns};
        }
        if (detected_phases_.find(match.phase) == detected_phases_.end()) {
            detected_phases_.insert(match.phase);
            should_update = true;
        } else if (live_state && (match.phase != current_phase_ ||
                                  trim_trailing_ellipsis(match.message) !=
                                      trim_trailing_ellipsis(current_message_))) {
            should_update = true;
        } else if ((marks_real_signal || (match.phase == PrintStartPhase::BED_MESH &&
                                          current_phase_ == PrintStartPhase::BED_MESH)) &&
                   static_cast<int>(match.phase) >= static_cast<int>(current_phase_) &&
                   trim_trailing_ellipsis(match.message) !=
                       trim_trailing_ellipsis(current_message_)) {
            should_update = true;
        } else if (marks_real_signal && heater_wait_shown_) {
            // A heater wait blocks the queue, so a line the printer narrates
            // means the wait has ended and this phase replaces its label.
            should_update = true;
        }
    }
    if (should_update) {
        if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
            update_phase(match.phase, match.message, match.progress);
        } else {
            update_phase(match.phase, match.message.c_str(), match.counts_probes);
        }
        spdlog::debug("[PrintStartCollector] Detected phase: {}", static_cast<int>(match.phase));
    }
}

bool PrintStartCollector::absorbed_by_owning_step(PrintStartPhase phase) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    switch (owning_step_.judge(phase)) {
    case helix::OwningStep::Verdict::Absorb:
        spdlog::debug("[PrintStartCollector] phase {} signal belongs to the running {} step",
                      static_cast<int>(phase), static_cast<int>(owning_step_.phase));
        return true;
    case helix::OwningStep::Verdict::Ends:
        spdlog::debug("[PrintStartCollector] phase {} signal ends the {} step's ownership",
                      static_cast<int>(phase), static_cast<int>(owning_step_.phase));
        owning_step_ = {};
        return false;
    case helix::OwningStep::Verdict::Pass:
        return false;
    }
    return false;
}

void PrintStartCollector::apply_leveling_phase(helix::PrintStartPhase phase) {
    PrintStartProfile::MatchResult match{phase, helix::print_start_phase_signal_message(phase), 0};
    apply_profile_match(match, /*marks_real_signal=*/false);
}

void PrintStartCollector::handle_phase_object_status(const json& status,
                                                     std::optional<uint64_t> seed_run) {
    const bool seed = seed_run.has_value();
    if (!profile_ || !profile_->has_phase_object()) {
        return;
    }
    if (!status.is_object()) {
        return;
    }

    const auto object = status.find(profile_->phase_object_name());
    if (object == status.end() || !object->is_object()) {
        return; // this frame does not carry the phase object (delta-only update)
    }
    const std::string& context_field = profile_->phase_object_context_field();
    const bool qualified = !context_field.empty();

    // A null state is a step that has begun without naming one yet; it
    // replaces the previous step's state rather than leaving it standing.
    const auto field = object->find(profile_->phase_object_field());
    const bool has_state =
        field != object->end() && (field->is_string() || (qualified && field->is_null()));
    const auto context = qualified ? object->find(context_field) : object->end();
    const bool has_context = context != object->end();
    if (!has_state && !has_context) {
        return;
    }

    // An answer checks its run under the same lock as its cache writes and its
    // latch, so a start() before either keeps it out of the next run. A start()
    // after the latch can still meet the one match already on its way, as it
    // can a frame that arrived just before it.
    const auto stale_seed_locked = [&] { return seed && *seed_run != run_generation_.load(); };
    std::string state;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stale_seed_locked()) {
            return;
        }
        if (has_state && !(seed && phase_object_state_cache_)) {
            phase_object_state_cache_ = field->is_string() ? field->get<std::string>() : "";
        }
        if (has_context && !(seed && phase_object_context_cache_)) {
            phase_object_context_cache_ = PrintStartProfile::innermost_context(*context);
            // An empty stack is no step at all: nothing is left to hold, and
            // a step pushed again with the same state is a new one.
            if (phase_object_context_cache_->empty()) {
                live_object_phase_ = PrintStartPhase::IDLE;
                last_phase_object_state_.clear();
            }
        }
        if (!phase_object_state_cache_ || phase_object_state_cache_->empty()) {
            return;
        }
        state = qualified
                    ? PrintStartProfile::qualify_state(phase_object_context_cache_.value_or(""),
                                                       *phase_object_state_cache_)
                    : *phase_object_state_cache_;
    }

    PrintStartProfile::MatchResult match;
    if (!profile_->try_match_state(state, match)) {
        return; // a state the profile has no mapping for is not a phase signal
    }
    // The start-up answer can be the last print's final state: a file that
    // never ran its end macro leaves that print's stack standing until the
    // next start pushes its own. Only a frame the new run sees may complete it.
    if (seed && match.phase == PrintStartPhase::COMPLETE) {
        return;
    }

    // Klipper notifies on every field change in the object, so an unchanged
    // state re-arrives regularly; only a NEW state applies. The latch keeps
    // the last MATCHED state — an unmapped state between two mapped ones is
    // not a change of phase.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stale_seed_locked() || state == last_phase_object_state_) {
            return;
        }
        last_phase_object_state_ = state;
        if (qualified) {
            live_object_phase_ = match.phase;
        }
    }

    spdlog::debug("[PrintStartCollector] Phase-object state '{}' -> phase {}", state,
                  static_cast<int>(match.phase));
    apply_profile_match(match, /*marks_real_signal=*/true, /*live_state=*/qualified);
}

bool PrintStartCollector::in_live_preprint_step() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return live_object_phase_ != PrintStartPhase::IDLE &&
           live_object_phase_ != PrintStartPhase::COMPLETE;
}

void PrintStartCollector::apply_phase_object_query(const json& response, uint64_t run) {
    // An answer to an earlier run's query describes that run's printer.
    if (!active_.load()) {
        return;
    }
    const json* result = json_util::find_member(response, "result");
    const json* status = result != nullptr ? json_util::find_member(*result, "status") : nullptr;
    if (status != nullptr) {
        handle_phase_object_status(*status, run);
    }
}

void PrintStartCollector::query_phase_object() {
    if (!profile_ || !profile_->has_phase_object() ||
        profile_->phase_object_context_field().empty()) {
        return;
    }
    const std::string& name = profile_->phase_object_name();
    json params = {
        {"objects", json::object({{name, json::array({profile_->phase_object_field(),
                                                      profile_->phase_object_context_field()})}})}};
    auto self = shared_from_this();
    const uint64_t run = run_generation_.load();
    client_.send_jsonrpc(
        "printer.objects.query", params,
        [self, run](const json& response) { self->apply_phase_object_query(response, run); },
        [](const MoonrakerError& /*err*/) {
            spdlog::debug("[PrintStartCollector] Failed to query the phase object");
        });
}

void PrintStartCollector::handle_status_signals(const json& status) {
    if (!profile_) {
        return;
    }
    const auto& rules = profile_->status_signals();
    if (rules.empty()) {
        return;
    }
    if (!status.is_object()) {
        return;
    }

    // Klipper sends each heater field only when it changes, so the last value
    // seen per field is the live one.
    const auto note_heater = [&status](const char* name, std::atomic<int>& temp,
                                       std::atomic<int>& target, std::atomic<bool>& target_seen) {
        const auto heater = status.find(name);
        if (heater == status.end() || !heater->is_object()) {
            return;
        }
        const auto t = heater->find("temperature");
        if (t != heater->end() && t->is_number()) {
            temp.store(static_cast<int>(t->get<double>()), std::memory_order_relaxed);
        }
        const auto g = heater->find("target");
        if (g != heater->end() && g->is_number()) {
            target.store(static_cast<int>(g->get<double>()), std::memory_order_relaxed);
            target_seen.store(true, std::memory_order_relaxed);
        }
    };
    note_heater("extruder", frame_ext_temp_, frame_ext_target_, frame_ext_target_seen_);
    note_heater("heater_bed", frame_bed_temp_, frame_bed_target_, frame_bed_target_seen_);

    for (const auto& rule : rules) {
        const auto object = status.find(rule.object);
        if (object == status.end() || !object->is_object()) {
            continue; // this frame does not carry the rule's object (delta-only update)
        }

        PrintStartProfile::MatchResult match;
        const bool holds = profile_->evaluate_status_signal(*object, rule, match);

        // EDGE-TRIGGERED per rule: fire only on the false->true transition.
        // Physical predicates hold for whole windows while the frame keeps
        // re-delivering them, so the latch turns the window into one event;
        // a frame where the predicate reads false (or cannot resolve — Klipper
        // pushes per-field deltas) re-arms it.
        bool fire = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // An after_heat rule waits for both heaters: forward extrusion
            // before then is a bucket purge ahead of the mesh or a tool load.
            // It stays unlatched so a frame after the heat fires it. On a
            // toolchanger the frames name only the first extruder, so a tool
            // still heating keeps the rule quiet rather than mislabeled.
            const bool before_heat =
                rule.after_heat && !(at_target(frame_ext_temp_.load(std::memory_order_relaxed),
                                               frame_ext_target_.load(std::memory_order_relaxed)) &&
                                     at_target(frame_bed_temp_.load(std::memory_order_relaxed),
                                               frame_bed_target_.load(std::memory_order_relaxed)));
            if (holds && before_heat) {
                held_status_signal_rules_.erase(rule.name);
            } else if (holds) {
                fire = held_status_signal_rules_.insert(rule.name).second;
            } else {
                held_status_signal_rules_.erase(rule.name);
            }
        }
        if (fire) {
            spdlog::debug("[PrintStartCollector] Status signal '{}' held -> phase {}", rule.name,
                          static_cast<int>(match.phase));
            // Inference, not narration: a rule's predicates read the same
            // frames the proactive detector reads, so counting one as a real
            // signal would gate that detector off with its own input.
            apply_profile_match(match, /*marks_real_signal=*/false);
        }
    }
}

bool PrintStartCollector::check_helix_phase_signal(const std::string& line) {
    // Check for HELIX:PHASE:* signals (definitive markers from plugin/macros)
    static const char* kHelixPhasePrefix = "HELIX:PHASE:";
    constexpr size_t PREFIX_LEN = 12; // strlen("HELIX:PHASE:")

    // Find the prefix in the line (may have quotes or other wrappers)
    size_t pos = line.find(kHelixPhasePrefix);
    if (pos == std::string::npos) {
        return false;
    }
    real_signal_seen_.store(true, std::memory_order_relaxed);

    // Extract the phase name
    std::string phase_name = line.substr(pos + PREFIX_LEN);
    // Trim trailing whitespace, quotes, etc.
    size_t end = phase_name.find_first_of(" \t\n\r\"'");
    if (end != std::string::npos) {
        phase_name = phase_name.substr(0, end);
    }

    spdlog::info("[PrintStartCollector] HELIX:PHASE signal: {}", phase_name);

    const auto phase = helix::print_start_phase_from_name(phase_name);
    // IDLE is not a signal a macro can raise: accepting it would let a stray
    // line reset a print start that is already under way.
    if (phase && *phase != PrintStartPhase::IDLE) {
        if (*phase == PrintStartPhase::INITIALIZING) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            print_start_detected_ = true;
        }
        update_phase(*phase, helix::print_start_phase_signal_message(*phase));
        if (*phase == PrintStartPhase::COMPLETE) {
            spdlog::info("[PrintStartCollector] Print start complete via HELIX:PHASE signal");
        }
        return true;
    }

    // Unknown phase - log but don't block
    spdlog::warn("[PrintStartCollector] Unknown HELIX:PHASE: {}", phase_name);
    return false;
}

bool PrintStartCollector::check_tag_signal(const std::string& line) {
    PrintStartProfile::MatchResult match;
    if (!profile_->try_match_tag(line, match)) {
        return false;
    }
    real_signal_seen_.store(true, std::memory_order_relaxed);
    if (absorbed_by_owning_step(match.phase)) {
        return true;
    }
    if (match.reported_percent >= 0) {
        update_phase(match.phase, fmt::format("{} — {}%", match.message, match.reported_percent),
                     match.reported_percent);
    } else {
        update_phase(match.phase, match.message.c_str());
    }
    return true;
}

/// Reset mesh probe counters when entering BED_MESH or when the BED_MESH
/// status message changes (sub-phase transition on firmwares that route
/// multiple distinct probe operations through one phase enum, e.g.
/// Snapmaker U1: Inspecting bed → Detecting plate → Bed mesh). Caller must
/// hold state_mutex_.
void PrintStartCollector::maybe_reset_for_mesh_subphase_locked(PrintStartPhase next_phase,
                                                               const std::string& next_message,
                                                               bool counts_probes) {
    if (next_phase != PrintStartPhase::BED_MESH) {
        if (current_phase_ == PrintStartPhase::BED_MESH) {
            current_mesh_message_.clear();
            mesh_adapted_total_ = 0;
        }
        return;
    }
    const bool entering = (current_phase_ != PrintStartPhase::BED_MESH);
    if (entering) {
        // A nozzle target raised after this is not the one the mesh ran at.
        mesh_entry_ext_target_ = cached_ext_target_.load(std::memory_order_relaxed);
    }
    const bool message_changed = !entering && (next_message != current_mesh_message_);
    if (!entering && !message_changed) {
        return;
    }
    current_mesh_message_ = next_message;
    mesh_counts_probes_ = counts_probes;
    mesh_probe_current_ = 0;
    mesh_probe_total_ = mesh_adapted_total_;
    mesh_points_.reset();
    mesh_first_probe_time_ = {};
    mesh_last_probe_time_ = {};
    mesh_seconds_per_probe_ = 0.0f;
    mesh_extrapolation_armed_ = false;
    pre_mesh_points_.reset();
    if (message_changed) {
        spdlog::debug("[PrintStartCollector] BED_MESH sub-phase change → '{}' (counters reset)",
                      next_message);
    }
}

void PrintStartCollector::enter_bed_mesh_with_buffer(const char* message) {
    // Capture the pre-mesh buffer before update_phase clears the mesh
    // counters — the probes buffered on the way in (console pre-mesh points,
    // or the front row seen before the position classifier's sweep-march
    // verdict) are the sweep's first points and must be credited, or the
    // displayed count lags the physical taps by however many buffered.
    helix::ProbePointCounter buffered{1};
    helix::sim::SimulatedClock::time_point last_probe_at{};
    bool has_buffered = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pre_mesh_points_.points() > 0) {
            buffered = pre_mesh_points_;
            last_probe_at = pre_mesh_last_probe_time_;
            has_buffered = true;
        }
    }

    update_phase(PrintStartPhase::BED_MESH, message);

    if (!has_buffered) {
        return;
    }

    char msg_buf[96];
    int count = 0, total = 0;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mesh_points_ = buffered;
        mesh_probe_current_ = buffered.points();
        mesh_first_probe_time_ = last_probe_at;
        mesh_last_probe_time_ = last_probe_at;
        count = mesh_probe_current_;
        total = mesh_probe_total_;
        label = current_mesh_message_.empty() ? lv_tr("Bed Mesh")
                                              : trim_trailing_ellipsis(current_mesh_message_);
    }
    if (total > 0) {
        snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", label.c_str(), count, total);
    } else {
        snprintf(msg_buf, sizeof(msg_buf), "%s (%d)", label.c_str(), count);
    }
    state_.print_state().set_print_start_state(PrintStartPhase::BED_MESH, msg_buf,
                                               calculate_progress());
    spdlog::debug("[PrintStartCollector] Credited {} buffered pre-mesh probes on BED_MESH entry",
                  count);
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const char* message,
                                       bool counts_mesh_probes) {
    int progress;
    bool should_save = false;
    bool has_predictions = false;
    bool entering_mesh = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        entering_mesh =
            (phase == PrintStartPhase::BED_MESH && current_phase_ != PrintStartPhase::BED_MESH);
        // A phase transition re-assesses the remaining work — entering
        // BED_MESH with 124s predicted after the countdown eased to 39s is
        // new information, not noise. Release the monotonic anchor so the
        // countdown steps to the honest value instead of freezing at the
        // pre-transition floor for the rest of the phase.
        if (current_phase_ != phase && last_remaining_ > 0) {
            spdlog::debug("[PrintStartCollector] Releasing monotonic anchor on phase change "
                          "({}→{}, last remaining {}s)",
                          static_cast<int>(current_phase_), static_cast<int>(phase),
                          last_remaining_);
            last_remaining_ = 0;
        }
        // Entering a leveling phase clears the pre-mesh probe buffer — any
        // stray probe lines that arrived before the QGL/Z_TILT regex hit
        // (rare, but possible if firmware emits a probe before the macro
        // echo) shouldn't carry into the eventual BED_MESH count.
        if ((phase == PrintStartPhase::QGL || phase == PrintStartPhase::Z_TILT) &&
            current_phase_ != phase) {
            pre_mesh_points_.reset();
        }
        maybe_reset_for_mesh_subphase_locked(phase, message ? message : "", counts_mesh_probes);
        entering_mesh = entering_mesh && mesh_counts_probes_;
        enter_phase_locked(phase);
        current_message_ = message ? message : "";
        // A phase signal means the queue moved on from any heater wait.
        heater_wait_shown_ = false;
        heater_wait_report_time_ = helix::sim::SimulatedClock::time_point::min();
        detected_phases_.insert(phase); // Track for progress calculation

        if (phase == PrintStartPhase::COMPLETE) {
            should_save = true;
        }

        progress = calculate_progress_locked();
        has_predictions = predictor_.has_predictions();
    }

    // When predictor has data, time-based progress in update_eta_display() is the
    // sole progress source. Don't override it with phase-weight progress here.
    if (has_predictions && phase != PrintStartPhase::COMPLETE) {
        auto* subj = state_.print_state().get_print_start_progress_subject();
        if (subj) {
            progress = lv_subject_get_int(subj);
        }
    }

    // Query probe count when entering BED_MESH for the first time
    if (entering_mesh) {
        query_mesh_probe_count();
    }

    // Call PrinterState outside the lock to avoid potential deadlocks
    state_.print_state().set_print_start_state(phase, message, progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const std::string& message,
                                       int progress) {
    int effective_progress;
    bool should_save = false;
    bool entering_mesh = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        entering_mesh =
            (phase == PrintStartPhase::BED_MESH && current_phase_ != PrintStartPhase::BED_MESH);
        if ((phase == PrintStartPhase::QGL || phase == PrintStartPhase::Z_TILT) &&
            current_phase_ != phase) {
            pre_mesh_points_.reset();
        }
        maybe_reset_for_mesh_subphase_locked(phase, message);
        enter_phase_locked(phase);
        current_message_ = message;
        // A phase signal means the queue moved on from any heater wait.
        heater_wait_shown_ = false;
        heater_wait_report_time_ = helix::sim::SimulatedClock::time_point::min();
        detected_phases_.insert(phase);

        // Monotonic progress guard: never allow progress to decrease in sequential mode
        // (except COMPLETE which always goes to 100%)
        if (phase == PrintStartPhase::COMPLETE) {
            effective_progress = 100;
            should_save = true;
        } else {
            effective_progress = std::max(progress, max_sequential_progress_);
            effective_progress = std::min(effective_progress, 95);
        }
        max_sequential_progress_ = effective_progress;
    }

    if (entering_mesh) {
        query_mesh_probe_count();
    }

    state_.print_state().set_print_start_state(phase, message.c_str(), effective_progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::forget_wait_line_locked() {
    wait_line_ = {};
    wait_line_read_ = false;
    wait_line_unreadable_ = false;
    wait_line_position_.reset();
    ++wait_line_gen_;
}

std::optional<PrintStartPhase>
PrintStartCollector::heater_waited_on(const helix::WaitLine& line,
                                      const std::string& chamber_heater) {
    switch (line.kind) {
    case helix::WaitKind::M109:
        return PrintStartPhase::HEATING_NOZZLE;
    case helix::WaitKind::M190:
        return PrintStartPhase::HEATING_BED;
    case helix::WaitKind::M191:
        // A printer with no chamber heater runs M191 as a macro over some
        // other wait, and an M191 there says nothing about a chamber heater.
        if (chamber_heater.empty()) {
            return std::nullopt;
        }
        return PrintStartPhase::SOAKING;
    case helix::WaitKind::TemperatureWait:
        if (line.sensor == "heater_bed") {
            return PrintStartPhase::HEATING_BED;
        }
        if (line.tool) {
            return PrintStartPhase::HEATING_NOZZLE;
        }
        if (helix::wait_names_chamber_heater(line, chamber_heater)) {
            return PrintStartPhase::SOAKING;
        }
        return std::nullopt;
    case helix::WaitKind::Other:
        break;
    }
    return std::nullopt;
}

// The command at the print position: the line starting at
// virtual_sdcard.file_position. Main thread; the position answer and the
// line land under state_mutex_ and are read on the next tick.
void PrintStartCollector::request_wait_line() {
    if (!active_.load()) {
        return;
    }
    const char* file = lv_subject_get_string(state_.print_state().get_print_filename_subject());
    if (!file || !file[0]) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api || !api->transfers().supports_range_reads()) {
        return; // the line could not be read, so where the print is would go unused
    }
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (wait_line_unreadable_) {
            return;
        }
        gen = wait_line_gen_;
    }
    auto self = shared_from_this();
    const std::string filename = file;
    const auto read_at = [self, gen, filename](uint64_t pos) {
        const std::optional<uint64_t> position = pos;
        uint64_t read_gen;
        {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            if (gen != self->wait_line_gen_ || position == self->wait_line_position_) {
                return; // the wait ended, or the line there is already read
            }
            self->wait_line_position_ = position;
            self->wait_line_ = {};
            self->wait_line_read_ = false;
            read_gen = ++self->wait_line_gen_;
        }
        helix::ui::queue_update(
            "PrintStartCollector::read_wait_line", [self, read_gen, filename, pos]() {
                IMoonrakerAPI* api = get_moonraker_api();
                if (!api || !api->transfers().supports_range_reads()) {
                    return;
                }
                helix::read_wait_line_at(
                    api->transfers(), filename, pos,
                    [self, read_gen](const helix::WaitLine& line) {
                        self->apply_wait_line(read_gen, line);
                    },
                    [self, read_gen, pos](const MoonrakerError& e) {
                        spdlog::debug("[PrintStartCollector] Wait line at {} not read: {}", pos,
                                      e.message);
                        std::lock_guard<std::mutex> lock(self->state_mutex_);
                        if (read_gen == self->wait_line_gen_) {
                            self->wait_line_unreadable_ = true;
                        }
                    });
            });
    };
    // A subscribed position is current; the query covers a printer whose
    // frames have not carried one yet.
    std::optional<uint64_t> subscribed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        subscribed = file_position_;
    }
    if (subscribed) {
        read_at(*subscribed);
        return;
    }
    client_.send_jsonrpc(
        "printer.objects.query", {{"objects", {{"virtual_sdcard", {"file_position"}}}}},
        [read_at](const json& resp) {
            if (const std::optional<uint64_t> position =
                    helix::virtual_sdcard_file_position(resp)) {
                read_at(*position);
            }
        },
        [](const MoonrakerError& e) {
            spdlog::debug("[PrintStartCollector] virtual_sdcard not read: {}", e.message);
        });
}

void PrintStartCollector::handle_start_block_position(const json& status) {
    const std::optional<uint64_t> position = helix::virtual_sdcard_file_position(status);
    const json* print_stats = json_util::find_member(status, "print_stats");
    const json* filename = print_stats ? json_util::find_member(*print_stats, "filename") : nullptr;
    if (!position && !(filename && filename->is_string())) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (position) {
            file_position_ = position;
        }
        if (filename && filename->is_string()) {
            frame_filename_ = filename->get<std::string>();
        }
    }
    if (!position || !profile_ || !profile_->scripted_start()) {
        return;
    }
    flush_deferred_start_block_step();
    std::optional<helix::StartBlockStep> step;
    bool request = false;
    uint64_t run = 0;
    std::string file;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!active_.load() || current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        run = run_generation_.load();
        if (!start_block_requested_) {
            start_block_requested_ = true;
            request = true;
            file = frame_filename_;
        } else {
            step = start_block_.advance(*position);
        }
    }
    if (request) {
        auto self = shared_from_this();
        helix::ui::queue_update("PrintStartCollector::request_start_block",
                                [self, run, file = std::move(file)]() mutable {
                                    self->request_start_block(run, std::move(file));
                                });
    }
    if (step) {
        apply_start_block_step(*step, run);
    }
}

void PrintStartCollector::request_start_block(uint64_t run, std::string filename) {
    // The run's own frames name its file; the subject is the fallback for a
    // reprint, whose frames need not repeat an unchanged name.
    if (filename.empty()) {
        const char* subject =
            lv_subject_get_string(state_.print_state().get_print_filename_subject());
        filename = subject ? subject : "";
    }
    IMoonrakerAPI* api = get_moonraker_api();
    const bool readable = !filename.empty() && api && api->transfers().supports_range_reads();
    if (!active_.load() || run != run_generation_.load() || !readable) {
        if (filename.empty()) {
            // The filename arrives in the same frames as the position; the
            // next position asks again.
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (run == run_generation_.load()) {
                start_block_requested_ = false;
            }
        }
        return;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (run != run_generation_.load()) {
            return;
        }
        start_block_cancel_ = cancel;
    }
    // The answer lands on the transfer's thread, which must not hold the last
    // reference: the destructor deletes an LVGL timer.
    std::weak_ptr<PrintStartCollector> weak = weak_from_this();
    api->transfers().download_file_range(
        "gcodes", filename, 0, helix::kStartBlockHeadBytes,
        [weak, run, filename](const std::string& head) {
            auto self = weak.lock();
            if (!self) {
                return;
            }
            auto steps = helix::scan_start_block(head, head.size() >= helix::kStartBlockHeadBytes);
            std::optional<helix::StartBlockStep> step;
            {
                std::lock_guard<std::mutex> lock(self->state_mutex_);
                if (run != self->run_generation_.load()) {
                    return;
                }
                spdlog::info("[PrintStartCollector] Start block of {}: {} steps", filename,
                             steps.size());
                self->start_block_.set_steps(std::move(steps));
                if (self->file_position_) {
                    step = self->start_block_.advance(*self->file_position_);
                }
            }
            if (step) {
                self->apply_start_block_step(*step, run);
            }
            // The weak_ptr's lock may be the last reference; hand it to the
            // main thread so the destructor runs there.
            helix::ui::queue_update("PrintStartCollector::start_block_read", [self]() {});
        },
        [filename](const MoonrakerError& e) {
            spdlog::debug("[PrintStartCollector] Start block of {} not read: {}", filename,
                          e.message);
        },
        cancel);
}

void PrintStartCollector::apply_start_block_step(const helix::StartBlockStep& step, uint64_t run) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!active_.load() || run != run_generation_.load() ||
            current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        if (console_phase_time_ + kStartBlockConsoleGrace > helix::sim::SimulatedClock::now()) {
            if (profile_ && profile_->start_block_step_owns(step.command, console_phase_line_)) {
                spdlog::debug("[PrintStartCollector] Start-block step at {} left to its own "
                              "console line",
                              step.offset);
                deferred_step_.reset();
            } else {
                spdlog::debug("[PrintStartCollector] Start-block step at {} deferred past the "
                              "previous command's console line",
                              step.offset);
                deferred_step_ = step;
                deferred_step_run_ = run;
            }
            return;
        }
        deferred_step_.reset();
    }
    const PrintStartPhase phase = helix::start_block_step_phase(step.kind);
    const char* message = helix::print_start_phase_signal_message(phase);
    // These refine the phase's banner for one step kind (which homing, a mesh
    // that probes); they are not the phase's own text.
    switch (step.kind) {
    case helix::StartBlockStepKind::HomeAxes:
        message = lv_tr("Homing axes...");
        break;
    case helix::StartBlockStepKind::ProbeZ:
        message = lv_tr("Probing Z...");
        break;
    case helix::StartBlockStepKind::BedMesh:
        message = lv_tr("Bed mesh...");
        break;
    default:
        break;
    }
    spdlog::debug("[PrintStartCollector] Start-block step at {} -> phase {}", step.offset,
                  static_cast<int>(phase));
    PrintStartProfile::MatchResult match{phase, message, 0};
    apply_profile_match(match, /*marks_real_signal=*/true, /*live_state=*/true);
}

void PrintStartCollector::flush_deferred_start_block_step() {
    std::optional<helix::StartBlockStep> step;
    uint64_t run = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!deferred_step_ ||
            console_phase_time_ + kStartBlockConsoleGrace > helix::sim::SimulatedClock::now()) {
            return;
        }
        step = std::move(deferred_step_);
        deferred_step_.reset();
        run = deferred_step_run_;
    }
    apply_start_block_step(*step, run);
}

void PrintStartCollector::apply_wait_line(uint64_t gen, const helix::WaitLine& line) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (gen != wait_line_gen_) {
        return; // read for a position or wait that has since moved on
    }
    spdlog::info("[PrintStartCollector] Waiting command: kind {}, sensor '{}'",
                 static_cast<int>(line.kind), line.sensor);
    wait_line_ = line;
    wait_line_read_ = true;
}

std::string PrintStartCollector::chamber_wait_message(int temp_deci, int target_deci) {
    const int temp_c = static_cast<int>(std::lround(helix::units::from_decidegrees(temp_deci)));
    if (target_deci > 0) {
        return fmt::format(
            fmt::runtime(lv_tr("Waiting for chamber {} / {}°C")), temp_c,
            static_cast<int>(std::lround(helix::units::from_decidegrees(target_deci))));
    }
    return fmt::format(fmt::runtime(lv_tr("Waiting for chamber {}°C")), temp_c);
}

void PrintStartCollector::relabel_heating_phase(PrintStartPhase resolved, bool heater_wait,
                                                const std::string& label) {
    const std::string message =
        !label.empty()                             ? label
        : resolved == PrintStartPhase::HEATING_BED ? std::string(lv_tr("Heating Bed..."))
        : resolved == PrintStartPhase::SOAKING     ? std::string(lv_tr("Heating chamber..."))
                                                   : std::string(lv_tr("Heating Nozzle..."));
    int progress;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // CAS guard: the caller decided from a snapshot, and a background gcode
        // signal may have advanced current_phase_ (e.g. to QGL or PURGING)
        // since. Relabeling then would regress a newer, correct phase.
        const bool heating = current_phase_ == PrintStartPhase::HEATING_BED ||
                             current_phase_ == PrintStartPhase::HEATING_NOZZLE;
        const bool wait_label = heating || current_phase_ == PrintStartPhase::SOAKING;
        bool takeover = false;
        bool allowed;
        if (heater_wait) {
            // A wait displaces a firmware phase once, then moves between its
            // own heater labels. The firmware's heating phases keep the
            // bed-first correction.
            takeover = !heater_wait_shown_ && !wait_label &&
                       current_phase_ != PrintStartPhase::IDLE &&
                       current_phase_ != PrintStartPhase::PURGING &&
                       current_phase_ != PrintStartPhase::COMPLETE;
            allowed = takeover || (heater_wait_shown_ && wait_label);
        } else {
            // Still in a heating phase, or in a mesh that finished its work
            // while the nozzle heats to a target set after it began. The
            // caller leaves a wait's own label out.
            const bool mesh_gave_way =
                current_phase_ == PrintStartPhase::BED_MESH &&
                resolved == PrintStartPhase::HEATING_NOZZLE && !mesh_probing_locked() &&
                cached_ext_target_.load(std::memory_order_relaxed) > mesh_entry_ext_target_;
            allowed = heating || mesh_gave_way;
        }
        // A label carrying live readings follows them within the phase.
        if (!allowed ||
            (current_phase_ == resolved && (label.empty() || current_message_ == label))) {
            return;
        }
        if (current_phase_ == resolved) {
            // A label with live readings follows them once a degree.
            spdlog::debug("[PrintStartCollector] Heater wait label: '{}'", message);
        } else {
            spdlog::info("[PrintStartCollector] Heating correction: phase {} -> {}{}",
                         static_cast<int>(current_phase_), static_cast<int>(resolved),
                         heater_wait ? " (heater wait)" : "");
        }
        if (takeover) {
            heater_wait_shown_ = true;
            pre_wait_phase_ = current_phase_;
            pre_wait_message_ = current_message_;
        }
        progress = switch_phase_locked(resolved, message);
    }
    publish_switched_phase(resolved, message, progress);
}

void PrintStartCollector::end_heater_wait() {
    PrintStartPhase phase;
    std::string message;
    int progress;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!heater_wait_shown_) {
            return;
        }
        heater_wait_shown_ = false;
        // Restore only over the wait's own label: anything else got there later.
        if (current_phase_ != PrintStartPhase::HEATING_BED &&
            current_phase_ != PrintStartPhase::HEATING_NOZZLE &&
            current_phase_ != PrintStartPhase::SOAKING) {
            return;
        }
        // A finished mesh gives way for good to a nozzle target raised after
        // it began, as the firmware-heating relabel treats it, once the bed
        // is ready too: the nozzle label stays.
        if (pre_wait_phase_ == PrintStartPhase::BED_MESH &&
            current_phase_ == PrintStartPhase::HEATING_NOZZLE &&
            cached_ext_target_.load(std::memory_order_relaxed) > mesh_entry_ext_target_ &&
            heating_target_reached_locked(PrintStartPhase::HEATING_BED)) {
            return;
        }
        phase = pre_wait_phase_;
        message = pre_wait_message_;
        progress = switch_phase_locked(phase, message);
    }
    spdlog::info("[PrintStartCollector] Heater wait ended, back to phase {} '{}'",
                 static_cast<int>(phase), message);
    publish_switched_phase(phase, message, progress);
}

int PrintStartCollector::switch_phase_locked(PrintStartPhase phase, const std::string& message) {
    maybe_reset_for_mesh_subphase_locked(phase, message);
    enter_phase_locked(phase);
    current_message_ = message;
    detected_phases_.insert(phase);
    // A sequential profile's progress moves only with its own signals.
    if (profile_ && profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
        return max_sequential_progress_;
    }
    // With a prediction, update_eta_display() owns progress.
    return predictor_.has_predictions() ? -1 : calculate_progress_locked();
}

void PrintStartCollector::publish_switched_phase(PrintStartPhase phase, const std::string& message,
                                                 int progress) {
    if (progress < 0) {
        auto* subj = state_.print_state().get_print_start_progress_subject();
        progress = subj ? lv_subject_get_int(subj) : 0;
    }
    state_.print_state().set_print_start_state(phase, message.c_str(), progress);
}

bool PrintStartCollector::mesh_probing_locked() const {
    return current_phase_ == PrintStartPhase::BED_MESH &&
           (mesh_probe_current_ > 0 || mesh_points_.points() > 0) &&
           helix::sim::SimulatedClock::now() - mesh_last_probe_time_ < kMeshProbeGapReset;
}

void PrintStartCollector::set_profile(std::shared_ptr<PrintStartProfile> profile) {
    if (active_.load()) {
        spdlog::warn("[PrintStartCollector] set_profile() called while active, ignoring");
        return;
    }
    profile_ = std::move(profile);
    if (profile_) {
        spdlog::debug("[PrintStartCollector] Using profile: {}", profile_->name());
    } else {
        spdlog::info("[PrintStartCollector] No profile set, signal/pattern matching disabled");
    }
}

int PrintStartCollector::calculate_progress() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return calculate_progress_locked();
}

int PrintStartCollector::calculate_progress_locked() const {
    if (predicted_phase_weights_.empty()) {
        return 0;
    }

    float progress = 0.0f;

    // Completed phases (not the current one)
    for (const auto& phase : detected_phases_) {
        if (phase == current_phase_)
            continue;
        auto it = predicted_phase_weights_.find(static_cast<int>(phase));
        if (it != predicted_phase_weights_.end()) {
            float share = it->second;
            if ((phase == PrintStartPhase::HEATING_BED ||
                 phase == PrintStartPhase::HEATING_NOZZLE) &&
                !heating_target_reached_locked(phase)) {
                // Concurrent heating: credit the phase by its live attainment
                // until the target lands, not the full share the moment the
                // chain's marker passes.
                share *= compute_heating_fraction_for_locked(phase);
            }
            progress += share;
        }
    }
    for (const auto& [phase, share] : predicted_phase_weights_) {
        if (phase_skipped_locked(phase)) {
            progress += share;
        }
    }

    // Partial progress for current phase
    auto cur_it = predicted_phase_weights_.find(static_cast<int>(current_phase_));
    if (cur_it != predicted_phase_weights_.end()) {
        float phase_frac = 0.0f;

        if (current_phase_ == PrintStartPhase::HEATING_BED ||
            current_phase_ == PrintStartPhase::HEATING_NOZZLE) {
            phase_frac = compute_heating_fraction();
        } else if (current_phase_ == PrintStartPhase::BED_MESH && mesh_probe_total_ > 0) {
            // Use exact probe count for deterministic progress
            phase_frac = std::min(static_cast<float>(mesh_probe_current_) /
                                      static_cast<float>(mesh_probe_total_),
                                  0.95f);
        } else {
            // Time-based for non-heating phases
            const float elapsed = static_cast<float>(phase_seconds_locked(current_phase_).count());
            auto pred = predictor_.predicted_phases();
            auto pred_it = pred.find(static_cast<int>(current_phase_));
            if (pred_it != pred.end() && pred_it->second > 0) {
                phase_frac = std::min(elapsed / static_cast<float>(pred_it->second), 0.95f);
            }
        }
        progress += cur_it->second * phase_frac;
    }

    return std::min(static_cast<int>(progress * 95.0f), 95);
}

bool PrintStartCollector::is_print_start_marker(const std::string& line) {
    return helix::regex_search(line, print_start_pattern_);
}

bool PrintStartCollector::is_completion_marker(const std::string& line) {
    return helix::regex_search(line, completion_pattern_);
}

// ============================================================================
// COMPLETED-PHASE SET
// ============================================================================

std::set<int> PrintStartCollector::get_completed_phase_ints_locked() const {
    std::set<int> result;
    for (const auto& phase : detected_phases_) {
        int p = static_cast<int>(phase);
        // Exclude current phase from "completed" set
        if (phase != current_phase_ && phase != PrintStartPhase::IDLE &&
            phase != PrintStartPhase::INITIALIZING) {
            // Concurrent-heat firmware (K1/K2 class) interleaves heating with
            // homing, wiping and meshing: the chain moving past a heating
            // phase does NOT mean the heater finished. A heating phase joins
            // the completed set only at its target.
            if ((phase == PrintStartPhase::HEATING_BED ||
                 phase == PrintStartPhase::HEATING_NOZZLE) &&
                !heating_target_reached_locked(phase)) {
                continue;
            }
            result.insert(p);
        }
    }
    for (const auto& [phase, _] : predicted_phase_weights_) {
        if (phase_skipped_locked(phase)) {
            result.insert(phase);
        }
    }
    return result;
}

bool PrintStartCollector::phase_skipped_locked(int phase) const {
    // The enum is not every macro's order: K1, K2, QIDI and AD5M starts clean
    // the nozzle before they mesh, and a bucket purge or filament flush can
    // run before leveling. A phase this printer's own history recorded may
    // still come at any point until COMPLETE, so with history nothing is
    // skipped.
    if (predictor_.has_predictions()) {
        return false;
    }
    const auto p = static_cast<PrintStartPhase>(phase);
    return phase < static_cast<int>(current_phase_) && p != PrintStartPhase::HEATING_BED &&
           p != PrintStartPhase::HEATING_NOZZLE &&
           detected_phases_.find(p) == detected_phases_.end();
}

int PrintStartCollector::get_current_phase_elapsed_seconds() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return static_cast<int>(phase_seconds_locked(current_phase_).count());
}

void PrintStartCollector::enter_phase_locked(PrintStartPhase phase) {
    if (owning_step_.judge(phase) == helix::OwningStep::Verdict::Ends) {
        owning_step_ = {};
    }
    if (phase != current_phase_) {
        if (std::find(phases_left_.begin(), phases_left_.end(), current_phase_) ==
            phases_left_.end()) {
            phases_left_.push_back(current_phase_);
        }
        const auto now = helix::sim::SimulatedClock::now();
        phase_time_spent_[static_cast<int>(current_phase_)] += now - phase_since_;
        phase_since_ = now;
    }
    current_phase_ = phase;
}

void PrintStartCollector::set_macro_analysis(const helix::PrintStartAnalysis& analysis) {
    std::vector<PrintStartPhase> phases;
    for (const auto& op : analysis.operations) {
        auto phase = helix::phase_of_category(op.category);
        if (phase && std::find(phases.begin(), phases.end(), *phase) == phases.end()) {
            phases.push_back(*phase);
        }
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    macro_expected_phases_ = std::move(phases);
}

helix::PreprintChecklistInput PrintStartCollector::checklist_input() const {
    helix::PreprintChecklistInput in;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (PrintStartPhase p : phases_left_) {
        if (p != current_phase_ && p != PrintStartPhase::IDLE &&
            p != PrintStartPhase::INITIALIZING) {
            in.done_in_order.push_back(p);
            in.actual_seconds[static_cast<int>(p)] =
                static_cast<int>(phase_seconds_locked(p).count());
        }
    }
    in.current = current_phase_;
    in.current_elapsed_seconds = static_cast<int>(phase_seconds_locked(current_phase_).count());
    if (start_block_.has_steps()) {
        for (const auto& step : start_block_.steps()) {
            in.expected_order.push_back(helix::start_block_step_phase(step.kind));
        }
    } else {
        in.expected_order = macro_expected_phases_;
    }
    in.predicted_seconds = predictor_.predicted_phases();
    return in;
}

std::chrono::seconds PrintStartCollector::phase_seconds_locked(PrintStartPhase phase) const {
    if (phase == PrintStartPhase::IDLE || phase == PrintStartPhase::INITIALIZING ||
        phase == PrintStartPhase::COMPLETE) {
        return std::chrono::seconds::zero();
    }
    helix::sim::SimulatedClock::duration spent{};
    if (const auto it = phase_time_spent_.find(static_cast<int>(phase));
        it != phase_time_spent_.end()) {
        spent = it->second;
    }
    if (phase == current_phase_) {
        spent += helix::sim::SimulatedClock::now() - phase_since_;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(spent);
}

// ============================================================================
// ETA DISPLAY UPDATE
// ============================================================================

void PrintStartCollector::update_eta_display() {
    if (!active_.load()) {
        return;
    }

    // Always update elapsed time since preparation started
    int total_elapsed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto now = helix::sim::SimulatedClock::now();
        total_elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - printing_state_start_).count());
    }
    state_.print_state().set_preprint_elapsed_seconds(total_elapsed);
    feed_thermal_sample();

    // Compute remaining from composite weights (thermal model + predictor).
    // This handles heating phases correctly (predictor alone doesn't track them).
    int remaining;
    int predicted_total;
    int current_snap = 0;
    int phase_elapsed_snap = 0;
    bool mesh_live_snap = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (predicted_phase_weights_.empty()) {
            return;
        }

        auto completed = get_completed_phase_ints_locked();
        int current = static_cast<int>(current_phase_);
        const int phase_elapsed = static_cast<int>(phase_seconds_locked(current_phase_).count());

        // Compute remaining from composite per-phase durations (weights * total)
        float remaining_f = 0.0f;
        for (const auto& [phase, weight] : predicted_phase_weights_) {
            float phase_dur = weight * predicted_total_seconds_;
            if (completed.count(phase) && phase != current) {
                continue; // completed phase
            } else if (phase == current) {
                // Current phase: use heating fraction for heating, probe
                // extrapolation for bed mesh, elapsed for others
                if (current == static_cast<int>(PrintStartPhase::HEATING_BED) ||
                    current == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                    float frac = compute_heating_fraction();
                    remaining_f += phase_dur * (1.0f - frac);
                } else if (current == static_cast<int>(PrintStartPhase::BED_MESH) &&
                           mesh_probe_total_ > 0 && mesh_seconds_per_probe_ > 0.0f) {
                    // Live per-probe time extrapolation overrides historical prediction
                    int probes_left = mesh_probe_total_ - mesh_probe_current_;
                    remaining_f += mesh_seconds_per_probe_ * static_cast<float>(probes_left);
                } else {
                    remaining_f += std::max(0.0f, phase_dur - static_cast<float>(phase_elapsed));
                }
            } else {
                // Future phase. A heating phase the chain already passed but
                // whose heater is still climbing (concurrent-heat firmware)
                // only owes its unheated fraction, not the full duration.
                if (phase == static_cast<int>(PrintStartPhase::HEATING_BED) ||
                    phase == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                    float frac =
                        compute_heating_fraction_for_locked(static_cast<PrintStartPhase>(phase));
                    remaining_f += phase_dur * (1.0f - frac);
                } else {
                    remaining_f += phase_dur; // future phase
                }
            }
        }
        // Live mesh extrapolation: the per-probe timing is armed and, for the
        // current BED_MESH phase, it IS the remaining contribution — through
        // the in-loop branch when history recorded the phase, or the
        // unrecorded-phase fallback right below.
        mesh_live_snap = current == static_cast<int>(PrintStartPhase::BED_MESH) &&
                         mesh_probe_total_ > 0 && mesh_seconds_per_probe_ > 0.0f;
        // A phase the printer runs but predictor history never mapped has no
        // weights key, so the loop above contributed nothing for the mesh
        // under way. No double count: the in-loop current-phase branch only
        // runs when the key exists.
        if (mesh_live_snap &&
            predicted_phase_weights_.find(current) == predicted_phase_weights_.end()) {
            remaining_f += mesh_seconds_per_probe_ *
                           static_cast<float>(mesh_probe_total_ - mesh_probe_current_);
        }
        // First tick where live extrapolation becomes the mesh's contribution:
        // every publish before probe #2 armed the timing was a provisional
        // guess, and the anchor those seeded would clamp the first honest
        // value back down forever. Release it once, under the same lock the
        // segment-change clears hold — arming off a stale snapshot after the
        // WebSocket thread zeroed the telemetry for a new segment would clamp
        // that segment's first honest value. After the release the estimate
        // only decreases, so the monotonic guards re-engage normally.
        if (mesh_live_snap && !mesh_extrapolation_armed_) {
            mesh_extrapolation_armed_ = true;
            last_remaining_ = 0;
        }
        remaining = static_cast<int>(remaining_f);

        predicted_total = static_cast<int>(predicted_total_seconds_);
        current_snap = current;
        phase_elapsed_snap = phase_elapsed;
    }

    // --- Diagnostic: monotonic bias data collection ---
    {
        auto pred_phases = predictor_.predicted_phases();
        int cur_phase_predicted = 0;
        auto pp_it = pred_phases.find(current_snap);
        if (pp_it != pred_phases.end())
            cur_phase_predicted = pp_it->second;

        spdlog::debug("[PrintStartCollector] ETA update: phase={} elapsed={}s, remaining={}s, "
                      "predicted_total={:.0f}s, phase_predicted={}s",
                      current_snap, phase_elapsed_snap, remaining, predicted_total_seconds_,
                      cur_phase_predicted);
    }

    // Always update the int subject for print time integration
    // Monotonic bias: once under 2 minutes, only decrease unless overrun > 20%
    if (remaining < 120 && last_remaining_ > 0 && last_remaining_ < 120) {
        if (remaining > last_remaining_) {
            float overrun_pct = static_cast<float>(remaining - last_remaining_) /
                                std::max(1.0f, predicted_total_seconds_) * 100.0f;
            if (overrun_pct < 20.0f) {
                auto pred_phases = predictor_.predicted_phases();
                int cur_phase_predicted = 0;
                auto pp_it = pred_phases.find(current_snap);
                if (pp_it != pred_phases.end())
                    cur_phase_predicted = pp_it->second;
                float per_phase_overrun_pct =
                    static_cast<float>(remaining - last_remaining_) /
                    std::max(1.0f, static_cast<float>(cur_phase_predicted)) * 100.0f;
                int remaining_before_bias = remaining;
                remaining = last_remaining_;
                spdlog::debug("[PrintStartCollector] Monotonic bias: suppressed {}s→{}s, "
                              "overrun={:.1f}% (total denom={:.0f}s), "
                              "per-phase would be {:.1f}% (phase denom={}s)",
                              last_remaining_, remaining_before_bias, overrun_pct,
                              predicted_total_seconds_, per_phase_overrun_pct, cur_phase_predicted);
            }
        }
    }
    // Strict monotonic guard: remaining time should never increase.
    // Weight recomputation (e.g. new heater target discovered) can cause
    // a large upward spike that looks jarring on screen. The underlying
    // estimate will naturally catch up as heating progresses.
    if (last_remaining_ > 0 && remaining > last_remaining_) {
        spdlog::debug("[PrintStartCollector] Clamping remaining {}s→{}s (monotonic)", remaining,
                      last_remaining_);
        remaining = last_remaining_;
    }
    // Downward rate limit: a phase change recomputes `remaining` from per-phase
    // weights, which can collapse the estimate in a single tick (e.g. 111→57→31)
    // — that reads as broken on screen. Cap each tick's decrease (proportional,
    // so a large estimate still converges from an overestimate while a small one
    // glides) so the number eases down instead of snapping. The COMPLETE
    // transition snaps to the print view, so a brief tail-end overshoot is fine.
    if (last_remaining_ > 0 && remaining < last_remaining_) {
        int max_drop = std::max(15, last_remaining_ / 6);
        if (last_remaining_ - remaining > max_drop) {
            spdlog::debug(
                "[PrintStartCollector] Easing remaining {}s→{}s (cap {}s/tick, target {}s)",
                last_remaining_, last_remaining_ - max_drop, max_drop, remaining);
            remaining = last_remaining_ - max_drop;
        }
    }
    last_remaining_ = remaining;

    state_.print_state().set_preprint_remaining_seconds(remaining);
    if (predicted_total > 0) {
        int effective_progress = calculate_progress();
        // Monotonic progress: never go backwards. When weights recompute
        // (e.g. new heater target discovered), the progress may recalculate
        // lower against a larger predicted total. Users should never see
        // the progress bar regress.
        auto* subj = state_.print_state().get_print_start_progress_subject();
        if (subj) {
            int current_progress = lv_subject_get_int(subj);
            effective_progress = std::max(effective_progress, current_progress);
            if (effective_progress != current_progress) {
                lv_subject_set_int(subj, effective_progress);
            }
        }
    }

    if (remaining <= 0) {
        state_.print_state().set_print_start_time_left("Almost ready");
        return;
    }

    // Round for stable display, then format as "~X min left" or "~X:XX left"
    int display_remaining = helix::format::round_eta_seconds(remaining);
    std::string text = "~" + helix::format::duration_remaining(display_remaining);
    state_.print_state().set_print_start_time_left(text.c_str());

    spdlog::trace("[PrintStartCollector] ETA: {}s remaining, predicted_total={}s", remaining,
                  predicted_total);
}

// ============================================================================
// PREDICTION HISTORY PERSISTENCE
// ============================================================================

void PrintStartCollector::load_prediction_history() {
    auto entries = helix::PreprintPredictor::load_entries_from_config();

    // Cold (1) vs Warm (2) based on current bed temp
    int bed_temp = helix::ui::temperature::deci_to_degrees(
        lv_subject_get_int(state_.temperature_state().get_bed_temp_subject()));
    int temp_bucket = helix::PreprintPredictor::temp_bucket_for(static_cast<float>(bed_temp));

    std::lock_guard<std::mutex> lock(state_mutex_);
    predictor_.load_entries(entries, temp_bucket, window_);
    loaded_temp_bucket_ = temp_bucket;

    if (!entries.empty()) {
        spdlog::debug(
            "[PrintStartCollector] Loaded {}/{} prediction entries for the {} / {} "
            "bucket (predicted total: {}s)",
            predictor_.get_entries().size(), entries.size(), temp_bucket == 2 ? "warm" : "cold",
            window_ == helix::PreprintWindow::HostPreStart ? "host-pre-start" : "printer-edge",
            predictor_.predicted_total());
    }
}

void PrintStartCollector::feed_thermal_sample() {
    auto& mgr = ThermalRateManager::instance();
    auto now_ms =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  helix::sim::SimulatedClock::now() - printing_state_start_)
                                  .count());

    PrintStartPhase phase;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
    }

    if (phase == PrintStartPhase::HEATING_BED || phase == PrintStartPhase::HEATING_NOZZLE) {
        // Temps are in decidegrees (value * 10)
        auto& temps = state_.temperature_state();
        const auto feed = [&](const char* heater, lv_subject_t* temp, lv_subject_t* target) {
            auto& model = mgr.get_model(heater);
            const float t = helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(temp));
            model.note_target(helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(target)),
                              t, now_ms);
            model.record_sample(t, now_ms);
        };
        feed("extruder", temps.get_active_extruder_temp_subject(),
             temps.get_active_extruder_target_subject());
        feed("heater_bed", temps.get_bed_temp_subject(), temps.get_bed_target_subject());
    }
}

void PrintStartCollector::compute_predicted_weights() {
    auto& mgr = ThermalRateManager::instance();

    int ext_target = helix::ui::temperature::deci_to_degrees(
        lv_subject_get_int(state_.temperature_state().get_active_extruder_target_subject()));
    int bed_target = helix::ui::temperature::deci_to_degrees(
        lv_subject_get_int(state_.temperature_state().get_bed_target_subject()));

    helix::PreprintHeating heating;
    heating.nozzle_seconds = mgr.estimate_heating_seconds(
        "extruder", static_cast<float>(start_ext_temp_), static_cast<float>(ext_target));
    heating.bed_seconds = mgr.estimate_heating_seconds(
        "heater_bed", static_cast<float>(start_bed_temp_), static_cast<float>(bed_target));
    heating.nozzle_target_known = ext_target > 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const helix::PreprintEstimate estimate = predictor_.estimate(
            heating, phase_overrides_.value_or(helix::PreprintPhaseOverrides{}));
        // Recorded into this start's history entry so the predictor can split
        // the constant part of a start from the heating that start owed.
        priced_heating_seconds_ = heating.priced_seconds();

        float durations_sum = 0.0f;
        for (const auto& [_, dur] : estimate.durations) {
            durations_sum += dur;
        }

        // Phase weights are fractions of the durations' sum so they always
        // sum to 1.0, whatever total the display uses.
        predicted_phase_weights_.clear();
        if (durations_sum > 0.0f) {
            for (const auto& [phase, dur] : estimate.durations) {
                predicted_phase_weights_[phase] = dur / durations_sum;
            }
        }
        predicted_total_seconds_ = estimate.total_seconds;
    }

    // Track targets and rates used so we can detect changes and recompute
    weights_ext_target_ = ext_target;
    weights_bed_target_ = bed_target;
    weights_ext_measured_ = mgr.get_model("extruder").measured_rate().has_value();
    weights_bed_measured_ = mgr.get_model("heater_bed").measured_rate().has_value();

    spdlog::debug("[PrintStartCollector] Predicted weights: total={:.0f}s, {} phases "
                  "(ext_target={}°C, bed_target={}°C)",
                  predicted_total_seconds_, predicted_phase_weights_.size(), ext_target,
                  bed_target);
}

float PrintStartCollector::compute_heating_fraction() const {
    return compute_heating_fraction_for_locked(current_phase_);
}

float PrintStartCollector::compute_heating_fraction_for_locked(helix::PrintStartPhase phase) const {
    // Use cached temps (thread-safe atomics) instead of LVGL subjects
    int ext_temp = cached_ext_temp_.load(std::memory_order_relaxed);
    int ext_target = cached_ext_target_.load(std::memory_order_relaxed);
    int bed_temp = cached_bed_temp_.load(std::memory_order_relaxed);
    int bed_target = cached_bed_target_.load(std::memory_order_relaxed);

    if (phase == PrintStartPhase::HEATING_NOZZLE && ext_target > 0) {
        float start = start_ext_temp_ > 0 ? static_cast<float>(start_ext_temp_) : 25.0f;
        float range = static_cast<float>(ext_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(ext_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    if (phase == PrintStartPhase::HEATING_BED && bed_target > 0) {
        float start = start_bed_temp_ > 0 ? static_cast<float>(start_bed_temp_) : 25.0f;
        float range = static_cast<float>(bed_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(bed_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    return 0.0f;
}

bool PrintStartCollector::at_target(int temp, int target) {
    constexpr int kWithinDegrees = 2;
    // Target unknown (never observed): we cannot judge, so fall back to
    // the marker-pass completion semantics rather than stalling the phase.
    return target <= 0 || temp >= target - kWithinDegrees;
}

bool PrintStartCollector::heating_target_reached_locked(helix::PrintStartPhase phase) const {
    if (phase == PrintStartPhase::HEATING_NOZZLE) {
        return at_target(cached_ext_temp_.load(std::memory_order_relaxed),
                         cached_ext_target_.load(std::memory_order_relaxed));
    }
    if (phase == PrintStartPhase::HEATING_BED) {
        return at_target(cached_bed_temp_.load(std::memory_order_relaxed),
                         cached_bed_target_.load(std::memory_order_relaxed));
    }
    return true; // not a heating phase — completion is not temp-gated
}

void PrintStartCollector::query_mesh_probe_count() {
    if (!active_.load())
        return;

    auto self = shared_from_this();
    // Query both bed_mesh (for probed_matrix from last run — authoritative point count,
    // handles adaptive meshing where probe_count config overstates actual probes)
    // and configfile settings (fallback when no prior mesh exists).
    json params = {{"objects", json::object({{"bed_mesh", nullptr},
                                             {"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [self](const json& response) {
            if (!self->active_.load())
                return;

            int total = 0;
            const char* source = "";

            // Printers using adaptive bed-mesh probing (Snapmaker U1, KAMP) probe a
            // variable, slicer-determined number of points — and on the U1 the mesh
            // is split across several sub-phases (Detecting Plate, Pre-scanning,
            // Levelling), none of which matches a stored grid. Neither a prior
            // probed_matrix nor the configfile probe_count predicts the live count,
            // so we set NO total and show "Bed Mesh (N)" — a denominator here just
            // lies (e.g. "(3/16)" when the U1 never probes 16). Skip both sources.
            const bool adaptive = self->profile_ && self->profile_->adaptive_meshing();

            // Prefer probed_matrix dimensions from last completed mesh (fixed-grid
            // printers only). probe_count config overstates on adaptive printers.
            // A missing or wrong-typed result/status chain leaves total at 0 and
            // falls through to the configfile probe_count fallback below.
            const json* result = json_util::find_member(response, "result");
            const json* status =
                result != nullptr ? json_util::find_member(*result, "status") : nullptr;
            if (!adaptive && status != nullptr) {
                if (status->contains("bed_mesh") &&
                    (*status)["bed_mesh"].contains("probed_matrix")) {
                    const auto& pm = (*status)["bed_mesh"]["probed_matrix"];
                    if (pm.is_array() && !pm.empty() && pm[0].is_array()) {
                        int rows = static_cast<int>(pm.size());
                        int cols = static_cast<int>(pm[0].size());
                        if (rows > 0 && cols > 0) {
                            total = rows * cols;
                            source = "probed_matrix";
                        }
                    }
                }
            }

            // Skip configfile probe_count fallback for printers using adaptive
            // bed-mesh probing — the slicer passes MESH_MIN/MESH_MAX overrides
            // and the configured grid (e.g. 13x13 = 169) overstates the actual
            // probe count by ~5x. Better to show "Bed Mesh (N)" without total
            // than "Bed Mesh (5/169)" lying.
            const bool skip_config_fallback = adaptive;
            if (total == 0 && !skip_config_fallback && status != nullptr) {
                const json* configfile = json_util::find_member(*status, "configfile");
                const json* settings = configfile != nullptr
                                           ? json_util::find_member(*configfile, "settings")
                                           : nullptr;
                if (settings != nullptr && settings->contains("bed_mesh") &&
                    (*settings)["bed_mesh"].contains("probe_count")) {
                    const auto& pc = (*settings)["bed_mesh"]["probe_count"];
                    if (pc.is_array() && pc.size() >= 2) {
                        total = json_util::as_int(pc[0], 0) * json_util::as_int(pc[1], 0);
                    } else if (pc.is_number_integer()) {
                        int n = pc.template get<int>();
                        total = n * n; // square grid
                    }
                    source = "probe_count";
                }
            }

            // A count the firmware already reported for this sweep (adapted
            // grid or "Probing point N/M") is exact; this one is a guess.
            if (total > 0) {
                std::lock_guard<std::mutex> lock(self->state_mutex_);
                if (self->mesh_probe_total_ > 0) {
                    spdlog::debug("[PrintStartCollector] Keeping reported mesh total {} over {} "
                                  "from {}",
                                  self->mesh_probe_total_, total, source);
                    return;
                }
                self->mesh_probe_total_ = total;
                spdlog::info("[PrintStartCollector] Mesh point total from {}: {}", source, total);
            }
        },
        [](const MoonrakerError& /*err*/) {
            spdlog::debug("[PrintStartCollector] Failed to query bed_mesh point count");
        });
}

void PrintStartCollector::save_prediction_entry() {
    // A timeout completion cuts phases short wherever they stood, so its
    // timings would teach the predictor a shorter pre-print than the real one.
    // The heating rates came from real samples along the way and stay valid.
    if (fallback_completion_) {
        spdlog::debug("[PrintStartCollector] Timeout completion: saving heating rates only");
        helix::ui::queue_update("PrintStartCollector::save_prediction_entry", []() {
            auto* cfg = Config::get_instance();
            ThermalRateManager::instance().save_to_config(*cfg);
        });
        return;
    }

    // Compute per-phase durations from timestamps
    std::map<int, int> phase_durations;
    auto now = helix::sim::SimulatedClock::now();
    helix::sim::SimulatedClock::time_point start_time;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time = printing_state_start_;

        // Heating phases are tracked by ThermalRateModel, not the predictor.
        // A phase entered within the second it left (G28 followed at once by
        // proactive HEATING_BED detection on AD5M) records nothing: HOMING=0
        // entries would poison the MAD filter and suppress the default-floor
        // fallback in compute_predicted_weights().
        for (int phase_int = 0; phase_int < helix::print_start_phase_count(); ++phase_int) {
            const auto phase = static_cast<PrintStartPhase>(phase_int);
            if (phase == PrintStartPhase::HEATING_BED || phase == PrintStartPhase::HEATING_NOZZLE) {
                continue;
            }
            const int duration = static_cast<int>(phase_seconds_locked(phase).count());
            if (duration > 0) {
                phase_durations[phase_int] = duration;
            }
        }

        // --- Diagnostic: predicted vs actual per-phase ---
        auto predicted = predictor_.predicted_phases();
        spdlog::debug("[PrintStartCollector] Print complete — predicted vs actual:");
        for (const auto& [phase, actual] : phase_durations) {
            auto p_it = predicted.find(phase);
            int pred = (p_it != predicted.end()) ? p_it->second : 0;
            float error_pct =
                pred > 0 ? ((static_cast<float>(actual) - pred) / pred * 100.0f) : 0.0f;
            spdlog::debug(
                "[PrintStartCollector]   phase {}: predicted={}s actual={}s error={:.1f}%", phase,
                pred, actual, error_pct);
        }
    }

    // Use wall-clock elapsed time as total: it includes heating phases omitted from
    // phase_durations. A start that only heated has no phase timings, but its total is still what
    // the next prediction needs, and its heating rates are still real.
    int wall_clock_total = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count());
    if (wall_clock_total <= 0) {
        spdlog::debug("[PrintStartCollector] No pre-print time to save");
        return;
    }

    // Cold (1) vs Warm (2) based on bed temp at start
    helix::PreprintEntry entry;
    entry.total_seconds = wall_clock_total;
    entry.timestamp = static_cast<int64_t>(std::time(nullptr));
    entry.phase_durations = std::move(phase_durations);
    entry.temp_bucket =
        helix::PreprintPredictor::temp_bucket_for(static_cast<float>(start_bed_temp_));
    entry.window = window_;
    if (priced_heating_seconds_) {
        entry.heating_seconds = static_cast<int>(std::round(*priced_heating_seconds_));
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase_overrides_) {
            entry.skipped_phases = helix::PreprintPredictor::phases_skipped_by(*phase_overrides_);
        }
        predictor_.add_entry(entry);
    }

    helix::ui::queue_update("PrintStartCollector::save_prediction_entry", [entry]() {
        auto* cfg = Config::get_instance();
        helix::PreprintPredictor::append_to_config(entry);
        ThermalRateManager::instance().save_to_config(*cfg);
        cfg->save();
        spdlog::debug("[PrintStartCollector] Saved prediction history entry ({}s)",
                      entry.total_seconds);
    });
}
