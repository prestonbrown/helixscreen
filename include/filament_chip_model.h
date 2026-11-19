// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a FILAMENTS-card chip says, decided without LVGL so it is unit-testable: the outline
// severity of one tool's pairing, the slot label the chip prints, the order and reasons the
// slot dropdown lists, the details row's material text, and the loaded-filament comparison
// for a printer with no filament system. Every match question is answered by FilamentMapper;
// nothing here re-implements one.

#include "filament_mapper.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// A chip's outline. Block: no lane, an empty lane, or a material that differs. Warn: the
/// colour differs and the file's colour is known. Ok: anything else.
enum class ChipSeverity { Ok, Warn, Block };

/// The severity of pairing @p tool with @p resolved, the lane it prints from
/// (FilamentMapper::resolve_printing_slot()); nullptr when it prints from none, an Auto
/// whose routing names no lane included, which blocks as the pre-flight gate does.
[[nodiscard]] ChipSeverity chip_severity(const GcodeToolInfo& tool, const AvailableSlot* resolved);

/// The slot label a chip prints for @p slot: its 1-based position in its unit ("3") when
/// @p all_slots spans one unit, otherwise "<unit>·<position>" ("2·1"), where <unit> is the
/// 1-based position of (backend_index, unit_index) in the order the slots first appear in
/// @p all_slots. Uses display_numbering.h's lane_number() and kUnitSeparator.
[[nodiscard]] std::string chip_slot_label(const AvailableSlot& slot,
                                          const std::vector<AvailableSlot>& all_slots);

/// The unit number chip_slot_label() prints for @p slot: the 1-based position of
/// (backend_index, unit_index) in the order the units first appear in @p all_slots, or 0 when
/// @p all_slots spans one unit.
[[nodiscard]] int chip_unit_number(const AvailableSlot& slot,
                                   const std::vector<AvailableSlot>& all_slots);

/// The dropdown row naming @p slot, in agreement with its chip_slot_label(): "Lane 3" across one
/// unit; "Turtle 2 · Lane 1" (the unit's own name) across several units of one backend; and
/// "2 · Lane 1" (chip_unit_number()) across several backends, whose unit names repeat or are
/// empty.
[[nodiscard]] std::string slot_row_label(const AvailableSlot& slot,
                                         const std::vector<AvailableSlot>& all_slots);

/// Why a slot sits where it does in the dropdown, best first.
enum class SlotFit { Exact, ColorDiffers, MaterialDiffers, Empty };

struct RankedSlot {
    std::size_t slot_pos = 0; ///< index into the slots vector ranked
    SlotFit fit = SlotFit::Exact;
    std::vector<int> also_tools; ///< G-code tools (other than this one) mapped to the slot
};

/// Every slot in @p slots ranked for @p tool: Exact, then ColorDiffers, then MaterialDiffers,
/// then Empty, each tier keeping @p slots' order. A colour that is unknown on the file side
/// never demotes a slot (it ranks as Exact when the material matches). @p mappings and
/// @p tools are parallel; the entry for @p tool itself is left out of also_tools.
[[nodiscard]] std::vector<RankedSlot> rank_slots_for_tool(const GcodeToolInfo& tool,
                                                          const std::vector<AvailableSlot>& slots,
                                                          const std::vector<GcodeToolInfo>& tools,
                                                          const std::vector<ToolMapping>& mappings);

/// The dropdown reason for @p ranked: "exact match", "color differs", "material differs",
/// "empty", plus ", also T2" (tool_label of each also_tool). Translated.
[[nodiscard]] std::string slot_fit_text(const RankedSlot& ranked);

/// The details row's material: the materials of the used tools in tool order, deduplicated,
/// joined by ", ". With @p used_tools nullopt (not known yet), every non-empty material in
/// @p materials, deduplicated. Empty when there is none.
[[nodiscard]] std::string used_materials_text(const std::vector<std::string>& materials,
                                              const std::optional<std::set<int>>& used_tools);

/// Per-tool materials from the slicer's ';'-separated filament_type metadata
/// ("PLA;PLA;PETG" -> PLA, PLA, PETG), each trimmed. Positions are kept, so an
/// empty entry stays an empty string at its tool's index.
[[nodiscard]] std::vector<std::string> split_filament_types(std::string_view filament_type);

/// A ';'-separated filament_type as people read it: every named material once,
/// in tool order ("ABS;ABS;PETG;" -> "ABS, PETG"). Empty when it names none.
[[nodiscard]] std::string filament_type_text(std::string_view filament_type);

/// What is loaded on a printer with no filament system.
struct LoadedFilament {
    bool known = false; ///< false: nothing says what is in the extruder
    std::string material;
    uint32_t color_rgb = 0;
    bool color_known = false;
};

/// A no-filament-system chip's body: the first used tool is what is Loaded; every later one
/// is a Swap the print asks for mid-job.
enum class LoadedRole { Loaded, Swap };

/// Severity of @p tool against what is loaded. Unknown loaded filament is Ok (grey, "?"). A
/// Swap tool is always Ok: nothing is loaded for it yet.
[[nodiscard]] ChipSeverity loaded_severity(const GcodeToolInfo& tool, const LoadedFilament& loaded,
                                           LoadedRole role);

} // namespace helix
