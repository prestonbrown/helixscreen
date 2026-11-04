// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_language_time.h
 * @brief Language & Time settings overlay - language, timezone, time format
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SystemSettingsManager for language persistence
 * @see DisplaySettingsManager for timezone and time format persistence
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

namespace helix::settings {

class LanguageTimeSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Language & Time";
    }
    const char* xml_component() const override {
        return "settings_language_time_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
    void on_activate() override;

  private:
    void init_language_dropdown();
    void init_timezone_dropdown();
};

inline LanguageTimeSettingsOverlay& get_language_time_settings_overlay() {
    return lazy_global<LanguageTimeSettingsOverlay>("LanguageTimeSettingsOverlay");
}

} // namespace helix::settings
