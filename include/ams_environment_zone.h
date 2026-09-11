// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <string>
#include <vector>

namespace helix::printer {

/**
 * @brief What a zone is doing, from the firmware's per-gate drying states.
 *
 * `Queued` is its own answer rather than a shade of `Active`: a backend that caps
 * simultaneous heaters leaves later zones waiting, and a waiting zone shown as idle reads
 * as a control that did nothing. `Cancelled` and `Complete` likewise differ - one answers
 * "someone stopped it", the other "it finished".
 */
enum class ZoneDryingState : int {
    Idle = 0,     ///< Not part of any cycle
    Queued = 1,   ///< In the cycle, waiting for a concurrency slot
    Active = 2,   ///< Heater on now
    Complete = 3, ///< Ran to the end
    Cancelled = 4 ///< Stopped before finishing
};

/**
 * @brief One filament environment: the gates that share a heater and a sensor.
 *
 * Hardware does not divide environments the way it divides units. A QuattroBox
 * is one heated enclosure over four gates; an EMU gives every lane its own
 * sealed box and its own sensor; a QIDI rig is one box per unit. A zone is the
 * granularity all three have in common, so the UI selects, renders and controls
 * zones and never asks which vendor produced them.
 *
 * @p env and @p dryer are independent. A passive EMU lane reports humidity with
 * `dryer.supported == false`: there is something to watch and nothing to drive.
 */
struct EnvironmentZone {
    std::string id;          ///< Stable across polls; derived from the object pair
    std::string label;       ///< Shown in the selector; the backend names it
    std::string heater_name; ///< Klipper object, empty when the zone is passive
    std::string sensor_name; ///< Klipper object, empty when nothing is monitored
    std::vector<int> gates;  ///< Global gate indices, ascending
    int unit_index = -1;     ///< Unit the gates fall in, -1 when they span several
    EnvironmentData env;     ///< Live readings
    DryerInfo dryer;         ///< supported=false on a passive zone
    ZoneDryingState state = ZoneDryingState::Idle; ///< Folded from the zone's gates
};

/**
 * @brief Collapse per-gate heater/sensor lists into zones.
 *
 * Gates naming the same (heater, sensor) pair are one zone, which is what makes
 * a shared enclosure come back as a single zone even when its one heater has to
 * be repeated once per gate. Klipper object names are unique, so two boxes can
 * never collide on the key.
 *
 * Gates naming neither a heater nor a sensor have no environment and are
 * dropped rather than forming an anonymous zone.
 *
 * Pass a shared enclosure as lists repeating the one name, so both the shared
 * and per-gate shapes take this single path.
 *
 * @param per_gate_heaters Heater object per gate; short lists read as empty
 * @param per_gate_sensors Sensor object per gate; short lists read as empty
 * @param gate_count Gates to consider, indices [0, gate_count)
 * @param per_gate_state Firmware drying state per gate; empty leaves every zone idle
 * @return Zones ordered by their lowest gate
 */
[[nodiscard]] std::vector<EnvironmentZone>
derive_environment_zones(const std::vector<std::string>& per_gate_heaters,
                         const std::vector<std::string>& per_gate_sensors, int gate_count,
                         const std::vector<std::string>& per_gate_state = {});

/**
 * @brief Reduce a zone's gates to one state.
 *
 * Precedence is Active > Queued > Cancelled > Complete > Idle. Live states outrank
 * finished ones because they describe what is happening now; `Cancelled` outranks
 * `Complete` so a partly-cancelled zone does not report as cleanly finished.
 *
 * Gates past the end of @p per_gate_state, and states the firmware does not define, read
 * as `Idle`.
 *
 * @param gates Global gate indices belonging to the zone
 * @param per_gate_state Firmware state per gate, indexed globally
 */
[[nodiscard]] ZoneDryingState
fold_zone_drying_state(const std::vector<int>& gates,
                       const std::vector<std::string>& per_gate_state);

/**
 * @brief Mark every zone that can dry but is not the one running as Queued.
 *
 * A concurrency-capped dryer runs one heater at a time (Happy Hare's
 * `max_concurrent_heaters` is one hardware shape of this): the base state each zone
 * already carries says which one that is, via its own `active` flag folded into
 * `Active` or `Idle`. This adds the Queued half on top of that.
 *
 * A no-op when nothing in @p zones is Active: idle heaters with no cycle running are
 * not waiting on anything.
 *
 * @param zones Zones to mutate in place
 */
void queue_zones_waiting_for_the_cap(std::vector<EnvironmentZone>& zones);

/**
 * @brief How the UI should let the user choose between zones.
 */
enum class ZonePresentation : int {
    Single = 0, ///< One zone (or none): no selector, go straight to detail
    Tabs = 1,   ///< A few interchangeable zones: segmented selector
    List = 2    ///< Many zones, or zones that differ in what they can do
};

/**
 * @brief Pick the selector shape for a set of zones.
 *
 * Count alone is the wrong rule. Zones that differ in whether they can dry are not
 * interchangeable, and a tab hides that difference behind a selection the user has to make
 * before they can see it, so any capability split goes to the list.
 */
[[nodiscard]] ZonePresentation select_zone_presentation(const std::vector<EnvironmentZone>& zones);

/// Above this many zones a segmented selector stops fitting the narrow breakpoints.
inline constexpr size_t kMaxZoneTabs = 4;

} // namespace helix::printer
