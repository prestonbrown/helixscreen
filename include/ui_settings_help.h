// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_help.h
 * @brief Help & About overlay - debug bundle, Discord, docs, about
 *
 * This overlay provides quick access to:
 * - Debug bundle upload for support
 * - Discord community link
 * - Documentation link
 * - About/version info overlay
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see AboutSettingsOverlay for version/update details
 */

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class HelpSettingsOverlay
 * @brief Overlay for help, support, and about actions
 *
 * @code
 * helix::settings::get_help_settings_overlay().show(parent_screen);
 * @endcode
 */
class HelpSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Help & About";
    }
    const char* xml_component() const override {
        return "settings_help_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
};

inline HelpSettingsOverlay& get_help_settings_overlay() {
    return lazy_global<HelpSettingsOverlay>("HelpSettingsOverlay");
}

} // namespace helix::settings
