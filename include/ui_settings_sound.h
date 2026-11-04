// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sound.h
 * @brief Sound settings overlay - master toggle, volume, UI sounds, theme, output device
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see AudioSettingsManager for persistence
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

namespace helix::settings {

class SoundSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Sound";
    }
    const char* xml_component() const override {
        return "settings_sound_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

    //
    // === Event Handlers (public for the callback table) ===
    //

    void handle_sounds_changed(bool enabled);
    void handle_volume_changed(int value);
    void handle_sound_theme_changed(int index);
    void handle_audio_device_changed(int index);
    void handle_preview_sounds();
    void handle_test_tracker();

  private:
    void init_volume_slider();
    void init_sound_theme_dropdown();
    void init_audio_device_dropdown();

    /// Volume value label text
    char volume_value_buf_[8]; // e.g., "100%"
};

inline SoundSettingsOverlay& get_sound_settings_overlay() {
    return lazy_global<SoundSettingsOverlay>("SoundSettingsOverlay");
}

} // namespace helix::settings
