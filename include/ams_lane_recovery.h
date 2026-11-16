// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_lane_recovery.h
 * @brief Capability of a filament system that can retract a lane stranded mid-path
 *
 * Reached with AmsBackend::as<ILaneRecovery>(). AFC implements it with AFC_LANE_RESET.
 */

#pragma once

#include "ams_error.h"

namespace helix {

class ILaneRecovery {
  public:
    /**
     * @brief Retract a lane's filament back to its lane from the bowden
     *
     * A physical filament move, not a fault clear. Recovers a lane left stranded
     * mid-path by a failed load or unload: filament past the lane but not at the
     * toolhead, which plain unload() cannot address because it assumes the head.
     *
     * @param slot_index Lane to recover (0-based)
     * @return AmsError indicating if the operation was started
     */
    virtual AmsError recover_lane_position(int slot_index) = 0;

    /**
     * @brief Whether a lane-position recovery is possible for this slot right now
     *
     * Per-slot and per-state, not a static capability: offering a recovery the
     * firmware will refuse produces an error the user cannot act on, and on AFC
     * that error latches in printer.AFC.message and keeps re-firing toasts.
     */
    [[nodiscard]] virtual bool can_recover_lane_position(int slot_index) const = 0;

    /**
     * @brief Whether this backend currently knows WHICH lane needs recovery
     *
     * Distinct from can_recover_lane_position(), which answers "is recovery
     * possible" per slot. This answers "do we know whose fault it is": some
     * backends share one physical sensor across every lane on a unit (AFC's hub
     * sensor), so an unattributed trigger cannot say whose filament caused it.
     * Callers use this to decide whether RecoverPosition should outrank Eject
     * (attributed: confident, single lane) or defer to it (unattributed: a
     * last-resort offer, since showing Recover on every lane would otherwise
     * hide Eject from lanes that are simply seated, not stranded).
     *
     * A backend MAY choose to make can_recover_lane_position() imply this, by
     * refusing recovery outright where it cannot attribute the strand. AFC does
     * (prestonbrown/helixscreen#1182): its lane reset opens with a blind retract,
     * so a wrong guess de-seats a working lane rather than refusing harmlessly.
     * The unattributed arm in the caller's ranking stays valid for backends
     * whose recovery is genuinely free to attempt.
     */
    [[nodiscard]] virtual bool lane_recovery_is_attributed() const = 0;

  protected:
    ~ILaneRecovery() = default;
};

} // namespace helix
