// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <utility>
#include <vector>

/**
 * @file pitch_estimator.h
 * @brief Harmonic-aware fundamental estimation for plucked belts
 *
 * A plucked belt produces a harmonic series. Which harmonic carries the most
 * energy varies with belt path and geometry, so taking the largest PSD bin
 * returns 2*f0 whenever the 2nd harmonic dominates. Measured on a Voron 2.4,
 * that was every pluck on one belt and no pluck on the other, from the same
 * printer minutes apart.
 *
 * The harmonic product spectrum multiplies the spectrum by its own decimations,
 * which reinforces a true fundamental and suppresses a lone harmonic. It is
 * sensitive to its search range: extend the floor below f0/2 and it locks onto
 * the subharmonic instead. Derive the range from span length via
 * search_window_for_span().
 */

namespace helix::calibration {

/// Voron reference point: a 150 mm span at correct tension rings at 110 Hz.
/// @note This is a single-printer reference - measured on one Voron 2.4 - and
/// assumes that printer's belt and tension. A bed-slinger or a different belt
/// profile silently inherits it as if it were physics. A per-printer
/// reference is phase-2 work.
inline constexpr float REFERENCE_SPAN_MM = 150.0f;
inline constexpr float REFERENCE_FREQUENCY_HZ = 110.0f;

/// Search window as a fraction of the expected fundamental. Validated against
/// every capture in tests/fixtures/belt_plucks/; widening the floor below
/// ~0.65 reintroduces subharmonic lock on the weakest captures.
inline constexpr float SEARCH_WINDOW_LO_FRACTION = 0.70f;
/// Widening this ceiling costs false locks onto a strong 2nd harmonic that
/// happens to land inside the window; 1.50 is the highest value that stayed
/// clean across every capture in tests/fixtures/belt_plucks/.
inline constexpr float SEARCH_WINDOW_HI_FRACTION = 1.50f;

/// Default harmonic count for both estimate_pitch() and required_bandwidth_hz().
/// Shared so the two defaults cannot drift apart - a caller relying on both
/// defaults needs a bandwidth that actually covers the harmonics requested.
inline constexpr int DEFAULT_HARMONICS = 4;

struct PitchEstimate {
    float frequency_hz = 0.0f;
    bool valid = false;
};

/**
 * @brief Expected fundamental for a belt span at reference tension
 * @param span_mm Free span length in mm
 * @return Expected Hz, or 0.0f if span_mm <= 0
 */
float expected_frequency_for_span(float span_mm);

/**
 * @brief Search window for the fundamental, derived from span length
 * @param span_mm Free span length in mm
 * @param lo_hz Out: lower bound
 * @param hi_hz Out: upper bound
 * @return false if span_mm <= 0 or either pointer is null
 */
bool search_window_for_span(float span_mm, float* lo_hz, float* hi_hz);

/**
 * @brief Bandwidth compute_psd must cover for a complete harmonic series
 *
 * estimate_pitch() skips any candidate whose harmonics fall outside the PSD
 * array. With the default 250 Hz cap that is every realistic belt frequency,
 * and the estimator returns nothing at all.
 *
 * @param search_hi_hz Top of the fundamental search window
 * @param n_harmonics Harmonics estimate_pitch() will multiply
 * @return Hz of bandwidth to request from compute_psd(), with 5% margin
 */
float required_bandwidth_hz(float search_hi_hz, int n_harmonics = DEFAULT_HARMONICS);

/**
 * @brief Estimate the fundamental via harmonic product spectrum
 * @param psd Output of compute_psd(); bin i is at frequency (i+1)*resolution
 * @param search_lo_hz Lower bound of the fundamental search
 * @param search_hi_hz Upper bound of the fundamental search
 * @param n_harmonics Harmonics to multiply, including the fundamental
 * @return Estimate with valid=false if input is degenerate or nothing is in range
 */
PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd, float search_lo_hz,
                             float search_hi_hz, int n_harmonics = DEFAULT_HARMONICS);

/**
 * @brief Estimate the fundamental for a belt span directly from accelerometer samples
 *
 * Owns the full chain that every caller otherwise has to repeat by hand:
 * search_window_for_span() -> required_bandwidth_hz() -> compute_psd() ->
 * estimate_pitch(). Repeating that chain at each call site is a chance to
 * omit the bandwidth step and silently get nothing back - see
 * required_bandwidth_hz()'s doc comment. Callers that have a span and a
 * sample rate should prefer this over assembling the chain themselves.
 *
 * @param samples Raw accelerometer samples for the ring-down to analyse
 * @param sample_rate Sample rate of `samples`, in Hz
 * @param span_mm Free span length in mm
 * @param n_harmonics Harmonics to multiply, including the fundamental
 * @return Estimate with valid=false if span_mm or sample_rate is non-positive,
 *         or if the underlying estimate_pitch() call finds nothing
 */
PitchEstimate estimate_pitch_for_span(const std::vector<AccelSample>& samples, float sample_rate,
                                      float span_mm, int n_harmonics = DEFAULT_HARMONICS);

} // namespace helix::calibration
