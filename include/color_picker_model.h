// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file color_picker_model.h
 * @brief The color picker's decisions, free of LVGL: the preset grids each caller
 *        brings, the HSV state the Sliders tab edits and the hex keypad.
 *
 * helix::ui::ColorPickerView (ui_color_picker_view.h) renders these.
 */

namespace helix {

/// The grid a picker shows. Each caller brings the one that fits what it edits.
enum class ColorPalette : uint8_t {
    Filament, ///< 10 x 6: a gray ramp, then ten hues at five tones
    Led,      ///< 10 x 3: white temperatures, full hues, pastel hues
    Theme,    ///< 6 x 5: a surface ramp, then six hue families at three tones
};

/// The palette's colors, row-major.
const std::vector<uint32_t>& color_palette(ColorPalette palette);

/// How many columns the palette's grid has.
int color_palette_columns(ColorPalette palette);

/// How many chips `chip_w` wide and `gap` apart fit whole in a row `avail_w` wide.
int whole_chips_in(int avail_w, int chip_w, int gap);

/// A color in the Sliders tab's units: hue in whole degrees [0, 359],
/// saturation and brightness (value) in whole percent [0, 100].
struct Hsv {
    int h = 0;
    int s = 0;
    int v = 0;
    bool operator==(const Hsv& o) const {
        return h == o.h && s == o.s && v == o.v;
    }
};

/// RGB (0xRRGGBB) to HSV, rounded. A gray has hue 0 and a black saturation 0.
Hsv rgb_to_hsv(uint32_t rgb);

/// HSV to RGB (0xRRGGBB), each channel rounded.
uint32_t hsv_to_rgb(const Hsv& hsv);

enum class HsvChannel : uint8_t { Hue, Saturation, Brightness };

/**
 * The color a picker holds and the HSV the Sliders tab shows for it.
 *
 * An RGB set from the grid, the strip or the keypad is kept exactly; the HSV
 * follows it, except that a color with no hue (a gray, or black) keeps the hue
 * the sliders had, and black keeps their saturation too, so dragging
 * brightness up from black returns to the color it came from. Changing a
 * channel recomputes the RGB from the HSV.
 */
class ColorPickerModel {
  public:
    static constexpr int kHueStep = 2;
    static constexpr int kPercentStep = 1;

    explicit ColorPickerModel(uint32_t rgb = 0x808080);

    void set_rgb(uint32_t rgb);
    /// Hue wraps into [0, 359]; saturation and brightness clamp to [0, 100].
    void set_channel(HsvChannel channel, int value);
    /// One [-]/[+] press: hue by kHueStep (wrapping), the others by kPercentStep.
    void step(HsvChannel channel, int direction);

    uint32_t rgb() const {
        return rgb_;
    }
    const Hsv& hsv() const {
        return hsv_;
    }
    int channel(HsvChannel channel) const;

  private:
    uint32_t rgb_ = 0;
    Hsv hsv_{};
};

/**
 * The hex keypad: up to six digits typed one at a time, backspace, and a
 * result once all six are in.
 */
class HexKeypad {
  public:
    static constexpr size_t kDigits = 6;

    void clear() {
        digits_.clear();
    }
    /// Appends @p c (0-9, A-F, a-f) while fewer than six are in. Returns
    /// whether it was taken.
    bool push(char c);
    void backspace();

    const std::string& digits() const {
        return digits_;
    }
    /// OK is enabled only with all six digits.
    bool complete() const {
        return digits_.size() == kDigits;
    }
    /// The typed color once complete().
    std::optional<uint32_t> result() const;

  private:
    std::string digits_;
};

} // namespace helix
