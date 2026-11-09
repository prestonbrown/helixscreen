// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

/**
 * @brief Correlation between "the router surfaced this fault" and
 *        AmsErrorBridge's last-resort fallback.
 *
 * One physical fault reaches the user through two independent observers:
 *
 *  (a) GcodeErrorRouter, off Klipper's `!!` broadcast — classified, then
 *      presented as a modal or a toast;
 *  (b) AmsErrorBridge, off the AmsAction::ERROR edge in printer status — a
 *      backstop that toasts `operation_detail` when nothing else surfaced
 *      the fault at all (AFC's stuck-action timeout raises ERROR with no
 *      `ErrorEvent` and no `!!` line, so the spinner would otherwise just
 *      stop, which reads as success).
 *
 * The bridge's "is this already visible" check can see the shared
 * RecoveryModalPresenter and AmsPanel's error dialog, but it cannot see a
 * toast — `ToastManager::is_visible()` is blanket, and using it would let any
 * unrelated toast silence a genuine fault. Nor can it see the plain
 * `ui_notification_error(..., modal=true)` the router raises for a CRITICAL
 * event with no recovery action. This registry closes both gaps by carrying
 * the one thing the bridge lacks: an identity for *which* fault got surfaced.
 *
 * The registry is deliberately one-directional in effect:
 *
 *  - the router records everything it surfaces, in both `detail` and
 *    `raw_detail` form, because the bridge holds AFC's `operation_detail` and
 *    only ever sees one of the two spellings;
 *  - the bridge records its own fallback toast, and the router consults the
 *    registry only on its TOAST arms. A modal is never suppressed by a prior
 *    toast — the modal carries the recovery actions, so downgrading it to
 *    "already covered" would lose the only actionable thing on screen.
 *
 * The bridge's match is EXACT-STRING, same reasoning as rpc_error_correlation: AFC emits
 * `!! <msg>` and queues the byte-identical string into `AFC.message`, so the
 * two channels agree exactly. Substring matching would let one fault mask an
 * unrelated one that happens to share a phrase, which is precisely the
 * "suppressed a genuine second fault" failure this must not have.
 *
 * The window (3 s) spans the causal gap between the channels: AFC emits `!!`
 * and only then calls `pause_print()`, so the status delta that raises
 * error_state — and with it the bridge's ERROR edge — trails the broadcast by
 * a Moonraker round trip. It is far too short to mask a fault the user could
 * have acted on in between.
 */
namespace helix::fault_surface_correlation {

/// Record that `detail` was put in front of the user by some surfacing path.
/// Empty strings are ignored.
void record_surfaced(const std::string& detail);

/// True if `detail` exactly matches something surfaced inside the window.
/// Always prunes expired entries before checking.
[[nodiscard]] bool was_recently_surfaced(const std::string& detail);

/// True if `detail` is, or wraps, something surfaced inside the window: the
/// surfaced text appears in it as a whole segment, starting the text, a line,
/// or the part after a ": ", and ending the text or a line. Firmware often
/// raises a cause and then the same cause under its context ("X", then
/// "Filament change error: X"); the wrapper adds nothing a second toast should
/// show. A shared phrase inside a sentence is not a segment, so an unrelated
/// fault that merely mentions it still surfaces. The router's toast arms use
/// this; the bridge keeps the exact match.
[[nodiscard]] bool was_recently_surfaced_within(const std::string& detail);

/// Whether `needle` is a whole segment of `text`, as was_recently_surfaced_within()
/// reads one.
[[nodiscard]] bool contains_as_segment(const std::string& text, const std::string& needle);

/// Drop all records — for tests. Called from HelixTestFixture::reset_all().
void clear_for_test();

} // namespace helix::fault_surface_correlation
