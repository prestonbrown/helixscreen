// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <string>

/**
 * @file upgrade_nudge.h
 * @brief In-app upgrade nudge coordinator
 *
 * Decides when (and how insistently) to surface an available update to the
 * user. The intensity is a persistent config setting under
 * `/upgrade_nudge/intensity` in settings.json. Ships as `OFF` for now; gets
 * flipped to `AGGRESSIVE` via a release-coordination setting change at 1.0
 * launch — no code change needed.
 *
 * Gating:
 * - `Intensity::OFF`              — this class effectively no-ops. The Settings
 *                                   > About entry still shows the update.
 * - `Intensity::NORMAL`           — red dot on the Settings icon when an
 *                                   update is available and the printer is idle.
 *                                   UI layer observes `should_show_settings_badge()`.
 * - `Intensity::AGGRESSIVE`       — persistent dismissible top-banner on every
 *                                   panel, plus the red dot. Dismissal is
 *                                   per-version (banner reappears on next
 *                                   launch, then suppresses until a *new*
 *                                   version is detected).
 *
 * Never fires while a job owns the machine: all visibility queries return
 * `false` when `job_holds_machine()` is true for the published print
 * lifecycle — Preparing and Paused included, not just Printing.
 *
 * This class is the DECISION layer only. It does not touch LVGL or create
 * widgets — the UI layer owns presentation. Consumers poll the query methods
 * (e.g. on panel show, or via an observer on update state) and render
 * accordingly.
 */

namespace helix {

class UpgradeNudge {
  public:
    enum class Intensity {
        Off = 0,        // No in-app nudge beyond Settings > About
        Normal = 1,     // Red dot on Settings icon
        Aggressive = 2, // Persistent top-banner for 1.0 rollout
    };

    static UpgradeNudge& instance();

    /// Read current intensity from Config. Cached; call `reload()` after
    /// a Config mutation to refresh.
    Intensity get_intensity() const;

    /// Returns true if the UI should show a red dot on the Settings icon.
    /// Requires: Intensity >= Normal, update available, not currently printing.
    bool should_show_settings_badge() const;

    /// Returns true if the UI should show the persistent top-banner.
    /// Requires: Intensity == Aggressive, update available, not currently
    /// printing, and the available version has not already been dismissed.
    bool should_show_banner() const;

    /// Current available version, or empty string if no update cached.
    /// Convenience wrapper over UpdateChecker::get_cached_update().
    std::string get_available_version() const;

    /// Record that the user dismissed the banner for the current available
    /// version. Persists to Config as `/upgrade_nudge/dismissed_version`.
    /// The banner will stay suppressed until UpdateChecker reports a
    /// *different* version than the dismissed one.
    void dismiss_current_version();

    /// Re-read Config values. Call after `Config::save()` if other code paths
    /// mutate the nudge settings directly.
    void reload();

    // Non-copyable singleton
    UpgradeNudge(const UpgradeNudge&) = delete;
    UpgradeNudge& operator=(const UpgradeNudge&) = delete;

  private:
    // Test-only seam (tests/test_helpers/upgrade_nudge_test_access.h). The two
    // public queries each layer an intensity/dismissal check on top of the
    // common gate, so reaching the gate itself is the only way to test the
    // print-state policy without those confounds.
    friend class UpgradeNudgeTestAccess;

    UpgradeNudge();
    ~UpgradeNudge() = default;

    /// Common gate used by both badge and banner queries.
    bool is_update_visible_now() const;

    mutable std::mutex mu_;
    Intensity intensity_ = Intensity::Off;
    std::string dismissed_version_;
};

} // namespace helix
