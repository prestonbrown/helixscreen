// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>

namespace helix {

/**
 * @brief Klipper firmware state (klippy_state from Moonraker)
 *
 * Represents the state of the Klipper firmware service, independent of
 * the Moonraker WebSocket connection. When klippy_state is not READY,
 * the printer cannot accept G-code commands even if Moonraker is connected.
 */
enum class KlippyState {
    READY = 0,    ///< Normal operation, printer ready for commands
    STARTUP = 1,  ///< Klipper is starting up (during RESTART/FIRMWARE_RESTART)
    SHUTDOWN = 2, ///< Emergency shutdown (M112)
    ERROR = 3     ///< Klipper error state (check klippy.log)
};

/**
 * @brief Map a Klipper state string (webhooks.state, printer.info "state") to KlippyState
 *
 * Klipper documents exactly ready/startup/shutdown/error. Anything else, such as
 * Moonraker's own "disconnected", is nullopt: inventing a state for it would be
 * fail-open on a liveness signal.
 */
std::optional<KlippyState> parse_klippy_state(std::string_view state);

/// The inverse of parse_klippy_state(): Klipper's own name for `state`.
const char* klippy_state_name(KlippyState state);

/**
 * @brief Whether a Klipper state_message reports a printer.cfg that failed to load
 *
 * Matches the wording of Klipper's own config errors (configfile.py, pins.py):
 * an option or section the config got wrong. A restart cannot clear one of these
 * until the file is fixed, which is what the recovery dialog has to tell the user.
 */
bool is_klippy_config_error(std::string_view state_message);

/**
 * @brief Whether a Klipper state_message reports an MCU it could not reach at startup
 *
 * Klipper's wording from mcu.py and serialhdl.py ("mcu 'mcu': Unable to connect").
 * This is what an unpowered mainboard looks like from the host.
 */
bool is_klippy_mcu_connect_error(std::string_view state_message);

} // namespace helix
