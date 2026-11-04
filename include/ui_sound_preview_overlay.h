// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

#include <string>

namespace helix::settings {

class SoundPreviewOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Preview Sounds";
    }
    const char* xml_component() const override {
        return "sound_preview_overlay";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

  private:
    void populate_buttons();
    void clear_buttons();

    static std::string display_name(const std::string& sound_name);
};

inline SoundPreviewOverlay& get_sound_preview_overlay() {
    return lazy_global<SoundPreviewOverlay>("SoundPreviewOverlay");
}

} // namespace helix::settings
