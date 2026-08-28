// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file belt_tension_calibrator.cpp
 * @brief Implementation of BeltTensionCalibrator state machine
 *
 * Orchestrates belt tension measurement workflow by delegating API calls
 * to MoonrakerAdvancedAPI and handling calibration-specific data processing
 * (PSD analysis, peak detection, similarity calculation).
 */

#include "belt_tension_calibrator.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "toolhead_homing.h"

#include <sstream>

namespace helix::calibration {

// ============================================================================
// Constructors / Destructor
// ============================================================================

BeltTensionCalibrator::BeltTensionCalibrator() : api_(nullptr) {
    spdlog::debug("[BeltTension] Created without API (test mode)");
}

BeltTensionCalibrator::BeltTensionCalibrator(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[BeltTension] Created with API");
}

BeltTensionCalibrator::~BeltTensionCalibrator() {
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// belt_path_to_axis_param() / belt_path_to_name()
// ============================================================================

std::string BeltTensionCalibrator::belt_path_to_axis_param(BeltPath path) const {
    switch (path) {
    case BeltPath::PATH_A:
        return "1,1";
    case BeltPath::PATH_B:
        return "1,-1";
    case BeltPath::X_AXIS:
        return "x";
    case BeltPath::Y_AXIS:
        return "y";
    default:
        return "1,1";
    }
}

std::string BeltTensionCalibrator::belt_path_to_name(BeltPath path) {
    switch (path) {
    case BeltPath::PATH_A:
        return "belt_path_a";
    case BeltPath::PATH_B:
        return "belt_path_b";
    case BeltPath::X_AXIS:
        return "belt_x";
    case BeltPath::Y_AXIS:
        return "belt_y";
    default:
        return "belt_path_a";
    }
}

// ============================================================================
// detect_hardware() — thin wrapper around API
// ============================================================================

void BeltTensionCalibrator::detect_hardware(BeltHardwareDetectCallback on_complete,
                                            BeltErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[BeltTension] detect_hardware called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    state_.store(State::DETECTING_HARDWARE);
    spdlog::info("[BeltTension] Detecting printer hardware capabilities");

    api_->advanced().detect_belt_hardware(
        lifetime_.bg_cb("BeltTensionCalibrator::detect_hw_success",
                        [this, on_complete](const BeltTensionHardware& hw) {
                            hardware_ = hw;
                            state_.store(State::IDLE);
                            if (on_complete)
                                on_complete(hw);
                        }),
        lifetime_.bg_cb("BeltTensionCalibrator::detect_hw_error",
                        [this, on_error](const MoonrakerError& err) {
                            state_.store(State::ERROR);
                            if (on_error)
                                on_error("Hardware detection failed: " + err.message);
                        }));
}

// ============================================================================
// process_csv_data() — calibration-layer data processing
// ============================================================================

void BeltTensionCalibrator::process_csv_data(const std::string& csv_data,
                                             BeltMeasurementCallback on_complete,
                                             BeltErrorCallback on_error) {
    auto samples = parse_accel_csv(csv_data);
    if (samples.empty()) {
        if (on_error)
            on_error("No valid samples in accelerometer data");
        return;
    }

    // Estimate sample rate from timestamps
    float sample_rate = 3200.0f; // Default Klipper ADXL rate
    if (samples.size() >= 2) {
        float duration = samples.back().time - samples.front().time;
        if (duration > 0.0f) {
            sample_rate = static_cast<float>(samples.size() - 1) / duration;
        }
    }

    auto psd = compute_psd(samples, sample_rate);
    auto peak = find_peak_frequency(psd, 20.0f, 200.0f);

    BeltMeasurement measurement;
    measurement.freq_response = std::move(psd);
    measurement.peak_frequency = peak.frequency;
    measurement.peak_amplitude = peak.amplitude;
    measurement.status =
        evaluate_belt_status(peak.frequency, results_.target_frequency, results_.tolerance);

    spdlog::info("[BeltTension] Measurement: peak={:.1f} Hz, status={}", measurement.peak_frequency,
                 static_cast<int>(measurement.status));

    if (on_complete) {
        on_complete(measurement);
    }
}

// ============================================================================
// execute_resonance_test() — delegates to API
// ============================================================================

void BeltTensionCalibrator::execute_resonance_test(BeltPath path, BeltProgressCallback on_progress,
                                                   BeltMeasurementCallback on_complete,
                                                   BeltErrorCallback on_error) {
    std::string axis_param = belt_path_to_axis_param(path);
    std::string name = belt_path_to_name(path);

    spdlog::info("[BeltTension] Starting resonance test: axis={}, name={}", axis_param, name);

    if (on_progress) {
        on_progress(10);
    }

    auto token = lifetime_.token();
    api_->advanced().test_belt_resonance(
        axis_param, name,
        nullptr, // API-level progress (not used currently)
        [this, token, on_progress, on_complete, on_error](const std::string& output_name) {
            // BG THREAD: log only (output_name is local to cb).
            spdlog::info("[BeltTension] Resonance test complete for {}", output_name);

            // MAIN THREAD: progress callback + register inner download cb
            // (api_-> deref + download_accel_csv registration).
            token.defer(
                "BeltTensionCalibrator::resonance_test_success",
                [this, token, output_name, on_progress, on_complete, on_error]() {
                    if (on_progress) {
                        on_progress(50);
                    }

                    // Download and process the CSV
                    api_->advanced().download_accel_csv(
                        output_name,
                        lifetime_.bg_cb("BeltTensionCalibrator::accel_csv_success",
                                        [this, on_complete, on_error](const std::string& csv_data) {
                                            process_csv_data(csv_data, on_complete, on_error);
                                        }),
                        lifetime_.bg_cb("BeltTensionCalibrator::accel_csv_error",
                                        [on_error](const MoonrakerError& err) {
                                            if (on_error)
                                                on_error("Failed to download data: " + err.message);
                                        }));
                });
        },
        lifetime_.bg_cb("BeltTensionCalibrator::resonance_test_error",
                        [this, on_error](const MoonrakerError& err) {
                            state_.store(State::ERROR);
                            if (on_error)
                                on_error("Resonance test failed: " + err.message);
                        }));
}

// ============================================================================
// test_path()
// ============================================================================

void BeltTensionCalibrator::test_path(BeltPath path, BeltProgressCallback on_progress,
                                      BeltMeasurementCallback on_complete,
                                      BeltErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[BeltTension] test_path called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Guard against concurrent runs
    State current = state_.load();
    if (current != State::IDLE && current != State::RESULTS_READY) {
        spdlog::warn("[BeltTension] Test already in progress (state={})",
                     static_cast<int>(current));
        if (on_error) {
            on_error("Measurement already in progress");
        }
        return;
    }

    state_.store((path == BeltPath::PATH_A || path == BeltPath::X_AXIS) ? State::TESTING_PATH_A
                                                                        : State::TESTING_PATH_B);

    spdlog::info("[BeltTension] Starting test for path {}", static_cast<int>(path));

    auto token = lifetime_.token();
    ensure_homed_then(
        api_, lifetime_,
        [this, token, path, on_progress, on_complete, on_error]() {
            // ensure_homed_then() always lands `then` on the main thread
            // (synchronously for the already-homed fast path, marshalled via
            // `lifetime_` for the G28-then-continue path), so it is safe to
            // touch `this` here directly.
            execute_resonance_test(
                path, on_progress,
                [this, token, path, on_complete](const BeltMeasurement& measurement) {
                    // execute_resonance_test's on_complete is invoked from
                    // process_csv_data, which now runs in a defer. That means
                    // we are already on main thread here, but defer again to
                    // keep the L081 detector quiet and ensure invariant if
                    // upstream changes.
                    token.defer(
                        "BeltTensionCalibrator::test_path_measurement",
                        [this, path, on_complete, measurement = measurement]() {
                            // Store result in appropriate slot
                            BeltMeasurement result = measurement;
                            result.path = path;

                            if (path == BeltPath::PATH_A || path == BeltPath::X_AXIS) {
                                results_.path_a = result;
                            } else {
                                results_.path_b = result;
                            }

                            // Update derived fields if both paths complete
                            if (results_.is_complete()) {
                                results_.frequency_delta = std::abs(results_.path_a.peak_frequency -
                                                                    results_.path_b.peak_frequency);
                                results_.similarity_percent = calculate_similarity(
                                    results_.path_a.freq_response, results_.path_b.freq_response);
                                state_.store(State::RESULTS_READY);
                                spdlog::info("[BeltTension] Both paths complete: "
                                             "delta={:.1f} Hz, similarity={:.0f}%",
                                             results_.frequency_delta, results_.similarity_percent);
                            } else {
                                state_.store(State::IDLE);
                            }

                            if (on_complete) {
                                on_complete(result);
                            }
                        });
                },
                on_error);
        },
        [this, on_error](const MoonrakerError& err) {
            std::string msg;
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[BeltTension] G28 response timed out (may still be running)");
                msg = "Homing timed out — printer may still be homing";
            } else {
                spdlog::error("[BeltTension] Homing failed: {}", err.message);
                msg = "Homing failed: " + err.message;
            }
            if (on_error) {
                on_error(msg);
            }
            state_.store(State::IDLE);
        });
}

// ============================================================================
// run_auto_sweep()
// ============================================================================

void BeltTensionCalibrator::run_auto_sweep(BeltProgressCallback on_progress,
                                           BeltResultCallback on_complete,
                                           BeltErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[BeltTension] run_auto_sweep called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Guard against concurrent runs
    State current = state_.load();
    if (current != State::IDLE && current != State::RESULTS_READY) {
        spdlog::warn("[BeltTension] Auto sweep already in progress (state={})",
                     static_cast<int>(current));
        if (on_error) {
            on_error("Measurement already in progress");
        }
        return;
    }

    spdlog::info("[BeltTension] Starting auto-sweep measurement");

    // Determine belt paths based on kinematics
    BeltPath first_path = BeltPath::PATH_A;
    BeltPath second_path = BeltPath::PATH_B;
    if (hardware_.kinematics == KinematicsType::CARTESIAN) {
        first_path = BeltPath::X_AXIS;
        second_path = BeltPath::Y_AXIS;
    }

    auto token = lifetime_.token();

    // Wrap progress to scale across both paths (0-50 for A, 50-100 for B)
    auto progress_a = [on_progress](int pct) {
        if (on_progress) {
            on_progress(pct / 2);
        }
    };

    auto progress_b = [on_progress](int pct) {
        if (on_progress) {
            on_progress(50 + pct / 2);
        }
    };

    // Test path A, then path B
    test_path(
        first_path, progress_a,
        [this, token, second_path, progress_b, on_complete, on_error](const BeltMeasurement&) {
            // test_path's on_complete fires from inside its own defer, so we
            // are already on main thread — but defer again to keep the L081
            // detector quiet and remain safe if upstream changes.
            token.defer("BeltTensionCalibrator::auto_sweep_a_done",
                        [this, token, second_path, progress_b, on_complete, on_error]() {
                            // Now test path B
                            test_path(
                                second_path, progress_b,
                                [this, token, on_complete](const BeltMeasurement&) {
                                    token.defer("BeltTensionCalibrator::auto_sweep_b_done",
                                                [this, on_complete]() {
                                                    spdlog::info(
                                                        "[BeltTension] Auto-sweep complete");
                                                    if (on_complete) {
                                                        on_complete(results_);
                                                    }
                                                });
                                },
                                on_error);
                        });
        },
        on_error);
}

// ============================================================================
// cancel()
// ============================================================================

void BeltTensionCalibrator::cancel() {
    spdlog::info("[BeltTension] Cancelling (was state={})", static_cast<int>(state_.load()));
    state_.store(State::IDLE);
}

// ============================================================================
// reset()
// ============================================================================

void BeltTensionCalibrator::reset() {
    spdlog::info("[BeltTension] Resetting calibrator");

    state_.store(State::IDLE);
    results_ = BeltTensionResult{};
    hardware_ = BeltTensionHardware{};
}

} // namespace helix::calibration
