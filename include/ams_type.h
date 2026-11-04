// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one enum discovery and the filament-system layer share. Its own header so a
// consumer that only names a filament system (printer_discovery.h, a capability
// question) does not pull in ams_types.h, the slot, unit, drying and endless-spool
// model behind it.

#pragma once

namespace helix {

/**
 * @brief Type of AMS system detected
 *
 * Supports both filament-switching systems (MMU/AMS) and physical tool changers.
 * Tool changers differ in that each "slot" is a complete toolhead with its own
 * extruder, rather than a filament path to a shared toolhead.
 */
enum class AmsType {
    NONE = 0,         ///< No AMS detected
    HAPPY_HARE = 1,   ///< Happy Hare MMU (mmu object in Moonraker)
    AFC = 2,          ///< AFC-Klipper-Add-On (afc object, lane_data database)
    ACE = 3,          ///< AnyCubic ACE Pro (ValgACE/BunnyACE/DuckACE Klipper drivers)
    TOOL_CHANGER = 4, ///< Physical tool changer (viesturz/klipper-toolchanger)
    AD5X_IFS = 5,     ///< FlashForge AD5X IFS (Intelligent Filament Switching)
    CFS = 6,          ///< Creality Filament System (K2 series, RS-485)
    SNAPMAKER = 7,    ///< Snapmaker U1 SnapSwap toolchanger
    QIDI_BOX =
        8, ///< QIDI Box filament changer (PLUS4, Q2, MAX4 — hub AMS, 4 slots chainable to 16)
    OPENAMS = 9, ///< klipper_openams through its own versioned oams_manager API
    INDX = 10    ///< Bondtech INDX: docked passive nozzles, one extruder and heater
};

} // namespace helix
