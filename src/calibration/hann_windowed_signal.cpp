// SPDX-License-Identifier: GPL-3.0-or-later
#include "hann_windowed_signal.h"

#include <cmath>

namespace helix::calibration {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

HannWindowedSignal::HannWindowedSignal(const float* x, size_t n) {
    if (x == nullptr || n < 2) {
        return;
    }

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mean += x[i];
    }
    mean /= static_cast<double>(n);

    // The window's cosine advances by a fixed rotation per sample, so the loop
    // needs no cos call.
    const double window_step = 2.0 * kPi / static_cast<double>(n - 1);
    const double wc = std::cos(window_step);
    const double ws = std::sin(window_step);
    double win_re = 1.0; // cos(window_step * i)
    double win_im = 0.0;
    v_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * win_re;
        v_[i] = (static_cast<double>(x[i]) - mean) * w;
        sum_w_ += w;
        sum_w2_ += w * w;

        const double nwr = win_re * wc - win_im * ws;
        win_im = win_re * ws + win_im * wc;
        win_re = nwr;
    }
}

double HannWindowedSignal::power(double omega) const {
    // exp(-j omega i) advances by complex multiplication rather than a cos/sin
    // per sample (5.8x faster on an Allwinner H616). It is double because a
    // float phasor accumulates enough rotation error over thousands of samples
    // to smear the peak.
    const double bc = std::cos(omega);
    const double bs = std::sin(omega);
    double ph_re = 1.0;
    double ph_im = 0.0;
    double acc_re = 0.0;
    double acc_im = 0.0;
    for (const double v : v_) {
        acc_re += v * ph_re;
        acc_im += v * ph_im;
        const double npr = ph_re * bc + ph_im * bs;
        ph_im = ph_im * bc - ph_re * bs;
        ph_re = npr;
    }
    return acc_re * acc_re + acc_im * acc_im;
}

namespace {

// The same operations, in the same order, as HannWindowedSignal::power() runs
// for each axis and frequency. The phasor update is the loop's serial chain:
// three axes share one, and B frequencies run B chains side by side.
template <size_t B>
void power_block(const double* const (&v)[3], size_t n, const double* omegas,
                 std::array<double, 3>* out) {
    double bc[B], bs[B], ph_re[B], ph_im[B];
    double re[3][B] = {}, im[3][B] = {};
    for (size_t b = 0; b < B; ++b) {
        bc[b] = std::cos(omegas[b]);
        bs[b] = std::sin(omegas[b]);
        ph_re[b] = 1.0;
        ph_im[b] = 0.0;
    }
    for (size_t i = 0; i < n; ++i) {
        for (size_t b = 0; b < B; ++b) {
            for (size_t a = 0; a < 3; ++a) {
                re[a][b] += v[a][i] * ph_re[b];
                im[a][b] += v[a][i] * ph_im[b];
            }
            const double npr = ph_re[b] * bc[b] + ph_im[b] * bs[b];
            ph_im[b] = ph_im[b] * bc[b] - ph_re[b] * bs[b];
            ph_re[b] = npr;
        }
    }
    for (size_t b = 0; b < B; ++b) {
        for (size_t a = 0; a < 3; ++a) {
            out[b][a] = re[a][b] * re[a][b] + im[a][b] * im[a][b];
        }
    }
}

} // namespace

void HannWindowedSignal::power(const std::array<HannWindowedSignal, 3>& axes, const double* omegas,
                               size_t count, std::array<double, 3>* out) {
    const size_t n = axes[0].v_.size();
    if (axes[1].v_.size() != n || axes[2].v_.size() != n) {
        for (size_t k = 0; k < count; ++k) {
            out[k] = {axes[0].power(omegas[k]), axes[1].power(omegas[k]), axes[2].power(omegas[k])};
        }
        return;
    }
    const double* const v[3] = {axes[0].v_.data(), axes[1].v_.data(), axes[2].v_.data()};
    constexpr size_t kBlock = 4;
    size_t k = 0;
    for (; k + kBlock <= count; k += kBlock) {
        power_block<kBlock>(v, n, omegas + k, out + k);
    }
    for (; k < count; ++k) {
        power_block<1>(v, n, omegas + k, out + k);
    }
}

} // namespace helix::calibration
