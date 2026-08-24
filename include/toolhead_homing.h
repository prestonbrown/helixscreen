// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

class IMoonrakerAPI;
struct MoonrakerError;

namespace helix {

class PrinterState;
class AsyncLifetimeGuard;

/**
 * @brief Whether the toolhead reports all three axes homed.
 *
 * Reads the live `homed_axes` subject, which is fed off `objects.subscribe` /
 * `notify_status_update` (src/printer/printer_motion_state.cpp:126) and seeded
 * from the subscribe response (src/api/moonraker_discovery_sequence.cpp:1487).
 * No RPC is issued.
 *
 * @warning Main thread only. This reads an LVGL subject.
 */
[[nodiscard]] bool toolhead_is_homed(const PrinterState& ps);

/**
 * @brief Home the toolhead if needed, then run a continuation.
 *
 * The synchronous "if not homed, send G28, then continue" idiom used to
 * exist as four near-identical bodies: private methods literally named
 * `ensure_homed_then` on both `BeltTensionCalibrator` and
 * `InputShaperCalibrator`, plus open-coded copies in `BedMeshPanel` (using
 * `lifetime_.bg_cb(...)`) and `ScrewsTiltPanel` (using
 * `token.expired()` + `token.defer(...)`). This is the one function that
 * replaces all four; callers supply their own `AsyncLifetimeGuard` and keep
 * any caller-specific behaviour (state resets, UI text, re-entry guards,
 * friendly-message extraction) in the `then` / `on_error` lambdas they pass
 * in.
 *
 * If `toolhead_is_homed()` is already true, `then()` runs synchronously,
 * on the caller's thread, before this function returns. Otherwise a `G28`
 * is sent at `IMoonrakerAPI::HOMING_TIMEOUT_MS`; `then()` on success or
 * `on_error()` on failure is marshalled onto the main thread through
 * `guard` (via `AsyncLifetimeGuard::bg_cb()`), so both fire safely even if
 * the owning object has since been destroyed.
 *
 * If `api` is null and homing is required, neither callback runs
 * synchronously — there is nothing to send G28 with, so `on_error()` (if
 * provided) is invoked synchronously with a `CONNECTION_LOST` error instead
 * of crashing on a null dereference.
 *
 * @warning Call from the main thread only — checks `toolhead_is_homed()`,
 * which reads an LVGL subject.
 */
void ensure_homed_then(IMoonrakerAPI* api, AsyncLifetimeGuard& guard, std::function<void()> then,
                       std::function<void(const MoonrakerError&)> on_error);

} // namespace helix
