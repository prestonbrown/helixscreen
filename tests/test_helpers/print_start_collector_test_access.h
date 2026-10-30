// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_start_collector.h"

class PrintStartCollectorTestAccess {
  public:
    /// Answer the current wait's read of the command at the print position.
    static void answer_wait_line(PrintStartCollector& c, const helix::WaitLine& line) {
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(c.state_mutex_);
            gen = c.wait_line_gen_;
        }
        c.apply_wait_line(gen, line);
    }
    /// The kind of the command read for the current wait; nullopt until a
    /// read for the current position answers.
    static std::optional<helix::WaitKind> wait_line_kind(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        if (!c.wait_line_read_) {
            return std::nullopt;
        }
        return c.wait_line_.kind;
    }

    static bool start_macro_seen_zero(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.start_macro_seen_zero_;
    }

    static bool is_heater_wait_report(const std::string& line) {
        return PrintStartCollector::is_heater_wait_report(line);
    }

    static bool is_print_start_marker(const std::string& line) {
        return PrintStartCollector::is_print_start_marker(line);
    }

    static bool is_completion_marker(const std::string& line) {
        return PrintStartCollector::is_completion_marker(line);
    }

    static bool is_respond_completion(const std::string& line) {
        return PrintStartCollector::is_respond_completion(line);
    }

    /// Wind back the start time to simulate elapsed seconds.
    ///
    /// Also winds back both activity stamps, so this models "N seconds passed
    /// and the printer showed nothing" — the case the timeouts exist for. Use
    /// set_last_activity_seconds_ago() afterwards to model a printer that is
    /// still working on its pre-print.
    static void set_elapsed_seconds(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        auto when = helix::sim::SimulatedClock::now() - std::chrono::seconds(seconds);
        c.printing_state_start_ = when;
        c.last_signal_time_ = when;
        c.last_inferred_activity_time_ = when;
    }

    /// Set how long ago the printer last showed pre-print activity of every
    /// kind (a matched line, a probe line, a climbing heater, a status-signal
    /// rule), independent of total elapsed time.
    static void set_last_activity_seconds_ago(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        const auto when = helix::sim::SimulatedClock::now() - std::chrono::seconds(seconds);
        c.last_signal_time_ = when;
        c.last_inferred_activity_time_ = when;
    }

    /// Set predicted_total_seconds_ directly for timeout threshold tests
    static void set_predicted_total(PrintStartCollector& c, float seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        c.predicted_total_seconds_ = seconds;
    }

    /// Read predicted_total_seconds_
    static float get_predicted_total(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.predicted_total_seconds_;
    }

    /// Wind back the silent-progression "temps ready" timestamp to simulate
    /// elapsed seconds since temps became ready. The collector sets this on
    /// the first tick where bed+nozzle reach target — bypassing the natural
    /// path keeps tests deterministic.
    static void set_temps_ready_elapsed_seconds(PrintStartCollector& c, int seconds) {
        c.temps_ready_time_ = helix::sim::SimulatedClock::now() - std::chrono::seconds(seconds);
    }

    /// Reset silent_progression_idx_ to N (default 0) so a test can rewind
    /// after a fired entry.
    static void set_silent_progression_idx(PrintStartCollector& c, size_t idx) {
        c.silent_progression_idx_ = idx;
    }

    /// Read mesh_probe_total_ — used to verify the "Adapted probe count"
    /// gcode parser updates the live denominator.
    static int get_mesh_probe_total(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.mesh_probe_total_;
    }

    /// Directly invoke the private CAS-guarded heating relabel. In production
    /// check_fallback_completion() calls this with a phase snapshot that a
    /// concurrent bg gcode signal may have invalidated; a test uses this to
    /// prove the guard refuses when current_phase_ has advanced past heating.
    static void relabel_heating_phase(PrintStartCollector& c, helix::PrintStartPhase resolved) {
        c.relabel_heating_phase(resolved);
    }

    /// Run one private ETA publish (update_eta_display is timer-driven in
    /// production). Does NOT run check_fallback_completion — tests call that
    /// public method separately to control exactly when weights recompute.
    static void run_eta_update(PrintStartCollector& c) {
        c.update_eta_display();
    }

    /// Fire the collector's periodic ETA timer once, as LVGL would.
    static void fire_eta_timer(PrintStartCollector& c) {
        if (!c.eta_timer_) {
            return;
        }
        lv_timer_ready(c.eta_timer_);
        lv_timer_handler();
    }

    /// Drop all predictor history so compute_predicted_weights() takes the
    /// theoretical-durations path (matches a first print / empty bucket).
    /// start() reloads history from Config, so call this AFTER start().
    static void clear_prediction_history(PrintStartCollector& c) {
        c.predictor_.load_entries({}, 0, helix::PreprintWindow::Unknown);
    }

    /// Read the counted mesh probes (the "N" in the displayed "N / M").
    static int get_mesh_probe_current(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.mesh_probe_current_;
    }

    /// Set the monotonic anchor directly. Production sets it on every ETA
    /// publish; a test uses this to model a prior low estimate without
    /// replaying the whole ease-down that produced it.
    static void set_last_remaining(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        c.last_remaining_ = seconds;
    }

    /// Read the monotonic anchor.
    static int get_last_remaining(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.last_remaining_;
    }

    /// Whether the mesh-extrapolation anchor release is armed for the mesh
    /// segment under way. A pure latch with no externally visible state until
    /// the next publish, so the per-run reset is pinned by reading it.
    static bool is_mesh_extrapolation_armed(PrintStartCollector& c) {
        return c.mesh_extrapolation_armed_;
    }

    /// Hand the collector a phase-object status as the answer to its
    /// start-up query, whenever that answer arrives.
    static void feed_phase_object_seed(PrintStartCollector& c, const nlohmann::json& status) {
        c.handle_phase_object_status(status, c.run_generation_.load());
    }

    /// The run a start-up query sent now would belong to.
    static uint64_t run_generation(PrintStartCollector& c) {
        return c.run_generation_.load();
    }

    /// Install the steps the follower reads from the print file's start block.
    static void set_start_block_steps(PrintStartCollector& c,
                                      std::vector<helix::StartBlockStep> steps) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        c.start_block_.set_steps(std::move(steps));
    }

    /// Show a start-block step as run `run` reached it.
    static void apply_start_block_step(PrintStartCollector& c, const helix::StartBlockStep& step,
                                       uint64_t run) {
        c.apply_start_block_step(step, run);
    }

    /// Deliver a start-up query answer tagged with run `run`.
    static void answer_phase_object_query(PrintStartCollector& c, const nlohmann::json& response,
                                          uint64_t run) {
        c.apply_phase_object_query(response, run);
    }

    /// Inject predictor history (deterministic per-phase durations) instead of
    /// relying on the theoretical-duration path.
    static void load_prediction_entries(PrintStartCollector& c,
                                        std::vector<helix::PreprintEntry> entries) {
        c.predictor_.load_entries(entries, helix::StartCondition::COLD);
    }

    /// The phases this run's start said it runs and skips; nullopt until told.
    static std::optional<helix::PreprintPhaseOverrides> phase_overrides(PrintStartCollector& c) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        return c.phase_overrides_;
    }
};
