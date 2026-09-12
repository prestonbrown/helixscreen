// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace helix::ui {

/**
 * @brief The word a backend uses for one physical filament position.
 *
 * An enum rather than a string so a caller cannot compare the wrong spelling
 * and silently fall through to the default.
 */
enum class LaneNoun {
    Slot,     ///< Bambu-style AMS, K2 CFS, ACE, QIDI Box, AD5X IFS
    Lane,     ///< AFC
    Gate,     ///< Happy Hare
    Tool,     ///< Tool changer: each position carries its own toolhead
    Feeder,   ///< Snapmaker U1: where filament enters the AMS ("Feeder 1".."Feeder 4")
    Toolhead, ///< Snapmaker U1: the printing end ("Toolhead 1".."Toolhead 4") - a
              ///< distinct physical thing from Feeder, 1:1 but not the same word
};

/**
 * @brief Spell a G-code tool.
 *
 * 0-based and T-prefixed, because this is the number a user types into a
 * console and the number the slicer emitted. Never renumbered.
 *
 * @param gcode_tool 0-based tool index; negative yields an empty string
 */
std::string tool_label(int gcode_tool);

/**
 * @brief Whether @p name is the generated G-code identity, not a configured one.
 *
 * A Klipper tool carries either a name its owner chose ("Left", "Right") or the
 * "T<n>" form tool_label() produces when none was configured. Only the
 * generated form may be replaced by a 1-based display number; a chosen name is
 * what the machine is physically labeled with and is shown verbatim.
 */
bool is_generated_tool_name(std::string_view name);

/**
 * @brief Convert a storage index to the number a user sees.
 *
 * The only + 1 in the codebase. Every display path routes through this so a
 * missing conversion is a call that is not here, rather than one correct
 * expression among many hand-written ones.
 *
 * @param index 0-based storage index
 * @return 1-based display number, or -1 when @p index is negative
 */
int lane_number(int index);

/// @brief lane_number() as text, or empty when @p index is negative.
std::string lane_number_text(int index);

/// @brief The translated word for @p noun ("Slot", "Lane", "Gate", "Tool").
std::string noun_text(LaneNoun noun);

/**
 * @brief A physical position: "Slot 1", "Lane 2", "Tool 4".
 * @return empty when @p index is negative
 */
std::string lane_label(LaneNoun noun, int index);

/**
 * @brief A physical position inside a named unit: "Turtle 1 · Slot 2".
 *
 * @p unit_display_name is user-configured (an AFC unit name) and is not
 * translated. An empty unit degrades to the single-unit form.
 * @return empty when @p index is negative
 */
std::string lane_label(LaneNoun noun, std::string_view unit_display_name, int index);

/**
 * @brief A span of physical positions: "Slots 1-4", "Lanes 1-8".
 *
 * The plural noun with a 1-based inclusive range, ordered low to high. Every
 * locale spells the plural as one fixed word, so the header never agrees with
 * either number.
 *
 * A one-position span is spelled as one position ("Slot 3"), not as a range
 * against itself.
 *
 * @return empty when either index is negative or when @p last_index precedes
 *         @p first_index, since neither describes a span
 */
std::string lane_range_label(LaneNoun noun, int first_index, int last_index);

/**
 * @brief How many positions a unit has: "4 slots", "8 lanes".
 *
 * One fixed translated form per noun, never a count spliced into a plural
 * word. Russian numerals take three forms (1 слот / 2-4 слота / 5+ слотов) and
 * a card header is not the place to decide between them, so each locale writes
 * the one form it wants to read there.
 *
 * @param count number of positions, shown verbatim
 */
std::string lane_count_label(LaneNoun noun, int count);

/**
 * @brief The noun for the printer currently connected.
 *
 * Resolved in one place so no widget derives it for itself. The nozzle badge
 * exists on machines with no AMS backend at all, which is why this falls back
 * rather than requiring a backend.
 *
 * @return the active AMS backend's lane_noun(), else LaneNoun::Slot
 */
LaneNoun active_lane_noun();

/**
 * @brief The noun for the printing end of the currently connected printer.
 *
 * Distinct from active_lane_noun(): most backends use the same noun for where
 * filament enters and where it prints, but a backend whose hardware uses two
 * different words (Snapmaker U1: "Feeder" vs "Toolhead") needs both answered
 * separately. Anything labeling a tool/nozzle/toolhead position - not a
 * filament lane - calls this one.
 *
 * @return the active AMS backend's tool_noun(), else LaneNoun::Tool
 */
LaneNoun active_tool_noun();

} // namespace helix::ui
