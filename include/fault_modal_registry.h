// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl/lvgl.h>

#include <string>

/**
 * @file fault_modal_registry.h
 * @brief Tracks alert modals that describe a fault ON THE PRINTER (#1266)
 *
 * An alert dialog is a statement about the world at the moment it was raised.
 * "Lost communication with MCU 'BoxTurtle'" stops being true the instant
 * Klipper comes back, and a dialog that outlives its condition is worse than no
 * dialog: it makes a healthy printer look broken, and a cascade of them has to
 * be dismissed one at a time before the screen is usable. Same lesson as #1185,
 * where the AMS error modal sat over a print that had already recovered.
 *
 * Registered dialogs are swept when the condition clears — in practice when
 * Klipper transitions back to READY, driven from EmergencyStopOverlay's
 * existing klippy_state observer.
 *
 * This lives in its own translation unit rather than inside ui_notification.cpp
 * so tests can drive the registry without raising notifications.
 */

namespace helix::ui {

/// Live fault dialogs allowed under one title; a newer one retires the oldest.
inline constexpr int kMaxFaultModalsPerTitle = 3;

/**
 * @brief Register a dialog as describing a printer-side fault
 *
 * The dialog is dropped from the registry automatically when it is deleted by
 * any route (user acknowledgement, backdrop tap, this sweep, teardown), so
 * callers never have to unregister.
 *
 * **Main thread only.** No-op on nullptr, so a failed modal_alert() can be
 * passed straight through.
 */
void track_fault_modal(lv_obj_t* dialog, const std::string& key = {});

/**
 * @brief The identity a fault dialog dedups on: its title and message with every
 *        number collapsed to "#", except inside single quotes
 *
 * One cause repeated with new numbers (coordinates, counters, line numbers, clock
 * ticks) is the same fault. A name Klipper quotes keeps its digits, so
 * "MCU 'mcu1' lost" and "MCU 'mcu2' lost" are different faults.
 */
std::string fault_dedup_key(const char* title, const char* message);

/**
 * @brief Whether a dialog tracked with @p key is still on screen, anywhere in the
 *        modal stack and not animating out
 *
 * A repeat of the same fault raised while another dialog sits on top of it must
 * not stack a second copy. **Main thread only.**
 */
bool fault_modal_showing(const std::string& key);

/**
 * @brief Dismiss every tracked fault modal still on screen
 *
 * Dialogs the user already acknowledged, and dialogs already animating out, are
 * skipped — so the return value counts real dismissals only.
 *
 * The dismissal is programmatic: the user never asked for it, so no per-modal
 * OK handler runs and nothing downstream should read it as an acknowledgement.
 * Mirrors AmsLoadingErrorModal::dismiss_silently() (#1185).
 *
 * **Main thread only.**
 *
 * @return Number of modals actually dismissed
 */
int dismiss_fault_modals();

/**
 * @brief Name the dialog that now restates the printer fault
 *
 * One fault gets one dialog. The Klipper recovery dialog shows klippy's own
 * shutdown reason plus the restart actions, so a "Printer Error" alert beside
 * it only stacks a second OK on top. Naming a carrier retires every tracked
 * fault alert, whatever fault it describes; their history rows remain. While
 * the carrier is on screen, fault_alert_gets_modal() demotes every new fault
 * to a toast and a history row.
 *
 * Pass nullptr when the carrier stops restating the fault (its text went
 * generic); modals already up are then left alone. A dialog that is no longer
 * on the stack counts as nullptr. The carrier is forgotten automatically when
 * it is deleted.
 *
 * **Main thread only.**
 */
void set_fault_carrier(lv_obj_t* dialog);

/**
 * @brief True while a fault carrier is on screen and not animating out
 *
 * **Main thread only.**
 */
bool fault_carrier_showing();

/**
 * @brief Whether an error alert should still be raised as a modal
 *
 * @p modal is what the caller asked for; a printer @p fault is demoted to a
 * toast while a fault carrier is on screen. Non-fault alerts are unaffected.
 *
 * **Main thread only.**
 */
bool fault_alert_gets_modal(bool modal, bool fault);

/**
 * @brief Number of dialogs currently tracked (live or not). Test seam.
 */
int tracked_fault_modal_count();

} // namespace helix::ui
