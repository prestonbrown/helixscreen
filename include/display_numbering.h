// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::ui {

/// U+00B7 MIDDLE DOT, the separator between a unit name and a position inside
/// it. Shared so every "Unit · N" label spells it the same way.
inline constexpr const char* kUnitSeparator = "\xc2\xb7";

/**
 * @brief The word a backend uses for one physical filament position.
 *
 * An enum rather than a string so a caller cannot compare the wrong spelling
 * and silently fall through to the default.
 */
enum class LaneNoun : uint8_t {
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
 * @brief How a printer numbers its tools where a number stands alone.
 *
 * Most machines are labeled from 1 ("Toolhead 1" on a U1, heads 1-4 on a
 * Creator 5), while a klipper-toolchanger rig is labeled with the G-code names
 * its owner wrote (`[tool T0]`, "T0" on the dock plate). The G-code name itself
 * (ToolLabelForm::Code, tool_label()) is the same in both styles.
 */
enum class ToolNumberStyle : uint8_t {
    Physical, ///< 1-based position: "Tool 1", badge "1". The default.
    Gcode,    ///< The G-code name the machine is labeled with: "T0", badge "T0".
};

/// The persisted per-printer override. Values are the stored ints, so never reorder.
enum class ToolNumberStyleSetting : uint8_t { Auto = 0, Physical = 1, Gcode = 2 };

/**
 * @brief The number of the tool at @p index in @p style: "1", or "T0".
 *
 * Physical is the 1-based position. Gcode is the tool's G-code identity: its
 * name when that has the generated shape (a `[tool T5]` at slot 2 is "T5"),
 * else tool_label(index).
 *
 * @return empty when @p index is negative
 */
std::string tool_number_text(ToolNumberStyle style, std::string_view name, int index);

/**
 * @brief A tool's badge: its configured name ("Left") verbatim, else tool_number_text().
 * @return empty when @p index is negative
 */
std::string tool_badge_label(ToolNumberStyle style, std::string_view name, int index);

/**
 * @brief What the tool at @p index is called on the machine.
 *
 * A configured name ("Left") is the machine's physical label and is returned
 * verbatim. A generated "T<n>" name gets the physical position ("Tool 1") in
 * Physical style and the G-code name ("T0") in Gcode style.
 *
 * @param noun what the printer calls a tool (active_tool_noun()): "Toolhead 1" on a U1
 * @return empty when @p index is negative
 */
std::string tool_name_label(LaneNoun noun, ToolNumberStyle style, std::string_view name, int index);

/**
 * @brief How much of a tool's identity a label spells.
 *
 * Klipper and every slicer count tools from T0, while the machine is labeled
 * from 1. A generated name gets both when there is room, so neither number
 * reads as the other's mistake; a narrow box gets the G-code name alone, the
 * one a user can type.
 */
enum class ToolLabelForm {
    Full, ///< "Tool 1 (T0)": the physical label with the G-code name it answers to
    Code, ///< "T0": the G-code name alone
};

/// Every ToolLabelForm, most informative first: the order a caller tries them in.
inline constexpr ToolLabelForm kToolLabelForms[] = {ToolLabelForm::Full, ToolLabelForm::Code};

/**
 * @brief The label for the tool at @p index in @p form.
 *
 * Full is tool_name_label() beside the G-code name: "Tool 1 (T0)" for a
 * generated name, "Left (T0)" for a configured one. Where the two halves are
 * the same text (a generated name in Gcode style) Full is the G-code name once,
 * "T0". Code is the G-code name alone ("T0"), configured name or not.
 *
 * @param noun what the printer calls a tool (active_tool_noun()): "Toolhead 1 (T0)" on a U1
 * @return empty when @p index is negative
 */
std::string tool_label_in_form(LaneNoun noun, ToolNumberStyle style, std::string_view name,
                               int index, ToolLabelForm form);

/**
 * @brief A tool that draws from a lane into the one head every tool shares.
 *
 * Behind a hub (a CFS, an ACE, a Box Turtle feeding one extruder) a tool is
 * not a thing on the machine; it is the slicer's name for a lane. Full names
 * the lane in the backend's own noun beside the G-code name, "Slot 1 (T0)",
 * so a remapped tool reads "Slot 3 (T0)". Code is the G-code name alone.
 *
 * @param lane_noun what the backend calls a position (active_lane_noun())
 * @param slot      global index of the lane the tool feeds
 * @param tool      0-based G-code tool
 * @return empty when @p slot or @p tool is negative
 */
std::string fed_lane_tool_label(LaneNoun lane_noun, int slot, int tool, ToolLabelForm form);

/**
 * @brief The most informative form @p fits accepts.
 *
 * Callers measure differently (a pill grid, a list row), so each supplies its
 * own fit test and this owns the preference order. Every caller labels all its
 * tools in one form, so a row of pills never mixes "Tool 1 (T0)" with "T1".
 *
 * @return the first form in kToolLabelForms that @p fits accepts, else Code:
 *         a label has to say something, and Code is the shortest
 */
ToolLabelForm pick_tool_label_form(const std::function<bool(ToolLabelForm)>& fits);

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

/**
 * @brief Whether per-lane tool badges tell the user anything the lane numbers do not.
 *
 * When every tool feeds the lane of its own index (T0 into lane 1, T1 into
 * lane 2, ...) a tool badge only repeats the lane number beside it. Badges
 * earn their place once a tool feeds a different lane, or names no lane this
 * system has.
 *
 * @param tool_to_slot tool_to_slot[tool] = global slot index, negative = unmapped
 * @param slot_count   number of slots in the system
 * @return false only for a non-empty identity map; an empty map proves nothing
 */
bool tool_badges_add_information(const std::vector<int>& tool_to_slot, int slot_count);

/// @brief The translated word for @p noun ("Slot", "Lane", "Gate", "Tool").
std::string noun_text(LaneNoun noun);

/**
 * @brief The Endless Spool field's name for @p noun: "Backup Slot", "Backup Gate".
 *
 * A whole translated phrase per noun, not "Backup " + noun_text(): the
 * qualifier goes after the noun in most locales and is often compounded into it.
 */
std::string backup_lane_text(LaneNoun noun);

/**
 * @brief A physical position: "Slot 1", "Lane 2", "Tool 4".
 * @return empty when @p index is negative
 */
std::string lane_label(LaneNoun noun, int index);

/**
 * @brief The refusal a Clear Spool shows when the lane feeds the active print.
 *
 * One sentence for both surfaces that can raise it (the context menu's
 * disabled-button hint and the dispatch's toast), so the wording cannot fork
 * between the affordance and the refusal. Names the position in the backend's
 * own word, like every other sentence that points a user at a lane.
 */
std::string clear_spool_blocked_hint(LaneNoun noun, int index);

/**
 * @brief A position label scoped inside its named unit: "<unit> · <body>".
 *
 * The one composition behind every unit-qualified position ("Turtle 1 ·
 * Slot 2", a fed lane's upstream caption "ACE 2 · 3"), so the separator and
 * spacing cannot drift between them. An empty unit degrades to the body
 * alone, the single-unit form.
 */
std::string unit_scoped_label(std::string_view unit_display_name, std::string_view body);

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

/**
 * @brief The style a setting and a backend's answer settle on.
 *
 * Auto takes the backend's @p backend_hint; Physical and Gcode force themselves.
 */
ToolNumberStyle resolve_tool_number_style(ToolNumberStyleSetting setting,
                                          ToolNumberStyle backend_hint);

/**
 * @brief The tool number style of the currently connected printer.
 *
 * The per-printer setting resolved against the active AMS backend's
 * tool_number_style(); Physical with no backend.
 */
ToolNumberStyle active_tool_number_style();

/// @brief tool_badge_label() under active_tool_number_style(): the nozzle badge text.
std::string active_tool_badge_label(std::string_view name, int index);

/**
 * @brief tool_label_in_form() under active_tool_noun() and active_tool_number_style():
 *        what every surface naming a tool of the connected printer shows
 *        ("Toolhead 3 (T2)" on a U1).
 */
std::string active_tool_label(std::string_view name, int index, ToolLabelForm form);

} // namespace helix::ui
