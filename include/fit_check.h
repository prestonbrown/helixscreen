// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File-fits-printer check: whether a G-code file stays inside the printer's
// travel, heater limits and tools, and was sliced for this nozzle and model.
// The checker and the limits read from configfile.settings are the twins of
// moonraker-plugin/helix_fitcheck.py; tests/fixtures/fit_check/cases.json
// pins both to one table. Pure: no LVGL objects, no subjects, no I/O.

#include "json_fwd.h"
#include "moonraker_types.h"

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// The HelixPrint plugin feature that holds a slicer's upload-and-print while
/// a blocking finding stands. Plugin-only: the screen never sees such a start.
inline constexpr const char* kFitCheckFeature = "fit_check_blocks_autostart";
inline constexpr int kFitCheckMinVersion = 1;

/// Three optional coordinates, x y z; an axis nobody knows is unset.
using FitAxes = std::array<std::optional<double>, 3>;

/// The extent of a file's moves; an axis no move named is unset.
struct FitBox {
    FitAxes min;
    FitAxes max;
};

/// What the printer allows. Unknown limits are unset and check nothing.
struct FitMachine {
    std::string kinematics;
    FitAxes axis_min;
    FitAxes axis_max;
    std::optional<double> print_radius; ///< a delta's reach from its centre
    std::optional<double> max_extruder_temp;
    std::optional<double> max_bed_temp;
    std::optional<int> tool_count;
    /// The extruders alone give tool_count. A count T macros raised is
    /// inferred, and a file past it only warns.
    bool tool_count_confirmed = false;
    std::vector<double> nozzle_diameters; ///< by extruder number
    /// By extruder number, normalised (fit_nozzle_volume_type()); "" when unknown.
    std::vector<std::string> nozzle_volume_types;
    std::string model; ///< empty when the printer's model is not known
};

/// What a file does.
struct FitFile {
    std::optional<FitBox> print;  ///< extruding moves
    std::optional<FitBox> travel; ///< every move; checked in preference to print
    std::optional<double> max_hotend_temp;
    std::optional<double> max_bed_temp;
    std::set<int> tools;
    std::vector<double> nozzle_diameters; ///< the slicer's, by extruder
    /// The slicer's, by extruder, normalised (fit_nozzle_volume_type()); "" when unknown.
    std::vector<std::string> nozzle_volume_types;
    std::string printer_model; ///< the slicer's printer, or empty
};

enum class FitKind {
    OutOfBounds,
    TempOverMax,
    TooManyTools,
    NozzleMismatch,
    NozzleFlowMismatch,
    PrinterModelMismatch
};
/// Block stops a start; Warn asks before one; Note is only shown, never asked about.
enum class FitSeverity { Note, Warn, Block };

/// One way a file does not fit. Which fields mean anything depends on the kind.
struct FitFinding {
    FitKind kind = FitKind::OutOfBounds;
    FitSeverity severity = FitSeverity::Block;
    std::string axis;   ///< OutOfBounds: "x", "y", "z" or "radius"
    std::string side;   ///< OutOfBounds: "min" or "max"
    std::string heater; ///< TempOverMax: "extruder" or "heater_bed"
    double value = 0.0; ///< the file's position, temperature or nozzle
    double limit = 0.0; ///< the printer's limit, tool count or nozzle
    int tool =
        -1; ///< TooManyTools: the highest tool; NozzleMismatch, NozzleFlowMismatch: the extruder
    std::string file_model;
    std::string printer_model;
    std::string file_volume_type;    ///< NozzleFlowMismatch: "standard" or "high_flow"
    std::string printer_volume_type; ///< NozzleFlowMismatch: "standard" or "high_flow"
};

/// What a printer reports at runtime about one extruder's nozzle, which can
/// differ from configfile.settings when the firmware keeps the fitted nozzle
/// elsewhere. Unset and "" mean the extruder reports nothing.
struct FitLiveNozzle {
    std::optional<double> diameter;
    std::string volume_type; ///< as reported; normalised when it is applied
};

/// "standard" or "high_flow" for any spelling of a nozzle's flow type
/// ("Standard", "High Flow", "high_flow", "HighFlow"), "" for anything else.
[[nodiscard]] std::string fit_nozzle_volume_type(std::string_view text);

/// The limits in configfile.settings @p settings. A delta has a print radius
/// and its height from the towers; every other kinematics reads stepper_x,
/// stepper_y and stepper_z where they exist, widened by a dual carriage.
/// tool_count counts extruders and T<n> macros; model is always empty.
[[nodiscard]] FitMachine fit_machine_from_settings(const nlohmann::json& settings);

/// @p base as this screen knows the printer: its model when the printer
/// database knows the machine, no tool count when a filament system may
/// add T commands the config does not show, and Y shorter by @p y_shrink_mm
/// when the filament system's hardware sits over the far edge of the bed.
[[nodiscard]] FitMachine fit_machine_for_screen(FitMachine base, std::string model,
                                                bool filament_system, double y_shrink_mm);

/// @p base with what its extruders report at runtime, @p live by extruder
/// number: a reported diameter replaces the config's for an extruder the
/// config lists, and a reported flow type is recorded (normalised).
[[nodiscard]] FitMachine fit_machine_with_live_nozzles(FitMachine base,
                                                       const std::vector<FitLiveNozzle>& live);

/// Every way @p file does not fit @p machine, in the plugin's order: bounds by
/// axis (min before max, the radius last), temperatures (hotend, bed), tools,
/// nozzle diameters by extruder, nozzle flow types by extruder, model. Nozzles
/// are compared only for the tools the file uses, when it names any.
[[nodiscard]] std::vector<FitFinding> check_fit(const FitMachine& machine, const FitFile& file);

/// Whether any finding blocks a start.
[[nodiscard]] bool fit_blocks(const std::vector<FitFinding>& findings);

/// The most severe of @p findings; nullopt when there are none.
[[nodiscard]] std::optional<FitSeverity> fit_worst(const std::vector<FitFinding>& findings);

/// Whether any finding is a warning to ask about before a start (blocks aside).
[[nodiscard]] bool fit_warns(const std::vector<FitFinding>& findings);

/// {"min": [x, y, z], "max": [x, y, z]} with null for an unknown axis; nullopt
/// for anything else.
[[nodiscard]] std::optional<FitBox> fit_box_from_json(const nlohmann::json& j);

/// The JSON shapes the shared table and the plugin use.
[[nodiscard]] nlohmann::json fit_machine_json(const FitMachine& machine);
[[nodiscard]] nlohmann::json fit_finding_json(const FitFinding& finding);

/// One sentence telling the user what @p finding means, translated.
[[nodiscard]] std::string fit_finding_text(const FitFinding& finding);

/// One line per finding of @p at_least severity or worse.
[[nodiscard]] std::string fit_findings_text(const std::vector<FitFinding>& findings,
                                            FitSeverity at_least);

/// What the first kPrinterStopScanBytes of a file declare without the
/// plugin: its slicer's printer and nozzles, and the hottest targets its
/// start G-code sets.
struct FitHeadFacts {
    std::string printer_model;
    std::vector<double> nozzle_diameters;
    std::vector<std::string> nozzle_volume_types; ///< normalised; "" when unknown
    std::optional<double> max_hotend_temp;
    std::optional<double> max_bed_temp;
};

/// Read @p head (a file's leading bytes) for FitHeadFacts.
[[nodiscard]] FitHeadFacts fit_head_facts(std::string_view head);

/// The file side of the check from what this screen holds: the plugin's
/// analysis when it answered (@p record), else the head scan, Moonraker's
/// metadata and the preview's model box, field by field.
[[nodiscard]] FitFile fit_file_from_sources(const std::optional<FitFile>& record,
                                            const FitHeadFacts* head,
                                            const std::optional<FileMetadata>& metadata,
                                            const std::optional<FitBox>& preview_bounds,
                                            std::set<int> tools);

} // namespace helix
