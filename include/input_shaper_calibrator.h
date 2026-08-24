// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file input_shaper_calibrator.h
 * @brief High-level orchestrator for input shaper calibration workflow
 *
 * InputShaperCalibrator manages the complete calibration process:
 * 1. Check accelerometer connectivity and noise level
 * 2. Run resonance tests on X and Y axes
 * 3. Store and compare results
 * 4. Apply chosen settings to printer
 * 5. Save configuration to printer.cfg
 *
 * This is a state machine that coordinates IMoonrakerAPI calls and
 * provides progress/error callbacks to the UI layer.
 */

#include "async_lifetime_guard.h"
#include "calibration_types.h"

#include <functional>
#include <memory>
#include <string>

// Forward declaration
class IMoonrakerAPI;
struct MoonrakerError;

namespace helix {
namespace calibration {

/**
 * @brief Configuration for applying input shaper settings
 */
struct ApplyConfig {
    char axis = 'X';            ///< Axis to configure ('X' or 'Y')
    std::string shaper_type;    ///< Shaper type (e.g., "mzv", "ei")
    float frequency = 0.0f;     ///< Shaper frequency in Hz
    float damping_ratio = 0.1f; ///< Damping ratio (default 0.1)
};

/**
 * @brief Callback types for InputShaperCalibrator
 */
using AccelCheckCallback = std::function<void(float noise_level)>;

/// Progress during a resonance run. The phase says whether the toolhead is
/// still sweeping or Klipper has moved on to fitting shapers; it is reported
/// explicitly because the percentage alone cannot distinguish the two.
using ProgressCallback = std::function<void(int percent, ShaperCalibrationPhase phase)>;
using ResultCallback = std::function<void(const InputShaperResult& result)>;
using SuccessCallback = std::function<void()>;
using ErrorCallback = std::function<void(const std::string& message)>;

/**
 * @brief High-level orchestrator for input shaper calibration workflow
 *
 * Manages the complete calibration process as a state machine:
 * - IDLE: Ready to start calibration
 * - CHECKING_ADXL: Verifying accelerometer connection
 * - TESTING_X: Running resonance test on X axis
 * - TESTING_Y: Running resonance test on Y axis
 * - READY: Calibration complete, results available
 *
 * Usage:
 * @code
 *   InputShaperCalibrator calibrator(api);
 *
 *   calibrator.check_accelerometer([](float noise) {
 *       // Accelerometer OK, noise level acceptable
 *   });
 *
 *   calibrator.run_calibration('X',
 *       [](int pct, ShaperCalibrationPhase phase) { update_progress(pct, phase); },
 *       [](const InputShaperResult& r) { show_result(r); },
 *       [](const std::string& err) { show_error(err); });
 * @endcode
 */
class InputShaperCalibrator {
  public:
    /**
     * @brief Calibrator state machine states
     */
    enum class State {
        IDLE,          ///< Ready to start, no calibration in progress
        CHECKING_ADXL, ///< Checking accelerometer connectivity
        TESTING_X,     ///< Running resonance test on X axis
        TESTING_Y,     ///< Running resonance test on Y axis
        READY          ///< Calibration complete, results available
    };

    /**
     * @brief Results container for both axes
     */
    struct CalibrationResults {
        InputShaperResult x_result; ///< X axis calibration result
        InputShaperResult y_result; ///< Y axis calibration result
        float noise_level = 0.0f;   ///< Measured accelerometer noise level

        /**
         * @brief Check if X axis result is valid
         */
        [[nodiscard]] bool has_x() const {
            return x_result.is_valid();
        }

        /**
         * @brief Check if Y axis result is valid
         */
        [[nodiscard]] bool has_y() const {
            return y_result.is_valid();
        }

        /**
         * @brief Check if both axes have valid results
         */
        [[nodiscard]] bool is_complete() const {
            return has_x() && has_y();
        }
    };

    /**
     * @brief Default constructor for tests without API
     *
     * Operations will fail with error callbacks when no API is available.
     */
    InputShaperCalibrator();

    /**
     * @brief Constructor with API dependency injection
     *
     * @param api Non-owning pointer to IMoonrakerAPI instance
     */
    explicit InputShaperCalibrator(IMoonrakerAPI* api);

    /**
     * @brief Destructor
     */
    ~InputShaperCalibrator() = default;

    // Non-copyable, movable
    InputShaperCalibrator(const InputShaperCalibrator&) = delete;
    InputShaperCalibrator& operator=(const InputShaperCalibrator&) = delete;
    InputShaperCalibrator(InputShaperCalibrator&&) = default;
    InputShaperCalibrator& operator=(InputShaperCalibrator&&) = default;

    /**
     * @brief Get current calibrator state
     * @return Current state
     */
    [[nodiscard]] State get_state() const {
        return state_;
    }

    /**
     * @brief Classify a calibration command failure as a printer-firmware halt.
     *
     * Klipper "Internal error on command" / shutdown faults halt the printer
     * mid-command. A real-world example is the Creality K2 `record_z_pos`
     * crash on a unit with a missing `z_pos.json`, which aborts any Z move
     * (including the one inside G28) and shuts Klipper down (#1021, bundle
     * 5LSSSKPX). These are printer firmware/config problems, not HelixScreen
     * issues; the user must restart the firmware — which the global
     * EmergencyStopOverlay recovery dialog already offers — before any
     * calibration can proceed.
     *
     * Detecting them lets the wizard show a clear, actionable message instead
     * of a raw `{"code":"key60", ...}` JSON-RPC envelope.
     *
     * @return A user-facing message when @p err indicates a firmware halt;
     *         an empty string otherwise (caller keeps its normal message).
     */
    static std::string firmware_halt_message(const MoonrakerError& err);

    /**
     * @brief Check accelerometer connectivity and measure noise level
     *
     * Runs MEASURE_AXES_NOISE to verify accelerometer is working and
     * measure background vibration level.
     *
     * @param on_complete Called with noise level on success
     * @param on_error Called with error message on failure
     */
    void check_accelerometer(AccelCheckCallback on_complete, ErrorCallback on_error = nullptr);

    /**
     * @brief Run resonance calibration on specified axis
     *
     * Executes SHAPER_CALIBRATE for the specified axis, collecting
     * frequency response data and all fitted shaper alternatives.
     *
     * @param axis Axis to test ('X' or 'Y')
     * @param on_progress Called with percentage (0-100) and the current phase
     * @param on_complete Called with calibration result on success
     * @param on_error Called with error message on failure
     */
    void run_calibration(char axis, ProgressCallback on_progress, ResultCallback on_complete,
                         ErrorCallback on_error);

    /**
     * @brief Cancel any in-progress calibration
     *
     * Aborts current test and returns to IDLE state.
     * Safe to call even if no calibration is running.
     *
     * NOTE: This only resets local state. Once SHAPER_CALIBRATE or
     * MEASURE_AXES_NOISE has been dispatched, Klipper will run it to
     * completion regardless. Use emergency_abort() to actually stop the
     * macro on the printer side.
     */
    void cancel() {
        state_ = State::IDLE;
    }

    /**
     * @brief True iff a calibration command has been dispatched and not
     *        yet completed (CHECKING_ADXL / TESTING_X / TESTING_Y).
     */
    [[nodiscard]] bool is_active() const {
        return state_ == State::CHECKING_ADXL || state_ == State::TESTING_X ||
               state_ == State::TESTING_Y;
    }

    /**
     * @brief Emergency-abort an in-progress calibration on the printer.
     *
     * Sends M112 followed by firmware_restart — the only reliable way to
     * stop SHAPER_CALIBRATE / TEST_RESONANCES once Klipper has started
     * the macro (it blocks the gcode queue, so CANCEL_PRINT won't help).
     * Fire-and-forget; logs success/failure. Sets local state to IDLE.
     *
     * Callers should suppress the EmergencyStopOverlay recovery dialog
     * and the disconnect modal *before* invoking this, since M112 +
     * restart triggers both.
     */
    void emergency_abort();

    /**
     * @brief Get stored calibration results
     * @return Reference to results container
     */
    [[nodiscard]] const CalibrationResults& get_results() const {
        return results_;
    }

    /**
     * @brief Apply input shaper settings to printer
     *
     * Sends SET_INPUT_SHAPER command with specified configuration.
     *
     * @param config Settings to apply
     * @param on_success Called on successful application
     * @param on_error Called with error message on failure
     */
    void apply_settings(const ApplyConfig& config, SuccessCallback on_success,
                        ErrorCallback on_error);

    /**
     * @brief Save current input shaper settings to printer.cfg
     *
     * Sends SAVE_CONFIG to persist settings across restarts.
     *
     * @param on_success Called on successful save
     * @param on_error Called with error message on failure
     */
    void save_to_config(SuccessCallback on_success, ErrorCallback on_error);

  private:
    /**
     * @brief Translate a G28 failure into the user-facing message
     *        ensure_homed_then()'s caller-supplied on_error expects.
     *
     * Shared by check_accelerometer() and run_calibration(), the two
     * ensure_homed_then() call sites: firmware-halt detection takes
     * priority (a distinct actionable message), then TIMEOUT, then the
     * generic friendly-message fallback.
     */
    static std::string homing_error_message(const MoonrakerError& err);

    IMoonrakerAPI* api_ = nullptr; ///< Non-owning pointer to API
    State state_ = State::IDLE;
    CalibrationResults results_;

    /**
     * @brief Async callback safety guard for ensure_homed_then().
     *
     * Held via unique_ptr (not a plain member) so InputShaperCalibrator
     * stays move-constructible/assignable — AsyncLifetimeGuard itself is
     * non-copyable/non-movable (see "InputShaperCalibrator is movable" in
     * tests/unit/test_input_shaper_calibrator.cpp), but the pointer that
     * owns it can move; the guard's identity, and any outstanding tokens,
     * travel with the calibrator.
     */
    std::unique_ptr<helix::AsyncLifetimeGuard> lifetime_ =
        std::make_unique<helix::AsyncLifetimeGuard>();
};

} // namespace calibration
} // namespace helix
