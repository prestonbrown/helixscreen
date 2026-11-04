// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver.h"

#if HELIX_ENABLE_SCREENSAVER

#include "config.h"
#include "json_fwd.h"
#include "platform_capabilities.h"
#include "refresh_period_hold.h"
#include "refresh_period_hold_test_access.h"
#include "refresh_timing_env.h"
#include "scoped_env.h"
#include "screensaver_base.h"
#include "screensaver_cpu_clock.h"
#include "screensaver_level_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace helix {

// Test-only seam. The manager owns its saver instances, its gate state and its clock privately.
class ScreensaverManagerTestAccess {
  public:
    /// The saver the manager is running, or nullptr (also while it shows a black screen).
    static helix::ui::SaverBase* active(const ScreensaverManager& mgr) {
        return mgr.active_;
    }

    /// Replaces the CPU clock; the next idle-check tick samples it at once.
    static void set_cpu_clock(ScreensaverManager& mgr, helix::ui::CpuClockFn clock) {
        mgr.cpu_clock_ = std::move(clock);
        mgr.sampled_ = false;
    }

    static void reset_baseline(ScreensaverManager& mgr) {
        mgr.baseline_.reset();
        mgr.sampled_ = false;
    }

    static double baseline_rate(const ScreensaverManager& mgr) {
        return mgr.baseline_.rate();
    }

    static size_t baseline_samples(const ScreensaverManager& mgr) {
        return mgr.baseline_.size();
    }

    static bool showing_black_screen(const ScreensaverManager& mgr) {
        return mgr.black_screen_type_ != ScreensaverType::OFF;
    }

    static lv_obj_t* black_screen(const ScreensaverManager& mgr) {
        return mgr.black_screen_.obj();
    }

    /// The board fingerprint a saver started now would store.
    static std::string current_board(const ScreensaverManager& mgr) {
        return helix::ui::board_fingerprint(mgr.board_facts(helix::PlatformCapabilities::detect()));
    }
};

/// Drives `mgr`'s gate from `clock` for the life of the object, then puts the process clock
/// back. Each switch drops the gate's baseline, so the next idle-check tick samples afresh.
class ScopedScriptedCpuClock {
  public:
    ScopedScriptedCpuClock(ScreensaverManager& mgr, helix::ui::CpuClockFn clock) : mgr_(mgr) {
        ScreensaverManagerTestAccess::set_cpu_clock(mgr_, std::move(clock));
        ScreensaverManagerTestAccess::reset_baseline(mgr_);
    }
    ~ScopedScriptedCpuClock() {
        ScreensaverManagerTestAccess::set_cpu_clock(mgr_, helix::ui::read_process_cpu_clock);
        ScreensaverManagerTestAccess::reset_baseline(mgr_);
    }
    ScopedScriptedCpuClock(const ScopedScriptedCpuClock&) = delete;
    ScopedScriptedCpuClock& operator=(const ScopedScriptedCpuClock&) = delete;

  private:
    ScreensaverManager& mgr_;
};

/// Runs with no gate overrides in the environment (HELIX_SCREENSAVER_BUDGET_PCT and
/// HELIX_SCREENSAVER_LEVEL) and no stored saver levels, and restores both however the test
/// exits.
class ScopedGateEnv {
  public:
    static constexpr const char* kLevelsPath = "/display/screensaver_levels";

    ScopedGateEnv() {
        if (const nlohmann::json* levels = Config::get_instance()->try_get_json(kLevelsPath)) {
            saved_levels_ = *levels;
        }
        clear_levels();
    }
    ~ScopedGateEnv() {
        clear_levels();
        if (saved_levels_) {
            Config::get_instance()->set<nlohmann::json>(kLevelsPath, *saved_levels_);
        }
    }
    ScopedGateEnv(const ScopedGateEnv&) = delete;
    ScopedGateEnv& operator=(const ScopedGateEnv&) = delete;

    /// Forgets every stored saver level.
    static void clear_levels() {
        Config* config = Config::get_instance();
        if (config->try_get_json(kLevelsPath) != nullptr) {
            config->get_json("/display").erase("screensaver_levels");
        }
    }

  private:
    ScopedEnv budget_{"HELIX_SCREENSAVER_BUDGET_PCT", nullptr};
    ScopedEnv level_{"HELIX_SCREENSAVER_LEVEL", nullptr};
    std::optional<nlohmann::json> saved_levels_;
};

/**
 * @brief A display refreshing every 40 ms outside a saver, and `saver_ms` as
 * HELIX_SCREENSAVER_REFR_PERIOD_MS (unset with nullopt), applied for the life of the object
 */
struct SaverRefreshEnv {
    explicit SaverRefreshEnv(std::optional<uint32_t> saver_ms = 20u) {
        setenv("HELIX_REFR_PERIOD_MS", "40", 1);
        unsetenv("HELIX_REFR_PERIOD_SCOPE");
        if (saver_ms) {
            setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", std::to_string(*saver_ms).c_str(), 1);
        } else {
            unsetenv("HELIX_SCREENSAVER_REFR_PERIOD_MS");
        }
        apply_refresh_timing(refresh_timing_from_env());
    }

    ScopedTimerPeriods timers;
    ScopedEnv global{"HELIX_REFR_PERIOD_MS"};
    ScopedEnv scope{"HELIX_REFR_PERIOD_SCOPE"};
    ScopedEnv saver{"HELIX_SCREENSAVER_REFR_PERIOD_MS"};
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
