// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_gcode_guards.h
 * @brief Shared send-layer guards for outgoing G-code.
 *
 * Three send paths need the same refusal logic (moonraker_client.cpp,
 * moonraker_api_controls.cpp, moonraker_motion_api.cpp); this header is the
 * single source of truth.
 *
 * NOTE: outgoing G-code is transmitted VERBATIM — HelixScreen adds nothing to
 * it, not even a trailing comment. An earlier " ; from helixscreen" provenance
 * tag lived here and broke three separate things (M117/M118 payloads, the AD5X
 * quoted-RESPOND echo, and Kalico/Klipper macros that branch on `rawparams`).
 * tests/unit/test_gcode_verbatim.cpp is the regression gate; read its header
 * before considering any outgoing-G-code rewriting.
 */

#pragma once

#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "moonraker_error.h"

#include <functional>
#include <string>

namespace helix {
class PrinterState;
}

namespace helix::api {

/// Reject app-initiated homing while a print is active (PRINTING or PAUSED).
/// On a loadcell-Z printer (e.g. AD5X) a mid-print G28 drives the nozzle into
/// the part. Returns true when it rejected the command (the caller must then
/// return immediately); logs a warning prefixed with @p log_tag (unless
/// @p silent) and invokes @p on_error with a NOT_READY error. Returns false to
/// let the command through.
bool reject_homing_during_active_print(
    const std::string& gcode, helix::PrinterState& state, bool silent,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error, const char* log_tag);

/// Reject a script while spools lie on the bed, unless every line is on the
/// spool latch allowlist (spool_latch_gate.h). Same contract as the homing
/// guard: true means refused and @p on_error already called
/// (prestonbrown/helixscreen#1730).
bool reject_motion_while_spools_on_bed(
    const std::string& gcode, helix::PrinterState& state, bool silent,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error, const char* log_tag);

/// Reject a Klipper or firmware restart while spools lie on the bed, with the
/// restart wording (spool_latch_restart_message). @p state may be null.
bool reject_restart_while_spools_on_bed(
    const helix::PrinterState* state, const char* method,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error);

/// Reject a print start, resume or queue start while spools lie on the bed.
/// @p state may be null (a sub-API built without one), which refuses nothing.
bool reject_job_while_spools_on_bed(
    const helix::PrinterState* state, const char* method,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error);

#if HELIX_HAS_PLUGINS
/// Apply the refusals the app's own send paths enforce to a raw JSON-RPC request issued on a
/// plugin's behalf. Covers print start (path and spool latch), resume, queue start, modified
/// and edited print, Klipper restarts and `printer.gcode.script`, and refuses every
/// `server.helix.*` method except print_modified, print_edited and the read-only ones
/// (helix::plugin::is_readonly_moonraker_method). Every other method passes. Returns true
/// when refused, with @p on_error already called and nothing to send.
bool reject_plugin_rpc(helix::PrinterState& state, const std::string& method,
                       const nlohmann::json& params,
                       const helix::SharedCallback<void(const MoonrakerError&)>& on_error);
#endif

} // namespace helix::api
