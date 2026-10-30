// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bed_mesh_probe_parser.h"
#include "chamber_wait_line.h"
#include "helix_regex.h"
#include "i_moonraker_client.h"
#include "leveling_probe_points.h"
#include "preprint_checklist.h"
#include "preprint_predictor.h"
#include "print_start_position_classifier.h"
#include "print_start_profile.h"
#include "print_start_purge_shape.h"
#include "printer_state.h"
#include "simulated_clock.h"
#include "start_block_steps.h"
#include "thermal_rate_model.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix {
struct PrintStartAnalysis;

/**
 * @brief A pre-print step that emits signals for other phases on its own
 *
 * A nozzle clean that parks at the purge position and pushes filament to check
 * flow looks exactly like the purge it is not. A profile pattern declares the
 * phases its step owns (`owns`); while the step runs, signals for them are the
 * step's, and the display stays on it. The step's own phase and the heating
 * it does for itself leave it running; any other step's signal ends it.
 */
struct OwningStep {
    PrintStartPhase phase = PrintStartPhase::IDLE; ///< IDLE: no step owns anything
    std::vector<PrintStartPhase> owns;

    enum class Verdict {
        Absorb, ///< the step emitted it; it names no phase change
        Pass,   ///< applies as usual and the step runs on
        Ends,   ///< applies as usual and the step is over
    };

    [[nodiscard]] Verdict judge(PrintStartPhase signal) const {
        if (phase == PrintStartPhase::IDLE) {
            return Verdict::Pass;
        }
        if (std::find(owns.begin(), owns.end(), signal) != owns.end()) {
            return Verdict::Absorb;
        }
        switch (signal) {
        case PrintStartPhase::IDLE:
        case PrintStartPhase::INITIALIZING:
        case PrintStartPhase::HEATING_BED:
        case PrintStartPhase::SOAKING:
        case PrintStartPhase::HEATING_NOZZLE:
            return Verdict::Pass;
        default:
            return signal == phase ? Verdict::Pass : Verdict::Ends;
        }
    }
};
} // namespace helix

/**
 * @file print_start_collector.h
 * @brief Monitors G-code responses to detect PRINT_START macro phases
 *
 * Subscribes to Moonraker's notify_gcode_response to parse G-code output
 * during print initialization. Detects common PRINT_START phases like
 * homing, heating, QGL, bed mesh, and purging through pattern matching.
 *
 * ## Usage
 * The collector is started when a print begins and stopped when the print
 * transitions to actual printing (or is cancelled). Progress is reported
 * through helix::PrinterState subjects which XML can bind to directly.
 *
 * ## Pattern Detection
 * Uses best-effort regex matching on G-code responses. Not all macros will
 * output all phases - the progress calculation handles missing phases gracefully.
 *
 * @see helix::PrintStartPhase enum in printer_state.h
 */
class PrintStartCollector : public std::enable_shared_from_this<PrintStartCollector> {
    friend class PrintStartCollectorTestAccess;

  public:
    /**
     * @brief Seconds spent in the current phase, on the simulated timeline.
     *
     * Reads SimulatedClock, so it scales with --sim-speed exactly as the
     * durations the collector records do.
     */
    [[nodiscard]] int get_current_phase_elapsed_seconds() const;

    /**
     * @brief Construct a PrintStartCollector
     * @param client helix::IMoonrakerClient for registering callbacks
     * @param state helix::PrinterState to update with phase progress
     */
    PrintStartCollector(helix::IMoonrakerClient& client, helix::PrinterState& state);

    ~PrintStartCollector();

    // Non-copyable
    PrintStartCollector(const PrintStartCollector&) = delete;
    PrintStartCollector& operator=(const PrintStartCollector&) = delete;

    /**
     * @brief Start monitoring for PRINT_START phases
     *
     * Registers for notify_gcode_response notifications and begins
     * parsing G-code output for phase detection patterns.
     */
    void start();

    /**
     * @brief Declare that a host-side pre-start block is part of this window
     *
     * Called when HelixScreen dispatches blocking pre-start gcode in front of
     * the job (a forced bed mesh, a printer setup macro). That work sits inside
     * the measured window, which on some printers is several minutes, so its
     * timings must not be averaged with printer-edge measurements.
     *
     * Klipper echoes the dispatched text back through notify_gcode_response.
     * That echo is our own command being repeated, not the printer narrating
     * work, so on_gcode_response() refuses to read any signal out of a line
     * that reproduces the block — profile patterns whose alternatives collide
     * with the block's parameter names would otherwise claim phases before
     * the printer has run any of them.
     *
     * Safe to call after start(): it re-filters the loaded history in place.
     * The window classification is idempotent; a second call still replaces
     * the remembered block text, so a run dispatching two blocks suppresses
     * both echoes.
     *
     * @param dispatched_block The exact gcode text dispatched, lines joined
     *   by newlines (as built by build_pre_start_gcode_block)
     */
    void note_host_side_pre_start(const std::string& dispatched_block);

    /**
     * @brief The phases this run's start runs and skips, from its option toggles
     *
     * The countdown's estimate takes them as print details does, so the two
     * agree. The phases skipped are recorded on the run's history entry, so a
     * later estimate can tell a phase no start ran from one the phase matcher
     * missed; a run never told records nothing, which reads as unknown. Call
     * on the main thread, after start().
     *
     * @param overrides PrintStartPhase value -> whether this start runs it
     */
    void note_phase_overrides(helix::PreprintPhaseOverrides overrides);

    /**
     * @brief Stop monitoring
     *
     * Unregisters callback and resets state. Called when print
     * initialization completes or print is cancelled.
     */
    void stop();

    /// stop(), and unregister the gcode-response handler stop() keeps: the collector is
    /// being replaced, and a handler left on the client would keep it collecting.
    void detach();

    /**
     * @brief Check if collector is currently active
     */
    [[nodiscard]] bool is_active() const {
        return active_.load();
    }

    /**
     * @brief Reset all per-run state; start() runs the same reset
     */
    void reset();

    /**
     * @brief Check fallback completion conditions
     *
     * Called by observers when layer count or progress changes.
     * Checks multiple fallback signals for printers that don't emit
     * layer markers in G-code responses (e.g., FlashForge AD5M).
     */
    void check_fallback_completion();

    /**
     * @brief Enable fallback detection after initial G-code response window
     *
     * Called shortly after start() to enable fallback signals.
     * Gives G-code response detection priority for the first few seconds.
     */
    void enable_fallbacks();

    /**
     * @brief Complete the pre-print phase from an external authoritative signal
     *
     * Called when an external source (e.g., Moonraker state transition) definitively
     * indicates the print has started. Immediately transitions to COMPLETE.
     *
     * @param source Description of the signal source (for logging)
     */
    void complete_from_external_signal(const char* source);

    /**
     * @brief Record an observed current_layer value for this print
     *
     * Arms the layer-1 completion edge: the first time current_layer is seen at
     * 0 (or below 1) since this print started, the collector latches
     * layer_zero_seen_. The pre-print → printing hand-off requires this latch so
     * a stale positive layer carried over from the previous print (before
     * reset_for_new_print() has zeroed the subject) can't complete the new
     * print's pre-print phase. See MoonrakerManager::should_complete_preprint().
     *
     * @param current_layer The layer value just observed
     */
    void note_current_layer(int current_layer) {
        if (current_layer < 1) {
            layer_zero_seen_.store(true, std::memory_order_relaxed);
            return;
        }

        // A layer counter that ADVANCES while we are collecting is live data for
        // this print, which is the same proof a 0 -> >=1 edge gives. Tracking it
        // separately matters because the 0 sample is not guaranteed to be
        // observed: notify_status_update is coalesced, so a fast first layer (or
        // a reconnect part-way in) can deliver a first sample that is already
        // >= 1 and the zero edge never arrives. Without this the pre-print
        // overlay stays up for the entire print.
        int prev = first_layer_observed_.load(std::memory_order_relaxed);
        if (prev < 0) {
            first_layer_observed_.store(current_layer, std::memory_order_relaxed);
        } else if (current_layer > prev) {
            layer_advanced_.store(true, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Whether current_layer==0 has been observed since this print started
     */
    [[nodiscard]] bool has_seen_layer_zero() const {
        return layer_zero_seen_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Whether current_layer has increased since collection began.
     *
     * A stale positive carried over from the previous print is static, so it
     * can never satisfy this — which is what the layer-zero latch was guarding
     * against.
     */
    [[nodiscard]] bool has_seen_layer_advance() const {
        return layer_advanced_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Whether the printer reports a pre-print step still under way
     *
     * True while the latest matched context-qualified phase-object state maps
     * to a phase short of COMPLETE (a priming line, a cool-down for probing).
     * Such a step holds the deadline timeouts and the first-extrusion
     * fallback. A bare phase-object state never counts: a display label
     * outlives its step.
     */
    [[nodiscard]] bool in_live_preprint_step() const;

    /**
     * @brief Record the printer's live bed-mesh presence
     *
     * Fed from the bed_mesh status stream. A mesh that disappears while the
     * collector is in CLEANING marks the start of leveling work (accurate Z
     * probing, bed-mesh corner validation) that Creality-class firmwares do
     * not echo to gcode_response — the display moves to BED_MESH ("Bed
     * Leveling...") instead of sitting on "Cleaning Nozzle..." through the
     * whole silent window. Mesh clears arriving before CLEANING are the
     * rough G28's own clear and carry no phase information.
     *
     * Thread-safe: called from the WebSocket background thread.
     */
    void note_bed_mesh_presence(bool present);

    /**
     * @brief Record the bed-mesh probe-area bounds (gcode mm)
     *
     * Fed from the same bed_mesh status updates as note_bed_mesh_presence().
     * The bounds anchor the position classifier's geometric zones: Z-probing
     * happens at the mesh centre, corner validation at the mesh corners, and
     * the K1-class wipe strip sits beyond mesh_max at the bed rear. Until
     * bounds arrive the classifier withholds all verdicts.
     *
     * Thread-safe: called from the WebSocket background thread.
     */
    void note_mesh_bounds(float x_min, float x_max, float y_min, float y_max);

    /**
     * @brief Feed one toolhead position sample (gcode mm)
     *
     * Fed from the toolhead.position subjects (subscribed already). When the
     * profile declares position_signals and the console has gone silent, the
     * inferred activity refines the status line: centre probes →
     * "Probing Z...", corner tour → "Checking Bed Mesh...", sweep march →
     * BED_MESH entry. Real gcode_response signals always win — this only
     * fills silence.
     * Whatever the profile, Z descents at two of the printer's configured
     * gantry-leveling / Z-tilt points show that phase.
     *
     * Thread-safe: observers fire on the main thread (queued subject sets).
     */
    void note_position_sample(float x_mm, float y_mm, float z_mm);

    /**
     * @brief Sample the toolhead, extruder and filament system for a stationary purge
     *
     * Reads the position and extruder-velocity subjects and the filament
     * system's action/loaded subjects, feeds helix::PurgeFlowTracker and
     * applies PURGING when helix::is_purge() says so. Called on every
     * position or extruder-velocity change and every ETA tick, so a purge
     * that holds everything still is still sampled.
     *
     * Main thread only: it reads LVGL subjects.
     */
    void check_purge_shape();

    /**
     * @brief Set the print start profile for pattern/signal matching
     *
     * Must be called before start(). Ignored if the collector is active.
     *
     * @param profile Profile to use, or nullptr to disable profile-based matching
     */
    void set_profile(std::shared_ptr<PrintStartProfile> profile);

    /**
     * @brief Hand over the PRINT_START macro analysis, the fallback source of the
     *        checklist's expected order when the file has no inline start block
     *
     * Survives start(): the macro belongs to the printer, not to one run. Any thread.
     */
    void set_macro_analysis(const helix::PrintStartAnalysis& analysis);

    /**
     * @brief What the pre-print checklist needs, as of now
     *
     * Done phases in the order they were left, the current phase and its elapsed
     * time, seconds spent per phase, the expected order (start block, else macro
     * analysis, else empty) and the predictor's per-phase seconds. Any thread;
     * takes state_mutex_. It changes only when print_start_phase or
     * print_start_progress does (both are set at every phase entry), plus the
     * elapsed time that the panel reads on its own tick.
     */
    [[nodiscard]] helix::PreprintChecklistInput checklist_input() const;

    /**
     * @brief Get the predictor for reading predictions
     *
     * Thread-safe: the predictor is loaded on start() and gains entries on
     * COMPLETE, both under state_mutex_. Its readers (predicted_phases(),
     * predicted_total(), has_predictions()) are const and safe from the main
     * thread.
     */
    [[nodiscard]] const helix::PreprintPredictor& predictor() const {
        return predictor_;
    }

    /**
     * @brief The heater a blocking wait command waits on
     *
     * M109 is the nozzle whichever way its temperature moves, M190 the bed,
     * and M191 or a TEMPERATURE_WAIT on @p chamber_heater itself the chamber
     * (SOAKING), but only with a chamber heater configured. A TEMPERATURE_WAIT on heater_bed or an
     * extruder is that heater. Anything else names no heater: nullopt.
     */
    [[nodiscard]] static std::optional<helix::PrintStartPhase>
    heater_waited_on(const helix::WaitLine& line, const std::string& chamber_heater);

  private:
    /**
     * @brief Handle incoming G-code response
     */
    void on_gcode_response(const nlohmann::json& msg);

    /**
     * @brief Whether a console line is the echo of the dispatched pre-start block
     *
     * The echo reproduces the dispatched text, either line-by-line or as the
     * whole block, so a dispatched line appearing verbatim in the response
     * identifies it. Caller must hold state_mutex_.
     */
    bool is_own_pre_start_echo_locked(const std::string& line) const;

    /**
     * @brief Check line against phase patterns
     */
    void check_phase_patterns(const std::string& line);

    /**
     * @brief Apply a profile match through the shared update path
     *
     * Every profile-driven phase match — console line or phase-object state —
     * lands here, so phase weights, progress, ETA re-baselining, and the
     * real-signal gate behave identically whichever feed produced the match.
     *
     * @param match The phase/message/weight the profile resolved
     * @param marks_real_signal Whether this match counts as firmware narration
     *   and so gates the proactive temperature detector off. A predicate over
     *   heater temperatures reads the same evidence that detector reads, so a
     *   status-signal match passes false: silencing the detector with its own
     *   input costs the HOMING and heating phases only it can supply. The same
     *   flag picks the quiet gate the match refreshes: narration holds every
     *   timeout, inference only the deadline timeouts (note_signal() and
     *   note_inferred_activity()).
     * @param live_state Whether the match names the step running now rather
     *   than an event that happened. A context-qualified phase-object state is
     *   live: it may move the display to any phase, an earlier one included,
     *   since the printer reports what it is doing, not a line that arrived late.
     */
    void apply_profile_match(const PrintStartProfile::MatchResult& match,
                             bool marks_real_signal = true, bool live_state = false);

    /// Judge a signal for @p phase against the owning step (helix::OwningStep):
    /// true when the step owns it, ending the step when another step begins.
    bool absorbed_by_owning_step(helix::PrintStartPhase phase);

    /// Show a gantry-leveling / Z-tilt phase inferred from the configured
    /// probe points. Inference, not narration: it never mutes the proactive
    /// detector and never relabels a phase already detected.
    void apply_leveling_phase(helix::PrintStartPhase phase);

    /**
     * @brief Check a status frame for the profile's phase object
     *
     * When the profile declares a phase object (PrintStartProfile::
     * has_phase_object()) and the frame carries it, reads the declared field,
     * matches the state string through the profile's state patterns, and feeds
     * the match through apply_profile_match(). An unchanged state string does
     * not re-fire; profiles without a phase object ignore every frame.
     *
     * With a context field declared, the state is matched as
     * "<innermost context>/<state>". Klipper sends only the fields that
     * changed, so each field's last value is cached and a frame carrying
     * either one re-matches the pair.
     *
     * Thread-safe: runs on the WebSocket background thread, like the other
     * notify_status_update handling in start().
     *
     * @param seed_run Set for the answer to the query start() sends, to the
     *   run that sent it: its values fill only the fields no frame has
     *   delivered yet, since a frame may be newer, and it is dropped once
     *   another run has begun.
     */
    void handle_phase_object_status(const nlohmann::json& status,
                                    std::optional<uint64_t> seed_run = std::nullopt);

    /// Fetch the phase object's current state and context once at start():
    /// a context opened before the collector armed is never sent again
    /// until it changes, and every state inside it would match without it.
    void query_phase_object();

    /// The start-up query's answer for run `run`; ignored once a later
    /// start() has begun another run.
    void apply_phase_object_query(const nlohmann::json& response, uint64_t run);

    /// Ask once at start() whether the firmware runs its start macro before
    /// streaming the file, and where the file stands. Arms the start-macro
    /// gate when both say the macro is still running.
    void query_start_macro_gate();

    /// The start-macro query's answer for run `run`; ignored once a later
    /// start() has begun another run.
    void apply_start_macro_gate_query(const nlohmann::json& response, uint64_t run);

    /// Disarms the start-macro gate and returns whether it was armed.
    bool take_start_macro_gate();

    /// Bumped by every start() under state_mutex_, so an answer can tell
    /// which run asked.
    std::atomic<uint64_t> run_generation_{0};

    /**
     * @brief Check a status frame against the profile's status-signal rules
     *
     * For every rule whose object the frame carries, evaluates the rule's
     * predicates and applies the match on the false->true rising edge only —
     * physical predicates hold for whole windows (the head sits at the cutter
     * for seconds) while frames keep re-arriving, so each rule latches by name
     * and re-arms once its predicate stops holding. Matches feed
     * apply_profile_match() without marking a real signal — a predicate over
     * heater or toolhead frames is inference, not narration. A profile without
     * status_signals ignores every frame.
     *
     * Thread-safe: runs on the WebSocket background thread, like the other
     * notify_status_update handling in start().
     */
    void handle_status_signals(const nlohmann::json& status);

    /// Record that the printer said something about its pre-print: a matched
    /// pattern or phase-object state, or a probe line. Feeds the quiet gate of
    /// every timeout, the ceiling included. `hold` is silent time the text
    /// announces (a heat soak's G4): the printer counts as talking until it
    /// ends. Takes state_mutex_ itself, so do not call it while already holding
    /// the lock.
    void note_signal(std::chrono::seconds hold = std::chrono::seconds::zero());

    /// Record pre-print work read from status frames rather than said: a heater
    /// climbing past its highest reading, or a status-signal rule firing. Feeds
    /// the quiet gate of the deadline timeouts only. Takes state_mutex_ itself.
    void note_inferred_activity();

    /**
     * @brief Check for HELIX:PHASE:* signals from plugin/macros
     *
     * These are definitive signals that take priority over regex detection.
     * Format: "HELIX:PHASE:STARTING", "HELIX:PHASE:HOMING", "HELIX:PHASE:COMPLETE", etc.
     *
     * @return true if a HELIX:PHASE signal was detected and handled
     */
    bool check_helix_phase_signal(const std::string& line);

    /**
     * @brief Check the line against the profile's tag patterns
     *
     * Tag patterns (PrintStartProfile::try_match_tag) are lines a firmware
     * prints to announce a step outright. A match sets the phase directly;
     * when the line reports its own progress the message carries it and the
     * progress follows it.
     *
     * @return true if a tag pattern matched and was applied
     */
    bool check_tag_signal(const std::string& line);

    /**
     * @brief Reset mesh probe counters on BED_MESH entry / sub-phase change
     *
     * Some firmwares (Snapmaker U1) route multiple distinct probe operations
     * through a single BED_MESH phase enum, varying only the status message
     * between them (Pre-scanning Bed, Levelling Bed, Detecting Plate,
     * Inspecting Bed). The probe counters need to reset between sub-phases
     * so the displayed "(N/M)" doesn't roll past M. Caller must hold
     * state_mutex_.
     *
     * @param counts_probes Whether the new BED_MESH step's probe lines are
     *        mesh points to count (mesh_counts_probes_).
     */
    void maybe_reset_for_mesh_subphase_locked(helix::PrintStartPhase next_phase,
                                              const std::string& next_message,
                                              bool counts_probes = true);

    /**
     * @brief Update phase and recalculate progress (weighted mode)
     *
     * @param counts_mesh_probes For BED_MESH: whether its probe lines count
     *        against a point total (see PrintStartProfile::MatchResult).
     */
    void update_phase(helix::PrintStartPhase phase, const char* message,
                      bool counts_mesh_probes = true);

    /**
     * @brief Update phase with explicit progress value (sequential mode)
     */
    void update_phase(helix::PrintStartPhase phase, const std::string& message, int progress);

    /**
     * @brief Relabel between the two heating phases from live temps (bed-first).
     *
     * Compare-and-swap: applies the relabel only if current_phase_ is STILL a
     * heating phase (HEATING_BED/HEATING_NOZZLE) at write time, or is a
     * BED_MESH with no probing under way whose nozzle target rose after the
     * mesh began and `resolved` is HEATING_NOZZLE. A background gcode signal
     * may have advanced current_phase_ between the caller's temperature
     * snapshot and this call; the CAS refuses in that case so a newer phase is
     * never regressed back to heating. The label is derived from `resolved`.
     *
     * With `heater_wait`, a heater wait is blocking the queue instead: the
     * relabel may displace any phase but IDLE, a heating one, SOAKING,
     * PURGING or COMPLETE, keeping it and its message for end_heater_wait(),
     * and afterwards moves
     * only between the wait's own labels. `resolved` may then be SOAKING for a
     * chamber. Main thread only, like everything that sets or clears a
     * wait's label.
     */
    void relabel_heating_phase(helix::PrintStartPhase resolved, bool heater_wait = false,
                               const std::string& label = {});

    /// The label a heater wait on the chamber shows: the live chamber
    /// temperature against its target, or alone when the heater has none.
    static std::string chamber_wait_message(int temp_deci, int target_deci);

    /// Follow virtual_sdcard.file_position through the start block, on a
    /// profile whose start sequence the slicer writes into the file. The
    /// first position read fetches and scans the file's head. Any thread.
    void handle_start_block_position(const nlohmann::json& status);
    /// Fetch and scan the head of @p filename for run @p run. Main thread.
    void request_start_block(uint64_t run, std::string filename);
    /// Show the step run @p run reached, unless the console already named
    /// what that line is doing. Dropped once another run has begun or the
    /// pre-print is COMPLETE. Any thread; takes state_mutex_ itself.
    void apply_start_block_step(const helix::StartBlockStep& step, uint64_t run);

    /// Run @p apply, the phase match of console line @p line, and if it
    /// changed the phase or message shown, stamp console_phase_time_ with it
    /// and drop any deferred step. Takes state_mutex_ itself; @p apply must
    /// not hold it.
    void apply_console_phase(const std::string& line, const std::function<void()>& apply);
    /// Show the deferred start-block step once the console grace has passed.
    /// Any thread; takes state_mutex_ itself.
    void flush_deferred_start_block_step();

    /// Make @p phase current, crediting the time since the last change to the
    /// phase it replaces. Caller holds state_mutex_.
    void enter_phase_locked(helix::PrintStartPhase phase);
    /// Time spent in @p phase this run, its visit now under way included.
    /// Zero for IDLE, INITIALIZING and COMPLETE, which are not timed. Caller
    /// holds state_mutex_.
    std::chrono::seconds phase_seconds_locked(helix::PrintStartPhase phase) const;

    /// Ask where the print is, and read the command there if the position
    /// moved since the last read. Main thread.
    void request_wait_line();
    /// Store read @p gen's answer if no later position superseded it. Any thread.
    void apply_wait_line(uint64_t gen, const helix::WaitLine& line);
    /// Drop the current wait's line. Caller holds state_mutex_.
    void forget_wait_line_locked();

    /// Give the phase a heater wait displaced back, if the wait's label is
    /// still the one showing. Main thread only; takes state_mutex_ itself.
    void end_heater_wait();

    /// Make `phase` current without a phase signal: the relabel and restore
    /// paths. Returns the progress to publish, -1 to keep the published one.
    /// Caller must hold state_mutex_.
    int switch_phase_locked(helix::PrintStartPhase phase, const std::string& message);

    /// Publish a switch_phase_locked() result. Main thread only.
    void publish_switched_phase(helix::PrintStartPhase phase, const std::string& message,
                                int progress);

    /// Whether a line is the temperature report Klipper emits once a second
    /// while M109, M190 or TEMPERATURE_WAIT blocks ("B:45.2 /100.0 T0:210.0 /210.0").
    [[nodiscard]] static bool is_heater_wait_report(const std::string& line) {
        return helix::regex_match(line, heater_wait_report_pattern_);
    }

    /// Whether BED_MESH is showing and a probe line arrived within
    /// kMeshProbeGapReset. Caller must hold state_mutex_.
    bool mesh_probing_locked() const;

    /**
     * @brief Calculate overall progress based on detected phases
     */
    int calculate_progress() const;

    /**
     * @brief Calculate progress (must be called with state_mutex_ held)
     */
    int calculate_progress_locked() const;

    /**
     * @brief Get completed phases (must be called with state_mutex_ held)
     */
    [[nodiscard]] std::set<int> get_completed_phase_ints_locked() const;

    /**
     * @brief Whether the sequence went past a phase without ever showing it
     *
     * A non-heating phase that was never detected and is ordered before the
     * phase on display is one this macro does not run (or ran unseen), so it
     * owes no time. With prediction history nothing is skipped: a recorded
     * phase may come in any order until COMPLETE. Heating
     * phases are judged by temperature instead. Caller must hold state_mutex_.
     */
    [[nodiscard]] bool phase_skipped_locked(int phase) const;

    /**
     * @brief Check for PRINT_START start marker
     */
    static bool is_print_start_marker(const std::string& line);

    /**
     * @brief Check for print start completion (layer 1, etc.)
     */
    static bool is_completion_marker(const std::string& line);

    /** @brief Check if a G-code response is a RESPOND-based print start completion */
    [[nodiscard]] static bool is_respond_completion(const std::string& line);

    // Dependencies
    helix::IMoonrakerClient& client_;
    helix::PrinterState& state_;

    // Registration state
    std::string handler_name_;
    std::atomic<bool> active_{false};
    std::atomic<bool> registered_{false};

    // Thread safety: protects all non-atomic members below
    // WebSocket callbacks run on background thread, check_fallback_completion() runs on main thread
    mutable std::mutex state_mutex_;

    // Phase tracking (protected by state_mutex_)
    std::set<helix::PrintStartPhase> detected_phases_;
    /// Phases left behind, in the order they were left, each phase once.
    std::vector<helix::PrintStartPhase> phases_left_;
    /// Phases of the PRINT_START macro's detected operations, in macro order.
    std::vector<helix::PrintStartPhase> macro_expected_phases_;
    helix::PrintStartPhase current_phase_ = helix::PrintStartPhase::IDLE;
    bool print_start_detected_ = false;
    int max_sequential_progress_ = 0; // Monotonic progress guard for sequential mode
    helix::sim::SimulatedClock::time_point printing_state_start_;

    /// When the printer last said something about its pre-print: a profile
    /// pattern or phase-object state matched, or a probe line arrived. Every
    /// timeout, the ceiling included, waits for kPreprintQuietTimeout past it,
    /// so a printer narrating a long pre-print is not cut short. Only the
    /// backstop ignores it.
    helix::sim::SimulatedClock::time_point last_signal_time_;

    /// When pre-print work last showed without being said: a heater climbed a
    /// degree above its highest reading under its current target, or a
    /// status-signal rule fired. The deadline timeouts wait for
    /// kPreprintQuietTimeout past it, which carries them through a silent M190
    /// or M109. The ceiling ignores it: a heater that never settles must not
    /// hold Preparing open.
    helix::sim::SimulatedClock::time_point last_inferred_activity_time_;

    /// End of the silent time a matched line announced, time_point::min() when
    /// none: the simulated timeline starts at its epoch, so a stamp wound back
    /// from it reads earlier than {}. Until then the printer counts as talking.
    /// held_for_ is the time holds have covered: the ceiling leaves all of it
    /// out of the elapsed time it measures, the backstop at most one ceiling of
    /// it, so a macro that keeps announcing holds still ends at the backstop.
    helix::sim::SimulatedClock::time_point hold_until_ =
        helix::sim::SimulatedClock::time_point::min();
    helix::sim::SimulatedClock::duration held_for_{};

    // Profile for signal/pattern matching (set via set_profile() or loaded by start())
    std::shared_ptr<PrintStartProfile> profile_;

    // Universal patterns (not profile-specific)
    static const helix::Regex print_start_pattern_;
    static const helix::Regex completion_pattern_;
    static const helix::Regex respond_completion_pattern_;
    static const helix::Regex heater_wait_report_pattern_;

    // Fallback detection constants.
    //
    // These are SIMULATED seconds: every duration the collector compares them
    // against comes from helix::sim::SimulatedClock, and the adaptive ceilings
    // are derived from predictions the collector measured on that same clock.
    // Keeping one unit is what makes the stuck-detection behaviour identical at
    // every --sim-speed, and outside --test the factor is 1.0, so these are
    // real seconds there.
    static constexpr auto kFallbackTimeout =
        std::chrono::seconds(300); ///< Last resort when no predictions
    /// The ceiling: past it, a pre-print ends once kPreprintQuietTimeout passes
    /// without a matched line or probe line, whatever the temperatures, so a
    /// heater that never settles still leaves Preparing. With a prediction the
    /// ceiling is the larger of this and predicted x kAbsoluteTimeoutMargin.
    /// Sits above the longest legitimate pre-print: the K2 Plus runs ~1140s
    /// (heat, ~390s mesh, purge), and a cold-start ASA soak pushes that further.
    static constexpr auto kAbsoluteMaxTimeout = std::chrono::seconds(1800);
    /// The backstop, as a multiple of the ceiling. The one fallback that waits
    /// for nothing, so a firmware that chatters forever still leaves Preparing.
    static constexpr int kBackstopCeilingMultiple = 2;
    /// How long the printer must say nothing before a timeout may complete the
    /// pre-print. Longer than the gap between mesh probe points on a slow bed
    /// (the K2 spends ~5s per point, ~3s on a manual sweep) with margin for a
    /// heat-soak step that emits nothing at all.
    static constexpr auto kPreprintQuietTimeout = std::chrono::seconds(90);
    static constexpr float kAdaptiveTimeoutMargin =
        1.5f; ///< Multiply predicted total for adaptive timeout
    static constexpr float kAbsoluteTimeoutMargin =
        2.5f; ///< Multiply predicted total for absolute ceiling
    static constexpr int kTempToleranceDecidegrees = 50; // 5°C (temps stored as value * 10)

    // Fallback detection state (for printers without G-code layer markers)
    // Baseline values snapshot stale subject data at collector start so fallbacks
    // only trigger on actual changes, not leftover data from the previous print.
    int baseline_layer_{0};
    int baseline_progress_{0};
    std::atomic<bool> fallbacks_enabled_{false};
    std::atomic<helix::SubscriptionId> macro_subscription_id_{0};

    // Phase timing for duration prediction (protected by state_mutex_). A
    // phase can be shown more than once in one start (a toolchanger probes Z
    // after every tool pick), so each keeps the sum of its visits.
    std::map<int, helix::sim::SimulatedClock::duration> phase_time_spent_;
    helix::sim::SimulatedClock::time_point phase_since_;
    helix::PreprintPredictor predictor_;
    int loaded_temp_bucket_{0};
    /// Which window this run is measuring. Defaults to PrinterEdge: an
    /// externally started print never announces a host-side block, and neither
    /// does a screen-started print on a printer that runs none.
    helix::PreprintWindow window_{helix::PreprintWindow::PrinterEdge};

    /// Trimmed lines of the host-side pre-start block this run dispatched.
    /// A console line reproducing one of them is the printer echoing our own
    /// text, so no pattern may read a phase out of it. Cleared in start() and
    /// reset(), with the rest of the per-run state.
    std::vector<std::string> host_pre_start_echo_lines_;

    /// The phases this run's start runs and skips, from note_phase_overrides();
    /// reset with the rest of the per-run state.
    std::optional<helix::PreprintPhaseOverrides> phase_overrides_;

    // Duration-proportional progress weights (protected by state_mutex_)
    std::map<int, float> predicted_phase_weights_; ///< Phase -> fraction of total (0.0-1.0)
    float predicted_total_seconds_ = 0.0f;         ///< Total predicted pre-print duration
    /// Nozzle+bed heating priced at this start's targets: 0 when the heaters are
    /// already there, nullopt until priced or when the nozzle target is unknown
    std::optional<float> priced_heating_seconds_;
    int start_ext_temp_ = 0; ///< Extruder temp at collector start (decideg/10)
    int start_bed_temp_ = 0; ///< Bed temp at collector start (decideg/10)
    // Cached temperature readings for thread-safe access from calculate_progress_locked()
    // Updated from main thread in check_fallback_completion() and start()
    std::atomic<int> cached_ext_temp_{0};   ///< Current extruder temp (decideg/10)
    std::atomic<int> cached_ext_target_{0}; ///< Current extruder target (decideg/10)
    std::atomic<int> cached_bed_temp_{0};   ///< Current bed temp (decideg/10)
    std::atomic<int> cached_bed_target_{0}; ///< Current bed target (decideg/10)
    // Heater readings from the status frames themselves (degrees), for
    // status-signal gates that run on the WebSocket thread between ticks.
    std::atomic<int> frame_ext_temp_{0};
    std::atomic<int> frame_ext_target_{0};
    std::atomic<int> frame_bed_temp_{0};
    std::atomic<int> frame_bed_target_{0};
    std::atomic<bool> frame_ext_target_seen_{false}; ///< A frame named a target since start()
    std::atomic<bool> frame_bed_target_seen_{false};
    int last_remaining_ = 0;           ///< For monotonic bias
    bool fallback_completion_ = false; ///< True if COMPLETE was triggered by timeout fallback

    // Bed mesh probe tracking (protected by state_mutex_)
    // Parsed from G-code responses during BED_MESH phase to provide sub-phase
    // progress and per-probe time extrapolation for ETA.
    int mesh_probe_current_ = 0;
    int mesh_probe_total_ = 0;
    /// Grid size from Klipper's "Adapted probe count" line. It can arrive just
    /// before BED_MESH is entered, so it outlives the entry reset and seeds
    /// mesh_probe_total_ there; cleared when the mesh phase ends.
    int mesh_adapted_total_ = 0;
    helix::sim::SimulatedClock::time_point mesh_first_probe_time_;
    helix::sim::SimulatedClock::time_point mesh_last_probe_time_;
    float mesh_seconds_per_probe_ = 0.0f; ///< Running average from observed probe intervals
    /// One-shot: live extrapolation has become this BED_MESH segment's ETA
    /// contribution (releases the monotonic anchor once). Cleared wherever the
    /// probe telemetry is zeroed — BED_MESH entry, sub-phase message change,
    /// probe-gap reset, and per run — so every mesh segment gets its own
    /// release. Every access holds state_mutex_: the WebSocket thread clears
    /// it for a new segment, the ETA timer tests-and-sets it, and the two must
    /// not straddle a segment change.
    bool mesh_extrapolation_armed_ = false;

    /// Unique probe POINTS (not sample lines) counted from the "probe at X,Y"
    /// fallback, for firmware that emits no "Probing point N/M". Reset on
    /// gap-detection, on mesh sub-phase change, and in reset().
    helix::ProbePointCounter mesh_points_;

    // Sub-phase tracking within BED_MESH. Some firmwares (Snapmaker U1) route
    // multiple distinct probe operations through one phase enum but vary the
    // status message — e.g. Pre-scanning Bed → Levelling Bed → Detecting
    // Plate → Inspecting Bed, each emitting its own probe-line burst. We
    // reset the probe counters when this message changes while the phase
    // stays BED_MESH so the displayed (N/M) doesn't roll past M. Also used
    // as the human label when rendering "<sub-phase> (N/M)" so the user sees
    // which sub-phase they're in. Empty when not in BED_MESH.
    std::string current_mesh_message_;

    /// The message the current phase was entered with, before any probe count
    /// is appended. Guarded by state_mutex_.
    std::string current_message_;

    /// When the last heater-wait report arrived. A wait reports once a second,
    /// so one older than kHeaterWaitReportGap means no wait is blocking.
    /// Guarded by state_mutex_.
    helix::sim::SimulatedClock::time_point heater_wait_report_time_ =
        helix::sim::SimulatedClock::time_point::min();
    static constexpr auto kHeaterWaitReportGap = std::chrono::seconds(3);

    /// Set while a heater wait is showing in place of a firmware phase, with
    /// the phase and message it displaced. Guarded by state_mutex_.
    bool heater_wait_shown_ = false;
    helix::PrintStartPhase pre_wait_phase_ = helix::PrintStartPhase::IDLE;
    std::string pre_wait_message_;

    /// When a fallback tick first saw the heater a wait's label names no
    /// longer short of its target, min() while it still is. Guarded by
    /// state_mutex_.
    helix::sim::SimulatedClock::time_point wait_heater_arrived_at_ =
        helix::sim::SimulatedClock::time_point::min();

    /// When a fallback tick first saw a wait still reporting with the nozzle
    /// and bed at target, min() otherwise. Guarded by state_mutex_.
    helix::sim::SimulatedClock::time_point heaters_ready_wait_since_ =
        helix::sim::SimulatedClock::time_point::min();
    /// How long a wait must outlast the nozzle and bed arriving before an
    /// unheated chamber is read as what it waits on.
    static constexpr auto kChamberWaitSettle = std::chrono::seconds(5);

    /// The command a heater wait is running: the line at
    /// virtual_sdcard.file_position, read again whenever the position moves,
    /// since one wait can hand straight to the next with no gap in the
    /// reports. wait_line_read_ is false until the read for
    /// wait_line_position_ answers; wait_line_unreadable_ stops the queries
    /// for the rest of a wait whose read failed. Guarded by state_mutex_.
    helix::WaitLine wait_line_;
    bool wait_line_read_ = false;
    /// The start block's steps and how far the print is through them, on a
    /// scripted-start profile. start_block_requested_ is set once the head
    /// read is on its way. Guarded by state_mutex_.
    helix::StartBlockFollower start_block_;
    bool start_block_requested_ = false;
    /// virtual_sdcard.file_position and print_stats.filename as this run's
    /// status frames last carried them. Guarded by state_mutex_.
    std::optional<uint64_t> file_position_;
    std::string frame_filename_;
    /// Set while a firmware that runs its start macro before the file is still
    /// at file_position 0: the first position past 0 means the macro, purge
    /// included, has returned. Guarded by state_mutex_.
    bool start_macro_gate_armed_ = false;
    /// A step that reached its line within the console grace, while the
    /// console's latest change came from a line its command does not print:
    /// the previous command's last words. It shows once the grace passes,
    /// unless the console changes the display again first. The console line
    /// behind that change is console_phase_line_. Guarded by state_mutex_.
    std::optional<helix::StartBlockStep> deferred_step_;
    uint64_t deferred_step_run_ = 0;
    std::string console_phase_line_;
    /// Set to stop the head read in flight: a new run or stop().
    std::shared_ptr<std::atomic<bool>> start_block_cancel_;
    /// When a console line last changed the phase shown. A step whose position
    /// arrives within kStartBlockConsoleGrace after it is the line the
    /// console line came from: Moonraker batches status frames, so a command's
    /// own first words reach us before the position that names its line.
    /// Guarded by state_mutex_.
    helix::sim::SimulatedClock::time_point console_phase_time_ =
        helix::sim::SimulatedClock::time_point::min();
    static constexpr auto kStartBlockConsoleGrace = std::chrono::seconds(1);
    bool wait_line_unreadable_ = false;
    std::optional<uint64_t> wait_line_position_;
    uint64_t wait_line_gen_ = 0;

    /// Nozzle target (°C) when BED_MESH was entered. A higher target while the
    /// mesh is idle is the print-temperature heat that follows it. Guarded by
    /// state_mutex_.
    int mesh_entry_ext_target_ = 0;

    /// Last state string MATCHED from the profile's phase object. Klipper
    /// notifies on every field change in the object, so the same state
    /// re-arrives; it must not re-fire the phase. Cleared in start()/reset().
    std::string last_phase_object_state_;

    /// Whether the current BED_MESH step's probe lines are mesh points to
    /// count. False for a step that only checks a stored mesh: its probes
    /// still show the printer working, but no "(N/M)" is shown and no point
    /// total is fetched. Guarded by state_mutex_.
    bool mesh_counts_probes_ = true;

    /// Last value of the phase object's state and context fields seen in
    /// any frame; nullopt until one arrives. A null or absent-in-firmware
    /// state caches as "". Guarded by state_mutex_; cleared in start().
    std::optional<std::string> phase_object_state_cache_;
    std::optional<std::string> phase_object_context_cache_;

    /// Phase the latest matched context-qualified phase-object state maps
    /// to; IDLE when none has matched. While it names a pre-print step the
    /// deadline timeouts wait for the printer to leave it. Guarded by
    /// state_mutex_; cleared in start().
    helix::PrintStartPhase live_object_phase_ = helix::PrintStartPhase::IDLE;

    /// Status-signal rules whose predicate held on the last frame carrying
    /// their object — the edge-trigger latch, keyed by rule name. A rule in
    /// this set cannot re-fire until its predicate reads false on a later
    /// frame (re-arm). Cleared in start()/reset(), like
    /// last_phase_object_state_.
    std::set<std::string> held_status_signal_rules_;

    /// Last bed-mesh presence reported via note_bed_mesh_presence(). The
    /// leveling trigger is the present→absent edge, so an unknown initial
    /// state (no report yet) never fires it.
    bool bed_mesh_present_{false};

    /// Position-stream inference for silent pre-print windows. Fed by
    /// note_position_sample()/note_mesh_bounds(); guarded by state_mutex_.
    helix::PrintStartPositionClassifier position_classifier_;
    helix::PositionActivity last_position_activity_ = helix::PositionActivity::NONE;
    /// Anchor for the classifier's millisecond sample clock (set in start()).
    helix::sim::SimulatedClock::time_point position_clock_start_{};

    /// [quad_gantry_level]/[z_tilt] probe points, copied from discovery in
    /// start() (main thread) because probe lines arrive on the WS thread.
    /// Guarded by state_mutex_.
    std::vector<helix::LevelingProbePoint> leveling_points_;
    /// Toolhead descents at leveling points; guarded by state_mutex_.
    helix::LevelingDescentTracker leveling_descents_;
    /// Stationary-purge stretch fed by check_purge_shape(); guarded by state_mutex_.
    helix::PurgeFlowTracker purge_flow_;
    /// The running step that owns other phases' signals; guarded by state_mutex_.
    helix::OwningStep owning_step_;

    /// Max gap between consecutive probe lines before resetting counters.
    /// Handles printers that emit "probe at" for non-mesh operations (e.g.
    /// nozzle wipe) before the actual mesh calibration begins.
    static constexpr auto kMeshProbeGapReset = std::chrono::seconds(30);

    // Pre-mesh probe buffering: don't auto-enter BED_MESH from probe lines
    // until we've seen enough distinct probe POINTS to distinguish mesh
    // calibration from isolated PROBE commands (e.g. nozzle wipe on AD5M
    // Klipper mod). Counting points rather than lines matters because Klipper
    // emits two lines per touch on firmware that reports z_compensation
    // separately, which halved the effective threshold.
    helix::ProbePointCounter pre_mesh_points_;
    helix::sim::SimulatedClock::time_point pre_mesh_last_probe_time_;

    /// Distinct pre-mesh probe points required before auto-entering BED_MESH.
    /// Must clear the largest non-mesh probe burst any firmware emits: K2 Plus
    /// BOX_NOZZLE_CLEAN touches 3 points on the wipe strip, Voron 2.4 QGL
    /// touches 4 corner pads, AD5M nozzle wipe touches 1-2. Every real mesh is
    /// far larger, so 5 costs nothing on the firmwares that need this path.
    static constexpr int kMeshProbeEntryThreshold = 5;

    // Targets used in last compute_predicted_weights() call — used to detect
    // when heater targets change (e.g. macro issues M109 after bed-first heating)
    // and weights need recomputing to include the new heating phase.
    int weights_ext_target_ = 0;
    int weights_bed_target_ = 0;
    // Whether that call had a measured heating rate for each heater. The first
    // measurement can be several times the default a size guess supplies, so
    // it recomputes the weights as a new target does; later EMA drift is left
    // to the countdown's monotonic guards.
    bool weights_ext_measured_ = false;
    bool weights_bed_measured_ = false;

    /// The highest reading a heater has shown under its current target, in
    /// decidegrees; high is -1 until a fallback tick samples the heater.
    struct HeaterHighWater {
        int high = -1;
        int target = 0;
    };

    /**
     * @brief Whether a heater climbed a whole degree above its highest reading
     *
     * A bed held near its target swings a degree or so for as long as it is
     * held, so a return to an earlier high is not progress. A new target, or
     * none, restarts the mark at the current reading. Temperatures are
     * decidegrees.
     */
    static bool heater_climbed(int temp, int target, HeaterHighWater& mark);

    /// Whether a heater is within the at-target band (2C), or has no target.
    /// Degrees.
    [[nodiscard]] static bool at_target(int temp, int target);

    // Climbing-heater marks. Main thread only, like the targets above.
    HeaterHighWater bed_climb_;
    HeaterHighWater ext_climb_;

    // Silent-phase progression (firmwares with silent cleaning/purge macros).
    // temps_ready_time_ is set the first time temps become ready (and remains
    // set across subsequent ticks); silent_progression_idx_ tracks how many
    // SilentPhaseEntry items have already fired. See
    // PrintStartProfile::SilentPhaseEntry for semantics.
    helix::sim::SimulatedClock::time_point temps_ready_time_; // {} = not yet ready
    size_t silent_progression_idx_ = 0;

    // Set true the moment any real firmware signal is observed for this print
    // (HELIX:PHASE, profile tag/signal/pattern/state match, PRINT_START
    // marker, or RESPOND completion). Status-signal rules are excluded: they
    // infer from the same frames the heuristic below reads, not from narration.
    // Gates the proactive temperature heuristic: once the firmware is actively
    // narrating its PRINT_START sequence it is authoritative, so the "temps
    // ready → INITIALIZING" fallback must not bounce the displayed phase back
    // to the generic "Preparing Print...". Atomic — written from the WebSocket
    // background thread (on_gcode_response, handle_phase_object_status) and
    // read from the main thread (check_fallback_completion).
    std::atomic<bool> real_signal_seen_{false};

    // Latched true the first time current_layer is observed < 1 since this
    // print started. Arms the layer-1 completion edge so a stale positive layer
    // from the previous print (before reset_for_new_print() zeroes the subject)
    // can't trigger the pre-print → printing hand-off. Reset in start()/reset().
    // Written/read on the main thread (LVGL observer callbacks); atomic for
    // consistency with the other cross-method flags above.
    std::atomic<bool> layer_zero_seen_{false};

    // Second, equally authoritative route out of Preparing for the case where
    // the 0 sample is never delivered: the first layer value seen while active
    // (-1 = none yet) and whether a later sample exceeded it. Same threading
    // note as layer_zero_seen_. Reset in start()/reset().
    std::atomic<int> first_layer_observed_{-1};
    std::atomic<bool> layer_advanced_{false};

    // LVGL timer for periodic ETA updates (main thread only)
    lv_timer_t* eta_timer_ = nullptr;
    static constexpr uint32_t kEtaUpdateIntervalMs = 5000;

    /**
     * @brief Update ETA display from timer callback (main thread)
     */
    void update_eta_display();

    /**
     * @brief Query configfile for bed_mesh probe_count to get expected total
     *
     * Fires an async query when entering BED_MESH phase. The response sets
     * mesh_probe_total_ for deterministic progress and ETA extrapolation.
     */
    void query_mesh_probe_count();

    /**
     * @brief Feed current temperature readings to ThermalRateModel during heating phases
     */
    void feed_thermal_sample();

    /// Every per-run field back to its start-of-run value. Caller holds state_mutex_.
    void reset_run_locked();

    /**
     * @brief Load prediction entries from helix::Config on start()
     */
    void load_prediction_history();

    /**
     * @brief Compute duration-proportional weights from predicted durations
     *
     * Combines thermal model estimates for heating phases with predictor
     * estimates for non-heating phases to assign each phase a weight
     * proportional to its fraction of total predicted time.
     */
    void compute_predicted_weights();

    /**
     * @brief Compute heating fraction for current phase based on temperature progress
     *
     * Returns 0.0-1.0 representing how far the current heater has progressed
     * from its starting temperature toward the target.
     */
    float compute_heating_fraction() const;

    /**
     * @brief Compute the heating fraction for an arbitrary phase
     *
     * Concurrent-heat firmwares (K1/K2 class) run heating in parallel with
     * homing, wiping and meshing, so a heating phase that is no longer
     * current may still be physically running. This is the phase-aware
     * generalization compute_heating_fraction() delegates to.
     */
    float compute_heating_fraction_for_locked(helix::PrintStartPhase phase) const;

    /**
     * @brief True once the phase's heater has reached its target (within 2C)
     *
     * The completion criterion for heating phases on concurrent-heat
     * firmware: the marker passing means the chain moved on, not that the
     * heater finished.
     */
    bool heating_target_reached_locked(helix::PrintStartPhase phase) const;

    /**
     * @brief Enter BED_MESH and credit any buffered pre-mesh probes
     *
     * update_phase() clears the mesh counters, so the probes buffered while
     * approaching the mesh (console pre-mesh points, or samples seen before
     * the position classifier's sweep-march verdict) have to be re-applied
     * after it returns — they are the sweep's first points. Every BED_MESH
     * promotion path (console threshold, bed-mesh flap, sweep march) goes
     * through here so none of them drops the count.
     */
    void enter_bed_mesh_with_buffer(const char* message);

    /**
     * @brief Save current print's phase timings to prediction history
     *
     * Called on COMPLETE. Computes per-phase durations from timestamps,
     * adds entry to predictor, and persists to Config.
     */
    void save_prediction_entry();
};
