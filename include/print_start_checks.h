// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file print_start_checks.h
 * @brief Pure core of the print-start gate pipeline.
 *
 * Gates are pure functions of a PrintStartContext snapshot; the controller
 * gathers the snapshot (gather_print_start_context()) on every run_gates_from()
 * call so each resume re-reads live state. No LVGL objects or subjects are
 * touched here.
 */

#include "ams_types.h"
#include "filament_mapper.h"
#include "moonraker_types.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix {

/**
 * @brief How many AMS lanes does this print actually need?
 *
 * More than one means the file extrudes from more than one tool, so the lane
 * mapping is live. One (or zero) means it does not: on a bypass print the
 * filament comes from the external spool and the mapping decides nothing.
 *
 * @p tools_used comes from the G-code scan and is authoritative. The palette
 * count is only a fallback for "the scan has not run yet", because slicers emit
 * a full-profile palette (e.g. PLA;ASA-GF;ASA-GF;PLA) even for a file that
 * extrudes from a single tool — counting the palette would misread every
 * single-tool file sliced on a multi-tool profile as a lane print.
 *
 * Shared rather than reimplemented: the pre-print gate uses it to decide whether
 * to compare against the external spool instead of the lanes, and the filament
 * mapping card uses it to decide whether offering a mapping means anything. Two
 * copies of this precedence would drift into a card that offers a control the
 * gate ignores.
 *
 * @param tools_used          Tools the scan saw extrude; empty = scan not run.
 * @param filament_color_count Slicer palette size, used only when @p tools_used
 *                             is empty.
 */
[[nodiscard]] size_t print_lane_requirement(const std::set<int>& tools_used,
                                            size_t filament_color_count);

/// Severity for a gate dialog. Maps 1:1 onto ModalSeverity in the controller
/// (kept separate so this header stays LVGL-free).
enum class GateSeverity { Info, Warning, Error };

/// Everything a gate may read. Gathered fresh by the controller per pipeline
/// (re-)entry; gates never fetch singleton state themselves.
struct PrintStartContext {
    // ---- file (from PrintSelectDetailView; valid only when has_detail_view) ----
    bool has_detail_view = false;
    std::optional<FileMetadata> metadata;
    std::vector<ToolMapping> mappings;
    std::vector<GcodeToolInfo> tool_info;
    std::vector<AvailableSlot> available_slots;
    std::vector<std::string> filament_materials;
    std::set<int> tools_used;
    std::map<int, int> effective_remap;

    /// Per-tool grams from the G-code footer, slot-aligned with tool index.
    /// Empty when the slicer emitted no per-tool line - which must read as
    /// "unknown", never "zero".
    std::vector<double> tool_grams;
    size_t filament_color_count = 0; ///< filament_colors_.size() on the controller

    // ---- environment (from AmsState / FilamentSensorManager) ----
    bool ams_available = false;           ///< AmsState::is_available()
    bool ams_manages_filament = false;    ///< any backend present
    bool has_active_backend = false;      ///< AmsState::get_backend() != nullptr
    bool any_auto_unload_backend = false; ///< any backend->auto_unloads_after_print()
    bool any_bypass_active = false;       ///< AmsState::any_bypass_active()
    /// Per-backend answer to toolhead_filament_unaccounted(), indexed like
    /// AmsState backends. nullopt = backend cannot determine.
    std::vector<std::optional<bool>> toolhead_unaccounted;

    /// Per-backend answer to can_clear_unaccounted_toolhead(), indexed like
    /// toolhead_unaccounted. Chooses which remedy the warning names; an index
    /// past the end reads as "cannot", the conservative answer.
    std::vector<bool> toolhead_clearable;
    /// Lane-truth result (tool_index, slot_index) for the print's required
    /// lanes; populated only when ams_manages_filament && has_active_backend.
    std::vector<std::pair<int, int>> empty_required_lanes;
    std::optional<SlotInfo> external_spool; ///< AmsState::get_external_spool_info()

    // ---- non-AMS aggregate runout fallback ----
    bool runout_enabled = false;
    bool runout_available = false;
    bool runout_detected = false;
};

struct CheckResult {
    enum class Verdict { Pass, Warn, Block };
    Verdict verdict = Verdict::Pass;
    std::string title;         // already-lv_tr'd by the gate
    std::string body;          // already-lv_tr'd by the gate
    std::string proceed_label; // "Start Anyway" / "Start Print"; empty for Block
    GateSeverity severity = GateSeverity::Warning;
};

struct PrintStartGate {
    std::string_view name;                             // for logging
    CheckResult (*evaluate)(const PrintStartContext&); // pure fn, no captures
};

/// Per-tool material mismatch detail (ported verbatim from
/// PrintStartController::MaterialMismatchDetail).
struct MaterialMismatchDetail {
    int tool_index = 0;
    /// Same polymer, different filler: the file wants PLA-CF and the lane
    /// holds PLA, or the reverse. FilamentMapper::materials_match() passes
    /// these (one compat group), so they are found by a second pass over
    /// filament::grades_match() and get their own dialog. A hard mismatch
    /// leaves this false.
    bool grade_only = false;
    std::string expected_material;
    std::string loaded_material;
    int expected_nozzle_min = 0;
    int expected_nozzle_max = 0;
    int expected_bed_temp = 0;
    int loaded_nozzle_min = 0;
    int loaded_nozzle_max = 0;
    int loaded_bed_temp = 0;
};

// ---- pure rules (each is the testable half of one gate) ----

/// Tools with no matching AMS slot. Single-color prints need no mapping;
/// bypass feeds without passing any slot (guaranteed noise); no mappings means
/// no AMS to resolve against.
std::vector<int> unresolved_tools_in(const PrintStartContext& ctx);

/// {needed_g, remaining_g} when the assigned external spool cannot cover the
/// print; nullopt otherwise. Weight falls back to a length-based estimate via
/// the spool's material density when the slicer emitted no weight.
std::optional<std::pair<float, float>> insufficient_spool_weight_in(const PrintStartContext& ctx);

/// One tool whose mapped lane holds less filament than the tool needs.
struct LaneWeightShortfall {
    int tool_index = -1;
    int mapped_slot = -1;
    int mapped_backend = -1;
    float needed_g = 0.0f;
    float remaining_g = 0.0f;
};

/**
 * @brief Tools whose mapped lane cannot supply what the file asks of them.
 *
 * The lane-fed counterpart to insufficient_spool_weight_in(), which can only
 * ever weigh the single external spool and therefore says nothing useful about
 * an AMS print (it compared the whole file against a spool the print would not
 * touch, and no remap could change either input).
 *
 * Silent - deliberately, in every direction where the data does not support an
 * answer:
 *   - no per-tool grams from the slicer, and more than one tool used: no split
 *     is invented. The whole-file total is only attributed to a single tool.
 *   - a mapped lane whose remaining weight is < 0: unknown, not empty. Only a
 *     Spoolman-linked or hand-weighed lane has a figure at all.
 *   - a mapping that resolves to no known lane: no lane, no opinion. Never a
 *     fabricated "0 g remaining" for an index outside the connected units.
 */
std::vector<LaneWeightShortfall> insufficient_lane_weights_in(const PrintStartContext& ctx);

/// Material mismatches, AMS path (ToolMapping::material_mismatch) and non-AMS
/// path (gcode material vs external spool), with filament-database temps.
std::vector<MaterialMismatchDetail> material_mismatches_in(const PrintStartContext& ctx);

/// The ordered production gate list. Order is behavior-critical: it preserves
/// the pre-pipeline check order with the two new gates inserted at 2 and 3.
const std::vector<PrintStartGate>& default_print_start_gates();

} // namespace helix
