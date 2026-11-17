// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_spool_retention.h
 * @brief Capability of a filament system whose firmware decides what an eject
 *        does to a lane's spool details
 *
 * Reached with AmsBackend::as<ISpoolRetention>(). AFC implements it with its
 * per-lane remember_spool setting. Without it HelixScreen's own "Keep Spool Info
 * on Eject" setting applies.
 */

#pragma once

#include "ams_error.h"

namespace helix {

class ISpoolRetention {
  public:
    /// Whether the firmware is retaining spool identity across an eject on
    /// every lane (AFC's per-lane remember_spool = true everywhere). Dynamic:
    /// it reflects a config choice, not a firmware property. Read alongside
    /// firmware_owns_spool_retention().
    [[nodiscard]] virtual bool printer_retains_spool_info() const = 0;

    /// Whether the firmware's setting is reported on every lane right now. When
    /// true that setting decides, and HelixScreen's "Keep Spool Info on Eject"
    /// does not apply: the switch shows printer_retains_spool_info() and writes
    /// through set_firmware_spool_retention().
    [[nodiscard]] virtual bool firmware_owns_spool_retention() const = 0;

    /// Write the firmware's own spool-retention setting on every lane.
    virtual AmsError set_firmware_spool_retention(bool keep) = 0;

  protected:
    ~ISpoolRetention() = default;
};

} // namespace helix
