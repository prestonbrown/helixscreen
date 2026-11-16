// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace helix::calibration {

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
    /// Sum of the window: a pure tone of amplitude A reads power() = (A sum_w / 2)^2.
    [[nodiscard]] double sum_w() const {
        return sum_w_;
    }
    [[nodiscard]] double sum_w2() const {
        return sum_w2_;
    }
    [[nodiscard]] size_t size() const {
        return v_.size();
    }

  private:
    std::vector<double> v_;
    double sum_w_ = 0.0;
    double sum_w2_ = 0.0;
};

} // namespace helix::calibration
