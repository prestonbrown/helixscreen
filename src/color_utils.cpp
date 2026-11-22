// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "color_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

namespace {

// describe_color's whole vocabulary. is_derived_color_name() reads the same
// tables, so a name describe_color can give always reads as derived.
enum Hue {
    kRed,
    kOrange,
    kYellow,
    kLime,
    kGreen,
    kTeal,
    kCyan,
    kBlue,
    kIndigo,
    kPurple,
    kMagenta,
    kPink
};
constexpr std::array<std::string_view, 12> kHueNames = {"Red",    "Orange", "Yellow",  "Lime",
                                                        "Green",  "Teal",   "Cyan",    "Blue",
                                                        "Indigo", "Purple", "Magenta", "Pink"};
enum Saturation { kGrayish, kMuted, kSoft, kVibrant };
constexpr std::array<std::string_view, 4> kSaturationNames = {"Grayish", "Muted", "Soft",
                                                              "Vibrant"};
enum Lightness { kDark, kDeep, kMedium, kLight, kPale };
constexpr std::array<std::string_view, 5> kLightnessNames = {"Dark", "Deep", "Medium", "Light",
                                                             "Pale"};
constexpr std::string_view kWhite = "White";
constexpr std::string_view kBlack = "Black";
constexpr std::string_view kDeepBlack = "Deep Black";
constexpr std::string_view kGray = "Gray";

template <size_t N>
bool names_one_of(std::string_view word, const std::array<std::string_view, N>& names) {
    return std::find(names.begin(), names.end(), word) != names.end();
}

} // namespace

std::string describe_color(uint32_t rgb) {
    float h, s, l;
    rgb_to_hsl(rgb, h, s, l);

    // Determine hue name based on angle
    Hue hue = kPink;
    if (h < 15.0f || h >= 345.0f) {
        hue = kRed;
    } else if (h < 45.0f) {
        hue = kOrange;
    } else if (h < 70.0f) {
        hue = kYellow;
    } else if (h < 80.0f) {
        hue = kLime;
    } else if (h < 163.0f) {
        hue = kGreen;
    } else if (h < 193.0f) {
        hue = kTeal;
    } else if (h < 210.0f) {
        hue = kCyan;
    } else if (h <= 240.0f) {
        hue = kBlue;
    } else if (h < 260.0f) {
        hue = kIndigo;
    } else if (h < 280.0f) {
        hue = kPurple;
    } else if (h < 320.0f) {
        hue = kMagenta;
    }

    // Determine saturation modifier
    std::string_view saturation_name;
    if (s < 10.0f) {
        saturation_name = kSaturationNames[kGrayish];
    } else if (s < 30.0f) {
        saturation_name = kSaturationNames[kMuted];
    } else if (s < 60.0f) {
        saturation_name = kSaturationNames[kSoft];
    } else if (s >= 80.0f) {
        saturation_name = kSaturationNames[kVibrant];
    }

    // Determine lightness modifier
    std::string_view lightness_name;
    if (l < 15.0f) {
        lightness_name = kLightnessNames[kDark];
    } else if (l < 40.0f) {
        lightness_name = kLightnessNames[kDeep];
    } else if (l < 55.0f) {
        lightness_name = kLightnessNames[kMedium];
    } else if (l >= 75.0f && l < 90.0f) {
        lightness_name = kLightnessNames[kLight];
    } else if (l >= 90.0f) {
        lightness_name = kLightnessNames[kPale];
    }

    // Build final color name
    std::string color_name;

    // Special cases: white, black, gray
    // Below ~8% lightness no hue is perceptible at any saturation; up to ~18% a
    // near-neutral still reads as black, and below ~4.5% a neutral is "Deep Black".
    // Near-white mirrors the black band.
    if ((l > 98.0f) || (s < 25.0f && l >= 94.0f)) {
        return std::string(kWhite);
    } else if (s < 25.0f && l < 4.5f) {
        return std::string(kDeepBlack);
    } else if ((l < 8.0f) || (s < 25.0f && l < 18.0f)) {
        return std::string(kBlack);
    } else if (s < 10.0f) {
        // Grayscale
        if (!lightness_name.empty()) {
            color_name = lightness_name;
            color_name += " ";
        }
        color_name += kGray;
        return color_name;
    }

    // Chromatic color: combine modifiers
    if (!lightness_name.empty()) {
        color_name = lightness_name;
    }

    if (s > 10.0f && !saturation_name.empty()) {
        if (!color_name.empty()) {
            color_name += " ";
        }
        color_name += saturation_name;
    }

    if (!color_name.empty()) {
        color_name += " ";
    }
    color_name += kHueNames[hue];

    return color_name;
}

bool is_derived_color_name(std::string_view name) {
    if (name == kWhite || name == kBlack || name == kDeepBlack)
        return true;
    // The rest is "[Lightness] [Saturation] Hue" or "[Lightness] Gray", one
    // space between words.
    std::vector<std::string_view> words;
    for (size_t start = 0;;) {
        const size_t space = name.find(' ', start);
        words.push_back(
            name.substr(start, space == std::string_view::npos ? space : space - start));
        if (space == std::string_view::npos)
            break;
        start = space + 1;
    }
    if (words.size() > 3)
        return false;
    size_t i = 0;
    if (words.size() > 1 && names_one_of(words[i], kLightnessNames))
        ++i;
    if (words.size() - i == 1 && words[i] == kGray)
        return true;
    if (words.size() - i > 1 && names_one_of(words[i], kSaturationNames))
        ++i;
    return words.size() - i == 1 && names_one_of(words[i], kHueNames);
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

// Special preset names that don't follow standard color naming
static const struct {
    uint32_t hex;
    const char* name;
} SPECIAL_COLOR_NAMES[] = {
    {0xD4AF37, "Gold"},   {0xCD7F32, "Bronze"},   {0x8B4513, "Wood"},         {0xE8E8FF, "Clear"},
    {0xC0C0C0, "Silver"}, {0xE0D5C7, "Marble"},   {0xFF7043, "Coral"},        {0x1A237E, "Navy"},
    {0xBCAAA4, "Taupe"},  {0x7A1E2C, "Burgundy"}, {0x1B5E20, "Forest Green"},
};

std::string get_color_name_from_hex(uint32_t rgb) {
    // Check for special preset names first
    for (const auto& entry : SPECIAL_COLOR_NAMES) {
        if (entry.hex == rgb) {
            return entry.name;
        }
    }
    // Use algorithmic color description
    return helix::describe_color(rgb);
}

} // namespace helix
