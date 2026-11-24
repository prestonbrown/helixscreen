// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_tool_park.h
 * @brief Capability of a machine with docked toolheads: put the mounted tool back
 *        in its dock, leaving the carriage empty
 *
 * Reached with AmsBackend::as<IToolPark>(), or through usable_tool_park(), which
 * also asks whether this machine can park now. The tool changer (and INDX through
 * it), the Snapmaker U1 (and MultiACE through it) and the mock implement it.
 * Without it nothing offers a park.
 */

#pragma once

#include "ams_capability.h"
#include "ams_error.h"

namespace helix {

class IToolPark {
  public:
    /// Whether park_active_tool() can act on this machine. A tool changer whose
    /// own swap commands have no unmount answers false.
    [[nodiscard]] virtual bool supports_tool_park() const = 0;

    /**
     * @brief Put the tool on the carriage back in its dock (async)
     *
     * A Snapmaker U1 sends PARK_EXTRUDER, klipper-toolchanger UNSELECT_TOOL.
     * Results arrive as a state change, like unload_filament().
     *
     * @return not_supported() where supports_tool_park() is false; not_loaded()
     *         when nothing is mounted
     */
    virtual AmsError park_active_tool() = 0;

  protected:
    ~IToolPark() = default;
};

/// @p host's tool park where this machine can park, else nullptr.
[[nodiscard]] inline IToolPark* usable_tool_park(const AmsCapabilityHost* host) {
    IToolPark* park = host != nullptr ? host->as<IToolPark>() : nullptr;
    return park != nullptr && park->supports_tool_park() ? park : nullptr;
}

} // namespace helix
