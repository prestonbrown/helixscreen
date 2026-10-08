// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix {

/**
 * @brief Bring up a demo overlay/modal populated with representative sample data.
 *
 * These are screens that cannot be reached by pure navigation in mock mode
 * because they only appear in response to a real printer event (a pre-print
 * filament check, a filament runout, an active print) or require configured
 * state (a lock-screen PIN). The remote-control `demo` command uses this so the
 * screenshot/automation pipeline can capture them with the real widget
 * lifecycle (init_subjects/create/on_activate), not an empty shell.
 *
 * Must be called on the LVGL/UI thread. Parent is the active screen.
 *
 * @param name One of: preflight-check, runout-modal, lock-screen,
 *             print-status, print-tune, ams-error-toast,
 *             spaghetti-detection.
 * @return true if the name was recognized and the overlay shown, false if unknown.
 */
bool show_demo_overlay(const std::string& name);

} // namespace helix
