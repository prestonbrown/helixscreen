// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl/lvgl.h>

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
 * because ui_notification.o is deliberately excluded from the test link
 * (mk/tests.mk — ui_test_utils.cpp stubs it), so anything defined there is
 * unreachable from a unit test. The registry is the part with actual logic, so
 * it belongs where tests can drive the real code.
 */

namespace helix::ui {

/**
 * @brief Register a dialog as describing a printer-side fault
 *
 * The dialog is dropped from the registry automatically when it is deleted by
 * any route (user acknowledgement, backdrop tap, this sweep, teardown), so
 * callers never have to unregister.
 *
 * **Main thread only.** No-op on nullptr, so a failed modal_show_alert() can be
 * passed straight through.
 */
void track_fault_modal(lv_obj_t* dialog);

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
 * @brief Number of dialogs currently tracked (live or not). Test seam.
 */
int tracked_fault_modal_count();

} // namespace helix::ui
