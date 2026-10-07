// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <memory>

namespace helix {
namespace ui {
class UiBufferSlider;
} // namespace ui

/// Home widget for the filament buffer: where the buffer between the feeder
/// and the extruder sits against its target. 1x1 is the upright slider with its
/// label and number; 2x1 adds the last minute as a trace and the lean in words.
/// Gated on a proportional reading existing (buffer_present).
class FilamentBufferWidget : public PanelWidget {
  public:
    FilamentBufferWidget();
    ~FilamentBufferWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "filament_buffer";
    }

  private:
    std::unique_ptr<ui::UiBufferSlider> slider_;
};

} // namespace helix
