// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace helix::ui {

/**
 * @brief A device action's multi-step UI, registered by id.
 *
 * A DeviceAction that names a flow in `guided_flow` is started through it instead of
 * being executed and toasted. A vendor module registers its flows once at startup, so the
 * generic section overlay never names the vendor or its dialogs.
 *
 * @threading Main thread only
 */
struct GuidedDeviceFlow {
    /// Appends the flow's status widgets to the section's action container, once per
    /// section build. Null when the flow has nothing to show in the section.
    std::function<void(lv_obj_t* actions_container)> attach_status;
    /// Runs the flow when its action's button is clicked. @p label is the action's label.
    std::function<void(const std::string& label)> start;
};

/// Registers @p flow under @p id, replacing any earlier registration.
void register_guided_device_flow(const std::string& id, GuidedDeviceFlow flow);

/// Removes the registration for @p id, if any.
void unregister_guided_device_flow(const std::string& id);

/// The flow registered under @p id, or null. The pointer is valid until the next
/// register/unregister call.
const GuidedDeviceFlow* find_guided_device_flow(const std::string& id);

/// Registers every built-in vendor flow. Startup calls this once, before any overlay
/// builds; the table of vendor registrars behind it lives in ui_guided_device_flow.cpp,
/// so adding a vendor's flows adds a row there and no startup code changes.
void register_builtin_guided_device_flows();

// The per-vendor registrars register_builtin_guided_device_flows() runs. Each is
// idempotent, since a registration replaces an earlier one under the same id.

/// Registers the Creality CFS calibration flows (`cfs_purge_chute`, `cfs_cutter`,
/// `cfs_belt_tension`). Does nothing in a build without CFS support.
void register_cfs_device_flows();

/// Registers the Snapmaker U1 flows (`snapmaker_nozzle_offset`). Does nothing in a
/// build without Snapmaker support.
void register_snapmaker_device_flows();

/// Registers the Bondtech INDX flows (`indx_cycle_all`, `indx_heater_calibration`,
/// `indx_fan_calibration`, `indx_seat_by_hand`).
void register_indx_device_flows();

} // namespace helix::ui
