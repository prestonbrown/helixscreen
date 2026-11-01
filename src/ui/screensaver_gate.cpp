// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_gate.h"

#include "env_whole_number.h"

namespace helix::ui {

double saver_budget_share(int cores, float bogomips, bool printing) {
    if (cores >= 4) {
        const bool slow = bogomips > 0.0f && bogomips < kSaverSlowBoardBogomips;
        return printing ? 0.25 : (slow ? 0.50 : 1.00);
    }
    if (cores == 3) {
        return printing ? 0.185 : 0.75;
    }
    if (cores == 2) {
        return printing ? 0.125 : 0.50;
    }
    return printing ? 0.05 : 0.10;
}

GateDecision decide_saver_level(double share, double budget, size_t level, size_t level_count) {
    if (share <= budget) {
        return GateDecision::KEEP;
    }
    return level + 1 < level_count ? GateDecision::STEP_DOWN : GateDecision::TOO_HEAVY;
}

GateDecision decide_saver_level_up(size_t level, int comfortable_windows, size_t ceiling) {
    if (level <= ceiling || comfortable_windows < kStepUpWindows) {
        return GateDecision::KEEP;
    }
    return GateDecision::STEP_UP;
}

void IdleBaseline::add(CpuSample sample) {
    samples_.push_back(sample);
    while (samples_.size() > 1 && sample.wall_ns - samples_.front().wall_ns > kSpanNs) {
        samples_.pop_front();
    }
}

void IdleBaseline::reset() {
    samples_.clear();
}

double IdleBaseline::rate() const {
    if (samples_.size() < 2) {
        return 0.0;
    }
    const CpuSample& first = samples_.front();
    const CpuSample& last = samples_.back();
    const uint64_t wall = last.wall_ns - first.wall_ns;
    if (wall < kMinSpanNs) {
        return 0.0;
    }
    return static_cast<double>(last.cpu_ns - first.cpu_ns) / static_cast<double>(wall);
}

void SaverGateSession::begin(CpuSample start, double baseline_rate) {
    start_ = start;
    window_start_ = start;
    warming_up_ = true;
    baseline_rate_ = baseline_rate;
}

void SaverGateSession::restart_window(CpuSample now) {
    window_start_ = now;
    warming_up_ = false;
}

std::optional<double> SaverGateSession::add(CpuSample sample) {
    if (warming_up_) {
        if (sample.wall_ns - start_.wall_ns < kWarmupNs) {
            return std::nullopt;
        }
        warming_up_ = false;
        window_start_ = sample;
        return std::nullopt;
    }
    const uint64_t wall = sample.wall_ns - window_start_.wall_ns;
    if (wall < kWindowNs) {
        return std::nullopt;
    }
    const double cpu = static_cast<double>(sample.cpu_ns - window_start_.cpu_ns);
    const double wall_d = static_cast<double>(wall);
    window_start_ = sample;
    return (cpu - baseline_rate_ * wall_d) / wall_d;
}

SaverEnvOverrides saver_env_overrides() {
    return {whole_number_from_env("HELIX_SCREENSAVER_BUDGET_PCT", 1, 400, "[ScreensaverManager]",
                                  "a whole percent of one core"),
            whole_number_from_env("HELIX_SCREENSAVER_LEVEL", 0, 99, "[ScreensaverManager]",
                                  "a whole level number")};
}

} // namespace helix::ui
