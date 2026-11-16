// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_feeder_control.h
 * @brief Capability of a filament system with a gripper on the frame
 *
 * Reached with AmsBackend::as<IFeederControl>(). The tool changer backend implements it,
 * and supports_feeder_control() says whether this machine has the gripper: a controller
 * reports it during discovery, so the answer is per machine, not per backend.
 */

#pragma once

#include "ams_error.h"

namespace helix {

class IFeederControl {
  public:
    /**
     * @brief Open or close the frame-side filament feeder (async)
     *
     * Only a machine whose filament is held by a gripper on the frame, not by
     * the travelling head, has one. Opening releases the filament so it can be
     * pulled by hand; closing grips it again. The reported state follows in a
     * later status frame, through feeder_open().
     */
    virtual AmsError set_feeder_open(bool open) = 0;

    /// Whether set_feeder_open() can act on this printer.
    [[nodiscard]] virtual bool supports_feeder_control() const = 0;

    /// Whether the feeder last reported itself open (filament released).
    [[nodiscard]] virtual bool feeder_open() const = 0;

  protected:
    ~IFeederControl() = default;
};

} // namespace helix
