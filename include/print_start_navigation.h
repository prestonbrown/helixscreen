// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "printer_state.h"

namespace helix {

/**
 * @brief Pure decision function for print-start auto-navigation
 *
 * Returns true only on an inactive→active edge: the previous state was not
 * an active print (STANDBY/COMPLETE/CANCELLED/ERROR) and the current state
 * is (PRINTING or PAUSED). Active→active transitions (pause, resume) never
 * navigate — a user who deliberately left the print status screen mid-print
 * must not be yanked back on resume.
 */
bool print_start_nav_should_navigate(PrintJobState prev, PrintJobState current);

/**
 * @brief Initialize auto-navigation to print status panel on print start
 *
 * Registers an observer on PrinterState's print_state_enum subject that
 * automatically navigates to the print status panel when a print job becomes
 * active (inactive→PRINTING or inactive→PAUSED edge), regardless of which
 * panel the user is currently viewing. Also performs a level check at
 * registration time: if the job is already active (e.g. firmware power-loss
 * recovery restored a PAUSED job before initial connect completed, #1099),
 * the overlay push is queued immediately.
 *
 * This handles the case where a print is started externally (via Mainsail,
 * OrcaSlicer, API, etc.) - the display reacts appropriately by showing
 * the print status overlay.
 *
 * Safe to call even when starting prints from the UI - the observer checks
 * if print status is already showing and won't double-navigate.
 *
 * @return ObserverGuard that manages the observer's lifetime
 */
ObserverGuard init_print_start_navigation_observer();

} // namespace helix
