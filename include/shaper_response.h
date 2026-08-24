// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file shaper_response.h
 * @brief Klipper input shaper transfer functions, ported for client-side scoring
 *
 * Reproduces the shaper tap definitions from klippy's shaper_defs.py and the
 * transfer-function evaluation from shaper_calibrate.py's _estimate_shaper(),
 * so a curve computed here matches the fitted column a real calibration CSV
 * carries (verified against a K1C run to within CSV rounding, 0.005).
 *
 * Used to re-score a printer's live-before shaper setting against freshly
 * measured PSD data, and to overlay the old setting's curve on the results
 * chart, without asking the firmware to do either.
 */

#pragma once

#include <string>
#include <vector>

namespace helix {
namespace calibration {

/// Klipper's default input shaper damping ratio (shaper_defs.py).
inline constexpr double SHAPER_DEFAULT_DAMPING_RATIO = 0.1;

/**
 * @brief Attenuation |H(f)| of a shaper at each frequency bin
 *
 * Evaluates the shaper transfer function the way the firmware does when
 * writing a calibration CSV: pessimized (maxed) over Klipper's test damping
 * ratios, so vendor forks that fit a single shaper still produce the same
 * column values the printer would have written.
 *
 * @param shaper_type    One of zv, zvd, mzv, ei, 2hump_ei, 3hump_ei
 * @param shaper_freq_hz Fitted shaper frequency in Hz
 * @param damping_ratio  Damping ratio used to generate the taps (Klipper
 *                       default 0.1)
 * @param freqs_hz       Frequency bins to evaluate at, in Hz
 * @return Attenuation per bin (H(0) == 1), or an EMPTY vector when the shaper
 *         type is unknown (e.g. Kalico smooth shapers, vendor-fork names) -
 *         callers must degrade gracefully, never crash.
 */
std::vector<double> shaper_transfer_curve(const std::string& shaper_type, double shaper_freq_hz,
                                          double damping_ratio,
                                          const std::vector<double>& freqs_hz);

/**
 * @brief Fraction of vibration a shaper leaves in a measured spectrum
 *
 * Reproduces klippy's _estimate_remaining_vibrations(): only signal above
 * psd.max()/SHAPER_VIBRATION_REDUCTION counts, the shaped spectrum is H*psd
 * (linear), and the residual is the thresholded ratio as a percentage. Using
 * the same formula as the firmware keeps the verdict comparable to the
 * vibrations% the comparison table shows for the same shaper.
 *
 * @param psd            Measured power spectral density, one value per bin
 * @param transfer_curve Attenuation per bin, as produced by
 *                       shaper_transfer_curve()
 * @return Residual vibration in percent, or -1 when the inputs are empty,
 *         size-mismatched, or the PSD carries no energy above the threshold
 *         (callers omit the verdict rather than guess).
 */
double residual_vibration_percent(const std::vector<double>& psd,
                                  const std::vector<double>& transfer_curve);

} // namespace calibration
} // namespace helix
