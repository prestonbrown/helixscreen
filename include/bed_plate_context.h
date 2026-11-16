// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What every plate view asks of the printer before it draws or moves: the plate area in
// G-code space, whether it is round, the envelope a move is clamped to, whether the plate
// may move the head now, and the lift for the next XY move. The Bed tab, the Move tab's
// presets and the home Toolhead widget all read these, so they agree.

#include "async_lifetime_guard.h"
#include "motion_presets.h"
#include "printer_motion_state.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {

class AsyncLifetimeGuard;
class IMoonrakerAPI;
class PrinterState;

struct BedPlateArea {
    AxisBounds area; ///< preset_area(): the plate in G-code space
    bool circular = false;
};

/// nullopt with no API or a degenerate area.
[[nodiscard]] std::optional<BedPlateArea> current_bed_plate(const PrinterState& ps,
                                                            IMoonrakerAPI* api);

/// The G-code envelope inset by kGcodeEdgeMarginMm: what absolute moves are clamped to.
[[nodiscard]] AxisBounds move_clamp_bounds(const PrinterState& ps);

/// Every axis homed, nav enabled, machine_motion_blocked clear and no external blocking
/// operation: the plate and the position presets may move the head.
[[nodiscard]] bool plate_moves_allowed(PrinterState& ps);

/// Whether a part may still be on the bed, from get_bed_occupancy_state(); false with no state.
[[nodiscard]] bool bed_may_be_occupied();

/// The height of a part that may still be on the bed, from get_bed_occupancy_state():
/// nullopt with no state, a clear bed, or an occupying job of unknown height. Pair it with
/// bed_may_be_occupied() to tell the last two apart.
[[nodiscard]] std::optional<double> bed_part_height();

/// plate_lift() for a move starting at @p start_z, with the clearance setting, the clamp
/// envelope's z_max and the bed-occupancy part height. With Z bounds unknown no lift can be
/// sized: Kind::Refuse while a part may be on the bed, Kind::None otherwise.
[[nodiscard]] PlateLift current_plate_lift(const PrinterState& ps, double start_z);

/// How far Park raises the nozzle before it travels.
inline constexpr double kParkZLiftMm = 10.0;

/// Park's lift from @p current_z: the higher of current_z + kParkZLiftMm and the plate
/// floor, capped at the clamp envelope's z_max. Refuse when current_plate_lift() refuses
/// (a part too tall to clear, or unknown Z bounds over a part); Kind::None when Z bounds are
/// unknown over a clear bed or the cap leaves no rise. Shared by the Move tab's Park and the home
/// Toolhead widget's Park slot.
[[nodiscard]] PlateLift park_plate_lift(const PrinterState& ps, double current_z);

/// Park with no ParkToolhead macro: plate_rear_park() with park_plate_lift() applied.
struct ParkOverPlate {
    enum class Kind {
        Unknown, ///< no plate area to park over: "Axis limits unknown"
        Refuse,  ///< a part too tall to clear: part_too_tall_text()
        Move,    ///< send `target`
    };
    Kind kind = Kind::Unknown;
    AxisTarget target;
};

/// Where Park goes over the plate for a move starting at @p start_z. Shared by the Move
/// tab's Park and the home Toolhead widget's Park slot.
[[nodiscard]] ParkOverPlate park_over_plate_target(const PrinterState& ps, IMoonrakerAPI* api,
                                                   double start_z);

/// Run the ParkToolhead standard macro with its Running / complete / failed notices.
/// Returns false, having done nothing, when none is configured: the caller parks over the
/// plate instead. @p lifetime guards the failure callback.
bool run_park_toolhead_macro(IMoonrakerAPI* api, AsyncLifetimeGuard& lifetime, const char* tag);

/// clamp_z_down_to_floor() with the clearance setting and the bed occupancy: a downward Z
/// step never ends below the floor while a part may be on the bed, whether or not its
/// height is known.
[[nodiscard]] double clamp_z_down_to_current_floor(double target_z);

/// The sentence announcing @p lift before a move made at @p shown_z: "Z 0.20, will lift to
/// 43mm", plus the part's height when a part on the bed raised it; part_too_tall_text() for
/// Refuse; empty for None. The Bed tab's readout and the home Toolhead widget's both show it.
[[nodiscard]] std::string plate_lift_readout(double shown_z, const PlateLift& lift);

/// What a Refuse lift tells the user: the part on the bed is too tall to clear.
[[nodiscard]] const char* part_too_tall_text();

/// The Cleared chips' action: ask whether the last print is off the bed, and only on a yes
/// mark it cleared, which drops the lift over it. The answer is read from the state live at
/// that moment, so a printer switch while the dialog is up clears nothing stale.
void confirm_bed_cleared();

/// Every calibration that moves the toolhead starts through this: @p start runs straight
/// away when no print may be on the bed, and otherwise only once the user answers the
/// same "Is the bed clear?" question with Cleared, which also marks the bed cleared.
/// Cancel or dismissing the question starts nothing and runs @p on_declined instead. @p owner
/// gates both, so a screen closed while the question is up hears nothing back.
void ensure_bed_clear_then(LifetimeToken owner, std::function<void()> start,
                           std::function<void()> on_declined = {});

} // namespace helix
