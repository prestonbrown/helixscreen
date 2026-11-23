// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_spoolman_tag_links.h
 * @brief Capability of a filament system whose spool tags can name a Spoolman spool
 *
 * Reached with AmsBackend::as<ISpoolmanTagLinks>(). CFS implements it. Without it
 * no tag names a spool, and the setting that links a lane from its tag is hidden.
 */

#pragma once

namespace helix {

class ISpoolmanTagLinks {
  public:
    /// Whether tag readings can name a Spoolman spool right now: true when
    /// tag_reading() fills TagReading::spoolman_id for tags that carry one. Gates
    /// the setting that links a lane to the spool its tag names; the linking
    /// itself reads only tag_reading().
    [[nodiscard]] virtual bool tags_name_spoolman_spools() const = 0;

  protected:
    ~ISpoolmanTagLinks() = default;
};

} // namespace helix
