// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_color_picker_view.h"

#include "overlay_base.h"

#include <cstdint>
#include <functional>

namespace helix::ui {

/**
 * The color picker as an overlay page: back, the readout and the Grid | Sliders
 * switch in the header, the picker filling the body. A host with a draft of its
 * own (the slot editor) opens it: every change reaches on_change at once, Back
 * returns, and the host's own Save commits. on_close fires whenever the page
 * leaves the stack.
 */
class ColorPickerPage : public OverlayBase {
  public:
    void open(lv_obj_t* parent_screen, uint32_t initial_rgb, const ColorPickerConfig& config,
              std::function<void(uint32_t rgb)> on_change, std::function<void()> on_close);

    ColorPickerView& view() {
        return view_;
    }

    const char* get_name() const override {
        return "Color Picker";
    }
    const char* xml_component() const override {
        return "color_picker_page";
    }
    bool destroy_on_close() const override {
        return true;
    }
    HELIX_KIND_TAG(ColorPickerPage)

  protected:
    void before_show() override;
    void on_dismissed() override;
    void on_ui_destroyed() override;

  private:
    ColorPickerView view_;
    ColorPickerConfig config_;
    uint32_t initial_ = 0;
    std::function<void(uint32_t)> on_change_;
    std::function<void()> on_close_;
};

ColorPickerPage& get_color_picker_page();

} // namespace helix::ui
