// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdio>
#include <string>

/**
 * @file spool_remaining_format.h
 * @brief How every screen states what is left on a spool
 *
 * One rule for the whole app: grams and the percent of the spool when its
 * size is known ("850 g · 85%"), the percent alone where a place is too narrow
 * for both ("85%"), and grams alone only when the size is unknown. Header-only
 * and free of LVGL, so the firmware and every branch carry it as is.
 */

namespace helix {

/// The percent of a @p total_g spool that @p remaining_g is, rounded and kept
/// within 0..100; -1 when either weight is unknown (negative) or the total is 0.
[[nodiscard]] inline int spool_remaining_percent(float remaining_g, float total_g) {
    if (remaining_g < 0.0f || total_g <= 0.0f) {
        return -1;
    }
    const float pct = std::round(remaining_g / total_g * 100.0f);
    return static_cast<int>(pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct));
}

/// "850 g"; "" when the weight is unknown (negative).
[[nodiscard]] inline std::string format_spool_grams(float remaining_g) {
    if (remaining_g < 0.0f) {
        return {};
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.0f g", static_cast<double>(remaining_g));
    return buf;
}

/// "850 g · 85%"; "850 g" when the spool's size is unknown; "" when the weight is.
[[nodiscard]] inline std::string format_spool_remaining(float remaining_g, float total_g) {
    std::string text = format_spool_grams(remaining_g);
    const int pct = spool_remaining_percent(remaining_g, total_g);
    if (pct >= 0) {
        text += " \xC2\xB7 " + std::to_string(pct) + "%";
    }
    return text;
}

/// The narrow form: "85%" when the spool's size is known, else "850 g"; "" when
/// the weight is unknown.
[[nodiscard]] inline std::string format_spool_remaining_short(float remaining_g, float total_g) {
    const int pct = spool_remaining_percent(remaining_g, total_g);
    return pct >= 0 ? std::to_string(pct) + "%" : format_spool_grams(remaining_g);
}

} // namespace helix
