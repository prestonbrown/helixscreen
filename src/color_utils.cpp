// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix {

void rgb_to_hsl(uint32_t rgb, float& h, float& s, float& l) {
    float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    float b = static_cast<float>(rgb & 0xFF) / 255.0f;

    float cmin = std::min({r, g, b});
    float cmax = std::max({r, g, b});
    float delta = cmax - cmin;

    // Lightness
    l = (cmax + cmin) / 2.0f;

    // Saturation and Hue
    if (delta < 0.00001f) {
        // Achromatic (gray)
        h = 0.0f;
        s = 0.0f;
    } else {
        // Saturation depends on lightness
        if (l < 0.5f) {
            s = delta / (cmax + cmin);
        } else {
            s = delta / (2.0f - cmax - cmin);
        }

        // Hue calculation
        if (std::abs(cmax - r) < 0.00001f) {
            float segment = (g - b) / delta;
            float shift = (segment < 0) ? 6.0f : 0.0f;
            h = segment + shift;
        } else if (std::abs(cmax - g) < 0.00001f) {
            h = (b - r) / delta + 2.0f;
        } else {
            h = (r - g) / delta + 4.0f;
        }

        h /= 6.0f;
    }

    // Convert to degrees and percentages
    h *= 360.0f;
    s *= 100.0f;
    l *= 100.0f;
}

std::string describe_color(uint32_t rgb) {
    float h, s, l;
    rgb_to_hsl(rgb, h, s, l);

    // Determine hue name based on angle
    const char* hue_name = "Unknown";
    if (h < 15.0f || h >= 345.0f) {
        hue_name = "Red";
    } else if (h < 45.0f) {
        hue_name = "Orange";
    } else if (h < 70.0f) {
        hue_name = "Yellow";
    } else if (h < 80.0f) {
        hue_name = "Lime";
    } else if (h < 163.0f) {
        hue_name = "Green";
    } else if (h < 193.0f) {
        hue_name = "Teal";
    } else if (h < 210.0f) {
        hue_name = "Cyan";
    } else if (h <= 240.0f) {
        hue_name = "Blue";
    } else if (h < 260.0f) {
        hue_name = "Indigo";
    } else if (h < 280.0f) {
        hue_name = "Purple";
    } else if (h < 320.0f) {
        hue_name = "Magenta";
    } else {
        hue_name = "Pink";
    }

    // Determine saturation modifier
    const char* saturation_name = "";
    if (s < 10.0f) {
        saturation_name = "Grayish";
    } else if (s < 30.0f) {
        saturation_name = "Muted";
    } else if (s < 60.0f) {
        saturation_name = "Soft";
    } else if (s >= 80.0f) {
        saturation_name = "Vibrant";
    }

    // Determine lightness modifier
    const char* lightness_name = "";
    if (l < 15.0f) {
        lightness_name = "Dark";
    } else if (l < 40.0f) {
        lightness_name = "Deep";
    } else if (l < 55.0f) {
        lightness_name = "Medium";
    } else if (l >= 75.0f && l < 90.0f) {
        lightness_name = "Light";
    } else if (l >= 90.0f) {
        lightness_name = "Pale";
    }

    // Build final color name
    std::string color_name;

    // Special cases: white, black, gray
    if (s < 5.0f && l > 95.0f) {
        return "White";
    } else if (s < 5.0f && l < 5.0f) {
        return "Black";
    } else if (s < 10.0f && l > 5.0f && l < 95.0f) {
        // Grayscale
        if (lightness_name[0] != '\0') {
            color_name = lightness_name;
            color_name += " Gray";
        } else {
            color_name = "Gray";
        }
        return color_name;
    }

    // Chromatic color: combine modifiers
    if (lightness_name[0] != '\0') {
        color_name = lightness_name;
    }

    if (s > 10.0f && saturation_name[0] != '\0') {
        if (!color_name.empty()) {
            color_name += " ";
        }
        color_name += saturation_name;
    }

    if (!color_name.empty()) {
        color_name += " ";
    }
    color_name += hue_name;

    return color_name;
}

bool parse_hex_color(const char* input, uint32_t& out_rgb) {
    if (input == nullptr) {
        return false;
    }

    // Skip leading whitespace
    while (*input && std::isspace(static_cast<unsigned char>(*input))) {
        ++input;
    }

    // Handle 0x/0X prefix
    if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        input += 2;
    }
    // Handle # prefix
    else if (*input == '#') {
        ++input;
    }

    // Count hex digits
    size_t hex_len = 0;
    const char* hex_start = input;
    while (std::isxdigit(static_cast<unsigned char>(input[hex_len]))) {
        ++hex_len;
    }

    // Must be exactly 3, 6, or 8 hex digits (8 = #RRGGBBAA)
    if (hex_len != 3 && hex_len != 6 && hex_len != 8) {
        return false;
    }

    // Skip trailing whitespace
    const char* after_hex = input + hex_len;
    while (*after_hex && std::isspace(static_cast<unsigned char>(*after_hex))) {
        ++after_hex;
    }

    // Must be end of string
    if (*after_hex != '\0') {
        return false;
    }

    // Parse the hex value
    uint32_t value = 0;
    for (size_t i = 0; i < hex_len; ++i) {
        char c = hex_start[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else {
            digit = 10 + (c - 'A');
        }
        value = (value << 4) | digit;
    }

    // Expand 3-digit shorthand (RGB → RRGGBB)
    if (hex_len == 3) {
        uint32_t r = (value >> 8) & 0xF;
        uint32_t g = (value >> 4) & 0xF;
        uint32_t b = value & 0xF;
        value = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    } else if (hex_len == 8) {
        // #RRGGBBAA. Alpha is meaningless to every consumer, so SHIFT it off.
        // Masking (value & 0xFFFFFF) would keep GGBBAA and paint the wrong hue:
        // #800080FF would become 0x0080FF, bright azure instead of purple.
        value >>= 8;
    }

    out_rgb = value;
    return true;
}

std::optional<uint32_t> parse_hex_color(const std::string& hex_str) {
    uint32_t rgb = 0;
    if (parse_hex_color(hex_str.c_str(), rgb)) {
        return rgb;
    }
    return std::nullopt;
}

std::string color_to_hex_string(uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", rgb & 0xFFFFFF);
    return std::string(buf);
}

} // namespace helix
