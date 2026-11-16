// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h" // helix::wizard::StepId

#include <cstddef>
#include <functional>
#include <vector>

namespace helix {
class Config;
}

/**
 * Wizard Container - Responsive Multi-Step UI Component
 *
 * Clean separation: This component handles ONLY navigation and layout.
 * Screen content and business logic belong in the wizard screen components.
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML components (globals.xml, wizard_container.xml, all wizard_*.xml)
 *   2. ui_wizard_init_subjects()
 *   3. ui_wizard_register_event_callbacks()
 *   4. ui_wizard_container_register_responsive_constants()  <- BEFORE creating XML
 *   5. ui_wizard_create(parent)
 *   6. ui_wizard_navigate_to_step(1)
 */

/**
 * Initialize wizard subjects
 *
 * Creates and registers reactive subjects for wizard state:
 * - current_step, total_steps (int)
 * - wizard_title, wizard_subtitle, wizard_step_current, wizard_step_total (string)
 * - wizard_total_visible, wizard_is_final_step, wizard_back_visible,
 *   wizard_show_skip, connection_test_passed (int flags)
 *
 * MUST be called BEFORE creating XML components.
 */
void ui_wizard_init_subjects();

/**
 * Deinitialize wizard subjects
 *
 * Disconnects observers from all wizard subjects before shutdown.
 * Called by StaticPanelRegistry during Application::shutdown().
 */
void ui_wizard_deinit_subjects();

/**
 * Register responsive constants to wizard_container scope and propagate to children
 *
 * Registers wizard_button_width to the wizard_container scope, then propagates it
 * to every wizard step component. Uses parent-defined constants pattern to avoid
 * polluting globals scope.
 *
 * wizard_button_width by the display's larger axis: 90 (≤272), 110 (≤460),
 * 140 (≤550), 160 above.
 *
 * MUST be called AFTER all wizard_*.xml components are registered and BEFORE ui_wizard_create().
 */
void ui_wizard_container_register_responsive_constants();

/**
 * Register event callbacks
 *
 * Registers internal navigation callbacks:
 * - on_back_clicked
 * - on_next_clicked
 *
 * MUST be called BEFORE creating XML components.
 */
void ui_wizard_register_event_callbacks();

/**
 * Create wizard container
 *
 * Creates the wizard UI from wizard_container.xml.
 * Returns the root wizard object.
 *
 * Prerequisites:
 * - ui_wizard_init_subjects() called
 * - ui_wizard_register_event_callbacks() called
 * - ui_wizard_container_register_responsive_constants() called
 *
 * @param parent Parent object (typically screen root)
 * @return The wizard root object, or NULL on failure
 */
lv_obj_t* ui_wizard_create(lv_obj_t* parent);

/**
 * Navigate to specific step
 *
 * Updates all wizard subjects (title, progress, button text).
 * Handles back button visibility (hidden on step 1).
 *
 * @param step Wizard step id from the step registry.
 */
void ui_wizard_navigate_to_step(helix::wizard::StepId step);

/**
 * Set wizard title
 *
 * Updates the wizard_title subject.
 *
 * @param title New title string
 */
void ui_wizard_set_title(const char* title);

/**
 * Refresh wizard header translations
 *
 * Re-translates the title and subtitle for the current step using lv_tr().
 * Should be called after language changes to update bound subjects with
 * newly translated text.
 */
void ui_wizard_refresh_header_translations();

/**
 * Record the hardware the current selections imply into hardware/expected
 *
 * Reads the heater/fan/LED role keys, the runout sensor chosen in the filament
 * step, and the live AMS backend's Klipper object, and adds each to this
 * printer's hardware/expected list. Also enables the home panel's AMS widget
 * when an AMS is present.
 *
 * Called by ui_wizard_complete() and again when a deferred hardware setup is
 * re-run as a targeted session (#1160) — ui_wizard_complete_targeted()
 * deliberately does no expected-hardware population of its own.
 *
 * @param config Config to read selections from and write the snapshot to
 * @return Number of hardware names recorded (0 = nothing was selected)
 */
size_t ui_wizard_record_expected_hardware(helix::Config* config);

/**
 * Hardware steps to re-run for a deferred setup (#1160)
 *
 * Evaluates the live skip vector (preset collapsing, "no AMS", "no LEDs",
 * sparse filament sensors) and returns the hardware steps that still have
 * something to ask, in wizard order. Empty means there is nothing to offer.
 *
 * A targeted session runs its list verbatim, so this filtering has to happen
 * before ui_wizard_create_targeted() is called.
 */
std::vector<helix::wizard::StepId> ui_wizard_deferred_hardware_steps();

/**
 * Complete wizard and transition to main UI
 *
 * Called when user clicks Finish on summary screen. Performs:
 * - Cleans up all wizard screens
 * - Deletes wizard container
 * - Connects to Moonraker using saved config
 * - Transitions to main UI (already created underneath)
 *
 * NOTE: Config should already be saved by wizard screens before calling this.
 */
void ui_wizard_complete();

/**
 * Create a targeted (subset) wizard session
 *
 * Launches the wizard running ONLY the requested step(s), in the given order,
 * for hardware reconfiguration WITHOUT re-running the full first-run wizard.
 * Reuses the standard container/subject setup (ui_wizard_create) and then
 * navigates to the first step in @p steps. While a targeted session is active,
 * the Next/Back navigation advances/retreats only within @p steps; finishing
 * the last step calls ui_wizard_complete_targeted() (which does NOT set the
 * wizard_completed flag) and fires @p on_complete.
 *
 * @param parent      Parent object (typically screen root)
 * @param steps       Ordered subset of wizard steps to run (non-empty)
 * @param on_complete Callback invoked after the subset finishes and the
 *                    container is torn down (typically returns to prior panel)
 * @return The wizard root object, or NULL on failure
 */
lv_obj_t* ui_wizard_create_targeted(lv_obj_t* parent, std::vector<helix::wizard::StepId> steps,
                                    std::function<void()> on_complete);

/**
 * Query the active targeted step subset
 *
 * Returns the ordered subset passed to ui_wizard_create_targeted(). Empty when
 * not running a targeted session (i.e. the normal first-run wizard, or no
 * wizard running).
 */
const std::vector<helix::wizard::StepId>& ui_wizard_active_step_subset();

/**
 * Complete a targeted (subset) wizard session
 *
 * Finishes targeted mode WITHOUT writing the wizard_completed flag and without
 * the expected-hardware population done by ui_wizard_complete(). Tears down the
 * wizard container (shared with ui_wizard_complete()), clears the active subset,
 * and fires the on_complete callback registered by ui_wizard_create_targeted().
 */
void ui_wizard_complete_targeted();
