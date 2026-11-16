// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_insert_signals.h
 * @brief Capability of a filament system whose lanes report spool inserts themselves
 *
 * Reached with AmsBackend::as<IInsertSignals>(). The Snapmaker U1 and MultiACE implement it.
 * A backend without it has its inserts judged on the slot status, on lanes with a prep sensor,
 * and has no tag reader to wait for.
 */

#pragma once

#include "ams_types.h"

#include <optional>

namespace helix {

class IInsertSignals {
  public:
    /**
     * @brief How the spool last inserted in this lane was identified
     *
     * A backend whose tag reader answers after the lane's presence rises
     * reports Pending until it does, so an insert is not taken as unnamed
     * before the reader has spoken.
     */
    [[nodiscard]] virtual InsertIdentity slot_insert_identity(int slot_index) const = 0;

    /**
     * @brief The presence reading the insert watcher judges on this lane
     *
     * An insert is this reading's empty to present edge. nullopt means the lane
     * cannot tell an insert. A backend whose status also moves for reasons other
     * than a spool going in (a tool parking, firmware metadata) answers from the
     * sensor itself.
     */
    [[nodiscard]] virtual std::optional<bool> slot_insert_presence(int slot_index) const = 0;

  protected:
    ~IInsertSignals() = default;
};

} // namespace helix
