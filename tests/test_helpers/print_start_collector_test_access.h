// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_start_collector.h"

class PrintStartCollectorTestAccess {
  public:
    static bool is_respond_completion(const std::string& line) {
        return PrintStartCollector::is_respond_completion(line);
    }

    static void on_gcode_response(PrintStartCollector& collector, const nlohmann::json& msg) {
        collector.on_gcode_response(msg);
    }

    /// Wind back the start time to simulate elapsed seconds.
    ///
    /// Also winds back both activity stamps, so this models "N seconds passed
    /// and the printer showed nothing" — the case the timeouts exist for. Use
    /// set_last_activity_seconds_ago() afterwards to model a printer that is
    /// still working on its pre-print.
    static void set_elapsed_seconds(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        auto when = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
        c.printing_state_start_ = when;
        c.last_signal_time_ = when;
        c.last_heater_activity_time_ = when;
    }

    /// Set how long ago the printer last showed pre-print activity of every
    /// kind (a matched line, a probe line, a climbing heater), independent of
    /// total elapsed time.
    static void set_last_activity_seconds_ago(PrintStartCollector& c, int seconds) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        const auto when = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
        c.last_signal_time_ = when;
        c.last_heater_activity_time_ = when;
    }

    /// Age every clock stamp the collector holds by `ms`, as though that much
    /// time had passed. A replay drives a recorded timeline this way: the
    /// collector's own activity and phase stamps age with it, so the test
    /// never restates which lines count as activity.
    static void advance_clock_ms(PrintStartCollector& c, int ms) {
        std::lock_guard<std::mutex> lock(c.state_mutex_);
        const auto d = std::chrono::milliseconds(ms);
        const auto age = [d](std::chrono::steady_clock::time_point& when) {
            if (when.time_since_epoch().count() != 0) {
                when -= d;
            }
        };
        age(c.printing_state_start_);
        age(c.last_signal_time_);
        age(c.last_heater_activity_time_);
        age(c.hold_until_);
        age(c.temps_ready_time_);
        age(c.mesh_first_probe_time_);
        age(c.mesh_last_probe_time_);
        age(c.pre_mesh_last_probe_time_);
        age(c.position_clock_start_);
        for (auto& entry : c.phase_enter_times_) {
            age(entry.second);
        }
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
        c.temps_ready_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
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

    /// Inject predictor history (deterministic per-phase durations) instead of
    /// relying on the theoretical-duration path.
    static void load_prediction_entries(PrintStartCollector& c,
                                        std::vector<helix::PreprintEntry> entries) {
        c.predictor_.load_entries(entries, helix::StartCondition::COLD);
    }
};
