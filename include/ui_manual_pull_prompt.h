// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ui_manual_pull_prompt.h
 * @brief Tell the user to pull the filament out when nothing else will.
 *
 * An AMS lane unload reels the filament back down into its own lane and the
 * user has nothing to do. Two unloads have no lane to retract into — the
 * bypass / external spool, and a printer with no AMS backend at all — and both
 * end with the filament parked just above the extruder and the rest of it still
 * threaded up the tube. The backend's job finishes there; somebody has to say so
 * out loud, or the user watches an operation "complete" with filament still
 * visibly in the machine.
 *
 * unload_needs_manual_pull() (filament_op_dispatch.h) answers WHICH unloads
 * qualify. This answers WHEN to speak, which is not the same instant on every
 * printer:
 *
 *   - With a toolhead filament sensor, the moment it goes clear. That is the
 *     physically meaningful edge — the filament is above the sensor, past the
 *     gears, and safe to pull. It also lands well before the backend calls the
 *     operation done.
 *   - Without one, when the caller reports the unload finished.
 *
 * Both paths are live at once and the first to fire wins, so no caller has to
 * ask whether this printer has a sensor. A printer without one leaves that
 * subject at -1, the edge never comes, and completion carries the prompt.
 *
 * Usage — three calls, all on the main LVGL thread (every site is a button
 * handler or a queued completion, so that is free):
 *
 *     if (unload_needs_manual_pull(backend != nullptr, slot)) {
 *         arm_manual_pull_prompt();
 *     }
 *     ...
 *     on success:  manual_pull_unload_finished();
 *     on failure:  disarm_manual_pull_prompt();
 */

namespace helix::ui {

/**
 * @brief Start watching for the filament to clear the toolhead.
 *
 * Only fires on a genuine 1 -> 0 transition of the toolhead sensor. Arming while
 * the sensor already reads clear (or while the printer has none) deliberately
 * leaves the sensor path dead: the retract has not happened yet, so filament the
 * user was told to pull would still be gripped by the gears.
 * manual_pull_unload_finished() is what speaks in that case.
 *
 * Re-arming replaces any previous arming rather than stacking.
 */
void arm_manual_pull_prompt();

/**
 * @brief Report that the unload finished; prompt now if the sensor never spoke.
 *
 * A no-op when not armed, or when the sensor edge already fired the prompt.
 */
void manual_pull_unload_finished();

/**
 * @brief Drop the prompt without firing it. Safe when not armed.
 *
 * For a dispatch that fails after arming, so a refused unload cannot leave a
 * prompt waiting to fire on the next unrelated sensor edge.
 */
void disarm_manual_pull_prompt();

} // namespace helix::ui
