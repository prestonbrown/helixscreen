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
#include "filament_database.h"
#include "filament_mapper.h"
#include "fit_check.h"
#include "moonraker_types.h"

#include <cstdint>
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

/// What the file scan found about commands this printer treats as an emergency stop.
struct PrinterStopCheck {
    enum class State {
        NotRun, ///< No answer: no scan of this file, a failed download, or unread macros
        Clean,  ///< The scanned lines call none of them
        Stops,  ///< The file calls one
    };
    State state = State::NotRun;
    std::string not_run_reason; ///< Why, when NotRun
    std::string command;        ///< The first call's command word as the file writes it
    size_t line_number = 0;     ///< Its 1-indexed line
    std::string stop_message;   ///< The macro's literal stop message; empty when unknown
    /// The MacroParamCache generation this answer was computed against. An
    /// answer from an older generation describes a macro set the printer may no
    /// longer have, so it is treated as no answer at all.
    uint64_t macro_generation = 0;
};

/// How much of a file's head the printer-stopping command check reads. Every
/// caller downloads exactly this much: the answer describes the scanned prefix,
/// so two callers reading different amounts would disagree about one file.
inline constexpr size_t kPrinterStopScanBytes = 200 * 1024;

/// Look through @p content for a command this printer's macros make an
/// emergency stop. A download cut at @p limit ends mid-line, so that last
/// partial line is not read: a truncated word could spell a command the file
/// never calls.
[[nodiscard]] PrinterStopCheck printer_stop_check_in(const std::string& content, size_t limit);

/// An answer of "no answer", carrying @p reason and the macro generation it was
/// made against.
[[nodiscard]] PrinterStopCheck printer_stop_not_run(std::string reason);

/// Everything a gate may read. Gathered fresh by the controller per pipeline
/// (re-)entry; gates never fetch singleton state themselves.
struct PrintStartContext {
    // ---- file (from PrintSelectDetailView or FileStartFacts; valid only when has_file_facts) ----
    bool has_file_facts = false;
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

    /// The printer-stopping command scan of this file (PrintPreparationManager).
    PrinterStopCheck printer_stop;

    /// What the file does and what the printer allows, for the fit check.
    /// No machine (the printer's config not read yet) checks nothing.
    FitFile fit_file;
    std::optional<FitMachine> fit_machine;

    // ---- environment (from AmsState / FilamentSensorManager) ----
    bool ams_available = false;           ///< AmsState::is_available()
    bool ams_manages_filament = false;    ///< any backend presents a filament system
    bool has_active_backend = false;      ///< AmsState::get_backend() != nullptr
    bool any_auto_unload_backend = false; ///< any backend->auto_unloads_after_print()
    bool any_bypass_active = false;       ///< AmsState::any_bypass_active()
    /// The firmware picks each tool's lane at print time
    /// (AmsState::collect_firmware_routing().chosen_at_print).
    bool lanes_picked_at_print = false;
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
    float filament_diameter_mm = filament::kDefaultDiameterMm; ///< [extruder] filament_diameter
    /// ToolState::has_multiple_toolheads(): the file's materials print through
    /// separate nozzles rather than taking turns in one.
    bool multiple_toolheads = false;

    // ---- non-AMS aggregate runout fallback ----
    bool runout_enabled = false;
    bool runout_available = false;
    bool runout_detected = false;

    // ---- enclosure doors (InputSensorManager) ----
    bool door_check_enabled = false;     ///< the user's door check, default on with a Door input
    std::vector<std::string> open_doors; ///< names of Door inputs reading open
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

/// The dialog for a file that calls a command this printer treats as an
/// emergency stop. Pass for every other state, so a caller can hand it any
/// check and read the verdict.
[[nodiscard]] CheckResult gate_printer_stopping_command(const PrintStartContext& ctx);

/// The dialog for a file that does not fit the printer: a hard stop for a
/// blocking finding (out of bounds, over a heater limit, a missing tool), a
/// warning with Start Anyway for a nozzle or model mismatch alone.
[[nodiscard]] CheckResult gate_fit_check(const PrintStartContext& ctx);

/// The dialog for a door or cover that reads open while the door check is on
/// and the print has a material that needs a warm enclosure: a warning with
/// Start Anyway naming that material. Pass otherwise.
[[nodiscard]] CheckResult gate_door_open(const PrintStartContext& ctx);

/// Per-tool material mismatch detail.
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

    /// The lane's display identity, copied from the AvailableSlot this
    /// shortfall was measured against so the gate dialog does not re-resolve
    /// (and, on a multi-unit backend, re-derive the wrong number from
    /// mapped_slot) what insufficient_lane_weights_in() already found.
    helix::ui::LaneNoun noun = helix::ui::LaneNoun::Slot;
    std::string unit_display_name;
    int local_slot_index = -1;
};

/**
 * @brief Tools whose mapped lane cannot supply what the file asks of them.
 *
 * The lane-fed counterpart to insufficient_spool_weight_in(), which can only
 * ever weigh the single external spool and therefore says nothing useful about
 * an AMS print (it compares the whole file against a spool the print would not
 * touch, and no remap can change either input).
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

/**
 * @brief Can these two materials not share one print?
 *
 * True when the hotter one needs a heated chamber and the two have no nozzle
 * temperature in common, both read from the filament database. The cooler
 * material then softens in the chamber and bed the hotter one needs, or chars
 * in a nozzle set for it. A name the database cannot resolve, directly or
 * through its base polymer, never conflicts.
 */
[[nodiscard]] bool materials_conflict_in_temperature(std::string_view a, std::string_view b);

/**
 * @brief The materials the print extrudes, deduplicated, in tool order.
 *
 * Only tools the file extrudes from count: the scan's tools_used, else the
 * slicer's per-tool weights. Each tool's material is the mapping card's tool
 * info, else the slicer palette. Empty without file facts.
 */
[[nodiscard]] std::vector<std::string> materials_in_print(const PrintStartContext& ctx);

/// The material among @p materials that needs a warm enclosure and wants the
/// warmest chamber (filament::MaterialInfo::needs_enclosure()); nullopt when
/// none does. A name the database cannot resolve never needs one.
[[nodiscard]] std::optional<std::string>
enclosure_material_in(const std::vector<std::string>& materials);

/// Whether an open door is worth a warning, and for which material: the door
/// check is on, a door reads open, and the print has a material that needs a
/// warm enclosure. Shared by the print-start gate and the mid-print warning.
[[nodiscard]] std::optional<std::string>
door_warning_material(bool door_check_enabled, bool any_door_open,
                      const std::vector<std::string>& materials);

/// The first pair of materials the file uses that cannot share a print, the
/// cooler one first.
struct MixedTemperatureMaterials {
    std::string cooler;
    std::string hotter;
};

/**
 * @brief Two materials the file prints with that cannot share a print.
 *
 * Only tools the file extrudes from count: the scan's tools_used, else the
 * slicer's per-tool weights. With neither, the palette alone cannot say which
 * tools print (a single-tool file sliced on a multi-tool profile lists them
 * all), so nothing is reported.
 */
std::optional<MixedTemperatureMaterials>
mixed_temperature_materials_in(const PrintStartContext& ctx);

/// The ordered production gate list. Order is behavior-critical.
const std::vector<PrintStartGate>& default_print_start_gates();

/// Log that a print is starting without the printer-stopping command check.
/// @param start_path The in-app path starting it, as the log line names it.
void warn_printer_stop_check_skipped(std::string_view start_path, std::string_view filename,
                                     std::string_view reason);

} // namespace helix
