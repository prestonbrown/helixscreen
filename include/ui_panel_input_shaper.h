// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_frequency_response_chart.h"
#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "calibration_types.h" // For InputShaperResult
#include "input_shaper_calibrator.h"
#include "overlay_base.h"
#include "platform_capabilities.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <array>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;
}
class IMoonrakerAPI;

namespace helix {
namespace ui {
struct InputShaperPanelTestAccess; // test-only friend (tests/test_helpers/)
} // namespace ui
} // namespace helix

/**
 * @file ui_panel_input_shaper.h
 * @brief Input Shaper calibration panel for resonance compensation tuning
 *
 * Interactive panel that guides users through SHAPER_CALIBRATE workflow.
 * Allows measuring resonance on X/Y axes, viewing recommendations, and
 * applying optimal shaper settings to reduce ringing/ghosting.
 *
 * ## State Machine:
 * - IDLE: Shows instructions and buttons to start calibration
 * - MEASURING: Calibration running, show spinner and cancel button
 * - RESULTS: Display recommendations, Apply/Dismiss buttons
 * - ERROR: Something went wrong, retry option
 *
 * ## Klipper Commands Used:
 * - MEASURE_AXES_NOISE: Check accelerometer noise level
 * - SHAPER_CALIBRATE AXIS=X/Y: Run resonance test
 * - SET_INPUT_SHAPER: Apply recommended settings
 * - SAVE_CONFIG: Save settings permanently (restarts Klipper)
 *
 * ## Usage:
 * ```cpp
 * InputShaperPanel& panel = get_global_input_shaper_panel();
 * panel.init_subjects();  // Once at startup
 * panel.create(screen);   // Lazy create
 * panel.show();           // Opens overlay
 * ```
 */
class InputShaperPanel : public OverlayBase {
    friend struct helix::ui::InputShaperPanelTestAccess;

  public:
    /**
     * @brief Panel state machine states
     */
    enum class State {
        IDLE,      ///< Ready to start, showing instructions
        MEASURING, ///< SHAPER_CALIBRATE or MEASURE_AXES_NOISE running
        RESULTS,   ///< Showing calibration recommendations
        ERROR      ///< Error occurred
    };

    InputShaperPanel() = default;
    ~InputShaperPanel() override;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects for reactive XML bindings
     *
     * Must be called BEFORE XML creation (from register_callbacks).
     * Subject bindings are resolved at XML parse time.
     */
    void init_subjects() override;

    /**
     * @brief Tear down the owned subjects and expire outstanding async tokens
     *
     * Registered with StaticSubjectRegistry from init_subjects(), so it runs in
     * the ordered subject-shutdown pass rather than waiting for the panel
     * registry to destroy this object. Invalidating both generation guards here
     * — not only in the destructor — is the load-bearing half: subjects can be
     * torn down and re-inited on a LIVE panel, which no destructor hook would
     * catch (prestonbrown/helixscreen#1180, #1146).
     */
    void deinit_subjects();

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent screen widget to attach overlay to
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Input Shaper"
     */
    const char* get_name() const override {
        return "Input Shaper";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets state to IDLE, clears previous results.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Cancels calibration if in progress.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /**
     * @brief Null widget pointers after destroy-on-close
     */
    void on_ui_destroyed() override;

    //
    // === Public API ===
    //

    /**
     * @brief Show overlay panel
     *
     * Pushes overlay onto navigation stack and registers with NavigationManager.
     * on_activate() will be called automatically after animation completes.
     */
    void show();

    /**
     * @brief Set Moonraker client and API for G-code commands
     *
     * Creates InputShaperCalibrator instance with the API.
     *
     * @param client helix::IMoonrakerClient (kept for potential future use)
     * @param api IMoonrakerAPI for G-code execution
     */
    void set_api(helix::IMoonrakerClient* client, IMoonrakerAPI* api);

    /**
     * @brief Get current panel state
     * @return Current State
     */
    [[nodiscard]] State get_state() const {
        return state_;
    }

    //
    // === Event Handlers (public for XML event_cb callbacks) ===
    //

    void handle_calibrate_x_clicked();
    void handle_calibrate_y_clicked();
    void handle_calibrate_all_clicked();
    void handle_measure_noise_clicked();
    void handle_cancel_clicked();
    void handle_apply_clicked();
    void handle_close_clicked();
    void handle_retry_clicked();
    void handle_save_config_clicked();
    void handle_save_clicked();
    void handle_print_test_pattern_clicked();
    void handle_help_clicked();
    void handle_chip_x_clicked(int index);
    void handle_chip_y_clicked(int index);

  private:
    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // State management
    State state_ = State::IDLE;
    bool demo_inject_pending_ = false;
    void set_state(State new_state);

    // Calibration commands
    void start_calibration(char axis);
    void measure_noise();
    void cancel_calibration();
    void apply_recommendation();
    void save_configuration();

    // Pre-flight noise check + calibration chain
    void start_with_preflight(char axis);
    void proceed_with_preflight(char axis);
    void calibrate_all();
    void on_preflight_complete(float noise_level);
    void on_preflight_error(const std::string& message);
    void continue_calibrate_all_y();
    void apply_y_after_x();

    // Live-before snapshot: the config active when the current run began, so
    // the results cards can show what changed. Queried fresh at run start - the
    // config may have changed since the panel opened, and what SAVE_CONFIG
    // later writes can differ from what was live (some forks overwrite the
    // staged X result with Y's values).
    void snapshot_config_before();
    [[nodiscard]] bool get_before_axis(char axis, std::string& type, float& freq) const;
    [[nodiscard]] double before_damping_ratio(char axis) const;

    // Result callbacks (from IMoonrakerAPI)
    void on_calibration_result(const InputShaperResult& result);
    void on_calibration_error(const std::string& message);

    // UI update helpers
    void populate_current_config(const InputShaperConfig& config);
    void clear_results();

    // Analysis-phase display: spinner + elapsed-seconds step label. Shared
    // cancel_analysis_display() runs from every path that stops showing the
    // analysis (phase transitions out of Analyzing, state changes away from
    // MEASURING, cleanup/deactivate, destructor) so the timer is never left
    // armed on a stale label.
    void begin_analysis_display();
    void cancel_analysis_display();

    // Per-axis result helpers
    static const char* get_shaper_explanation(const std::string& type);
    static int get_vibration_quality(float vibrations);
    static const char* get_quality_description(float vibrations);
    void populate_axis_result(char axis, const InputShaperResult& result);
    void populate_axis_delta(char axis, const InputShaperResult& result);

    // Widget/client references (overlay_root_ inherited from OverlayBase)
    lv_obj_t* parent_screen_ = nullptr;
    helix::IMoonrakerClient* client_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;

    // Private setup helper (called by create())
    void setup_widgets();

    // Per-axis comparison table subjects (up to MAX_SHAPERS rows per axis, bound in XML)
    static constexpr size_t MAX_SHAPERS = 11;
    static constexpr size_t CMP_TYPE_BUF = 32;
    static constexpr size_t CMP_VALUE_BUF = 24;

    struct ComparisonRow {
        char type_buf[CMP_TYPE_BUF] = {};
        lv_subject_t type{};
        char freq_buf[CMP_VALUE_BUF] = {};
        lv_subject_t freq{};
        char vib_buf[CMP_VALUE_BUF] = {};
        lv_subject_t vib{};
        char accel_buf[CMP_VALUE_BUF] = {};
        lv_subject_t accel{};
    };

    std::array<ComparisonRow, MAX_SHAPERS> x_cmp_;
    std::array<ComparisonRow, MAX_SHAPERS> y_cmp_;

    // Error message subject (replaces imperative lv_label_set_text)
    char is_error_message_buf_[128] = {};
    lv_subject_t is_error_message_{};

    // Current config display subjects
    lv_subject_t is_shaper_configured_{};
    char is_current_x_type_buf_[32] = {};
    lv_subject_t is_current_x_type_{};
    char is_current_x_freq_buf_[32] = {};
    lv_subject_t is_current_x_freq_{};
    char is_current_y_type_buf_[32] = {};
    lv_subject_t is_current_y_type_{};
    char is_current_y_freq_buf_[32] = {};
    lv_subject_t is_current_y_freq_{};
    char is_current_max_accel_buf_[32] = {};
    lv_subject_t is_current_max_accel_{};

    // Measuring state labels
    char is_measuring_axis_label_buf_[64] = {};
    lv_subject_t is_measuring_axis_label_{};
    char is_measuring_step_label_buf_[64] = {};
    lv_subject_t is_measuring_step_label_{};
    lv_subject_t is_measuring_progress_{};
    lv_subject_t
        is_measuring_has_progress_{}; // 0=indeterminate (spinner), 1=determinate (progress bar)

    // Analysis-phase elapsed label. The analysis reports no percent, so while
    // its spinner is shown a 1 Hz timer refreshes the step label
    // ("Analyzing data... Ns") from the tick stored when the phase began.
    // ElapsedLabelTimer (ui_timer_guard.h) owns the timer; its destructor and
    // cancel() are both cancel-safe from every teardown path.
    helix::ui::ElapsedLabelTimer analysis_elapsed_;

    // Per-axis result subjects
    lv_subject_t is_results_has_x_{};
    lv_subject_t is_results_has_y_{};

    // Header button disabled state (1 = disabled, 0 = enabled)
    lv_subject_t is_calibrate_all_disabled_{};

    // Recommended row index per axis (for table highlight)
    lv_subject_t is_x_recommended_row_{};
    lv_subject_t is_y_recommended_row_{};

    // Number of shapers per axis (controls table row visibility)
    lv_subject_t is_x_num_shapers_{};
    lv_subject_t is_y_num_shapers_{};
    /// Chart chips actually backed by a curve, per axis. The XML declares a
    /// fixed MAX_SHAPERS of them, so the surplus must be hidden or they render
    /// as empty outlines.
    lv_subject_t is_x_num_chips_{};
    lv_subject_t is_y_num_chips_{};

    // X axis result display
    char is_result_x_shaper_buf_[48] = {};
    lv_subject_t is_result_x_shaper_{};
    char is_result_x_explanation_buf_[128] = {};
    lv_subject_t is_result_x_explanation_{};
    char is_result_x_vibration_buf_[96] = {};
    lv_subject_t is_result_x_vibration_{};
    char is_result_x_max_accel_buf_[32] = {};
    lv_subject_t is_result_x_max_accel_{};
    lv_subject_t is_result_x_quality_{};

    // Y axis result display
    char is_result_y_shaper_buf_[48] = {};
    lv_subject_t is_result_y_shaper_{};
    char is_result_y_explanation_buf_[128] = {};
    lv_subject_t is_result_y_explanation_{};
    char is_result_y_vibration_buf_[96] = {};
    lv_subject_t is_result_y_vibration_{};
    char is_result_y_max_accel_buf_[32] = {};
    lv_subject_t is_result_y_max_accel_{};
    lv_subject_t is_result_y_quality_{};

    // Live-before delta display per axis: the was -> measured line and the old
    // setting's residual-vibration verdict against the freshly measured PSD.
    // Each row is gated by a has-subject so it hides when the inputs are
    // missing (no before-config, no PSD data, or an unported shaper type).
    char is_x_delta_buf_[64] = {};
    lv_subject_t is_x_delta_text_{};
    char is_y_delta_buf_[64] = {};
    lv_subject_t is_y_delta_text_{};
    lv_subject_t is_x_has_delta_{};
    lv_subject_t is_y_has_delta_{};
    char is_x_verdict_buf_[96] = {};
    lv_subject_t is_x_verdict_text_{};
    char is_y_verdict_buf_[96] = {};
    lv_subject_t is_y_verdict_text_{};
    lv_subject_t is_x_has_verdict_{};
    lv_subject_t is_y_has_verdict_{};

    // Firmware overwrote the staged X result with Y's values during the Y run
    // (1 = show the warning row on the X results card)
    lv_subject_t is_x_fw_overwrite_warn_{};

    // Low-RAM warning modal before resonance calibration (see memory_utils.h)
    lv_obj_t* low_ram_warn_dialog_ = nullptr;
    char pending_calib_axis_ = 'X';

    // Calibrate All flow tracking
    bool calibrate_all_mode_ = false; ///< True when doing X+Y sequential calibration
    InputShaperResult x_result_;      ///< Stored X result when doing Calibrate All
    /// True when the Y run reported the firmware's copy_TestAxis_y_to_x
    /// marker: the saved X value is Y's, and the measured X result the X card
    /// shows was discarded by the firmware.
    bool x_saved_value_overwritten_ = false;
    helix::AsyncLifetimeGuard
        calibration_lifetime_; ///< Generation guard to discard stale calibration callbacks

    // Live-before configuration snapshot (see snapshot_config_before())
    InputShaperConfig config_before_{};
    bool has_config_before_ = false;

    // Results data
    char current_axis_ = 'X';
    char last_calibrated_axis_ = 'X'; ///< Axis most recently calibrated (for apply)
    std::string recommended_type_;
    float recommended_freq_ = 0.0f;

    // Frequency response chart data per axis
    struct AxisChartData {
        std::vector<std::pair<float, float>> freq_response; // (freq, psd)
        std::vector<ShaperResponseCurve> shaper_curves;
        ui_frequency_response_chart_t* chart = nullptr;
        int raw_series_id = -1;
        int shaper_series_ids[MAX_SHAPERS]; // initialized to -1 in constructor
        bool shaper_visible[MAX_SHAPERS] = {};
        int old_setting_series_id = -1; ///< Muted overlay of the live-before shaper curve

        AxisChartData() {
            std::fill(shaper_series_ids, shaper_series_ids + MAX_SHAPERS, -1);
        }
    };

    AxisChartData x_chart_;
    AxisChartData y_chart_;

    // Freq data availability subjects (gating chart visibility in XML)
    lv_subject_t is_x_has_freq_data_{};
    lv_subject_t is_y_has_freq_data_{};

    // Chip label subjects (dynamically set from shaper names)
    static constexpr size_t CHIP_LABEL_BUF = 20;
    struct ChipRow {
        char label_buf[CHIP_LABEL_BUF] = {};
        lv_subject_t label{};
        lv_subject_t active{}; // 0=off, 1=on
    };
    std::array<ChipRow, MAX_SHAPERS> x_chips_;
    std::array<ChipRow, MAX_SHAPERS> y_chips_;

    // Legend subjects (shaper name label, updated on chip toggle)
    char is_x_legend_shaper_label_buf_[CHIP_LABEL_BUF] = {};
    lv_subject_t is_x_legend_shaper_label_{};
    char is_y_legend_shaper_label_buf_[CHIP_LABEL_BUF] = {};
    lv_subject_t is_y_legend_shaper_label_{};

    // Legend dot widget pointers (for programmatic color updates)
    lv_obj_t* legend_x_shaper_dot_ = nullptr;
    lv_obj_t* legend_y_shaper_dot_ = nullptr;

    // Chart management helpers
    void populate_chart(char axis, const InputShaperResult& result);
    void clear_chart(char axis);
    void toggle_shaper_overlay(char axis, int index);
    void create_chart_widgets();
    void update_legend(char axis);

    // Calibrator for delegating operations
    std::unique_ptr<helix::calibration::InputShaperCalibrator> calibrator_;

  public:
    /**
     * @brief Request demo results injection after next on_activate()
     *
     * Sets a pending flag so that on_activate() will call inject_demo_results()
     * after finishing its normal reset. Call before show().
     */
    void request_demo_inject() {
        demo_inject_pending_ = true;
    }

    /**
     * @brief Inject demo results for screenshot/demo mode
     *
     * Populates the panel with realistic input shaper calibration results
     * for both X and Y axes, including frequency response chart data.
     * Values match mock backend.
     */
    void inject_demo_results();

    /**
     * @brief Get calibrator for testing
     * @return Pointer to calibrator, or nullptr if not created yet
     */
    [[nodiscard]] helix::calibration::InputShaperCalibrator* get_calibrator() const {
        return calibrator_.get();
    }

    /**
     * @brief Test seam: raw handle of the analysis elapsed timer
     *
     * The unit-test harness (lv_timer_handler_safe) only executes timers with
     * a finite repeat count, so the 1 Hz elapsed timer - periodic by design -
     * never fires there. Tests use this handle to lend the timer a temporary
     * finite repeat count so the harness can run it, restoring -1 afterwards.
     * Mirrors PIDCalibrationPanel::eta_timer_for_test().
     */
    [[nodiscard]] lv_timer_t* analysis_elapsed_timer_for_test() const {
        return analysis_elapsed_.timer_for_test();
    }
};

// Global instance accessor
InputShaperPanel& get_global_input_shaper_panel();

/**
 * @brief Register XML event callbacks for input shaper panel
 *
 * Call this once at startup before creating any input_shaper_panel XML.
 * Registers callbacks for all button events (calibrate_x, calibrate_y, etc.).
 */
void ui_panel_input_shaper_register_callbacks();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Must be called during app initialization before XML creation.
 * Registers "on_input_shaper_row_clicked" callback.
 */
void init_input_shaper_row_handler();
