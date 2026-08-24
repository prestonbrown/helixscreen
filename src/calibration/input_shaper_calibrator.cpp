// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file input_shaper_calibrator.cpp
 * @brief Implementation of InputShaperCalibrator class
 *
 * Orchestrates input shaper calibration workflow using IMoonrakerAPI.
 * Manages state transitions, result storage, and error handling.
 */

#include "input_shaper_calibrator.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "toolhead_homing.h"

#include <cctype>

namespace helix {
namespace calibration {

// ============================================================================
// Constructors
// ============================================================================

InputShaperCalibrator::InputShaperCalibrator() : api_(nullptr) {
    spdlog::debug("[InputShaperCalibrator] Created without API (test mode)");
}

InputShaperCalibrator::InputShaperCalibrator(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[InputShaperCalibrator] Created with API");
}

// ============================================================================
// firmware_halt_message()
// ============================================================================

std::string InputShaperCalibrator::firmware_halt_message(const MoonrakerError& err) {
    // A NOT_READY error is the execute_gcode() preflight guard refusing to
    // send while Klipper is already halted ("Klipper is halted — restart
    // firmware to continue") — unambiguously a firmware halt.
    bool halted = err.type == MoonrakerErrorType::NOT_READY;

    if (!halted) {
        // Otherwise look for Klipper shutdown / internal-error signatures in
        // the raw envelope. extract_friendly_message() pulls the "msg"/
        // "message" field out of a {"code":"keyNNN","msg":"..."} envelope so a
        // raw JSON-RPC string still matches; we also scan the original in case
        // the signature lives outside the extracted field.
        const std::string friendly = MoonrakerError::extract_friendly_message(err.message);
        auto signals_halt = [](const std::string& s) {
            return s.find("Internal error on command") != std::string::npos ||
                   s.find("shutdown") != std::string::npos ||
                   s.find("Klipper is halted") != std::string::npos ||
                   s.find("halted") != std::string::npos;
        };
        halted = signals_halt(friendly) || signals_halt(err.message);
    }

    if (!halted) {
        return {};
    }

    // The printer is now halted. The global EmergencyStopOverlay recovery
    // dialog auto-pops on SHUTDOWN and offers the firmware restart, so we point
    // the user there rather than adding a redundant control to the wizard.
    return "Printer firmware error — the printer halted. This is a firmware/config "
           "problem on the printer, not HelixScreen. Restart the firmware (use the "
           "recovery prompt), then try calibration again.";
}

// ============================================================================
// homing_error_message()
// ============================================================================

std::string InputShaperCalibrator::homing_error_message(const MoonrakerError& err) {
    if (err.type == MoonrakerErrorType::TIMEOUT) {
        spdlog::warn("[InputShaperCalibrator] G28 response timed out (may still be running)");
        return "Homing timed out — printer may still be homing";
    }
    if (std::string halt = firmware_halt_message(err); !halt.empty()) {
        // Klipper aborted the homing move and shut down (e.g. the K2
        // record_z_pos crash, #1021). Surface the firmware-fault message
        // instead of dumping the raw JSON-RPC envelope.
        spdlog::error("[InputShaperCalibrator] Homing aborted (firmware halt): {}", err.message);
        return halt;
    }
    spdlog::error("[InputShaperCalibrator] Homing failed: {}", err.message);
    return "Homing failed: " + MoonrakerError::extract_friendly_message(err.message);
}

// ============================================================================
// check_accelerometer()
// ============================================================================

void InputShaperCalibrator::check_accelerometer(AccelCheckCallback on_complete,
                                                ErrorCallback on_error) {
    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] check_accelerometer called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Transition to CHECKING_ADXL state
    state_ = State::CHECKING_ADXL;
    spdlog::info("[InputShaperCalibrator] Starting accelerometer check");

    // Ensure homed before measuring (toolhead needs to be positioned)
    ensure_homed_then(
        api_, *lifetime_,
        [this, on_complete, on_error]() {
            api_->advanced().measure_axes_noise(
                [this, on_complete](float noise_level) {
                    results_.noise_level = noise_level;
                    state_ = State::IDLE;

                    spdlog::info(
                        "[InputShaperCalibrator] Accelerometer check complete, noise={:.4f}",
                        noise_level);

                    if (on_complete) {
                        on_complete(noise_level);
                    }
                },
                [this, on_error](const MoonrakerError& err) {
                    state_ = State::IDLE;

                    spdlog::error("[InputShaperCalibrator] Accelerometer check failed: {}",
                                  err.message);

                    if (on_error) {
                        std::string halt = firmware_halt_message(err);
                        on_error(!halt.empty()
                                     ? halt
                                     : MoonrakerError::extract_friendly_message(err.message));
                    }
                });
        },
        [this, on_error](const MoonrakerError& err) {
            std::string msg = homing_error_message(err);
            state_ = State::IDLE;
            if (on_error) {
                on_error(msg);
            }
        });
}

// ============================================================================
// run_calibration()
// ============================================================================

void InputShaperCalibrator::run_calibration(char axis, ProgressCallback on_progress,
                                            ResultCallback on_complete, ErrorCallback on_error) {
    // Normalize axis to uppercase
    char normalized_axis = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));

    // Validate axis
    if (normalized_axis != 'X' && normalized_axis != 'Y') {
        spdlog::warn("[InputShaperCalibrator] Invalid axis: {}", axis);
        if (on_error) {
            on_error("Invalid axis: " + std::string(1, axis) + " (must be X or Y)");
        }
        state_ = State::IDLE;
        return;
    }

    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] run_calibration called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Guard against concurrent runs - only allow from IDLE or READY states
    if (state_ != State::IDLE && state_ != State::READY) {
        spdlog::warn("[InputShaperCalibrator] Calibration already in progress (state={})",
                     static_cast<int>(state_));
        if (on_error) {
            on_error("Calibration already in progress");
        }
        return;
    }

    // Transition to appropriate testing state
    state_ = (normalized_axis == 'X') ? State::TESTING_X : State::TESTING_Y;
    spdlog::info("[InputShaperCalibrator] Starting calibration for axis {}", normalized_axis);

    // Ensure homed before running resonance test (needs absolute coordinates)
    ensure_homed_then(
        api_, *lifetime_,
        [this, normalized_axis, on_progress, on_complete, on_error]() {
            auto api_progress = [on_progress](int percent, ShaperCalibrationPhase phase) {
                if (on_progress) {
                    on_progress(percent, phase);
                }
            };

            api_->advanced().start_resonance_test(
                normalized_axis, api_progress,
                [this, normalized_axis, on_complete](const InputShaperResult& result) {
                    if (normalized_axis == 'X') {
                        results_.x_result = result;
                    } else {
                        results_.y_result = result;
                    }

                    if (results_.is_complete()) {
                        state_ = State::READY;
                        spdlog::info("[InputShaperCalibrator] Both axes calibrated, state=READY");
                    } else {
                        state_ = State::IDLE;
                        spdlog::info(
                            "[InputShaperCalibrator] Axis {} complete, awaiting other axis",
                            normalized_axis);
                    }

                    if (on_complete) {
                        on_complete(result);
                    }
                },
                [this, on_error](const MoonrakerError& err) {
                    state_ = State::IDLE;
                    spdlog::error("[InputShaperCalibrator] Calibration failed: {}", err.message);
                    if (on_error) {
                        std::string halt = firmware_halt_message(err);
                        on_error(!halt.empty()
                                     ? halt
                                     : MoonrakerError::extract_friendly_message(err.message));
                    }
                });
        },
        [this, on_error](const MoonrakerError& err) {
            std::string msg = homing_error_message(err);
            state_ = State::IDLE;
            if (on_error) {
                on_error(msg);
            }
        });
}

// ============================================================================
// emergency_abort()
// ============================================================================

void InputShaperCalibrator::emergency_abort() {
    state_ = State::IDLE;

    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] emergency_abort called without API");
        return;
    }

    spdlog::info("[InputShaperCalibrator] Emergency abort: sending M112 + firmware_restart");

    IMoonrakerAPI* api = api_;
    api_->emergency_stop(
        [api]() {
            spdlog::debug("[InputShaperCalibrator] M112 sent, restarting firmware");
            api->restart_firmware(
                []() { spdlog::debug("[InputShaperCalibrator] Firmware restart initiated"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[InputShaperCalibrator] Firmware restart failed: {}",
                                  err.message);
                });
        },
        [](const MoonrakerError& err) {
            spdlog::error("[InputShaperCalibrator] Emergency stop failed: {}", err.message);
        });
}

// ============================================================================
// apply_settings()
// ============================================================================

void InputShaperCalibrator::apply_settings(const ApplyConfig& config, SuccessCallback on_success,
                                           ErrorCallback on_error) {
    // Validate config - shaper_type must not be empty
    if (config.shaper_type.empty()) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called with empty shaper_type");
        if (on_error) {
            on_error("Invalid configuration: shaper_type cannot be empty");
        }
        return;
    }

    // Validate config - frequency must be positive
    if (config.frequency <= 0.0f) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called with invalid frequency: {}",
                     config.frequency);
        if (on_error) {
            on_error("Invalid configuration: frequency must be positive");
        }
        return;
    }

    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    spdlog::info("[InputShaperCalibrator] Applying settings: axis={}, type={}, freq={:.1f}Hz",
                 config.axis, config.shaper_type, config.frequency);

    // Adapt error callback from MoonrakerError to string
    auto error_adapter = [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    };

    // Call API to set input shaper
    api_->advanced().set_input_shaper(config.axis, config.shaper_type,
                                      static_cast<double>(config.frequency), on_success,
                                      error_adapter);
}

// ============================================================================
// save_to_config()
// ============================================================================

void InputShaperCalibrator::save_to_config(SuccessCallback on_success, ErrorCallback on_error) {
    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] save_to_config called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    spdlog::info("[InputShaperCalibrator] Saving configuration to printer.cfg");

    // Adapt error callback from MoonrakerError to string
    auto error_adapter = [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    };

    // Call API to save config
    api_->advanced().save_config(on_success, error_adapter);
}

} // namespace calibration
} // namespace helix
