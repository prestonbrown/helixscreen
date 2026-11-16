// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_gate_selector.h
 * @brief Capability of a filament system with a selector that moves between gates
 *
 * Reached with AmsBackend::as<IGateSelector>(). Happy Hare and the mock implement it.
 * BackendTraits says which of these operations a machine offers (supports_gate_select,
 * supports_gate_check, supports_lane_preload, supports_recover_with_state).
 */

#pragma once

#include "ams_error.h"

#include <string>

namespace helix {

struct RecoverStateRequest;

class IGateSelector {
  public:
    /// Move the selector to gate @p slot_index (0-based) without loading filament.
    virtual AmsError select_gate(int slot_index) = 0;

    /// Move the selector to (current gate + @p delta), clamped to the valid range, without
    /// loading filament.
    virtual AmsError move_selector(int delta) = 0;

    /// Probe gate @p slot_index's filament sensor (0-based).
    virtual AmsError check_gate(int slot_index) = 0;

    /// Probe every gate's filament sensor in one pass.
    virtual AmsError check_all_gates() = 0;

    /// Untranslated label for the sidebar's reset() button. A selector's reset() homes the
    /// mechanism, so "Reset" would collide with the recover and re-sync concept.
    [[nodiscard]] virtual std::string reset_button_label() const = 0;

    /// Feed gate @p slot_index's filament up to its park position, ready for a later load,
    /// without loading it to the toolhead.
    virtual AmsError preload_lane(int slot_index) = 0;

    /// Re-sync the firmware's tracked state to what the user says is true.
    ///
    /// State-only, like clear_fault(): tells the firmware which gate is selected and whether
    /// filament is loaded, without moving anything. Fields left unset in @p request are not
    /// asserted.
    virtual AmsError recover_with_state(const RecoverStateRequest& request) = 0;

    /// Firmware-reported bowden loading progress: 0-100, or -1 when not reported.
    [[nodiscard]] virtual int get_bowden_progress() const = 0;

  protected:
    ~IGateSelector() = default;
};

} // namespace helix
