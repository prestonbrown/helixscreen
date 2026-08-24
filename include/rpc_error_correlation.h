// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

/**
 * @brief Cross-source dedup for Klipper-rejection error reports.
 *
 * A single Klipper rejection (e.g., `RESUME` failing because exception 523 is
 * latched) emits the same error through two transport-distinct channels:
 *
 *  (a) the JSON-RPC error response to printer.gcode.script — handled by the
 *      Request Tracker, which invokes the caller's error_cb;
 *  (b) the broadcast `!! e1_filament runout` line on Klipper's gcode response
 *      stream — handled by the GcodeError handler in Application.
 *
 * Both channels arrive within the same WebSocket transport window because
 * Klipper emits them from the same gcode dispatcher call. Without dedup the
 * user sees two toasts for one event. With dedup we record the message on the
 * RPC path when someone is definitely reporting the rejection — the caller's
 * own error UI, or the tracker's generic fallback — and the `!!` handler skips
 * toasting messages that match exactly within a short causal window.
 *
 * `silent` alone does NOT record. It means "no automatic toast from us", not
 * "the user has been told"; recording on it would mute the `!!` copy for a
 * failure nobody ever saw. The decision lives in one place —
 * helix::rpc_error_policy::decide(), see include/rpc_error_policy.h.
 *
 * The window is intentionally narrow (1.5 s) because the two channels are
 * causally tied — Moonraker forwards both within milliseconds typically. The
 * window only needs to absorb network jitter, NOT mask user-visible errors
 * that legitimately arrive later.
 *
 * Match is EXACT-STRING (not substring) because Klipper sends the same error
 * payload through both channels. Substring matching would mask unrelated
 * errors that happen to share a phrase.
 */
namespace helix::rpc_error_correlation {

/// Record that the RPC error pipeline reported an error message which the
/// caller is handling on its own (via its error_cb). The GcodeError handler
/// will suppress its toast if it sees the same exact message within the
/// dedup window.
void record_caller_handled(const std::string& message);

/// True if `message` exactly matches a recently-recorded caller-handled
/// error. Always prunes expired entries before checking.
bool was_recently_handled(const std::string& message);

/// Clear all recorded messages — for tests.
void clear_for_test();

} // namespace helix::rpc_error_correlation
