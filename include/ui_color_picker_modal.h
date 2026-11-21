// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_color_picker_view.h"
#include "ui_modal.h"

#include <cstdint>
#include <functional>
#include <string>

namespace helix::ui {

/**
 * The color picker in a modal: Cancel, the readout, the Grid | Sliders switch
 * and Done over a ColorPickerView. For hosts with no page of their own to put
 * the picker in (the LED control, the Spoolman edit modal, the spool wizard).
 *
 * Every change reaches the preview callback, so a host can show it live (the
 * LED lights it). Done reports the color through the color callback and, when
 * remember_on_done is set, adds it to the strip's recents. Cancel, and any close
 * that is not Done (backdrop, ESC), reports the original color to the preview
 * callback, so a live preview is undone.
 */
class ColorPickerModal : public Modal {
  public:
    using ColorCallback = std::function<void(uint32_t color_rgb, const std::string& color_name)>;

    ColorPickerModal() = default;
    ~ColorPickerModal() override;
    ColorPickerModal(const ColorPickerModal&) = delete;
    ColorPickerModal& operator=(const ColorPickerModal&) = delete;

    /// The palette and strip to show; set before show_with_color().
    void set_config(const ColorPickerConfig& config) {
        config_ = config;
    }
    /// Done adds the color to the strip's recents (the LED's commit point).
    /// Filament hosts commit on their own Save and leave this off.
    void set_remember_on_done(bool remember) {
        remember_on_done_ = remember;
    }
    void set_color_callback(ColorCallback callback) {
        color_callback_ = std::move(callback);
    }
    void set_preview_callback(std::function<void(uint32_t rgb)> callback) {
        preview_callback_ = std::move(callback);
    }
    /// Fires on every close, after the color or preview callback.
    void set_dismiss_callback(std::function<void()> callback) {
        dismiss_callback_ = std::move(callback);
    }

    bool show_with_color(lv_obj_t* parent, uint32_t initial_rgb);

    /// The picker inside, while shown (tests drive it).
    ColorPickerView& view() {
        return view_;
    }

    void done();
    void on_cancel() override;

    [[nodiscard]] const char* get_name() const override {
        return "Color Picker";
    }
    [[nodiscard]] const char* component_name() const override {
        return "color_picker_modal";
    }
    HELIX_KIND_TAG(ColorPickerModal)

  protected:
    void on_show() override;
    void on_hide() override;
    void on_dismissed() override;

  private:
    void restore_original();

    ColorPickerConfig config_;
    ColorPickerView view_;
    uint32_t original_ = 0;
    bool remember_on_done_ = false;
    ColorCallback color_callback_;
    std::function<void(uint32_t)> preview_callback_;
    std::function<void()> dismiss_callback_;
};

} // namespace helix::ui
