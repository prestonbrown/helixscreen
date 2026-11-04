// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace helix {
class Config;
}

/**
 * @file glass_gate.h
 * @brief Whether this board may show the glass appearance at all
 *
 * A board fails outright below 24 bpp (radial gradients band into rings), on a tier without
 * full style effects, on a software-rotated display, and with a draw backend that renders only
 * two gradient stops. Any other board draws a glass frame
 * offscreen once per board fingerprint and app version, and passes when it fits the budget.
 * The result is stored at kGlassProbeStorePath, beside the screensaver levels.
 */

namespace helix::ui {

/// One probe's result, and the board and app version it was measured on.
struct GlassProbeRecord {
    bool passed = false;
    double frame_ms = 0;
    std::string version;
    std::string board;

    bool operator==(const GlassProbeRecord& o) const {
        return passed == o.passed && frame_ms == o.frame_ms && version == o.version &&
               board == o.board;
    }
};

/// Everything the gate decides from.
struct GlassGateFacts {
    int color_depth = 0; ///< What the display renders and scans out at, not LV_COLOR_DEPTH
    bool style_effects_allowed = false;
    /// The draw backend renders a gradient's every stop. NanoVG's reads only the first two.
    bool multi_stop_gradients = true;
    bool software_rotated = false;
    std::optional<bool> env_force; ///< HELIX_GLASS
    std::optional<GlassProbeRecord> stored;
    std::string version;
    std::string board;
};

enum class GlassGateVerdict { Pass, Fail, NeedsProbe };

/// HELIX_GLASS wins; then the hard fails; then a record stored for this version and board;
/// otherwise the board must be measured.
GlassGateVerdict decide_glass_gate(const GlassGateFacts& f);

/// HELIX_GLASS: "1" forces a pass, "0" a fail, anything else (or unset) is no override.
std::optional<bool> glass_env_force();

constexpr const char* kGlassProbeStorePath = "/display/glass_probe";

/// The record at `node`, or nullopt unless it is an object with a boolean "passed", a number
/// "frame_ms" and string "version" and "board".
std::optional<GlassProbeRecord> parse_glass_probe(const nlohmann::json* node);
nlohmann::json glass_probe_json(const GlassProbeRecord& r);

std::optional<GlassProbeRecord> load_glass_probe(const helix::Config& c);

/// Writes the record and saves the config file.
void save_glass_probe(helix::Config& c, const GlassProbeRecord& r);

/// A frame may take a third of a 60 Hz refresh.
constexpr double kGlassFrameBudgetMs = 6.0;

/// Draws a representative glass frame at `width` x `height` into an offscreen canvas in the
/// display's color format (one full-size Surface and twelve Raised buttons, each with its glass
/// drop shadow), five times, and returns the median ms.
/// Nothing reaches the display. Main thread.
double measure_glass_frame_ms(int32_t width, int32_t height);

// ============================================================================
// The running gate: availability, the effective state, live switching
// ============================================================================

using GlassMeasureFn = std::function<double(int32_t width, int32_t height)>;

/// How long after boot the probe waits, so it never competes with startup work.
constexpr uint32_t kGlassProbeDelayMs = 10000;

/// The facts for this run: the display's depth, the draw backend, this board's tier, display
/// rotation, HELIX_GLASS, the stored record, and this version and board fingerprint. Main thread.
GlassGateFacts glass_gate_facts_now();

/// Decides the gate once, after boot: Pass or Fail sets `glass_available` at once; NeedsProbe
/// measures kGlassProbeDelayMs later, stores the record and then sets it. From then on glass is
/// effective while `glass_available` and `settings_glass_effects` are both 1. Main thread.
void glass_gate_start();

/// glass_gate_start() with the facts and the measurement supplied.
void glass_gate_start(const GlassGateFacts& facts, GlassMeasureFn measure);

/// Undoes glass_gate_start(): stops observing, cancels a pending probe, and goes flat.
void glass_gate_stop();

/// On: the theme styles take glass and every widget on every screen and layer is attached.
/// Off: the reverse. Main thread.
void glass_set_effective(bool on);

bool glass_effective();

/// The theme apply hook: attaches a widget being created while glass is effective.
void glass_on_object_created(lv_obj_t* obj);

} // namespace helix::ui
