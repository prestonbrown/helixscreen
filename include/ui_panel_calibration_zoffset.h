// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_calibration_holds.h"
#include "ui_calibration_result.h"

#include "calibration_owned_heat.h"
#include "calibration_run.h"
#include "core/observer_guard.h"
#include "core/operation_timeout_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "save_config_restart.h"
#include "src/ui/calibration/z_offset_tape.h"
#include "subject_managed_panel.h"
#include "z_offset_utils.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

/**
 * @file ui_panel_calibration_zoffset.h
 * @brief Z-Offset calibration panel with strategy-aware dispatch
 *
 * Interactive panel that guides the user through the paper test calibration
 * process. Supports three strategies based on printer configuration:
 * - PROBE_CALIBRATE: Klipper's PROBE_CALIBRATE -> TESTZ -> ACCEPT -> SAVE_CONFIG
 * - ENDSTOP: Z_ENDSTOP_CALIBRATE -> TESTZ -> ACCEPT -> SAVE_CONFIG
 * - FIRMWARE_MANAGED: G28 -> move to center -> G1 Z adjustments -> SET_GCODE_OFFSET
 *   (a scanning probe runs PROBE_CALIBRATE's sequence with its own calibrate command)
 *
 * ## State Machine:
 * - IDLE: Shows instructions and Start button
 * - PROBING: Waiting for calibration to begin (homes + positions)
 * - ADJUSTING: User adjusts Z with paper test (+/- buttons)
 * - SAVING: Saving offset (ACCEPT/SAVE_CONFIG or SET_GCODE_OFFSET)
 * - COMPLETE: Calibration successful
 * - ERROR: Something went wrong
 *
 * ## Usage:
 * ```cpp
 * auto& overlay = get_global_zoffset_cal_panel();
 * overlay.set_api(get_moonraker_api());
 * overlay.show(parent_screen);
 * ```
 */
class ZOffsetCalibrationPanel : public OverlayBase {
  public:
    /**
     * @brief Calibration state machine states
     */
    enum class State {
        IDLE,      ///< Ready to start, showing instructions
        PROBING,   ///< PROBE_CALIBRATE running
        ADJUSTING, ///< Interactive Z adjustment phase
        SAVING,    ///< ACCEPT sent, waiting for SAVE_CONFIG
        COMPLETE,  ///< Calibration finished successfully
        ERROR,     ///< Error occurred
        WARMING,   ///< Bed warming before calibration (6)
        REVIEW     ///< The accepted result, with Save to Config / Keep for Now / Discard (7)
    };

    ~ZOffsetCalibrationPanel() override;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Call once at startup before any panel instances are created.
     * Registers the zoffset_cal_state subject and all XML event callbacks.
     */
    void init_subjects() override;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent screen widget to attach overlay to
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* xml_component() const override {
        return "calibration_zoffset_panel";
    }

    /**
     * @brief Get human-readable overlay name
     * @return "Z-Offset Calibration"
     */
    const char* get_name() const override {
        return "Z-Offset Calibration";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets state to IDLE.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Aborts calibration if in progress.
     */
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /// Clear a bed this run warmed while the client can still send.
    void before_disconnect();

    /// Whether this panel still owns a heater target it must turn off.
    [[nodiscard]] bool holds_owned_heat() const {
        return heat_.held();
    }

    /// Freed on close; the next open rebuilds it. The calibration state lives in
    /// state_ and the zoffset_cal_state subject, never in the widgets.
    bool destroy_on_close() const override {
        return true;
    }

    //
    // === Public API ===
    //

    /**
     * @brief Set the IMoonrakerAPI for G-code commands
     *
     * @param api IMoonrakerAPI for sending commands
     */
    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    /// How a guided run's step ended.
    enum class RunEnd {
        Accepted, ///< Continue after ACCEPT answered; z is the accepted position
        Skipped,  ///< the owner chose Skip this step
        Left      ///< closed without accepting or skipping
    };
    using RunDone = std::function<void(RunEnd end, float z, helix::ZOffsetCalibrationStrategy)>;

    /// Open for a guided calibration run and start at once: the run's Before
    /// you start page stands in for the intro, so the printer homes and probes
    /// (after warming the bed when @p warm_bed) as soon as the panel shows.
    /// Accept only stages the result (ACCEPT, no SAVE_CONFIG: the run saves
    /// everything at once) and Continue returns to the run. @p on_done runs once
    /// on the main thread with how the step ended. @p step_index (0-based) and
    /// @p step_count place it in the run's header.
    void open_for_run(helix::IMoonrakerAPI* api, int step_index, int step_count, bool warm_bed,
                      RunDone on_done);
    /// The guided run stops while this panel serves it: a manual probe ends with
    /// ABORT (nothing is sent when none is running) and the panel closes.
    void abort_run();
    void handle_continue_clicked();
    void on_dismissed() override;

    /**
     * @brief Get current calibration state
     * @return Current State
     */
    State get_state() const {
        return state_;
    }

    /**
     * @brief Update Z position display (called from external state updates)
     * @param z_position Current Z position from printer state
     */
    void update_z_position(float z_position);

    /**
     * @brief Handle calibration completion/error from Moonraker
     * @param success true if calibration completed successfully
     * @param message Status message
     */
    void on_calibration_result(bool success, const std::string& message);

    /// What a state holds: warming and probing move or heat the machine; the paper
    /// test also needs the user's hands, so the screen stays awake through it.
    [[nodiscard]] static helix::calibration::StageNeeds stage_needs(State state);

  private:
    helix::calibration::StageHolds holds_;
    void on_ui_destroyed() override;

    // API reference
    // Note: overlay_root_ inherited from OverlayBase
    helix::IMoonrakerAPI* api_ = nullptr;

    // State management
    State state_ = State::IDLE;
    void show_saved_z_offset();
    /// The printer is still running a probe this panel started.
    [[nodiscard]] bool run_in_progress() const {
        return state_ == State::PROBING || state_ == State::ADJUSTING;
    }
    void set_state(State new_state);

    // UI setup (called by create())
    void setup_widgets();

    // Strategy-aware gcode command helpers
    void start_calibration();
    /// start_calibration() once the bed is known clear.
    void start_once_bed_clear();
    /// Warm-up or probing: the start once every check has passed.
    void begin_calibration();
    /// Ask before calibrating with a tool other than the probe's reference tool.
    /// @param reference_tool the printer tool the probe offset belongs to
    /// @param active_tool    the mounted tool (a lane where tools are lanes)
    void confirm_tool_then_start(int reference_tool, int active_tool);
    /// Whether a calibration may still begin now: the answer to a question can
    /// arrive after the panel left the screen, a print started or Klipper went away.
    bool can_begin_now() const;
    /// Close the mounted-tool question, if it is open, without starting anything.
    void close_tool_confirm();
    /// The printer's strategy as this panel runs it: a scanning probe runs its
    /// own calibrate command even on a FIRMWARE_MANAGED printer
    /// (zoffset_cal::run_strategy()). Read live from the profile and the probe list.
    helix::ZOffsetCalibrationStrategy resolve_run_strategy() const;
    /// The strategy of the run in progress, latched when it starts, so a probe
    /// rediscovery mid-session cannot switch its jogs, Accept or Abort to another
    /// strategy's commands; resolve_run_strategy() while no run is latched.
    helix::ZOffsetCalibrationStrategy run_strategy() const;
    /// Fix the strategy for the run starting now. Released when the panel
    /// reaches IDLE, COMPLETE or ERROR.
    void latch_run_strategy();
    void begin_probe_sequence(); ///< Start homing/probing (called after warming or directly)
    void adjust_z(float delta);
    void send_accept();
    /// The mounted-tool question will ask before this start (one shared probe, another tool
    /// mounted).
    [[nodiscard]] bool mounted_tool_question_due() const;
    void send_abort();
    void send_bed_off();
    /// Puts ABORT on the wire and leaves the panel state alone.
    void send_manual_probe_abort();
    /// Ends a manual probe session this panel's run left open. Klipper registers
    /// ABORT only during a session, so without one nothing is sent.
    void abort_manual_probe_if_active();

    // Event handlers
    void handle_start_clicked();
    void handle_z_adjust(float delta);
    void handle_accept_clicked();
    void handle_abort_clicked();
    /// In a run, close the panel so the run asks whether to go on; nothing
    /// outside a run.
    void leave_run_unaccepted();
    void handle_done_clicked();
    void handle_result_save();
    void handle_result_keep();
    void handle_result_discard();
    /// After a standalone ACCEPT: reads what Klipper staged and shows the result page.
    void show_review();
    void handle_retry_clicked();
    void handle_warm_bed_toggled();

    /// Turn off bed heater if we turned it on for calibration
    void turn_off_bed_if_needed();

    /// The readouts' texts: zoffset_saved_text, zoffset_z_text, zoffset_final_text and
    /// zoffset_error_text. A no-op until init_subjects().
    void set_readout(helix::Subject& subject, const char* text);
    /// The result texts a freshly built overlay shows
    void reset_result_texts();

    /// 0=IDLE, 1=PROBING, 2=ADJUSTING, 3=SAVING, 4=COMPLETE, 5=ERROR
    helix::Subject cal_state_subject_{};
    /// Warm bed toggle: 0=off, 1=on
    helix::Subject warm_bed_subject_{};
    helix::Subject saved_text_subject_{};
    helix::Subject z_text_subject_{};
    helix::Subject final_text_subject_{};
    helix::Subject error_text_subject_{};
    char saved_text_buf_[32] = {0};
    char z_text_buf_[32] = {0};
    char final_text_buf_[96] = {0};
    char error_text_buf_[512] = {0};

    /// Set while the panel serves a guided run; see open_for_run().
    RunDone run_done_;
    /// The run's "Warm the bed first" choice, which stands in for the Warm Bed
    /// switch while the panel serves a run.
    bool run_warm_bed_ = false;
    /// open_for_run() queued its show; the activation it brings starts probing.
    bool run_start_pending_ = false;
    /// Warm the bed before probing: the run's choice in a run, else the switch.
    [[nodiscard]] bool wants_warm_bed() const;
    /// 1 while the panel serves a guided run.
    helix::Subject run_mode_subject_{};
    helix::Subject accepting_subject_{};
    helix::calibration::CalibrationResultView result_view_{"zoffset_result"};
    std::unique_ptr<helix::calibration::RunFinisher> discarder_;
    /// 1 outside a guided run: the footer hides Skip there.
    helix::Subject not_run_subject_{};
    helix::Subject back_text_subject_{};
    char back_text_buf_[32] = {0};
    // The run's header: where this step sits in the run.
    helix::Subject run_step_count_subject_{};
    helix::Subject run_step_current_subject_{};
    helix::Subject run_subtitle_subject_{};
    helix::Subject run_subtitle_short_subject_{};
    char run_subtitle_buf_[64] = {0};
    char run_subtitle_short_buf_[32] = {0};
    int run_step_index_ = 0;
    int run_step_count_ = 1;
    /// Texts this panel formats from translated strings, written again on a
    /// live language switch.
    void render_translated_texts();
    ObserverGuard language_observer_;

    // The paper test's tape: the selected step (an index into ztape::kStepMicrons)
    // and its size in microns, the Z the nozzle is at in microns, the estimated
    // drag point in microns (ztape::kNoEstimate for none), and the line under
    // the readout in two lengths with its tone (ztape::GuideTone).
    helix::Subject step_subject_{};
    helix::Subject step_microns_subject_{};
    helix::Subject z_microns_subject_{};
    helix::Subject estimate_subject_{};
    helix::Subject guide_text_subject_{};
    helix::Subject guide_short_subject_{};
    helix::Subject guide_tone_subject_{};
    char guide_text_buf_[128] = {0};
    char guide_short_buf_[64] = {0};
    /// Where the estimate comes from, set by the calibrate command sent.
    helix::ztape::EstimateSource estimate_source_ = helix::ztape::EstimateSource::None;
    /// The first Z of this adjusting session, and whether this panel's own
    /// calibrate command put the nozzle there.
    std::optional<int> session_start_microns_;
    bool session_from_probe_ = false;
    void begin_adjust_session(bool from_probe);
    void refresh_guide();
    void set_run_mode(bool on);
    void handle_step_pick(int index);
    void select_step(int index);
    void handle_nudge(int direction);
    void handle_back_clicked();
    void handle_skip_clicked();
    /// Tell the run how the step ended and leave run mode.
    void finish_run(RunEnd end);

    // Current Z position during calibration
    float current_z_ = 0.0f;
    float final_offset_ = 0.0f;
    std::optional<helix::ZOffsetCalibrationStrategy> latched_strategy_;
    float cumulative_z_delta_ = 0.0f; ///< Tracks total Z adjustment in gcode_offset mode

    // Warm bed for calibration
    bool tool_confirm_pending_ = false;       ///< The mounted-tool question is on screen
    lv_obj_t* tool_confirm_dialog_ = nullptr; ///< That question's dialog, while open
    /// The bed this panel warmed, until something turns it off.
    helix::CalibrationOwnedHeat heat_{"ZOffsetCal", [this](const std::string&) { send_bed_off(); }};
    int warm_bed_target_deci_ = 0;      ///< Target temp in decidegrees during WARMING
    SubjectLifetime bed_temp_lifetime_; ///< Lifetime token for bed temp subject (#734)
    ObserverGuard bed_temp_observer_;   ///< Watches bed temp during WARMING phase

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Observer guards for manual_probe state changes (RAII cleanup)
    ObserverGuard manual_probe_active_observer_;
    ObserverGuard manual_probe_z_observer_;

    // Operation timeout guard (PROBING: 180s, SAVING: 30s)
    OperationTimeoutGuard operation_guard_;
    static constexpr uint32_t kWarmingTimeoutMs = 300000; // 5 min for bed to reach temp
    static constexpr uint32_t kSavingTimeoutMs = 30000;

    /// Arm (or re-arm) the SAVING-state timeout.
    void arm_saving_timeout();

    /// Start/stop watching klippy state for the duration of a save.
    void begin_saving_restart_watch();
    void end_saving_restart_watch();

    /// SAVE_CONFIG restarts Klipper; some printers chain a second config write +
    /// restart tens of seconds later. Each extension buys another
    /// kSavingTimeoutMs, but only if this save actually restarted Klipper — so
    /// a genuinely hung save still fails after one kSavingTimeoutMs.
    static constexpr unsigned kSavingTimeoutMaxExtensions = 4;
    unsigned saving_timeout_extensions_ = 0;

    /// Latches restart activity seen during a save. Reset on every entry to and
    /// exit from State::SAVING so repeated saves in one session start clean.
    /// Owns the SAVE_CONFIG contract for the z-offset save: absorbs the rpc the
    /// restart drops and reports success only once Klipper is back (#1359).
    helix::ui::SaveConfigWatch save_config_watch_;

    helix::ui::SaveRestartLatch save_restart_latch_;

    friend class ZOffsetCalibrationTestAccess;

    /// Watches klippy state while saving, to feed save_restart_latch_.
    /// No paired SubjectLifetime: PrinterState::get_klippy_state_subject() is a
    /// static singleton-lifetime subject (no lifetime-token overload), unlike the
    /// per-sensor bed temp subject above.
    ObserverGuard klippy_state_observer_;
};

// Global instance accessor
// Defined out of line: the ESP32 build, which excludes this panel, supplies its own.
ZOffsetCalibrationPanel& get_global_zoffset_cal_panel();

namespace helix::zoffset_cal {
/// The bed temperature (°C) the Warm Bed warm-up heats to: the config's
/// /calibration/warm_bed_temp, 45 when unset.
[[nodiscard]] int warm_bed_target_c();
} // namespace helix::zoffset_cal

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Must be called during app initialization before XML creation.
 * Registers "on_zoffset_row_clicked" callback.
 */
void init_zoffset_row_handler();

/**
 * @brief Initialize event callbacks for the Z-Offset calibration panel
 *
 * Must be called during app initialization before XML creation.
 * Registers all button click callbacks used by calibration_zoffset_panel.xml.
 */
void init_zoffset_event_callbacks();
