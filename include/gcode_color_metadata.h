// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helix::gcode {

/**
 * @brief Parse a slicer comment line for the per-tool filament color palette.
 *
 * Slicers (OrcaSlicer, PrusaSlicer, Bambu Studio) emit metadata comments like:
 *   ; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000
 *   ; filament_colour = "#FF0000"
 *   ; filament_colour = #800080,#63A5BB,#000000,#FFFFFF
 *   ;extruder_color = #00FF00
 *
 * Recognized keys (case-insensitive): `extruder_colour`, `extruder_color`,
 * `filament_colour`, `filament_color`.
 *
 * Entries may be separated by ';' or ',' (OrcaSlicer's joined-config form uses
 * ','; mixed separators are accepted too).
 *
 * The palette is slot-aligned: invalid or empty tokens between separators
 * become empty strings so callers can index by tool number. e.g.
 * `#A;;#B` → `{"#A", "", "#B"}`.
 *
 * @param line The full gcode comment line (with or without leading `;`).
 * @param out_palette Receives the parsed palette on success. Cleared first.
 * @return true if the line was a recognized filament-color line AND at least
 *         one valid `#RRGGBB[AA]` token was extracted; false otherwise.
 */
bool parse_filament_color_palette(std::string_view line, std::vector<std::string>& out_palette);

/**
 * @brief A standalone value cleaned into one "#RRGGBB" token, or an empty view.
 *
 * Surrounding whitespace and quotes are stripped (the same tolerance the
 * palette splitter applies per entry); a result is returned only for '#' +
 * 6 or 8 hex digits. For callers validating a value the list parser would
 * reject - a colon-separated key form, say - so only a real color token is
 * ever stored, never a raw blob.
 *
 * 8-digit RGBA is accepted and stays 8-digit here; consumers drop the alpha
 * when they convert, via helix::parse_hex_color().
 */
std::string_view clean_color_hex(std::string_view value);

/**
 * @brief The first entry that actually holds a color, or nullptr.
 *
 * A palette's slot-0 entry can be an empty placeholder (unknown slot), and
 * every consumer projecting the list onto a legacy single-color field wants
 * the color the file does state, not "" and not a silently dropped answer.
 */
inline const std::string* first_named_color(const std::vector<std::string>& palette) {
    for (const auto& entry : palette) {
        if (!entry.empty()) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * @brief One file's classified answer to "what color is this print?"
 *
 * Classification only — callers map the decision onto their own renderers
 * (2D layer renderer, 3D renderer, swatch UI) themselves. Which fields are
 * offered depends on the classify_* overload used; a field left empty means
 * the file carries no answer of that kind.
 */
struct FileColorDecision {
    /// Per-tool palette from slicer metadata. Empty = no per-tool answer.
    std::vector<std::string> palette;
    /// Single hex color ("#RRGGBB" or "#RRGGBBAA") when the file carries one.
    /// Empty = no single-color answer. Non-empty does NOT mean parseable — it
    /// can be a bare '#' — so convert with helix::parse_hex_color() and act
    /// only on success, rather than trusting has_single_color().
    std::string single_color;
    /// Tool index the file starts on; -1 when unknown.
    int initial_tool = -1;

    bool has_palette() const {
        return !palette.empty();
    }
    bool has_single_color() const {
        return !single_color.empty();
    }
};

/**
 * @brief Classify the color answer from streaming index stats.
 *
 * A palette is a per-tool answer only when it covers more than one tool — a
 * 1-entry palette is indistinguishable from a single color, and collapsing it
 * to one extrusion color would paint a print that starts on a non-T0 tool in
 * palette[0]'s color. The single-color answer prefers palette[initial_tool]
 * when the palette covers that tool with a non-empty entry, else the index's
 * leading filament_color. Exactly one of palette / single_color is offered.
 */
FileColorDecision classify_file_colors(const std::vector<std::string>& palette,
                                       const std::string& filament_color, int initial_tool_index);

/**
 * @brief Classify the color answer from a fully-parsed gcode file.
 *
 * The palette is offered whenever the slicer emitted one (any size — callers
 * layer it with the single color, which acts as the per-segment fallback for
 * tools the palette doesn't cover). single_color is filament_color_hex,
 * offered when it is long enough to hold a hex value past its prefix.
 */
FileColorDecision classify_file_colors(const std::vector<std::string>& palette,
                                       const std::string& filament_color_hex);

} // namespace helix::gcode
