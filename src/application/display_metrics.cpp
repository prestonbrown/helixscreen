// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "display_metrics.h"

#include "core/text_io.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

/// Panels that are permanently attached to a board, so the board alone identifies
/// them. Diagonals are measured, not taken from spec sheets - the fleet survey
/// found reported physical sizes wrong or absent on 6 of 8 devices, including a
/// Raspberry Pi whose driver hands back the official 7" touchscreen's exact
/// active area for a ~115mm panel.
///
/// Deliberately absent: Generic (Pi, x86 and ESP32 drive a user-chosen display, so
/// a panel size here would be a guess imposed on every user of that build), the
/// AD5X and Creator 5 line (no hardware to measure) and the Snapmaker U1 (its
/// reported size is bogus and the true diagonal is not yet measured).
struct PanelEntry {
    board::Board board;
    PanelSpec spec;
};

constexpr PanelEntry kKnownPanels[] = {
    // FlashForge Adventurer 5M - 221.5 DPI. Reports 108x64mm (189 DPI), ~17% low.
    {board::Board::Ad5m, {800, 480, 107.0}},
    // Elegoo Centauri Carbon - 131.0 DPI. simpledrm synthesizes 96 DPI here.
    {board::Board::Cc1, {480, 272, 107.0}},
    // Creality K1/K1C/K1 Max - 218.2 DPI. Reports 0x0 over the ioctl, so this table
    // is the only source it has. Panel is physically identical to the K2 Plus. All
    // three mount it the same way and report no kernel panel_orientation. Every
    // other mips box that resolves to K1 is told apart by its framebuffer geometry
    // (board_mount_rotation). Geometry and mount must match platforms.json "k1".
    {board::Board::K1, {480, 800, 108.6, 270}},
    // Creality K2 Plus - 218.2 DPI. One of only two devices that measured honestly.
    {board::Board::K2, {480, 800, 108.6}},
};

/// Sizes actually generated for each font family, ascending. Mirrors the
/// SIZES_* lists in scripts/regen_text_fonts.sh and the mdi rungs in
/// scripts/regen_mdi_fonts.sh. A face named here but not linked on this
/// platform is unreachable in practice: only the XXLarge tier carries anything
/// above 40 (26 for light), and no printer build declares that tier, so their
/// scale never leaves 1.0 and no remap is ever requested.
struct FontFamily {
    const char* prefix;
    const int sizes[16];
    int count;
};

constexpr FontFamily kFontFamilies[] = {
    {"noto_sans_", {8, 10, 11, 12, 14, 16, 18, 20, 24, 26, 28, 32, 40, 48, 64}, 15},
    {"noto_sans_light_", {10, 11, 12, 14, 16, 18, 20, 26, 32, 40}, 10},
    {"noto_sans_bold_", {14, 16, 18, 20, 24, 28, 32, 40, 48, 64}, 10},
    {"source_code_pro_", {8, 10, 12, 14, 16, 18, 20, 24}, 8},
    {"mdi_icons_", {14, 16, 24, 32, 48, 64, 80, 96, 128}, 9},
};

/// Process-wide UI scale. Set once during display init; screens do not resize
/// or change DPI at runtime (see project notes on fixed-geometry screens).
double g_active_scale = 1.0;
double g_auto_scale = 1.0;

} // namespace

const char* dpi_source_name(DpiSource source) {
    switch (source) {
    case DpiSource::UserOverride:
        return "user-override";
    case DpiSource::PlatformMeasured:
        return "platform-measured";
    case DpiSource::KnownPanel:
        return "known-panel";
    case DpiSource::Fallback:
        return "fallback";
    }
    return "unknown";
}

double DisplayMetrics::ui_scale_for_dpi(double dpi) {
    if (!std::isfinite(dpi) || dpi <= kScaleDeadbandDpi) {
        return 1.0;
    }
    const double scale = 1.0 + (dpi - kScaleDeadbandDpi) * kScaleSlopePerDpi;
    return std::min(scale, kMaxScale);
}

double DisplayMetrics::logical_dpi(double physical_dpi, int native_w, int native_h, int logical_w,
                                   int logical_h) {
    if (native_w <= 0 || native_h <= 0 || logical_w <= 0 || logical_h <= 0) {
        return physical_dpi;
    }
    const double native_diag =
        std::hypot(static_cast<double>(native_w), static_cast<double>(native_h));
    const double logical_diag =
        std::hypot(static_cast<double>(logical_w), static_cast<double>(logical_h));
    return physical_dpi * logical_diag / native_diag;
}

bool DisplayMetrics::dpi_is_plausible(double dpi) {
    return std::isfinite(dpi) && dpi >= kMinPlausibleDpi && dpi <= kMaxPlausibleDpi;
}

bool DisplayMetrics::dpi_is_synthesized(double dpi) {
    return std::isfinite(dpi) && std::fabs(dpi - kSynthesizedDpi) <= kSynthesizedDpiEpsilon;
}

bool DisplayMetrics::dpi_is_trustworthy(double dpi) {
    return dpi_is_plausible(dpi) && !dpi_is_synthesized(dpi);
}

std::optional<PanelSpec> DisplayMetrics::known_panel(board::Board board) {
    for (const auto& entry : kKnownPanels) {
        if (board == entry.board) {
            return entry.spec;
        }
    }
    return std::nullopt;
}

int DisplayMetrics::board_mount_rotation(board::Board board, int native_w, int native_h) {
    const auto panel = known_panel(board);
    if (!panel || panel->width_px != native_w || panel->height_px != native_h) {
        return -1;
    }
    return panel->upright_rotation;
}

double DisplayMetrics::dpi_of(const PanelSpec& panel) {
    if (panel.diagonal_mm <= 0.0) {
        return 0.0;
    }
    const double diagonal_px =
        std::hypot(static_cast<double>(panel.width_px), static_cast<double>(panel.height_px));
    return diagonal_px / (panel.diagonal_mm / 25.4);
}

ResolvedDpi DisplayMetrics::resolve_dpi(int user_override_dpi,
                                        std::optional<double> platform_measured,
                                        board::Board board) {
    // An explicit override wins outright, but a nonsense value is ignored
    // rather than obeyed — it would otherwise be indistinguishable from a
    // deliberate choice and strand the user with an unusable UI.
    if (user_override_dpi > 0) {
        const double requested = static_cast<double>(user_override_dpi);
        if (dpi_is_plausible(requested)) {
            return {requested, DpiSource::UserOverride};
        }
    }

    // Only ever populated by a platform we trust to measure honestly, and even
    // then the value has to survive the plausibility and synthesis checks.
    if (platform_measured && dpi_is_trustworthy(*platform_measured)) {
        return {*platform_measured, DpiSource::PlatformMeasured};
    }

    if (const auto panel = known_panel(board)) {
        return {dpi_of(*panel), DpiSource::KnownPanel};
    }

    return {kReferenceDpi, DpiSource::Fallback};
}

ScaleDecision DisplayMetrics::decide_scale(int user_override_dpi,
                                           std::optional<double> platform_native_dpi, int native_w,
                                           int native_h, int logical_w, int logical_h,
                                           board::Board board, int scale_setting) {
    std::optional<double> platform_dpi;
    if (platform_native_dpi) {
        platform_dpi = logical_dpi(*platform_native_dpi, native_w, native_h, logical_w, logical_h);
    }
    ScaleDecision d{};
    d.resolved = resolve_dpi(user_override_dpi, platform_dpi, board);
    d.auto_scale = ui_scale_for_dpi(d.resolved.dpi);
    d.ui_scale = scale_for_setting(scale_setting, d.auto_scale);
    d.lvgl_dpi = static_cast<int32_t>(std::lround(kReferenceDpi * d.ui_scale));
    return d;
}

int32_t DisplayMetrics::scaled_px(int32_t authored_px, double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) {
        return authored_px;
    }
    const auto scaled = static_cast<int32_t>(std::lround(authored_px * scale));
    // A positive authored size must never round away to nothing — a 1px
    // hairline or divider stays visible.
    if (authored_px > 0 && scaled < 1) {
        return 1;
    }
    return scaled;
}

std::string DisplayMetrics::scaled_font_name(const std::string& font_name, double scale) {
    if (!std::isfinite(scale) || std::fabs(scale - 1.0) < 0.001) {
        return font_name;
    }

    // Split on the LAST underscore so "noto_sans_light_26" keeps its weight:
    // the prefix is "noto_sans_light_", not "noto_sans_".
    const auto underscore = font_name.rfind('_');
    if (underscore == std::string::npos || underscore + 1 >= font_name.size()) {
        return font_name;
    }
    const std::string prefix = font_name.substr(0, underscore + 1);
    const std::string digits = font_name.substr(underscore + 1);
    if (digits.find_first_not_of("0123456789") != std::string::npos) {
        return font_name;
    }

    const int authored = text_io::parse_leading<int>(digits).value_or(0);
    const auto target = static_cast<double>(authored) * scale;

    for (const auto& family : kFontFamilies) {
        if (prefix != family.prefix) {
            continue;
        }
        // Nearest rung, not the next one down: a scaled 40 wants 63, where 64
        // is off by 1 and 48 is off by 15.
        int best = family.sizes[0];
        double best_delta = std::fabs(static_cast<double>(best) - target);
        for (int i = 1; i < family.count; ++i) {
            const double delta = std::fabs(static_cast<double>(family.sizes[i]) - target);
            if (delta < best_delta) {
                best = family.sizes[i];
                best_delta = delta;
            }
        }
        // Never hand back something smaller than authored — the scale only
        // ever grows, and a low-DPI panel must keep today's face.
        if (best <= authored) {
            return font_name;
        }
        return prefix + std::to_string(best);
    }

    return font_name;
}

double DisplayMetrics::scale_for_setting(int user_scale_percent, double auto_scale) {
    const double fallback = (std::isfinite(auto_scale) && auto_scale >= 1.0) ? auto_scale : 1.0;
    if (user_scale_percent < kMinScaleSettingPercent ||
        user_scale_percent > kMaxScaleSettingPercent) {
        return fallback;
    }
    return static_cast<double>(user_scale_percent) / 100.0;
}

void DisplayMetrics::set_auto_scale(double scale) {
    g_auto_scale = (std::isfinite(scale) && scale >= 1.0) ? scale : 1.0;
}

double DisplayMetrics::auto_scale() {
    return g_auto_scale;
}

void DisplayMetrics::set_active_scale(double scale) {
    g_active_scale = (std::isfinite(scale) && scale >= 1.0) ? scale : 1.0;
}

double DisplayMetrics::active_scale() {
    return g_active_scale;
}

} // namespace helix
