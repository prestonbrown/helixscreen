// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ams_unclaimed_toolhead.h
 * @brief When to ask about filament at the toolhead that no lane claims.
 *
 * A filament system can see filament at its toolhead sensor while no lane is
 * loaded and the bypass is off: a spool fed by hand before the system was
 * installed, or one left behind when bypass was switched off. Nothing then
 * accounts for that filament, so HelixScreen asks once whether it is the
 * bypass spool or should be unloaded.
 */

namespace helix::ams {

/// One look at the primary filament system, as the prompt rule reads it.
struct UnclaimedToolheadReading {
    /// The backend has read its lane records and its startup grace is over.
    bool settled = false;
    /// A job holds the machine.
    bool printing = false;
    /// The backend is mid-operation (its action is not IDLE). A load trips the
    /// toolhead sensor before the backend names the lane, so a busy look says
    /// nothing about who owns the filament.
    bool busy = false;
    /// AmsBackend::toolhead_filament_unaccounted() answered true.
    bool filament_unclaimed = false;
    bool bypass_active = false;
    /// The bypass is a switch the user flips, not a sensor that sets itself.
    bool bypass_switchable = false;
};

/// Filament at the toolhead that bypass could claim and nothing else does.
[[nodiscard]] constexpr bool unclaimed_toolhead_filament(const UnclaimedToolheadReading& r) {
    return r.filament_unclaimed && !r.bypass_active && r.bypass_switchable;
}

/// What a look at the unclaimed state asks of the prompt.
enum class UnclaimedToolheadVerdict {
    None,    ///< Leave things as they are
    Open,    ///< Open the prompt
    Dismiss, ///< Close the open prompt: what it asks about is gone
};

/**
 * @brief Asks once per occurrence of the unclaimed state.
 *
 * The prompt opens on the first settled, idle look that finds the state, and
 * not again until a settled, idle look finds it cleared (a lane loads, bypass
 * goes on, the sensor clears); that look also closes a prompt still open. A
 * busy look neither opens, closes nor re-arms it. While the prompt is open no
 * second one opens, whatever resets the gate.
 */
class UnclaimedToolheadPromptGate {
  public:
    [[nodiscard]] UnclaimedToolheadVerdict observe(const UnclaimedToolheadReading& r) {
        if (!r.settled || r.busy) {
            return UnclaimedToolheadVerdict::None;
        }
        if (!unclaimed_toolhead_filament(r)) {
            armed_ = true;
            if (open_) {
                open_ = false;
                return UnclaimedToolheadVerdict::Dismiss;
            }
            return UnclaimedToolheadVerdict::None;
        }
        if (r.printing || open_ || !armed_) {
            return UnclaimedToolheadVerdict::None;
        }
        armed_ = false;
        open_ = true;
        return UnclaimedToolheadVerdict::Open;
    }

    /// The prompt closed, by either action or a dismissal.
    void prompt_closed() {
        open_ = false;
    }

    /// A different filament system, or none: ask afresh. A prompt still open
    /// stays counted, so the next look cannot stack a second one over it.
    void reset() {
        armed_ = true;
    }

    [[nodiscard]] bool prompt_open() const {
        return open_;
    }

  private:
    bool armed_ = true;
    bool open_ = false;
};

} // namespace helix::ams
