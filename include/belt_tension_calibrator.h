// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file belt_tension_calibrator.h
 * @brief One-path-at-a-time belt tension measurement orchestrator
 *
 * BeltTensionCalibrator coordinates IMoonrakerAPI calls and marshals the
 * API's off-thread callbacks onto the LVGL thread for the UI layer. One
 * measure_path() call homes if needed, then sweeps one belt path.
 *
 * @see InputShaperCalibrator for the equivalent input shaper workflow
 */

#include "async_lifetime_guard.h"
#include "belt_settle.h"
#include "belt_tension_types.h"
#include "i_moonraker_sub_apis.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>

// Forward declaration
namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

class BeltTensionCalibrator {
  public:
    /// State machine states
    enum class State {
        IDLE,               ///< Ready to start, no measurement in progress
        DETECTING_HARDWARE, ///< Querying printer for capabilities
        HOMING,             ///< Homing printer axes
        SETTLING,           ///< Running the settling laps
        MEASURING,          ///< Running the resonance sweep on one path
        BUZZING,            ///< STEPPER_BUZZ on one path's motor
        ERROR,              ///< An error occurred
    };

    /**
     * @brief Default constructor for tests without API
     *
     * Operations will fail with error callbacks when no API is available.
     */
    BeltTensionCalibrator();

    /**
     * @brief Constructor with API dependency injection
     *
     * @param api Non-owning pointer to IMoonrakerAPI instance
     */
    explicit BeltTensionCalibrator(IMoonrakerAPI* api);

    ~BeltTensionCalibrator();

    // Non-copyable, non-movable (shared alive_ makes move unsound)
    BeltTensionCalibrator(const BeltTensionCalibrator&) = delete;
    BeltTensionCalibrator& operator=(const BeltTensionCalibrator&) = delete;
    BeltTensionCalibrator(BeltTensionCalibrator&&) = delete;
    BeltTensionCalibrator& operator=(BeltTensionCalibrator&&) = delete;

    // State Queries

    [[nodiscard]] State get_state() const {
        return state_.load();
    }
    [[nodiscard]] const BeltTensionHardware& get_hardware() const {
        return hardware_;
    }

    /// Klipper XY-vector form of a path's axis: PATH_A sweeps (1,-1), PATH_B
    /// (1,1). The letters follow the Voron motor names Shake&Tune uses: a 1,1
    /// move turns only stepper_x, which is a Voron's B motor.
    [[nodiscard]] static const char* axis_param(BeltPath p);

    /// TEST_RESONANCES NAME= for a path; also the CSV basename Klipper writes.
    [[nodiscard]] static const char* output_name(BeltPath p);

    // Hardware Detection

    /**
     * @brief Detect printer hardware capabilities
     *
     * Queries printer.objects.list and printer.objects.query to determine
     * kinematics type, ADXL presence, belted Z, and PWM LED availability.
     *
     * @param on_complete Called with detected hardware on success
     * @param on_error Called with error message on failure
     */
    void detect_hardware(BeltHardwareDetectCallback on_complete, BeltErrorCallback on_error);

    // Measurement

    /**
     * @brief Home if needed, then sweep one belt path
     *
     * Runs TEST_RESONANCES on the path's diagonal and reports the parsed
     * curves. All three callbacks run on the LVGL thread. A run already in
     * progress is cancelled first.
     *
     * @param path Which belt diagonal to sweep
     * @param on_progress (percent 0-100, current sweep frequency Hz)
     * @param on_complete The parsed curves: psd_xyz and psd_x + psd_y, ascending
     * @param on_error Error message on failure (homing or sweep)
     * @param window Optional frequency range to limit the sweep to
     */
    void measure_path(BeltPath path, std::function<void(int percent, float freq_hz)> on_progress,
                      std::function<void(BeltResponse)> on_complete, BeltErrorCallback on_error,
                      std::optional<BeltSweepWindow> window = std::nullopt);

    /**
     * @brief Home if XY is not homed, then run the settling laps
     *
     * Homing goes through ensure_homed_then(). plan_belt_settle() decides the
     * moves before homing; a refusal reaches on_refused and nothing is sent,
     * not even the G28. Otherwise the script is sent with the plan's timeout and on_done
     * fires on its ack (the script's M400 holds the ack until the laps
     * finish). All callbacks run on the LVGL thread. A run already in
     * progress is cancelled first, and cancel() silences this one the same way.
     */
    void settle(const BeltSettleInputs& inputs, std::function<void()> on_done,
                std::function<void(BeltSettleRefusal)> on_refused, BeltErrorCallback on_error);

    /**
     * @brief Buzz the motor that drives one path's belt
     *
     * Sends STEPPER_BUZZ for belt_path_stepper(path): the motor moves 1 mm
     * back and forth ten times, so the user can see and hear which belt the
     * path is. Klipper's force_move module is always loaded and the buzz
     * needs no homing. The state reads BUZZING until the command answers;
     * either answer returns it to IDLE, since a refused buzz leaves nothing
     * half done. Both callbacks run on the LVGL thread, and cancel() silences
     * them.
     */
    void buzz_path(BeltPath path, std::function<void()> on_done, BeltErrorCallback on_error);

    // Control

    /// Stop listening to the running sweep. The printer keeps sweeping.
    void cancel();

    /// Stop the printer: cancel() then emergency_stop_and_restart().
    void emergency_abort();

    /// Cancel any run and clear detected hardware
    void reset();

  private:
    std::atomic<State> state_{State::IDLE};
    IMoonrakerAPI* api_ = nullptr;
    BeltTensionHardware hardware_;

    /// Silences the running sweep's callbacks; null when no run is active.
    IAdvancedAPI::BeltRunCancel run_cancel_;

    /// Bumped by cancel(). Each step of a run (the post-homing start, progress,
    /// completion, error) acts only while it still holds the current value, so a
    /// run cancelled mid-homing never starts its sweep.
    uint32_t run_generation_ = 0;

    /// Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
