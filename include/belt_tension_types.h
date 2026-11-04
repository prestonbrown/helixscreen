// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/**
 * @file belt_tension_types.h
 * @brief Data structures for belt tension tuning calibration
 *
 * Types for CoreXY/Cartesian belt tension measurement and PSD analysis.
 * Used by BeltTensionCalibrator and the belt tension UI panel/wizard.
 *
 * Z belts are deliberately not supported. Measured on a Voron 2.4: four plucks
 * of one Z belt returned 228, 164, 92 and 58 Hz, versus five identical readings
 * on an A/B belt, with the signal 2-5x the noise floor against 12-14x. The
 * gantry decouples Z belt vibration from a toolhead-mounted accelerometer, and
 * Klipper's TEST_RESONANCES cannot sweep Z at all (resonance_tester.py
 * _parse_axis accepts only x, y, or a two-component XY vector). Supporting Z
 * needs a different sensor location, not different code.
 */

namespace helix::calibration {

// ============================================================================
// Belt Identification
// ============================================================================

/// Belt path identifiers for CoreXY
enum class BeltPath {
    PATH_A, ///< CoreXY diagonal A (1,-1)
    PATH_B, ///< CoreXY diagonal B (1,1)
};

/// The Klipper stepper whose motor drives a path's belt loop. CoreXY moves
/// stepper_x by x+y and stepper_y by x-y, so a 1,1 move turns only stepper_x
/// and a 1,-1 move only stepper_y, on every CoreXY whatever its belt routing.
[[nodiscard]] const char* belt_path_stepper(BeltPath path);

/// Which physical belt the user says each path is. Paired so the two names
/// can never contradict; persisted per printer as this int.
enum class BeltLayout {
    UNSET = 0,           ///< "Path A" / "Path B"
    A_LEFT_B_RIGHT = 1,  ///< A is the left belt, B the right
    A_RIGHT_B_LEFT = 2,  ///< A is the right belt, B the left
    A_UPPER_B_LOWER = 3, ///< A is the upper belt, B the lower
    A_LOWER_B_UPPER = 4, ///< A is the lower belt, B the upper
};
inline constexpr int kBeltLayoutCount = 5;

/// A stored layout value; anything out of range reads as UNSET.
[[nodiscard]] BeltLayout belt_layout_from_int(int value);

/// What kind of tensioner a belt path uses.
enum class BeltTensionerKind {
    UNKNOWN,
    SCREW,
    SPRING,
};

/// Frequency range a TEST_RESONANCES sweep is limited to, sent as FREQ_START
/// (floored) and FREQ_END (ceiled). Klipper rejects a FREQ_START under 1 Hz, so
/// both bounds must be real frequencies; pass no window for the printer's own
/// [resonance_tester] range.
struct BeltSweepWindow {
    float freq_start_hz = 0.0f;
    float freq_end_hz = 0.0f;
};

/// Kinematics type detected from printer
enum class KinematicsType {
    UNKNOWN,
    COREXY,
    CARTESIAN,
};

// ============================================================================
// Hardware Detection
// ============================================================================

/// Hardware capabilities detected from printer
struct BeltTensionHardware {
    KinematicsType kinematics = KinematicsType::UNKNOWN;
    bool has_adxl = false;
    std::string kinematics_name; ///< Raw string from Klipper
};

// ============================================================================
// Path Comparison
// ============================================================================

/// One path's response: (frequency Hz, psd_xyz) pairs, ascending frequency.
using BeltCurve = std::vector<std::pair<float, float>>;

/// One sweep's response on one frequency grid: psd_xyz for the similarity
/// summary, psd_x + psd_y for tuning. Belt modes move the toolhead in the XY
/// plane; Z motion in a band is the frame, not the belt.
struct BeltResponse {
    BeltCurve xyz;
    BeltCurve in_plane; ///< empty when the CSV carried no psd_x and psd_y columns
};

enum class BeltVerdict { MATCHED, CLOSE, ADJUST };

/// Provisional: they sort every real capture in tests/fixtures/belt_sweeps/
/// plausibly, but no known-good reference exists yet for a printer that reads
/// Poor (prestonbrown/helixscreen#1737).
namespace belt_verdict {
inline constexpr float kPeakThresholdFraction = 0.10f; ///< of the in-band maximum
inline constexpr float kPairMaxHz = 10.0f;             ///< pairing distance ceiling
inline constexpr float kMatchedSimilarity = 90.0f;
inline constexpr float kCloseSimilarity = 75.0f;
} // namespace belt_verdict

/// One local maximum of a curve inside the analysis band.
struct BeltPeak {
    float freq_hz = 0.0f;
    float amplitude = 0.0f;
};

/// A peak from each curve the pairing judged the same resonance.
struct BeltPeakPair {
    BeltPeak a;
    BeltPeak b;
};

/// Peaks of two curves paired by frequency, Shake&Tune's _pair_peaks.
struct BeltPeakPairing {
    std::vector<BeltPeakPair> pairs;  ///< sorted by a.amplitude + b.amplitude, descending
    std::vector<BeltPeak> unpaired_a; ///< A peaks with no B partner within threshold_hz
    std::vector<BeltPeak> unpaired_b;
    float threshold_hz = 0.0f;
};

struct BeltComparison {
    bool valid = false; ///< false when either curve has < 3 in-band bins
    float similarity_percent = 0.0f;
    BeltVerdict verdict = BeltVerdict::ADJUST; ///< from similarity only
    BeltPeakPairing peaks;
    float band_max_psd = 0.0f; ///< the stronger curve's in-band maximum
};

/// The curve's bins with band_min_hz <= f <= band_max_hz, ascending frequency.
[[nodiscard]] BeltCurve curve_in_band(const BeltCurve& curve, float band_min_hz, float band_max_hz);

/// Pearson correlation x100 of the two curves inside the band, clamped to 0..100.
/// B is linearly interpolated onto A's in-band frequencies. Returns 0 when either
/// curve has fewer than 3 in-band bins or zero variance.
[[nodiscard]] float band_similarity(const BeltCurve& a, const BeltCurve& b, float band_min_hz,
                                    float band_max_hz);

/// Local maxima inside the band: value > left neighbour, value >= right neighbour,
/// value >= kPeakThresholdFraction * the in-band maximum. Ascending frequency.
[[nodiscard]] std::vector<BeltPeak> detect_belt_peaks(const BeltCurve& curve, float band_min_hz,
                                                      float band_max_hz);

/// Greedy closest-first pairing of two peak lists. The distance threshold is
/// min(median + 1.5 * IQR over all |fa - fb|, kPairMaxHz) - kPairMaxHz when
/// there are no distances at all.
[[nodiscard]] BeltPeakPairing pair_belt_peaks(const std::vector<BeltPeak>& a,
                                              const std::vector<BeltPeak>& b);

/// Tier from similarity_percent alone.
[[nodiscard]] BeltVerdict verdict_for_similarity(float similarity_percent);

/// Compare two belt-path resonance curves inside the printer's sweep range:
/// band-limited similarity (the headline) and paired peaks. Invalid when either
/// curve has < 3 in-band bins.
[[nodiscard]] BeltComparison compare_belt_paths(const BeltCurve& a, const BeltCurve& b,
                                                float band_min_hz, float band_max_hz);

// ============================================================================
// Analysis Functions
// ============================================================================

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

/**
 * @brief A signal ready for single-bin DFTs: mean removed, Hann windowed, in double
 *
 * The window is the symmetric w[i] = 0.5 - 0.5 cos(2 pi i / (n - 1)). Every
 * spectrum the accelerometer paths compute reads its bins off one of these and
 * applies its own scale: compute_psd() divides |X|^2 by n * rate, a relative
 * scale its peak searches only compare with itself; goertzel_psd()
 * (sweep_response_estimator.h) scales to a one-sided density,
 * 2 |X|^2 / (rate * sum(w^2)), the units Klipper's calibration CSV carries.
 * Empty for n < 2.
 */
class HannWindowedSignal {
  public:
    HannWindowedSignal(const float* x, size_t n);

    /// |X|^2 at `omega` radians per sample.
    [[nodiscard]] double power(double omega) const;
    /// power() of three signals of one length at each of @p count frequencies,
    /// into out[0..count). Each result is bit-identical to that signal's own
    /// power(omega): the axes share one phasor per frequency, and several
    /// frequencies advance together so their serial chains overlap.
    static void power(const std::array<HannWindowedSignal, 3>& axes, const double* omegas,
                      size_t count, std::array<double, 3>* out);
    [[nodiscard]] double sum_w2() const {
        return sum_w2_;
    }
    [[nodiscard]] size_t size() const {
        return v_.size();
    }

  private:
    std::vector<double> v_;
    double sum_w2_ = 0.0;
};

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
using BeltErrorCallback = std::function<void(const std::string& message)>;

} // namespace helix::calibration
