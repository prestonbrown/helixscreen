// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "lvgl.h"
#include "subject_managed_panel.h"

#include <string>

/**
 * @file upgrade_banner.h
 * @brief Persistent dismissible top-banner for the 1.0 upgrade rollout
 *
 * Presentation layer (one instance, owned by Application) that consumes UpgradeNudge decisions and
 * renders the banner on `lv_layer_top` so it floats above panel navigation.
 * Ships dormant: UpgradeNudge::should_show_banner() returns false in OFF/
 * NORMAL intensity, so the widget is created but stays hidden until the
 * intensity setting is flipped to AGGRESSIVE for the 1.0 rollout.
 *
 * Observers:
 * - UpdateChecker::status_subject() — re-evaluates visibility when an
 *   update becomes available or the cached update changes.
 * - PrinterState print_active_subject (via UpgradeNudge gating) — hides
 *   the banner mid-print; reappears when the printer goes idle.
 */

namespace helix {

class UpgradeBanner {
  public:
    UpgradeBanner() = default;
    ~UpgradeBanner() = default;

    /// Create the banner widget on `lv_layer_top` and start observing.
    /// Must be called from the LVGL thread after XML components are
    /// registered. Idempotent.
    void init();

    /// Tear down LVGL state. Safe to call before `lv_deinit`.
    void shutdown();

    /// Force re-evaluation of visibility. Useful after a Config mutation
    /// that might flip `/upgrade_nudge/intensity` at runtime.
    void refresh();

    UpgradeBanner(const UpgradeBanner&) = delete;
    UpgradeBanner& operator=(const UpgradeBanner&) = delete;

  private:
    void evaluate_visibility();
    void update_message_text();

    // XML event callbacks — registered at init.
    static void on_update_clicked(lv_event_t* e);
    static void on_dismiss_clicked(lv_event_t* e);

    lv_obj_t* banner_ = nullptr;
    SubjectManager subjects_;
    lv_subject_t message_subject_{};
    bool message_subject_initialized_ = false;

    ObserverGuard status_observer_;
    ObserverGuard version_observer_;
    AsyncLifetimeGuard lifetime_;

    std::string last_rendered_version_;
};

} // namespace helix
