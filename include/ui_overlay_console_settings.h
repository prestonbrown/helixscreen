// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl.h"
#include "overlay_base.h"

/**
 * @file ui_overlay_console_settings.h
 * @brief Settings overlay for the gcode console panel.
 *
 * Hosts the user-facing toggles that control console output filtering:
 *   - "Hide Temperature Reports" — drops periodic `T:.../B:...` status lines.
 *   - "Hide Firmware Noise"      — drops raw debug output emitted by Creality
 *                                   K2 / FlashForge / similar firmware modules.
 *
 * Toggle state is owned by `helix::SettingsManager` (see its
 * `console_filter_*_subject_` fields) — this class is a thin XML-bound view
 * that forwards switch events to the manager.
 */
class ConsoleSettingsOverlay : public OverlayBase {
  public:
    void register_callbacks() override;
    [[nodiscard]] const char* get_name() const override {
        return "Console Settings";
    }
    const char* xml_component() const override {
        return "console_settings_overlay";
    }
};

inline ConsoleSettingsOverlay& get_global_console_settings() {
    return helix::lazy_global<ConsoleSettingsOverlay>("ConsoleSettingsOverlay");
}
