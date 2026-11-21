// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_picker_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace helix {

const std::vector<uint32_t>& color_palette(ColorPalette palette) {
    // Row 1: ten grays, OKLCH lightness 1.0 down to 0.17. Rows 2-6: ten OKLCH hues
    // (27, 55, 90, 125, 150, 190, 235, 268, 305, 350) at the tones pastel (L .90,
    // C .07), muted (.70, .08), vivid (.64, .21), deep (.46, .17) and dark (.32,
    // .10), chroma reduced until the color fits sRGB. Generated once; the device
    // never does OKLCH math. The middle gray is 0x7F7F7F, not the 0x808080 it
    // rounds to: that value is the slot's "no color" marker (picked_color()).
    static const std::vector<uint32_t> filament = {
        0xFFFFFF, 0xDEDEDE, 0xBEBEBE, 0x9E9E9E, 0x7F7F7F, 0x636363, 0x4A4A4A, 0x353535, 0x222222,
        0x0F0F0F, 0xFFD2CC, 0xFFD4B8, 0xF0DDA9, 0xD3E7B4, 0xBEECC6, 0xA8EEE8, 0xBAE5FF, 0xD0DEFF,
        0xE7D5FF, 0xFFCEE4, 0xCC8B84, 0xC6916E, 0xB29D63, 0x93A770, 0x7AAD84, 0x5EAFA9, 0x6BA7C9,
        0x899DD1, 0xAA92C7, 0xC58AA5, 0xF1453F, 0xD36C00, 0xAB8700, 0x769C00, 0x00A84D, 0x00A19B,
        0x0097D2, 0x5B81FF, 0xAB61F0, 0xE2459E, 0xA21A1B, 0x874300, 0x6D5500, 0x496200, 0x006B2E,
        0x006662, 0x006087, 0x304CB5, 0x6F35A2, 0x971C65, 0x5C1714, 0x512500, 0x403100, 0x293900,
        0x003F18, 0x003C39, 0x003851, 0x1D2E65, 0x3F225B, 0x56173A,
    };
    // Row 1: white temperatures 1900, 2300, 2700, 3000, 3500, 4000, 4500, 5000,
    // 5800 and 6500 K. Row 2: HSV hues 0, 25, 50, 90, 130, 165, 195, 225, 270, 310
    // at full saturation and brightness. Row 3: the same hues at 40% saturation.
    // No dark rows: brightness is the light's own control.
    static const std::vector<uint32_t> led = {
        0xFF8400, 0xFF9732, 0xFFA757, 0xFFB16E, 0xFFC18D, 0xFFCEA6, 0xFFDABB, 0xFFE4CE,
        0xFFF3E7, 0xFFFEFA, 0xFF0000, 0xFF6A00, 0xFFD500, 0x80FF00, 0x00FF2A, 0x00FFBF,
        0x00BFFF, 0x0040FF, 0x8000FF, 0xFF00D4, 0xFF9999, 0xFFC399, 0xFFEE99, 0xCCFF99,
        0x99FFAA, 0x99FFE6, 0x99E6FF, 0x99B3FF, 0xCC99FF, 0xFF99EE,
    };
    // Rows 1-2: a 12-step surface ramp, dark to light. Rows 3-5: six hue
    // families (red, amber, green, teal, blue, violet) by column, in muted, mid
    // and deep tones by row. Drawn from the tones the shipped themes occupy.
    static const std::vector<uint32_t> theme = {
        0x0B0D14, 0x1E1E2E, 0x282828, 0x2E3440, 0x3D484D, 0x565F89, 0x6C6F85, 0x8A8980,
        0xA9B1D6, 0xD3C6AA, 0xE5E5E5, 0xFFFFFF, 0xE06C75, 0xE0AF68, 0x9ECE6A, 0x7FBBB3,
        0x7AA2F7, 0xBB9AF7, 0xD9455F, 0xD79921, 0x689D6A, 0x2AA198, 0x268BD2, 0x8839EF,
        0xA02030, 0x8F6A0A, 0x40A02B, 0x0F7B78, 0x1E4FA8, 0x5B3FBF,
    };
    switch (palette) {
    case ColorPalette::Led:
        return led;
    case ColorPalette::Theme:
        return theme;
    case ColorPalette::Filament:
        break;
    }
    return filament;
}

int color_palette_columns(ColorPalette palette) {
    return palette == ColorPalette::Theme ? 6 : 10;
}

Hsv rgb_to_hsv(uint32_t rgb) {
    const double r = ((rgb >> 16) & 0xFF) / 255.0;
    const double g = ((rgb >> 8) & 0xFF) / 255.0;
    const double b = (rgb & 0xFF) / 255.0;
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double d = mx - mn;
    double h = 0.0;
    if (d > 0.0) {
        if (mx == r) {
            h = std::fmod((g - b) / d, 6.0);
        } else if (mx == g) {
            h = (b - r) / d + 2.0;
        } else {
            h = (r - g) / d + 4.0;
        }
        h *= 60.0;
        if (h < 0.0) {
            h += 360.0;
        }
    }
    Hsv out;
    out.h = static_cast<int>(std::lround(h)) % 360;
    out.s = mx > 0.0 ? static_cast<int>(std::lround(d / mx * 100.0)) : 0;
    out.v = static_cast<int>(std::lround(mx * 100.0));
    return out;
}

uint32_t hsv_to_rgb(const Hsv& hsv) {
    const double h = hsv.h;
    const double s = hsv.s / 100.0;
    const double v = hsv.v / 100.0;
    auto f = [&](double n) {
        const double k = std::fmod(n + h / 60.0, 6.0);
        return v - v * s * std::max(0.0, std::min({k, 4.0 - k, 1.0}));
    };
    auto byte = [](double c) {
        return static_cast<uint32_t>(std::clamp(std::lround(c * 255.0), 0L, 255L));
    };
    return (byte(f(5)) << 16) | (byte(f(3)) << 8) | byte(f(1));
}

ColorPickerModel::ColorPickerModel(uint32_t rgb) {
    rgb_ = rgb & 0xFFFFFF;
    hsv_ = rgb_to_hsv(rgb_);
}

void ColorPickerModel::set_rgb(uint32_t rgb) {
    rgb_ = rgb & 0xFFFFFF;
    const Hsv next = rgb_to_hsv(rgb_);
    if (next.s > 0 && next.v > 0) {
        hsv_.h = next.h;
    }
    if (next.v > 0) {
        hsv_.s = next.s;
    }
    hsv_.v = next.v;
}

int ColorPickerModel::channel(HsvChannel channel) const {
    switch (channel) {
    case HsvChannel::Hue:
        return hsv_.h;
    case HsvChannel::Saturation:
        return hsv_.s;
    case HsvChannel::Brightness:
        break;
    }
    return hsv_.v;
}

void ColorPickerModel::set_channel(HsvChannel channel, int value) {
    switch (channel) {
    case HsvChannel::Hue:
        hsv_.h = ((value % 360) + 360) % 360;
        break;
    case HsvChannel::Saturation:
        hsv_.s = std::clamp(value, 0, 100);
        break;
    case HsvChannel::Brightness:
        hsv_.v = std::clamp(value, 0, 100);
        break;
    }
    rgb_ = hsv_to_rgb(hsv_);
}

void ColorPickerModel::step(HsvChannel channel, int direction) {
    const int d = direction < 0 ? -1 : 1;
    const int by = channel == HsvChannel::Hue ? kHueStep : kPercentStep;
    set_channel(channel, this->channel(channel) + d * by);
}

bool HexKeypad::push(char c) {
    if (digits_.size() >= kDigits || !std::isxdigit(static_cast<unsigned char>(c))) {
        return false;
    }
    digits_.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return true;
}

void HexKeypad::backspace() {
    if (!digits_.empty()) {
        digits_.pop_back();
    }
}

std::optional<uint32_t> HexKeypad::result() const {
    if (!complete()) {
        return std::nullopt;
    }
    uint32_t v = 0;
    for (char c : digits_) {
        const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : c - 'A' + 10;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    return v;
}

} // namespace helix
