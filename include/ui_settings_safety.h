// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_safety.h
 * @brief Safety & Alerts overlay - e-stop, cancel escalation, completion alerts
 *
 * This overlay allows users to configure:
 * - E-Stop confirmation toggle
 * - Cancel escalation toggle and timeout
 * - Print completion alert mode
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SafetySettingsManager for persistence
 * @see AudioSettingsManager for completion alert persistence
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class SafetySettingsOverlay
 * @brief Overlay for configuring safety and notification settings
 *
 * Every row binds its settings subject and its callback writes straight
 * through the settings manager.
 */
class SafetySettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Safety & Alerts";
    }
    const char* xml_component() const override {
        return "settings_safety_overlay";
    }

    void register_callbacks() override;
};

inline SafetySettingsOverlay& get_safety_settings_overlay() {
    return lazy_global<SafetySettingsOverlay>("SafetySettingsOverlay");
}

} // namespace helix::settings
