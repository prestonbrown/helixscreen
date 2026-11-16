// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_view.h"

#include "lvgl.h" // lv_tr()
#include "motor_ripple_analysis.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix::calibration::ripple {

namespace {

constexpr float kSecondsPerSpeed = 3.5f;

const char* motor_letter(MotorId m) {
    return m == MotorId::StepperX ? "X" : "Y";
}

/// The highest value of one harmonic over every motor's sweep, and its speed.
std::pair<float, float> worst(const std::vector<MotorSweep>& sweeps, size_t k) {
    float best = 0.0f;
    float at = 0.0f;
    for (const auto& s : sweeps) {
        for (const auto& r : s.speeds) {
            if (r.ripple[k] && *r.ripple[k] > best) {
                best = *r.ripple[k];
                at = r.speed_mm_s;
            }
        }
    }
    return {best, at};
}

} // namespace

Kinematics ripple_kinematics(const std::string& kinematics) {
    if (kinematics == "corexy" || kinematics == "limited_corexy") {
        return Kinematics::CoreXY;
    }
    if (kinematics == "cartesian" || kinematics == "limited_cartesian") {
        return Kinematics::Cartesian;
    }
    return Kinematics::Unsupported;
}

bool ripple_check_supported(const std::string& kinematics, bool has_accelerometer,
                            bool has_geometry) {
    return has_accelerometer && has_geometry &&
           ripple_kinematics(kinematics) != Kinematics::Unsupported;
}

std::string ripple_progress_text(MotorId motor, float speed, size_t index, size_t total,
                                 bool compensated) {
    const char* fmt_str = compensated
                              ? lv_tr("{0} motor, compensation on: {1:.0f} mm/s ({2} of {3})")
                              : lv_tr("{0} motor: {1:.0f} mm/s ({2} of {3})");
    return fmt::format(fmt::runtime(fmt_str), motor_letter(motor), speed, index + 1, total);
}

std::string ripple_band_summary(const std::vector<RippleBand>& bands) {
    const auto merged = merge_bands(bands);
    if (merged.empty()) {
        return lv_tr("No speed shook the printer clearly more than the rest.");
    }
    std::string out;
    for (const auto& b : merged) {
        if (!out.empty()) {
            out += '\n';
        }
        out += fmt::format(fmt::runtime(lv_tr("{0:.0f}-{1:.0f} mm/s: the {2}x ripple meets a "
                                              "{3:.0f} Hz resonance")),
                           b.lo_mm_s, b.hi_mm_s, b.harmonic, b.freq_hz);
    }
    return out;
}

std::string ripple_reduction_summary(const SweepResult& result, bool factory_values) {
    std::string out;
    for (size_t k = 0; k < kHarmonics.size(); ++k) {
        const auto before = worst(result.sweeps, k);
        const auto after = worst(result.compensated_sweeps, k);
        if (!(before.first > 0.0f)) {
            continue;
        }
        const int less = static_cast<int>(
            std::lround(100.0f * (1.0f - std::min(after.first, before.first) / before.first)));
        if (!out.empty()) {
            out += '\n';
        }
        out += fmt::format(fmt::runtime(lv_tr("{0}x ripple at {1:.0f} mm/s: {2:.0f} before, "
                                              "{3:.0f} after ({4}% less)")),
                           kHarmonics[k], before.second, before.first, after.first, less);
    }
    if (factory_values && !out.empty()) {
        out += '\n';
        out += lv_tr("\"After\" is compensation as your printer applies it, with its factory "
                     "settings.");
    }
    return out;
}

std::vector<float> ripple_resample_uniform(const std::vector<float>& xs,
                                           const std::vector<float>& ys, float lo, float hi,
                                           size_t n) {
    std::vector<float> out(n, NAN);
    if (n < 2 || xs.empty() || xs.size() != ys.size()) {
        return out;
    }
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const float x = lo + (hi - lo) * static_cast<float>(i) / static_cast<float>(n - 1);
        if (x < xs.front() || x > xs.back()) {
            continue;
        }
        while (j + 1 < xs.size() && xs[j + 1] < x) {
            ++j;
        }
        if (x == xs[j]) {
            out[i] = ys[j];
            continue;
        }
        if (j + 1 >= xs.size()) {
            continue;
        }
        const float t = (x - xs[j]) / (xs[j + 1] - xs[j]);
        out[i] = ys[j] + t * (ys[j + 1] - ys[j]); // NaN if either side is
    }
    return out;
}

int ripple_estimate_minutes(size_t motors, size_t speeds, bool compare) {
    const float seconds =
        static_cast<float>(motors * speeds * (compare ? 2 : 1)) * kSecondsPerSpeed;
    return static_cast<int>(std::ceil(seconds / 60.0f));
}

} // namespace helix::calibration::ripple
