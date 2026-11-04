// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/text_io.h"

#include <algorithm>
#include <cstdlib>

namespace helix {

/// Parse a float tuning knob from the environment, with clamping.
///
/// Returns @p fallback when the variable is unset, empty, non-numeric, or
/// non-positive — an unusable value, not a boundary to clamp to. Any usable
/// value is clamped to [@p lo, @p hi]. The HELIX_JZ_* piezo-rig knobs
/// pioneered this shape inline (jz_pwm_sound_backend.cpp); this is the
/// shared home so the parse/clamp rules live once.
inline float env_float(const char* name, float fallback, float lo, float hi) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }
    float value = std::strtof(raw, nullptr);
    if (value <= 0.0f) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

/// The one truthiness rule for environment flags: `1`, `true`, `yes` or `on`,
/// ASCII case-insensitive and ignoring surrounding whitespace (helixscreen.env
/// values can carry a stray space). Everything else is false: nullptr, "",
/// "0", "false", and anything unrecognised.
inline bool env_truthy(const char* value) {
    if (value == nullptr) {
        return false;
    }
    const std::string v = text_io::to_lower(text_io::trim(value));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

/// env_truthy() applied to the environment variable @p name.
inline bool env_flag(const char* name) {
    return env_truthy(std::getenv(name));
}

} // namespace helix
