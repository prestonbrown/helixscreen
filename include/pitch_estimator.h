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

/// Power ratio, against the quiet window's own median bin, above which a
/// frequency counts as already occupied before anyone plucked anything.
///
/// A genuinely quiet broadband window peaks at roughly 5x its own median
/// across the ~700 Hz analysed here (measured: 4.8x), so a lower threshold
/// would flag ordinary noise as a steady tone. 12 sits clear of that with
/// room to spare, and a real fan tone runs orders of magnitude above it.
inline constexpr float BACKGROUND_PROMINENCE_TOLERANCE = 12.0f;

/// Floor on the per-bin discount, so contamination saturates instead of
/// running away.
///
/// This is a guard, not a tuned figure. Against a realistic quiet window
/// nothing reaches BACKGROUND_PROMINENCE_TOLERANCE in the first place, so the
/// floor never engages and every value from 1e-4 to 1e-1 behaves identically
/// (measured: 60 of 60 swept window phases estimate the same fundamental at
/// each). What it defends against is a degenerate quiet window - a sensor
/// whose noise sits below its own quantisation step, so the median bin is
/// near zero and every bin reads as wildly contaminated. Saturating there
/// keeps the discount order-preserving instead of letting the shape of the
/// quiet window reorder candidates. Past about 30 dB over the tolerance a
/// frequency is simply occupied, and 40 dB versus 80 dB says nothing more.
inline constexpr float MIN_BACKGROUND_WEIGHT = 0.001f;

/// How far off a nominal frequency a bin may sit and still be the same tone.
/// The quiet window and the ring-down are different lengths, so their bins do
/// not line up; 2% is wider than that mismatch and narrower than the gap
/// between a belt fundamental and its neighbours.
inline constexpr float BIN_MATCH_FRACTION = 0.02f;

/// Half-width, in bins, of the band credited to each harmonic by
/// harmonic_concentration(). A real peak straddles two bins whenever it falls
/// between bin centres, so a single bin undercounts genuine plucks.
inline constexpr float HARMONIC_MATCH_BINS = 2.0f;

/// Fraction of in-band energy that must sit on a candidate's harmonic series
/// for the event to have been a pluck at all. A thump, a door, or a stepper
/// cogging spreads its energy; a plucked string does not.
///
/// Both sides measured through the live path in the same harness - the
/// exhaustive alignment sweep in test_belt_listen_session.cpp, every one of
/// the 340 window alignments in a batch period, for each of the five captures
/// that clear the energy gate:
///
///   real captures      0.2965 - 0.4879   (floor: b_belt_82hz_3, leads 2304-2312)
///   broadband thump    up to 0.2138      (worst of 200 seeds)
///
/// 0.25 sits between them with about 16% of headroom either side. That is the
/// narrowest margin of any constant in this file, and it is set by real data
/// rather than chosen: the corridor is only 0.083 wide. An earlier 0.30
/// rejected b_belt_82hz_3 - the STRONGEST capture in the set - at nine
/// alignments, on a measurement that was correct at 82.0 Hz.
inline constexpr float MIN_HARMONIC_CONCENTRATION = 0.25f;

/**
 * @brief The spectrum of a window captured while the machine was still
 *
 * A belt tone is absent before the pluck and present after it; a fan tone is
 * present in both. That difference is the whole discriminator, and it needs no
 * user action - the offending fan on the reference machine sits on an
 * accessory unit that Klipper cannot see, let alone quiet.
 *
 * Prominence is measured against the quiet window's OWN median bin, so nothing
 * here depends on the two windows having the same length, gain, or scale.
 */
class QuietSpectrum {
  public:
    /// Adopt a PSD measured while the machine was still.
    /// @return false if the spectrum is too short or has no positive median
    ///         (a dead sensor or an all-zero capture), in which case the
    ///         object stays invalid and discounts nothing.
    bool learn(std::vector<std::pair<float, float>> psd);

    [[nodiscard]] bool valid() const {
        return reference_ > 0.0f && !psd_.empty();
    }

    /// Power at `freq_hz` as a multiple of this window's median bin. 0 if
    /// invalid. 1.0 means "as loud as a typical quiet bin".
    [[nodiscard]] float prominence_at(float freq_hz) const;

    /// Multiplier in (0, 1] for energy seen at `freq_hz` during a pluck.
    ///
    /// 1.0 for any frequency that was quiet beforehand, falling towards
    /// MIN_BACKGROUND_WEIGHT the further a frequency stood above the quiet
    /// window's own noise. It is a
    /// graduated discount, never a veto: a belt tone that happens to land on
    /// top of a fan tone can still outscore the fan if its harmonic series is
    /// genuinely there, which is exactly the case the reference machine hits
    /// with a fan near 115 Hz and a 110 Hz target.
    [[nodiscard]] float weight_at(float freq_hz) const;

    void clear();

  private:
    std::vector<std::pair<float, float>> psd_;
    float reference_ = 0.0f;  ///< median bin power
    float resolution_ = 0.0f; ///< Hz per bin
};

/**
 * @brief Measure the quiet spectrum for a span, at the analysis bandwidth
 *
 * Runs the same search_window_for_span() -> required_bandwidth_hz() ->
 * compute_psd() chain the pluck path runs, so the two spectra cover the same
 * harmonics. Callers should not assemble it by hand - see
 * required_bandwidth_hz().
 *
 * @return an invalid QuietSpectrum if the buffer, rate or span is degenerate
 */
QuietSpectrum quiet_spectrum_for_span(const std::vector<AccelSample>& quiet, float sample_rate,
                                      float span_mm, int n_harmonics = DEFAULT_HARMONICS);

/**
 * @brief Fraction of in-band energy sitting on f0's harmonic series
 *
 * The strongest single "was this a pluck?" discriminator. A plucked belt puts
 * its energy into a harmonic series; a thump spreads it across the band.
 *
 * The band runs from `band_lo_hz` to (n_harmonics + 0.5) * f0. The lower bound
 * matters: a structural gantry mode dominates the spectrum well below the
 * search window (roughly 38-58 Hz on the reference machine, moving with
 * toolhead position), and counting it would drown every real pluck. Pass the
 * search window's lower bound.
 *
 * @param background When non-null and valid, each bin is weighted by
 *        QuietSpectrum::weight_at() before being counted, in numerator and
 *        denominator alike. Without it a loud fan owns most of the band and a
 *        genuine pluck scores as unconcentrated as a thump.
 * @return 0.0f for degenerate input or an empty band
 */
float harmonic_concentration(const std::vector<std::pair<float, float>>& psd, float f0,
                             int n_harmonics, float band_lo_hz,
                             const QuietSpectrum* background = nullptr);

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
 * @param background When non-null and valid, every harmonic bin a candidate is
 *        scored on is discounted by QuietSpectrum::weight_at(). A fan tone
 *        contaminates 2f and 4f as readily as f, so the discount has to apply
 *        to the whole series, not just the fundamental.
 * @return Estimate with valid=false if input is degenerate or nothing is in range
 */
PitchEstimate estimate_pitch(const std::vector<std::pair<float, float>>& psd, float search_lo_hz,
                             float search_hi_hz, int n_harmonics = DEFAULT_HARMONICS,
                             const QuietSpectrum* background = nullptr);

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
 * @param out_psd When non-null, receives the PSD this call computed - the
 *        same array estimate_pitch() searched. Lets a caller that wants both
 *        the estimate and the spectrum (e.g. for a live display) get them
 *        from one bandwidth/PSD pass instead of repeating the
 *        search_window_for_span() -> required_bandwidth_hz() -> compute_psd()
 *        chain itself, which is exactly the mistake this function's own doc
 *        comment warns a repeated call site risks. Filled whenever a PSD was
 *        computed, whether or not the estimate that followed is valid;
 *        untouched if span_mm/sample_rate is non-positive or no search window
 *        exists, since nothing was computed in that case.
 * @param background Forwarded to estimate_pitch() - see there.
 * @return Estimate with valid=false if span_mm or sample_rate is non-positive,
 *         or if the underlying estimate_pitch() call finds nothing
 */
PitchEstimate estimate_pitch_for_span(const std::vector<AccelSample>& samples, float sample_rate,
                                      float span_mm, int n_harmonics = DEFAULT_HARMONICS,
                                      std::vector<std::pair<float, float>>* out_psd = nullptr,
                                      const QuietSpectrum* background = nullptr);

} // namespace helix::calibration
