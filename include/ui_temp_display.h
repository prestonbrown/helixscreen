// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

/**
 * @file ui_temp_display.h
 * @brief Reusable temperature display widget with 4-state color coding
 *
 * The temp_display widget provides a standardized way to show temperature
 * information across all panels. It displays current temp with optional target
 * in the format "210 / 245°C" or "210°C" (current only).
 *
 * ## Color States (4-state logic)
 *
 * The widget automatically colors the current temp based on thermal state:
 * - **Off** (gray): Heater disabled (target=0)
 * - **Heating** (red): Actively heating (current < target - 2°C)
 * - **At-temp** (green): Stable at target (within ±2°C)
 * - **Cooling** (blue): Cooling toward target (current > target + 2°C)
 *
 * ## XML Usage
 *
 * Current-only display (default - target hidden):
 * @code{.xml}
 * <temp_display name="chamber_temp" size="sm"
 *               bind_current="chamber_temp"/>
 * @endcode
 *
 * Full display with target (opt-in):
 * @code{.xml}
 * <temp_display name="nozzle_temp" size="lg" show_target="true"
 *               bind_current="extruder_temp"
 *               bind_target="extruder_target"/>
 * @endcode
 *
 * Clickable temperature display:
 * @code{.xml}
 * <temp_display name="nozzle_temp" size="lg" show_target="true"
 *               bind_current="extruder_temp"
 *               bind_target="extruder_target"
 *               event_cb="on_nozzle_temp_clicked"/>
 * @endcode
 *
 * ## XML Properties (API)
 *
 * | Property       | Type    | Default | Description                                      |
 * |----------------|---------|---------|--------------------------------------------------|
 * | size           | string  | "md"    | Font size: "sm", "md", or "lg"                   |
 * | show_target    | bool    | "false" | Show "/ target" portion (opt-in)                 |
 * | align_current  | bool    | "false" | Current reading at a fixed, right-aligned width  |
 * |                |         |         | so stacked displays line up at the separator     |
 * | bind_current   | subject | -       | Subject name for current temp (decidegrees×10) |
 * | bind_target    | subject | -       | Subject name for target temp (decidegrees×10)  |
 * | bind_unit      | subject | -       | String subject for the unit text (the user's
 * temperature unit unbound; empty = none) | | bind_raw | subject | - | Int subject: nonzero shows
 * the reading unconverted, for a bind_unit of the caller's own | | bind_mode      | subject | - |
 * Subject name for ChamberMode int (cooling-ceiling color in Maintaining) | | event_cb       |
 * string  | ""      | Click callback name (makes widget clickable)     |
 *
 * @note When show_target="true" and target=0, displays "--" instead of "0"
 *
 * @note The widget expects temperature subjects in **decidegrees** (value×10),
 *       which matches PrinterState's format. The widget converts internally:
 *       - Subject value 2050 → displays as "205°C"
 *       - Subject value 600  → displays as "60°C"
 *
 * ## Size Mapping
 *
 * | Size | Font Token    | Typical Use                      |
 * |------|---------------|----------------------------------|
 * | sm   | font_small    | Compact displays, status bars    |
 * | md   | font_body     | Standard temperature readouts    |
 * | lg   | font_heading  | Hero displays, main panels       |
 *
 * ## Styling
 *
 * The widget uses semantic colors from the theme:
 * - Current temp: 4-state color (see Color States above)
 * - Separator (" / "): text_muted
 * - Target temp: text_primary
 * - Unit ("°C"): text_muted
 *
 * Standard lv_obj style properties can also be applied (align, width, etc.)
 *
 * ## C++ Usage
 *
 * @code{.cpp}
 * // Find widget and set values directly (without subjects)
 * lv_obj_t* temp = lv_obj_find_by_name(panel, "nozzle_temp");
 * ui_temp_display_set(temp, 210, 245);  // Shows "210 / 245°C"
 *
 * // Update only current during heating animation
 * ui_temp_display_set_current(temp, 215);  // Shows "215 / 245°C"
 *
 * // Read values back
 * int current = ui_temp_display_get_current(temp);
 * int target = ui_temp_display_get_target(temp);
 * @endcode
 *
 * @see ui_temp_display_init() - Must be called during startup
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the temp_display widget for XML usage
 *
 * Registers the custom widget so it can be used as <temp_display> in XML.
 * Call this during UI initialization, after fonts are registered but
 * BEFORE loading any XML components that use <temp_display>.
 *
 * Called from Application::register_widgets(), after helix::ui::icon::register_widget()
 */
void ui_temp_display_init(void);

/**
 * @brief Set both current and target temperature
 *
 * Use this for direct (non-reactive) temperature updates.
 * If using bind_current/bind_target subjects, prefer updating
 * the subjects instead for automatic reactivity.
 *
 * @param obj temp_display widget
 * @param current Current temperature in °C
 * @param target Target temperature in °C
 */
void ui_temp_display_set(lv_obj_t* obj, int current, int target);

/**
 * @brief Update only the current temperature (keeps target unchanged)
 *
 * Use this for efficient updates during heating animation when
 * only the current value is changing.
 *
 * @param obj temp_display widget
 * @param current Current temperature in °C
 */
void ui_temp_display_set_current(lv_obj_t* obj, int current);

/**
 * @brief Get the current temperature value
 *
 * @param obj temp_display widget
 * @return Current temperature in °C, or -1 if not a valid temp_display
 */
int ui_temp_display_get_current(lv_obj_t* obj);

/**
 * @brief Get the target temperature value
 *
 * @param obj temp_display widget
 * @return Target temperature in °C, or -1 if not a valid temp_display
 */
int ui_temp_display_get_target(lv_obj_t* obj);

/**
 * @brief Check if this is a valid temp_display widget
 *
 * @param obj Object to check
 * @return true if this is a temp_display widget created by this module
 */
bool ui_temp_display_is_valid(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
