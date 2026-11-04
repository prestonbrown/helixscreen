// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_belt_names_modal.h"
#include "ui_belt_retest_menu.h"
#include "ui_observer_guard.h"
#include "ui_timer_guard.h"
#include "ui_widget_ref.h"

#include "belt_gating.h"
#include "belt_settle.h"
#include "belt_tension_calibrator.h"
#include "belt_tension_types.h"
#include "belt_tune_session.h"
#include "live_sweep_preview.h"
#include "memory_utils.h"
#include "operation_timeout_guard.h"
#include "overlay_base.h"
#include "platform_capabilities.h"
#include "resonance_console.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

// Forward declaration - see ui_frequency_response_chart.h
struct ui_frequency_response_chart_t;

namespace helix {
class IMoonrakerClient;
}

/// Test hook: tests/unit/test_belt_tension_panel_states.cpp reads the chart
/// and ghost series, which no subject exposes.
class BeltPanelFixture;

/**
 * @file ui_panel_belt_tension.h
 * @brief Belt tension panel: two resonance sweeps, compared
 *
 * Runs Klipper's TEST_RESONANCES down each CoreXY belt diagonal and compares
 * the resulting curves. Five view states drive one XML layout:
 * - START: gate result + hardware summary + start button
 * - RUNNING: the settling laps, then one sweep at a time, live chart cursor +
 *   elapsed time
 * - RESULTS: strongest peak pair, verdict, similarity, ghost of the previous run
 * - ERROR: the failure, with retry
 * - TUNING: the guided loop (#1736). BeltTuneSession decides whether the belts
 *   already match, which zone a calibration turn moved, the advice and each
 *   round's outcome; the panel runs the calibration's full sweeps and each
 *   round's narrow sweeps, and renders what the session says.
 *
 * START's action is gated on `bt_can_start`, which is driven solely by
 * evaluate_belt_gate() plus a klippy-READY leg. Hiding the Advanced-panel menu
 * row is not a gate: the panel is still reachable by `ctl navigate`, by a deep
 * link, and by a printer whose accelerometer drops out after entry. Binding
 * the button's disabled state to a subject holds in all three cases.
 *
 * ## Usage:
 * ```cpp
 * auto& panel = get_global_belt_tension_panel();
 * panel.init_subjects();
 * panel.create(screen);
 * panel.show();
 * ```
 */
class BeltTensionPanel : public OverlayBase {
  public:
    enum class ViewState {
        START = 0,   ///< gate result + hardware summary + start button
        RUNNING = 1, ///< sweeping one path
        RESULTS = 2, ///< both paths measured, compared
        ERROR = 3,
        TUNING = 4, ///< guided tuning loop, phased by TunePhase
    };

    /// bt_tune_phase: what the TUNING view is waiting on.
    enum class TunePhase {
        ADJUST = 0,          ///< the user turns a tensioner, then re-measures
        SWEEPING = 1,        ///< settling, then a calibration sweep or a narrow round
        MATCHED = 2,         ///< the zone is matched (or none is left)
        CALIBRATE = 3,       ///< the user makes the calibration turn, then measures
        ALREADY_MATCHED = 4, ///< round 0 already matches: Good Enough or Calibrate Anyway
    };

    /// What a run feeds and where it lands.
    enum class RunKind {
        CHECK,       ///< Start, Re-test or Confirm: full sweeps into runs_, RUNNING then RESULTS
        CALIBRATION, ///< full sweeps of both paths for the session's calibration step
        TUNE_ROUND,  ///< both paths over the session's sweep_window()
    };

    /// A sweep that goes this long without a progress line is treated as
    /// dead: Klipper prints a "Testing frequency" line every second, so two
    /// minutes of silence means the run is not coming back.
    static constexpr uint32_t kStallTimeoutMs = 120000;

    /// Pairs and unpaired peaks named in the results facts and marked on the
    /// chart, strongest first.
    static constexpr size_t kMaxListedPeaks = 3;

    /// How often RESULTS refreshes the notes' ages.
    static constexpr uint32_t kNoteAgeRefreshMs = 30000;

    BeltTensionPanel() = default;
    ~BeltTensionPanel() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void deinit_subjects();
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* xml_component() const override {
        return "panel_belt_tension";
    }

    const char* get_name() const override {
        return "Belt Tension";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;
    void on_ui_destroyed() override;

    //
    // === Public API ===
    //

    using OverlayBase::show;
    void show();
    void set_api(helix::IMoonrakerClient* client, helix::IMoonrakerAPI* api);

    /// Pin the platform tier the chart decision uses, so tests can force
    /// EMBEDDED (no chart at all). Call before create(); production reads
    /// PlatformCapabilities::detect() when the first measurement starts.
    void set_render_tier_for_test(helix::PlatformTier tier, bool supports_animations);

    /// Pin the memory the low-memory check reads. Production reads the host's
    /// own memory, which is the printer's: the check only runs co-located.
    void set_memory_for_test(const helix::MemoryInfo& mem);

    //
    // === Event Handlers (public for XML callbacks) ===
    //

    void handle_start_clicked();
    void handle_stop_clicked();
    void handle_retry_clicked();
    void handle_retest_clicked(helix::calibration::BeltPath path);
    /// Re-test the path RESULTS shows in header slot @p slot (0 = left).
    void handle_retest_slot_clicked(int slot);
    /// The compact layouts' single Re-test button: a menu of the two paths.
    void handle_retest_menu_clicked(lv_obj_t* anchor);

    // Which belt is which
    /// Open the "Which belt is which?" dialog.
    void handle_names_clicked();
    /// Buzz @p path's motor (STEPPER_BUZZ), so the user can see which belt it
    /// drives. Allowed exactly when Start is, so never during a sweep or
    /// while another buzz is still running.
    void handle_identify_clicked(helix::calibration::BeltPath path);

    // Tuning loop
    void handle_tune_clicked();
    void handle_tune_measure_clicked();
    /// "Good Enough": back to RESULTS as last shown, no sweep.
    void handle_tune_done_clicked();
    void handle_tune_swap_clicked();
    /// The turn stepper: +1 / -1 steps of belt_tune::kTurnStep, clamped to
    /// [0, kTurnMax]. Zero re-measures without a turn.
    void handle_tune_amount_step(int direction);
    void handle_tune_next_zone_clicked();
    /// A full A+B check against the round-0 similarity, landing on RESULTS.
    void handle_tune_confirm_clicked();
    void handle_tune_keep_clicked();
    /// ALREADY_MATCHED's secondary action: go on to the calibration turn.
    void handle_tune_calibrate_anyway_clicked();

  private:
    /// One path's measurement history. `previous` holds the curve the
    /// re-test is measuring against, so RESULTS can ghost it.
    struct PathRun {
        helix::calibration::BeltCurve curve;    ///< psd_xyz
        helix::calibration::BeltCurve in_plane; ///< psd_x + psd_y; empty when the CSV had neither
        helix::calibration::BeltCurve previous;
        bool has = false;
        bool has_previous = false;
        uint32_t measured_at_ms = 0;
    };

    void set_view_state(ViewState state);
    void on_hardware_detected(const helix::calibration::BeltTensionHardware& hw);
    void on_error(const std::string& message);

    //
    // === Run orchestration ===
    //

    /// Queue paths for measurement. `clear_previous` drops both paths' ghost
    /// history (Start runs fresh); a single-path re-test keeps the sibling's.
    /// A CHECK switches to RUNNING and lands on RESULTS; a CALIBRATION or
    /// TUNE_ROUND keeps the view on TUNING (phase SWEEPING) and feeds the
    /// session instead of `runs_`. A TUNE_ROUND sweeps the session's
    /// sweep_window(). `settle_first = false` skips the settling laps.
    void begin_run(const std::vector<helix::calibration::BeltPath>& queue, bool clear_previous,
                   RunKind kind = RunKind::CHECK, bool settle_first = true);
    void start_next_measurement();
    /// Re-read [resonance_tester] for its probe point, then run the settling
    /// laps; the run's first sweep starts on their ack.
    void start_settle();
    void on_settle_done();
    void on_settle_refused(helix::calibration::BeltSettleRefusal refusal);
    /// The laps' inputs from the printer's limits, homing origin, max velocity
    /// and homed axes, with the probe point from sweep_cfg_.
    [[nodiscard]] helix::calibration::BeltSettleInputs settle_inputs() const;
    /// Seconds the laps take on this printer; 0 when they cannot be planned.
    [[nodiscard]] float settle_seconds() const;
    void on_sweep_progress(int percent, float freq_hz);
    void on_sweep_complete(helix::calibration::BeltPath path,
                           helix::calibration::BeltResponse response);
    void on_sweep_error(const std::string& message);
    /// The stall guard fired: no progress line for kStallTimeoutMs.
    void on_stall();
    /// Queue empty: compare the runs and populate RESULTS, or error when a
    /// sweep returned too little in-band frequency data to compare.
    void finish_run();
    void populate_results(const helix::calibration::BeltComparison& cmp);
    /// Cancel the sweep, the stall guard, the elapsed timer and the queue.
    void cancel_run();
    void back_to_start();
    /// RUNNING, or TUNING while a round sweeps (the settling laps included).
    [[nodiscard]] bool measuring() const;

    //
    // === Tuning loop ===
    //

    /// Sweep both paths over the session's sweep_window(), twice for a
    /// baseline pair. Only the BASELINE chained onto a calibration sweep skips
    /// the laps: no belt changed between.
    void start_tune_round(bool settle_first = true);
    /// The run's last pair is in: score the round and leave SWEEPING.
    void finish_tune_round();
    /// The calibration run's two full sweeps are in: feed the session, then
    /// chain the BASELINE, ask for another turn, or give up.
    void finish_calibration();
    /// Feeds tune_curves_ to the session as one round; returns it.
    const helix::calibration::BeltTuneRound& add_tune_round();
    /// The phase the session's stage asks for, outside a sweep.
    [[nodiscard]] TunePhase tune_phase_for_stage() const;
    void set_tune_phase(TunePhase phase);
    /// Every bt_tune_* subject, and the gauge, from the session.
    void refresh_tune();
    [[nodiscard]] std::string tune_last_message() const;
    /// The stepper restarts at zero: "not turned".
    void reset_tune_turns();
    /// The gauge's half-span for this zone: wide enough for its first gap.
    void reset_tune_gauge_range();
    /// Drop the session and its curves.
    void end_tuning();
    /// Minutes a full A+B check takes at the printer's sweep rate, laps included.
    [[nodiscard]] int full_check_minutes() const;
    /// Minutes a tuning round takes over the swept `window`, laps included:
    /// both paths, `pairs` times.
    [[nodiscard]] int round_minutes(const helix::calibration::BeltSweepWindow& window,
                                    int pairs) const;
    /// Pairs the next round sweeps: two for a baseline (and after a baseline
    /// that did not repeat), else one.
    [[nodiscard]] int tune_round_pairs() const;

    //
    // === Subject refresh ===
    //

    /// bt_slot_*_note from the runs, in header-slot order: "sweeping" for the running path, "done"
    /// for a path this run already measured, else an age ("just now", "3 min ago"). Touches no
    /// chart.
    void refresh_notes();
    /// bt_similarity_was: "was N%" after a re-test or Confirm, else empty.
    void refresh_similarity_was();
    /// Every subject and drawing that names a path, from the saved layout:
    /// the headers and Re-test buttons, the unpaired-peak line, run title, the
    /// tuning strings and the START sketch.
    void refresh_names();
    /// bt_facts and bt_unpaired from a comparison, in header-slot order and
    /// under the current path names.
    void refresh_peak_lines(const helix::calibration::BeltComparison& cmp);
    void set_identify_status(int state, const std::string& text);
    void hide_names_dialog();
    /// RESULTS keeps the notes' ages current on this timer.
    void start_note_age_timer();
    /// bt_run_title for the path sweeping now; the laps keep their own.
    void refresh_run_title();
    void refresh_run_detail();
    void start_elapsed_timer();

    //
    // === Gate ===
    //

    /// The single place the gate is computed. Nothing else may decide whether
    /// Start is live.
    void refresh_gate();
    /// Attach the gate's subject observers once. Idempotent: the accelerometer
    /// subject is owned by PrinterCapabilitiesState and may not exist yet the
    /// first time this runs, so activation retries.
    void ensure_gate_observers();
    /// Fetch the klippy UDS path from Moonraker and probe co-location.
    void probe_klippy_socket();
    /// Ask the printer its [resonance_tester] range for bt_hw_sweep.
    void query_hw_facts();
    /// bt_hw_sweep from sweep_cfg_ and the laps; nothing until the range is known.
    void refresh_hw_sweep();
    /// Adopt a [resonance_tester] answer; Tune Belts waits for one the printer
    /// reported (bt_tune_range_known).
    void set_sweep_config(const helix::calibration::ResonanceTesterConfig& cfg);
    /// Ask the calibrator for kinematics and the accelerometer; the answer
    /// clears detection_pending_.
    void start_hardware_detection();

    //
    // === Chart ===
    //

    /// Series color for a path: primary for A, warning for B. One helper so a
    /// dedicated path palette lands in one place.
    [[nodiscard]] static lv_color_t path_color(helix::calibration::BeltPath path);
    /// Create the chart on first use (never on EMBEDDED). Returns null when
    /// the tier forbids a chart or the view carries no host.
    ui_frequency_response_chart_t* ensure_chart();
    void destroy_chart();
    /// Re-read belt_path_a/b into the chart's series after a theme change.
    void apply_path_colors();
    /// Park the chart obj in the RUNNING host; called when a new run starts.
    void chart_to_running_host();
    /// The host the chart belongs in for `state`: the ultrawide host beside the
    /// sidebar when ui_is_ultrawide is 1, else that state's own host. Null for
    /// states that show no chart.
    [[nodiscard]] lv_obj_t* chart_host_for(ViewState state) const;
    /// Move the chart into chart_host_for(state) when it lives elsewhere.
    void place_chart(ViewState state);
    /// Fit the chart's axes to the curves it holds (or the sweep range before any).
    void push_chart_data();
    void push_chart_markers(const helix::calibration::BeltComparison& cmp);
    /// TUNING: both paths' in-plane curves, over the band until a zone is
    /// known and over the zone after, the adjusted path's earlier rounds
    /// muted, hollow rings at the session's centroids.
    void push_tune_chart_data();
    /// One curve into a series as a percentage of `tallest`; hidden when empty.
    void push_series_data(int series_id, const helix::calibration::BeltCurve& curve, float tallest);
    /// Arm the live preview for `path`'s sweep, which starts now. No-op without
    /// a chart or without a live_sweep_target().
    void arm_preview(helix::calibration::BeltPath path);
    /// Main thread: the preview's points so far for `path`.
    void on_preview_points(helix::calibration::BeltPath path,
                           const std::vector<helix::calibration::SweepPoint>& points);
    /// Lay each live path's preview out on the chart's current axis in place of
    /// its own series. Runs at the end of every chart push.
    void refresh_preview_series();
    /// Stop the stream and drop both previews; the next chart push shows each
    /// path's own series again.
    void clear_previews();
    void run_after_ram_check(std::function<void()> go);
    /// Re-check the gate before any sweep starts; false (logged) when it is
    /// shut. `action` names the button in the log.
    [[nodiscard]] bool gate_allows_sweep(const char* action);

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // View state subject lives in the .cpp (static) but registers here.
    lv_subject_t can_start_subject_{};
    lv_subject_t gate_message_subject_{};
    char gate_message_buf_[128] = {};

    lv_subject_t hw_kinematics_subject_{};
    char hw_kinematics_buf_[64] = {};
    lv_subject_t hw_accel_subject_{};
    char hw_accel_buf_[64] = {};
    lv_subject_t hw_sweep_subject_{};
    char hw_sweep_buf_[64] = {};

    lv_subject_t run_title_subject_{};
    char run_title_buf_[96] = {};
    lv_subject_t run_detail_subject_{};
    char run_detail_buf_[96] = {};
    lv_subject_t running_path_subject_{};

    // Which belt is which: the Identify status in the names dialog
    /// 0 idle, 1 a motor is buzzing, 2 the buzz failed
    lv_subject_t identify_state_subject_{};
    lv_subject_t identify_status_subject_{};
    char identify_status_buf_[192] = {};

    // RESULTS header slots, left then right (helix::ui::belt_path_in_slot)
    lv_subject_t slot_note_subject_[2]{};
    char slot_note_buf_[2][64] = {};
    lv_subject_t slot_name_subject_[2]{};
    char slot_name_buf_[2][48] = {};
    /// 1 when the left slot shows Path B
    lv_subject_t slots_swapped_subject_{};

    lv_subject_t verdict_subject_{};
    lv_subject_t verdict_text_subject_{};
    char verdict_text_buf_[128] = {};
    lv_subject_t facts_subject_{};
    char facts_buf_[256] = {};
    lv_subject_t unpaired_subject_{};
    char unpaired_buf_[256] = {};
    lv_subject_t has_unpaired_subject_{};
    lv_subject_t similarity_subject_{};
    char similarity_buf_[16] = {};
    lv_subject_t similarity_was_subject_{};
    char similarity_was_buf_[32] = {};
    lv_subject_t has_similarity_was_subject_{};
    lv_subject_t chart_available_subject_{};
    lv_subject_t error_message_subject_{};
    char error_message_buf_[256] = {};

    // Tuning loop
    lv_subject_t tune_cta_subject_{};
    char tune_cta_buf_[32] = {};
    lv_subject_t tune_phase_subject_{};
    lv_subject_t tune_kind_subject_{};
    lv_subject_t tune_dir_subject_{};
    lv_subject_t tune_path_subject_{};
    lv_subject_t tune_severity_subject_{};
    lv_subject_t tune_baseline_subject_{};
    lv_subject_t tune_has_zone_subject_{};
    lv_subject_t tune_has_next_zone_subject_{};
    lv_subject_t tune_shape_warning_subject_{};
    lv_subject_t tune_instruction_subject_{};
    char tune_instruction_buf_[160] = {};
    lv_subject_t tune_amount_subject_{};
    char tune_amount_buf_[32] = {};
    lv_subject_t tune_gap_subject_{};
    char tune_gap_buf_[24] = {};
    lv_subject_t tune_gap_caption_subject_{};
    char tune_gap_caption_buf_[128] = {};
    lv_subject_t tune_round_subject_{};
    char tune_round_buf_[32] = {};
    lv_subject_t tune_zone_subject_{};
    char tune_zone_buf_[48] = {};
    lv_subject_t tune_rate_subject_{};
    char tune_rate_buf_[32] = {};
    lv_subject_t tune_last_subject_{};
    char tune_last_buf_[384] = {};
    lv_subject_t tune_has_last_subject_{};
    lv_subject_t tune_hint_subject_{};
    char tune_hint_buf_[256] = {};
    lv_subject_t tune_power_hint_subject_{};
    char tune_power_hint_buf_[512] = {};
    lv_subject_t tune_has_power_hint_subject_{};
    lv_subject_t tune_has_gap_subject_{};
    lv_subject_t tune_unrepeatable_subject_{};
    lv_subject_t tune_can_swap_subject_{};
    lv_subject_t tune_range_known_subject_{};
    lv_subject_t tune_measure_short_label_subject_{};
    char tune_measure_short_label_buf_[32] = {};
    lv_subject_t tune_tensioner_subject_{};
    char tune_tensioner_buf_[96] = {};
    lv_subject_t tune_measure_label_subject_{};
    char tune_measure_label_buf_[48] = {};
    lv_subject_t tune_confirm_label_subject_{};
    char tune_confirm_label_buf_[64] = {};
    lv_subject_t tune_next_zone_label_subject_{};
    char tune_next_zone_label_buf_[64] = {};

    // Gate observers. Every one carries PrinterState's own SubjectLifetime -
    // the observe<V> factory requires it as its fourth parameter; a
    // default-constructed token leaves the guard tokenless (#705).
    ObserverGuard accel_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard connected_observer_;
    ObserverGuard klippy_observer_;
    ObserverGuard max_velocity_observer_;
    ObserverGuard theme_observer_;
    ObserverGuard ultrawide_observer_;
    ObserverGuard belt_layout_observer_;
    ObserverGuard language_observer_;
    bool gate_observers_wired_ = false;

    // Klippy's UDS path, from Moonraker's /server/config. Reachability is
    // probed once per activation, not per gate refresh: the gate recomputes on
    // every subject change and a connect() syscall each time would be waste.
    std::string klippy_socket_path_;
    bool klippy_socket_reachable_ = false;

    // Calibrator
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    helix::IMoonrakerAPI* api_ = nullptr;
    helix::IMoonrakerClient* client_ = nullptr;

    // Chart. One chart, re-parented between the RUNNING and RESULTS hosts.
    ui_frequency_response_chart_t* chart_ = nullptr;
    int series_[2] = {-1, -1};
    int ghost_series_[2] = {-1, -1};
    /// Per path, the adjusted path's two previous tuning rounds, oldest first.
    int tune_ghost_series_[2][2] = {{-1, -1}, {-1, -1}};
    lv_obj_t* chart_host_running_ = nullptr;
    lv_obj_t* chart_host_results_ = nullptr;
    lv_obj_t* chart_host_tuning_ = nullptr;
    lv_obj_t* chart_host_wide_ = nullptr;
    /// One series per path for its live preview, styled like series_.
    int preview_series_[2] = {-1, -1};
    helix::calibration::LiveSweepPreview preview_;
    /// The points each path's preview shows.
    std::vector<helix::calibration::SweepPoint> preview_points_[2];
    /// The path's preview stands in for its series: from its first point until
    /// the chart draws the CSV curve that preview was measuring. A full check
    /// swaps at that path's CSV; a tuning or calibration run keeps every
    /// preview until the run's last sweep is in.
    bool preview_live_[2] = {false, false};

    /// Render tier override for tests; nullopt reads PlatformCapabilities.
    std::optional<helix::PlatformTier> tier_override_;
    bool tier_animations_ = true;
    /// The printer's [resonance_tester] sweep; Klipper defaults until the query lands.
    helix::calibration::ResonanceTesterConfig sweep_cfg_;
    bool hw_sweep_known_ = false; ///< a [resonance_tester] query has answered

    // Hardware detection cache. Feeds BeltGateInputs::is_corexy.
    helix::calibration::BeltTensionHardware detected_hw_;
    bool detection_pending_ = false; ///< detect_hardware() has not answered yet

    // Run state
    PathRun runs_[2];
    // The similarity of the latest comparison, and the one a re-test or
    // Confirm is measuring against: bt_similarity_was reads "was N%" from it.
    float similarity_percent_ = 0.0f;
    float was_similarity_percent_ = 0.0f;
    std::optional<helix::MemoryInfo> mem_override_;
    helix::ui::WidgetRef low_ram_dialog_;
    /// The low-RAM warning was accepted in this tuning session; later rounds
    /// do not ask again.
    bool tune_low_ram_accepted_ = false;
    /// Deactivated for the screensaver: the next activation is a wake, which
    /// keeps the view, the run and the tuning session.
    bool suspended_ = false;
    /// RESULTS's Re-test menu on compact layouts.
    helix::ui::BeltRetestMenu retest_menu_;
    /// "Which belt is which?", reshown on each open.
    helix::ui::BeltNamesModal names_modal_;
    /// Close the Re-test menu and drop its callback, so nothing it dispatches
    /// later can start a sweep.
    void hide_retest_menu();
    void hide_low_ram_dialog();
    std::vector<helix::calibration::BeltPath> queue_;
    /// Size of the queue this run started with, for "2 of 2" detail lines.
    size_t run_queue_total_ = 0;
    /// True from begin_run() to cancel_run(): sweep callbacks that land after
    /// a cancel belong to a run this panel has already abandoned.
    bool run_active_ = false;
    uint32_t run_started_ms_ = 0;
    /// The run is in its settling laps; no sweep has started yet.
    bool settling_ = false;
    /// The settle is waiting on its [resonance_tester] query, which a
    /// suspension expires; a wake re-asks it.
    bool settle_query_pending_ = false;
    /// Bumped by every start_settle() and cancel_run(); a query answer acts
    /// only while it still holds the current value.
    uint32_t settle_generation_ = 0;
    helix::ui::LvglTimerGuard elapsed_timer_;
    helix::ui::LvglTimerGuard note_age_timer_;
    OperationTimeoutGuard stall_guard_;
    RunKind run_kind_ = RunKind::CHECK;
    /// The window of the tuning round in flight; nullopt for full sweeps.
    std::optional<helix::calibration::BeltSweepWindow> run_window_;

    // Tuning loop
    /// The comparison RESULTS shows; round 0 of a tuning session.
    helix::calibration::BeltComparison last_cmp_;
    std::optional<helix::calibration::BeltTuneSession> tune_;
    TunePhase tune_phase_ = TunePhase::ADJUST;
    /// The latest calibration or round sweeps; valid once tune_has_curves_.
    helix::calibration::BeltResponse tune_curves_[2];
    bool tune_has_curves_ = false;
    /// The latest calibration step's verdict; cleared when a zone is entered.
    std::optional<helix::calibration::BeltCalibrationResult> tune_last_calibration_;
    /// The adjusted path's curves from earlier rounds of this zone, oldest
    /// first, at most two.
    std::vector<helix::calibration::BeltCurve> tune_adj_history_;
    std::optional<helix::calibration::BeltTuneOutcome> tune_last_outcome_;
    /// The direction the last round's turn was advised in: what the user acted on.
    helix::calibration::BeltTurnDirection tune_last_direction_ =
        helix::calibration::BeltTurnDirection::TIGHTEN;
    /// How far the gap moved in the last round, Hz.
    float tune_last_move_hz_ = 0.0f;
    float tune_turns_ = 0.0f; ///< the stepper; zero is "not turned"
    float tune_gauge_range_hz_ = 2.0f * helix::calibration::belt_tune::kGaugeWarnHz;
    bool tune_shape_differs_ = false; ///< round 0's tallest peaks are unpaired
    float tune_round0_similarity_ = 0.0f;
    /// Appended to the facts line by the Confirm run's RESULTS.
    std::string tune_facts_suffix_;

    friend class BeltPanelFixture;
};

// Global instance accessor
inline BeltTensionPanel& get_global_belt_tension_panel() {
    return helix::lazy_global<BeltTensionPanel>("BeltTensionPanel");
}

/// Open the Belt Tension panel over @p parent_screen, wired to the active API.
namespace helix::ui {
void open_belt_tension_panel(lv_obj_t* parent_screen);
} // namespace helix::ui

/**
 * @brief Register XML event callbacks for belt tension panel
 *
 * Call once at startup before creating any panel_belt_tension XML.
 * Registers callbacks for all button events and initializes subjects.
 */
void ui_panel_belt_tension_register_callbacks();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Registers "on_belt_tension_row_clicked" callback.
 */
void init_belt_tension_row_handler();
