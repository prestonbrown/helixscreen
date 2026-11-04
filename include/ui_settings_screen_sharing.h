// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_screen_sharing.h
 * @brief Screen Sharing settings overlay: share, webcam entry, remote control, pairing (#1825)
 *
 * Opened from Settings > Connection. A thin XML-bound view: the toggle state (per
 * printer) and the pairing code belong to RemoteScreenSettingsManager.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

namespace helix::settings {

class ScreenSharingSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Screen Sharing";
    }
    const char* xml_component() const override {
        return "settings_screen_sharing_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
};

inline ScreenSharingSettingsOverlay& get_screen_sharing_settings_overlay() {
    return lazy_global<ScreenSharingSettingsOverlay>("ScreenSharingSettingsOverlay");
}

} // namespace helix::settings
