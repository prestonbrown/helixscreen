// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "belt_gating.h"
#include "belt_tension_calibrator.h"
#include "belt_tension_types.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class IMoonrakerAPI;

namespace helix {
class IMoonrakerClient;
}

namespace helix::calibration {
class BeltListenSession;
class BeltStreamClient;
struct AccelBatch;
} // namespace helix::calibration

// Forward declare
struct ui_frequency_response_chart_t;

/**
 * @file ui_panel_belt_tension.h
 * @brief Belt tension tuning panel for CoreXY/Cartesian belt frequency measurement
 *
 * Interactive overlay that guides users through belt tension measurement:
 * - Detects hardware (kinematics, accelerometer)
 * - Runs frequency sweeps on belt paths A and B
 * - Displays resonant frequency comparison and recommendations
 *
 * ## State Machine:
 * - START: Gate result + hardware summary + start button
 * - POSITION: Park the gantry so the free span is nominal, show which run to pluck
 * - LISTEN: Live meter for one belt at a time
 * - COMPARE: A vs B, delta, verdict
 * - ERROR: Error display with retry
 *
 * START's action is gated on `bt_can_start`, which is driven solely by
 * evaluate_belt_gate(). Hiding the Advanced-panel menu row is not a gate: the
 * panel is still reachable by `ctl navigate`, by a deep link, and by a printer
 * whose accelerometer drops out after entry. Binding the button's disabled
 * state to a subject holds in all three cases.
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
        START = 0,    ///< gate result + hardware summary + start button
        POSITION = 1, ///< park the gantry, show which run to pluck
        LISTEN = 2,   ///< live meter for one belt
        COMPARE = 3,  ///< A vs B
        ERROR = 4,
    };

    /// Positioning move, not a print move - the gantry is empty and nothing is
    /// being extruded, so this only has to be quick and undramatic.
    static constexpr double PARK_FEEDRATE_MM_MIN = 3000.0;

    /// Samples collected with the machine still before the strength gate has a
    /// floor to compare against. About one second at the measured 3053 Hz. Kept
    /// short deliberately: a pluck landing inside this window poisons the floor
    /// and every later strike is measured against an inflated baseline.
    static constexpr size_t NOISE_FLOOR_SAMPLES = 3000;

    BeltTensionPanel() = default;
    ~BeltTensionPanel() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void deinit_subjects();
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Belt Tension";
    }

    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;
    void on_ui_destroyed() override;

    //
    // === Public API ===
    //

    void show();
    void set_api(helix::IMoonrakerClient* client, IMoonrakerAPI* api);

    //
    // === Event Handlers (public for XML callbacks) ===
    //

    void handle_start_clicked();
    void handle_cancel_clicked();
    void handle_retry_clicked();
    void handle_position_confirmed();
    /// The single LISTEN action button. Its label is bound to bt_advance_label,
    /// so one button reads "Next belt" on A and "Compare" on B rather than two
    /// buttons swapping visibility.
    void handle_advance_clicked();

    /// Lets BeltTrace (and anything else outside this panel) attach an
    /// observer that releases safely across deinit_subjects()/init_subjects()
    /// re-registration, the same contract PrinterState::get_subjects_lifetime()
    /// gives observers of its subjects (#705).
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_lifetime_;
    }

    /// Read a HELIX_BELT_CAPTURE_DIR-style file and feed its ring-down and
    /// spectrum into BeltLiveData, so the LISTEN widgets draw it exactly as
    /// they would a real pluck. Driven by the `bt_replay_path` subject (set
    /// with `ctl set bt_replay_path <file>`) rather than a dedicated ctl
    /// verb - it reuses the generic subject-set plumbing every other
    /// scripted control already goes through, and BeltLiveData is a
    /// singleton independent of this panel's own lifecycle, so a replay
    /// lands even before LISTEN has ever been shown. Main thread only; does
    /// its own file I/O, which is fine for an operator-triggered diagnostic
    /// action but would not be for anything on the stream's batch path.
    void replay_capture(const std::string& path);

  private:
    /// Everything one batch produced, already reduced to what the UI shows.
    /// Assembled on the stream's loop thread and copied across to the main
    /// thread; nothing in it points back into the session.
    struct LiveSnapshot {
        size_t accepted = 0;
        float median_hz = 0.0f;
        float last_hz = 0.0f;
        bool committed = false;
        uint32_t ms_since_event = 0;
        uint32_t ms_since_reject = UINT32_MAX;
        /// True when the last rejection was a shape/spectrum rejection rather
        /// than a soft strike. "Pluck harder" is the wrong instruction there -
        /// the strike was firm and something else in the room was louder.
        bool reject_not_a_pluck = false;
        std::vector<helix::calibration::AccelSample> window;
        /// Only non-empty when this batch's event was a freshly ACCEPTED
        /// pluck - see BeltListenSession::last_spectrum(). publish_live_values()
        /// leaves BeltLiveData's spectrum alone when this is empty, so the
        /// strip holds the last analysed spectrum between plucks instead of
        /// animating noise.
        std::vector<std::pair<float, float>> spectrum;
    };

    void set_view_state(ViewState state);
    void on_hardware_detected(const helix::calibration::BeltTensionHardware& hw);
    void on_error(const std::string& message);
    /// Fill the COMPARE state from the two committed medians.
    void populate_comparison(float a_hz, float b_hz);

    //
    // === Live measurement ===
    //

    /// Open the stream and begin a fresh session for one belt.
    void start_listening(char belt);
    /// Close the stream and drop the session. Idempotent, main thread only.
    void stop_listening();
    /// Runs on BeltStreamClient's loop thread. Does the DSP and marshals only
    /// finished numbers to the main thread. Must not touch LVGL. The token is
    /// captured once when the stream starts, because LifetimeToken::defer() is
    /// the only deferral safe to call from a background thread.
    void on_batch_bg(const helix::calibration::AccelBatch& batch, const helix::LifetimeToken& tok);
    /// Main thread. Every lv_subject_set_* for the live meter happens here.
    void publish_live_values(const LiveSnapshot& snap);
    /// Main thread. A dead stream is an error, unlike a quiet one.
    void on_stream_error(const std::string& message);
    void reset_live_subjects();
    void handle_next_belt_clicked();
    void handle_compare_clicked();
    /// Read resonance_tester.accel_chip, falling back to the first sensor
    /// AccelSensorManager discovered.
    void query_accel_chip();

    /// The single place the gate is computed. Nothing else may decide whether
    /// Start is live.
    void refresh_gate();
    /// Attach the gate's subject observers once. Idempotent: the accelerometer
    /// subject is owned by PrinterCapabilitiesState and may not exist yet the
    /// first time this runs, so activation retries.
    void ensure_gate_observers();
    void handle_park_gantry();
    /// Second half of the park, run after the Y move (which sets the free
    /// span) succeeds. Centres X - see park_x_center() - or, if bounds are
    /// not known yet, leaves the toolhead where the Y move put it.
    void park_center_x();
    /// Fetch the klippy UDS path from Moonraker and probe co-location.
    void probe_klippy_socket();
    [[nodiscard]] std::optional<float> span_offset_for_current_printer() const;

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // Start screen subjects
    lv_subject_t hw_kinematics_subject_{};
    char hw_kinematics_buf_[64] = {};
    lv_subject_t hw_adxl_subject_{};
    char hw_adxl_buf_[64] = {};
    lv_subject_t target_freq_subject_{};
    char target_freq_buf_[32] = {};

    // Gate subjects - START's action is bound to these, not to a hidden menu row
    lv_subject_t can_start_subject_{};
    lv_subject_t gate_message_subject_{};
    char gate_message_buf_[128] = {};

    // Positioning subjects
    lv_subject_t has_target_subject_{};
    lv_subject_t park_status_subject_{};
    char park_status_buf_[64] = {};
    lv_subject_t current_belt_subject_{};
    char current_belt_buf_[16] = {};

    // Live meter subjects. Frequencies are formatted as whole Hz on purpose:
    // see BELT_RESOLUTION_HZ in belt_live_data.h.
    lv_subject_t live_freq_subject_{};
    char live_freq_buf_[16] = {};
    lv_subject_t median_freq_subject_{};
    char median_freq_buf_[48] = {};
    lv_subject_t pluck_count_subject_{};
    char pluck_count_buf_[32] = {};
    lv_subject_t hint_subject_{};
    char hint_buf_[96] = {};
    lv_subject_t committed_subject_{};
    lv_subject_t match_percent_subject_{};
    /// Bumped once per publish_live_values() call (10 Hz while listening).
    /// BeltTrace observes this and invalidates itself - the data provider and
    /// the widget never need to know about each other, same as
    /// perf_history_tick driving HelixSparkline.
    lv_subject_t live_tick_subject_{};
    /// Path to a HELIX_BELT_CAPTURE_DIR-style file; setting it (`ctl set
    /// bt_replay_path <file>`) triggers replay_capture(). Write-only from the
    /// UI's perspective - nothing reads it back.
    lv_subject_t replay_path_subject_{};
    char replay_path_buf_[256] = {};
    lv_subject_t reference_freq_subject_{};
    char reference_freq_buf_[16] = {};
    lv_subject_t has_reference_subject_{};
    lv_subject_t advance_label_subject_{};
    char advance_label_buf_[32] = {};

    // Result subjects
    lv_subject_t result_a_freq_subject_{};
    char result_a_freq_buf_[32] = {};
    lv_subject_t result_a_status_subject_{};
    char result_a_status_buf_[32] = {};
    lv_subject_t result_b_freq_subject_{};
    char result_b_freq_buf_[32] = {};
    lv_subject_t result_b_status_subject_{};
    char result_b_status_buf_[32] = {};
    lv_subject_t result_delta_subject_{};
    char result_delta_buf_[64] = {};
    lv_subject_t result_similarity_subject_{};
    char result_similarity_buf_[32] = {};
    lv_subject_t result_recommendation_subject_{};
    char result_recommendation_buf_[256] = {};
    lv_subject_t has_results_subject_{};

    // Error subject
    lv_subject_t error_message_subject_{};
    char error_message_buf_[256] = {};

    // Gate observers. Every one carries PrinterState's own SubjectLifetime -
    // the observe_* factories take it as a defaulted fourth parameter, so
    // omitting it silently leaves the guard tokenless (#705).
    ObserverGuard accel_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard connected_observer_;
    bool gate_observers_wired_ = false;

    /// Watches replay_path_subject_ and calls replay_capture(). Registered in
    /// init_subjects() against this panel's own subjects_lifetime_, exactly
    /// like the gate observers above watch PrinterState's.
    ObserverGuard replay_observer_;

    /// Handed to BeltTrace's tick observer via get_subjects_lifetime().
    /// Flipped false and replaced on every deinit_subjects()/init_subjects()
    /// cycle, exactly like PrinterState::subjects_lifetime_ (#705) - a trace
    /// widget that outlives one cycle must not call lv_observer_remove() on a
    /// subject that has already been deinited.
    SubjectLifetime subjects_lifetime_ = std::make_shared<bool>(true);

    // Klippy's UDS path, from Moonraker's /server/config. Reachability is
    // probed once per activation, not per gate refresh: the gate recomputes on
    // every subject change and a connect() syscall each time would be waste.
    std::string klippy_socket_path_;
    bool klippy_socket_reachable_ = false;

    // Calibrator
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    IMoonrakerAPI* api_ = nullptr;
    helix::IMoonrakerClient* client_ = nullptr;

    // Chart
    ui_frequency_response_chart_t* chart_ = nullptr;
    int chart_series_a_ = -1;
    int chart_series_b_ = -1;

    // Hardware detection cache. Feeds BeltGateInputs::is_corexy.
    helix::calibration::BeltTensionHardware detected_hw_;

    // Klipper config section name for the accelerometer, e.g. "adxl345" or
    // "adxl345 hotend". Handed to BeltStreamClient whole - it splits the chip
    // type from the mux key itself.
    std::string sensor_name_;

    // Free span handed to the session, which sets its harmonic search window.
    // TARGET_SPAN_MM when the park succeeded, the span implied by the current
    // Y when only an offset is known, and TARGET_SPAN_MM as a last resort with
    // bt_has_target left at 0 so the UI shows matching only.
    float listen_span_mm_ = helix::calibration::TARGET_SPAN_MM;

    char listening_belt_ = 'A';
    /// Belt A's committed median, 0 until it commits. Also the match reference.
    float reference_hz_ = 0.0f;
    float belt_a_hz_ = 0.0f;
    float belt_b_hz_ = 0.0f;

    // --- Touched by the stream's loop thread ---
    //
    // stop_listening() closes the stream before it clears any of this, and
    // BeltStreamClient::stop() joins its loop thread, so the two threads cannot
    // overlap in practice. The mutex makes that invariant local: a reader of
    // this file should not have to go and verify stop()'s internals to see that
    // session_ is safe to reset.
    std::mutex listen_mutex_;
    std::unique_ptr<helix::calibration::BeltListenSession> session_;
    std::vector<helix::calibration::AccelSample> noise_prefix_;
    std::chrono::steady_clock::time_point last_event_tp_{};
    std::chrono::steady_clock::time_point last_reject_tp_{};
    bool had_reject_ = false;
    bool reject_not_a_pluck_ = false;

    /// Declared LAST so it is destroyed FIRST. ~BeltStreamClient() joins its
    /// loop thread, which must happen before session_, noise_prefix_ and the
    /// timestamps above are destroyed - on_batch_bg touches all of them. The
    /// destructor also calls stop_listening() explicitly; this ordering is the
    /// structural backstop for the day someone deletes that line.
    std::unique_ptr<helix::calibration::BeltStreamClient> stream_;
};

// Global instance accessor
BeltTensionPanel& get_global_belt_tension_panel();

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
