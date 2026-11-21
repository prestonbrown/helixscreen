// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The calibration catalog: which calibration steps a printer has, in what
// order, how much each matters on this printer, and what each needs from the
// owner (prerequisites, time, heat, noise, hands).
//
// Pure: it takes plain capability answers (CatalogInputs) and returns plain
// data. calibration_catalog_inputs.h fills the inputs from the live printer.
// The Calibration screen and the guided run read only this.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix {
/// printer_profile_state.h defines it; declared here so the catalog stays free of LVGL.
enum class ZOffsetCalibrationStrategy;
} // namespace helix

namespace helix::calibration {

enum class StepKind : uint8_t {
    BeltTension,           ///< motorized belt tensioning run by the firmware
    CutterPosition,        ///< the filament cutter's position, found by the firmware
    ZOffset,               ///< nozzle-to-bed distance
    Heaters,               ///< PID / MPC heater calibration
    BedMesh,               ///< probed bed mesh
    InputShaper,           ///< resonance measurement with an accelerometer
    PressureAdvance,       ///< measured pressure advance
    BeltCompare,           ///< compare the two belt paths with a resonance sweep
    BedScrews,             ///< bed screw adjustment (screws_tilt_adjust)
    ToolOffsets,           ///< offsets between tools
    VerticalArtifact,      ///< stepper resonance compensation for fine vertical lines
    PressureAdvanceTuning, ///< pressure advance tuned by eye on a test print
    VibrationCheck,        ///< the print speeds that shake the printer
};

/// How much a step matters on this printer. Order is significance: Required first.
enum class Tier : uint8_t { Required, Recommended, Optional, OnRequest };

/// "required", "recommended", "optional", "on_request"; nullopt for anything else.
[[nodiscard]] std::optional<Tier> parse_tier(std::string_view text);
/// The database spelling of @p tier (the inverse of parse_tier()).
[[nodiscard]] const char* tier_id(Tier tier);

/// What the owner must have done or have at hand before a step runs.
enum Prereq : uint32_t {
    kPrereqNone = 0,
    kPrereqBedClear = 1u << 0,         ///< nothing on the bed
    kPrereqToolheadUnloaded = 1u << 1, ///< no filament in the toolhead
    kPrereqFilamentLoaded = 1u << 2,   ///< filament loaded and ready to extrude
    kPrereqBuildPlate = 1u << 3,       ///< the build plate is on the bed
    kPrereqPaper = 1u << 4,            ///< a sheet of paper for the paper test
    kPrereqAccelerometer = 1u << 5,    ///< an accelerometer is attached
};
using PrereqSet = uint32_t;

struct StepInfo {
    StepKind kind{};
    /// Stable key, also the calibration history key.
    const char* id = "";
    /// Untranslated English name; the UI passes it through lv_tr().
    const char* name = "";
    Tier tier = Tier::Optional;
    int minutes = 0;
    bool heats = false;
    bool loud = false;
    bool moves = false;
    bool needs_hands = false;
    bool power_cycle = false;
    PrereqSet prereqs = kPrereqNone;
    /// Position in the fixed run order; lower runs first.
    int order = 0;
};

/// A per-printer tier from the printer database: step id and tier.
using TierOverride = std::pair<std::string, Tier>;

/// Everything the catalog needs to know about one printer.
struct CatalogInputs {
    bool has_probe = false;
    bool has_bed_mesh = false;
    bool has_accelerometer = false;
    bool can_tension_belts = false;
    bool can_calibrate_cutter = false;
    bool has_pa_cal = false;
    bool has_tool_offset_cal = false;
    bool has_screws_tilt = false;
    bool has_stepper_resonance_cal = false;
    bool supports_belt_compare = false;
    bool supports_ripple_check = false;
    /// The firmware applies each head's own Z offset at pickup.
    bool z_offset_reapplied_per_head = false;
    /// The tool offset calibration also sets the reference tool's Z.
    bool hide_manual_z_calibration = false;
    /// Value-initialized: PROBE_CALIBRATE.
    ZOffsetCalibrationStrategy z_strategy{};
    /// The probe measures with the nozzle itself (see probe_contacts_nozzle()).
    bool nozzle_contact_probe = false;
    std::vector<TierOverride> tier_overrides;
};

/// How many steps the catalog knows.
inline constexpr size_t kStepCount = 13;
/// Every step id the catalog knows, in run order, whatever the printer has.
[[nodiscard]] const std::array<const char*, kStepCount>& all_step_ids();

/// The steps this printer has, in run order.
[[nodiscard]] std::vector<StepInfo> applicable_steps(const CatalogInputs& in);

/// The prerequisites of every step in @p steps.
[[nodiscard]] PrereqSet prerequisites_for(const std::vector<StepInfo>& steps);
[[nodiscard]] int total_minutes(const std::vector<StepInfo>& steps);
/// Minutes of the steps the owner has to take part in.
[[nodiscard]] int hands_on_minutes(const std::vector<StepInfo>& steps);

/// Database `calibration.steps` entries as overrides: entries with an unknown
/// step id or tier are dropped.
[[nodiscard]] std::vector<TierOverride>
tier_overrides_from(const std::vector<std::pair<std::string, std::string>>& entries);

} // namespace helix::calibration
