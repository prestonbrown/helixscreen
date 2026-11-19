// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Z offset and bed mesh by material (prestonbrown/helixscreen#2142).
//
// A heated bed expands differently at each temperature, so a printer keeps one
// Z offset per material, applied absolutely by its print-start macro, and
// optionally one stored bed mesh per material. Where that table lives depends
// on who wrote the start macro, so this module is a provider table in the
// z_offset_persistence shape: one row per storage scheme, and capability
// questions over it. Generic code (discovery, the status path, the Save path,
// Settings) asks these functions and never names a storage scheme.
//
// The value is the gcode offset for that material, on top of the probe's own
// z_offset and never folded into it: nothing here emits Z_OFFSET_APPLY_PROBE
// or SAVE_CONFIG (docs/devel/MATERIAL_Z_OFFSET.md).

#include "json_fwd.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct FileMetadata; // NAMESPACE_OK: moonraker_types.h declares it at global scope

namespace helix {
class PrinterDiscovery;
} // namespace helix

namespace helix::material_zoffset {

/// The key of the entry every material without its own falls back to.
inline constexpr const char* kDefaultKey = "default";

/// One printer's per-material table, as read from its status frames.
///
/// Keys are the row's storage keys (material_key()): lower case, so "PETG"
/// and "petg" are one entry.
struct Table {
    /// Storage key -> offset in microns. kDefaultKey holds DEFAULT.
    std::map<std::string, int> offsets;
    /// Storage keys that have a stored bed mesh.
    std::set<std::string> meshes;
    /// What the start macro last applied, where the row records it.
    std::string applied;
    /// How many times the start macro has applied an offset since Klipper
    /// started, where the row counts it; -1 where it does not.
    int apply_count = -1;
    /// The printer-side switch the start macro obeys, where the row has one;
    /// nullopt until read.
    std::optional<bool> enabled;
};

// --- Detection -------------------------------------------------------------

/// Whether a row matches this printer. False wherever a z_offset_persistence
/// provider matches: that firmware already owns the gcode offset.
bool supported(const PrinterDiscovery& hw);

/// The matched row's name, for logging. Empty when none.
std::string provider_name(const PrinterDiscovery& hw);

/// Klipper status objects to subscribe so merge_status() sees the table.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Whether the row can persist a value right now. A row can match yet need
/// one more piece of setup (HelixScreen's macro without [save_variables]).
bool write_ready(const PrinterDiscovery& hw);

/// Whether the start macro reports each time it applies an offset, so a Save
/// can tell a print that applied a material from one whose PRINT_START never
/// called the macro. A row that is itself the start macro's own store applies
/// on every print and does not need to report.
bool reports_application(const PrinterDiscovery& hw);

/// Whether the start macro applies per-material offsets at print start on its
/// own, whatever this screen's setting says: always for a vendor macro that
/// keeps the table, and while its printer-side switch is on for HelixScreen's.
/// While true, folding the live offset into the probe would double it at the
/// next print (prestonbrown/helixscreen#1401), so no save may do it.
bool start_macro_applies(const PrinterDiscovery& hw, const Table& table);

/// G-code that turns the start macro's printer-side switch on or off, or empty
/// where the row has none.
std::string enable_gcode(const PrinterDiscovery& hw, bool on);

// --- Reading ---------------------------------------------------------------

/// Fold this status frame's news into @p table. The storing objects are
/// delta-only, so a key missing from a frame is "no news", never "cleared".
/// Returns true when the table changed.
bool merge_status(const PrinterDiscovery& hw, const nlohmann::json& status, Table& table);

/// The storage key for @p material on this printer: kDefaultKey for an empty
/// (unknown) material, nullopt when the row cannot store that name or no row
/// matches.
std::optional<std::string> material_key(const PrinterDiscovery& hw, std::string_view material);

/// The display name of a storage key: "DEFAULT", "PETG".
std::string display_name(std::string_view key);

/// The offset the start macro applies for @p material: its own entry, else
/// DEFAULT, else nullopt.
std::optional<int> applied_offset(const PrinterDiscovery& hw, const Table& table,
                                  std::string_view material);

// --- Writing ---------------------------------------------------------------

/// What persisting one material's offset takes.
struct WritePlan {
    /// G-code to send now; may be empty when only the file edit applies.
    std::string gcode;
    /// The value lives in a config file that must also be edited
    /// (edit_config_text()), section config_section().
    bool edits_config_file = false;
    /// The value is in effect for the next print without a Klipper restart.
    bool live_now = false;
};

/// The write for @p material at @p microns, or nullopt when the row cannot
/// store that material. Never contains Z_OFFSET_APPLY_PROBE or SAVE_CONFIG.
std::optional<WritePlan> write_plan(const PrinterDiscovery& hw, const Table& table,
                                    std::string_view material, int microns);

/// The config section whose file holds the table, for a row that edits one;
/// empty otherwise. Lower case, single-spaced: "gcode_macro _start_print_vars".
std::string config_section(const PrinterDiscovery& hw);

/// G-code that tells the start macro which material the print of @p filename
/// (gcodes-relative, as print_stats reports it) uses, or empty where the row
/// takes the material from the slicer only. The macro ignores the hint for any
/// other job, so one a cancelled start left behind never reaches the next
/// print. An empty or unstorable material, or a file name a G-code parameter
/// cannot carry, gives the clearing form instead.
std::string start_hint_gcode(const PrinterDiscovery& hw, std::string_view material,
                             std::string_view filename);

/// Whether the row loads a stored mesh per material.
bool stores_mesh(const PrinterDiscovery& hw);

/// The bed mesh profile name for @p material, or empty when the row stores no
/// mesh or cannot store that material.
std::string mesh_profile_name(const PrinterDiscovery& hw, std::string_view material);

// --- The config file edit (pure) -------------------------------------------

/// The result of editing a config file's text.
struct ConfigEdit {
    std::optional<std::string> text; ///< The whole new file, or nullopt on refusal
    std::string error;               ///< Why it refused, for the log and the user
};

/// How many live (not commented) headers for @p section the text holds,
/// matched case-insensitively with inner whitespace collapsed.
int count_sections(std::string_view content, std::string_view section);

/// Set `variable_offset_<MATERIAL>: <mm>` inside @p section of @p content.
///
/// Replaces the value of the one existing line for that material (any case),
/// keeping its key spelling and any inline comment, or inserts a new line
/// after the section's last `variable_` option when there is none. Every other
/// byte is kept, line endings included. Refuses when the section is not found
/// exactly once, when the material has more than one line, or when the line's
/// value continues onto the next line.
ConfigEdit edit_config_text(std::string_view content, std::string_view section,
                            std::string_view material, int microns);

/// The files a config file includes, as written (`[include x]`), in order.
std::vector<std::string> include_targets(std::string_view content);

/// Whether @p path matches an include pattern with Klipper's glob: `*` any
/// run of characters but '/', `?` one character but '/', `[...]` one character
/// of a class (ranges, `!` to negate).
bool include_matches(std::string_view pattern, std::string_view path);

// --- The primary material (pure) -------------------------------------------

/// One tool's share of a file.
struct ToolMaterial {
    int tool = -1; ///< -1 for a figure the file gives only for the whole print
    double grams = 0.0;
    std::string material;
};

/// The material of the tool that prints the most by weight, ties going to the
/// lower tool; the first named material when no tool has grams. Empty when
/// nothing names one.
std::string primary_material(const std::vector<ToolMaterial>& tools);

/// The primary material of a sliced file: per-tool grams from its metadata
/// (print_cost::file_tool_grams), else @p footer_grams (per tool, from the
/// G-code footer), each tool named from the file's filament_type list.
std::string primary_material_for_file(const FileMetadata& metadata,
                                      const std::vector<double>& footer_grams, float diameter_mm);

/// The first filament a file names: the first entry of a ';'-separated
/// filament_type list. Empty when none.
std::string first_filament(std::string_view filament_type);

/// The usual bed temperature of the closest-named material, for choosing
/// which material a fresh mesh belongs to. -1 when unknown.
int usual_bed_temp(std::string_view material);

/// Of @p materials, the one whose usual bed temperature is closest to
/// @p bed_temp, ties going to the first. Empty when none has a temperature.
std::string closest_material(const std::vector<std::string>& materials, int bed_temp);

} // namespace helix::material_zoffset
