// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_shared_spool_lanes.h
 * @brief Capability of a filament system where one spool can show on two lanes
 *
 * Reached with AmsBackend::as<ISharedSpoolLanes>(). MultiACE implements it: a bay
 * routed to a toolhead lane is that lane's spool. Generic code asks through
 * upstream_slot() and spool_display_number() below, which answer for every
 * backend.
 */

#pragma once

#include <optional>

namespace helix {

class AmsBackend;

class ISharedSpoolLanes {
  public:
    /**
     * @brief The lane whose spool is this lane's, when the two lanes share one
     *
     * A lane fed from storage upstream (a bay routed to a toolhead lane) shows
     * another lane's spool: the spool's home is the bay, and the fed lane is
     * the same filament's path onward. nullopt when the lane's spool is its own.
     */
    [[nodiscard]] virtual std::optional<int> upstream_slot(int slot_index) const = 0;

    /**
     * @brief The number a spool badge shows for this lane
     *
     * Resolves through upstream_slot(), so a fed lane and the storage lane
     * feeding it wear one number and each spool counts once.
     *
     * @return the number, or a negative value for a lane with none to show
     */
    [[nodiscard]] virtual int spool_display_number(int slot_index) const = 0;

  protected:
    ~ISharedSpoolLanes() = default;
};

/// The lane whose spool @p slot_index shows; nullopt when the lane's spool is
/// its own, which is every lane on a backend without ISharedSpoolLanes.
[[nodiscard]] std::optional<int> upstream_slot(const AmsBackend& backend, int slot_index);

/// The number a spool badge shows for @p slot_index: the lane's global number
/// (ui::lane_number) on a backend without ISharedSpoolLanes.
[[nodiscard]] int spool_display_number(const AmsBackend& backend, int slot_index);

} // namespace helix
