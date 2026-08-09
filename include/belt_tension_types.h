// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * @file belt_tension_types.h
 * @brief Data structures for belt tension tuning calibration
 *
 * Types for CoreXY/Cartesian belt tension measurement, PSD analysis,
 * and Z belt corner diagnostics. Used by BeltTensionCalibrator and
 * the belt tension UI panel/wizard.
 */

namespace helix::calibration {

// ============================================================================
// Belt Identification
// ============================================================================

/// Belt path identifiers for CoreXY
enum class BeltPath {
    PATH_A, ///< CoreXY diagonal A (1,1)
    PATH_B, ///< CoreXY diagonal B (1,-1)
    X_AXIS, ///< Cartesian X
    Y_AXIS, ///< Cartesian Y
};

/// Z belt corner positions
enum class ZBeltCorner {
    FRONT_LEFT,
    FRONT_RIGHT,
    REAR_LEFT,
    REAR_RIGHT,
};

/// Kinematics type detected from printer
enum class KinematicsType {
    UNKNOWN,
    COREXY,
    CARTESIAN,
};

// ============================================================================
// Status Evaluation
// ============================================================================

/// Status thresholds for belt tension
enum class BeltStatus {
    GOOD,    ///< Within +/-tolerance of target
    WARNING, ///< 1-2x tolerance from target
    BAD,     ///< >2x tolerance from target
};

/// Convert BeltStatus to user-facing display string
const char* belt_status_to_string(BeltStatus status);

// ============================================================================
// Hardware Detection
// ============================================================================

/// Hardware capabilities detected from printer
struct BeltTensionHardware {
    KinematicsType kinematics = KinematicsType::UNKNOWN;
    bool has_adxl = false;
    bool has_belted_z = false; ///< quad_gantry_level present
    bool has_pwm_led = false;
    std::string pwm_led_pin;     ///< Pin name for strobe LED
    std::string kinematics_name; ///< Raw string from Klipper
};

// ============================================================================
// Measurement Results
// ============================================================================

/// Result for a single belt path measurement
struct BeltMeasurement {
    BeltPath path = BeltPath::PATH_A;
    float peak_frequency = 0.0f; ///< Detected resonant frequency (Hz)
    float peak_amplitude = 0.0f; ///< PSD amplitude at peak
    BeltStatus status = BeltStatus::GOOD;
    std::vector<std::pair<float, float>> freq_response; ///< (freq_hz, psd)

    [[nodiscard]] bool is_valid() const {
        return peak_frequency > 0.0f;
    }
};

/// Z belt measurement for one corner
struct ZBeltMeasurement {
    ZBeltCorner corner = ZBeltCorner::FRONT_LEFT;
    float peak_frequency = 0.0f;
    BeltStatus status = BeltStatus::GOOD;

    [[nodiscard]] bool is_valid() const {
        return peak_frequency > 0.0f;
    }
};

/// Complete belt tension results
struct BeltTensionResult {
    BeltMeasurement path_a;
    BeltMeasurement path_b;
    float similarity_percent = 0.0f; ///< Pearson correlation * 100
    float frequency_delta = 0.0f;    ///< |A - B| in Hz
    float target_frequency = 110.0f; ///< Target Hz
    float tolerance = 10.0f;         ///< +/-Hz tolerance

    /// Z belt results (if applicable)
    std::vector<ZBeltMeasurement> z_belts;

    [[nodiscard]] bool has_path_a() const {
        return path_a.is_valid();
    }
    [[nodiscard]] bool has_path_b() const {
        return path_b.is_valid();
    }
    [[nodiscard]] bool is_complete() const {
        return has_path_a() && has_path_b();
    }
    [[nodiscard]] bool has_z_results() const {
        return !z_belts.empty();
    }

    /// Evaluate overall status based on thresholds
    [[nodiscard]] BeltStatus overall_status() const;

    /// Generate user-facing recommendation string
    [[nodiscard]] std::string recommendation() const;
};

// ============================================================================
// Analysis Functions
// ============================================================================

/// Evaluate belt status from frequency vs target
BeltStatus evaluate_belt_status(float measured_hz, float target_hz, float tolerance_hz);

/// Calculate Pearson correlation between two PSD curves (returns 0-100)
float calculate_similarity(const std::vector<std::pair<float, float>>& curve_a,
                           const std::vector<std::pair<float, float>>& curve_b);

// ============================================================================
// Accelerometer Data Processing
// ============================================================================

/// Single accelerometer sample from Klipper CSV
struct AccelSample {
    float time;
    float x, y, z;
};

/// Parse Klipper raw CSV accelerometer data
/// Format: #time,accel_x,accel_y,accel_z
std::vector<AccelSample> parse_accel_csv(const std::string& csv_data);

/// Compute PSD via DFT from accelerometer samples.
///
/// Returns vector of (frequency_hz, power) pairs. Bin i sits at
/// (i+1)*resolution - there is no DC bin.
///
/// @param max_freq_hz Highest frequency to compute, clamped to Nyquist. The
///        default preserves the original 250 Hz behaviour. Harmonic analysis
///        needs roughly n_harmonics * f0 of bandwidth or the upper harmonics
///        fall outside the array; see pitch_estimator.h.
std::vector<std::pair<float, float>> compute_psd(const std::vector<AccelSample>& samples,
                                                 float sample_rate = 3200.0f,
                                                 float max_freq_hz = 250.0f);

/// Peak frequency search result
struct PeakResult {
    float frequency = 0.0f;
    float amplitude = 0.0f;
    bool found = false;
};

/// Find peak frequency in PSD data within range
PeakResult find_peak_frequency(const std::vector<std::pair<float, float>>& psd,
                               float min_freq = 20.0f, float max_freq = 200.0f);

// ============================================================================
// Callback Types
// ============================================================================

using BeltHardwareDetectCallback = std::function<void(const BeltTensionHardware&)>;
using BeltProgressCallback = std::function<void(int percent)>;
using BeltMeasurementCallback = std::function<void(const BeltMeasurement&)>;
using BeltResultCallback = std::function<void(const BeltTensionResult&)>;
using BeltErrorCallback = std::function<void(const std::string& message)>;

} // namespace helix::calibration
