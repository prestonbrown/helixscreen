// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file xml_registration.h
 * @brief XML component registration: a short eager list, the rest on first use
 *
 * @threading Main thread only; must complete before UI creation
 * @note Fonts and images are registered via AssetManager::register_fonts/images()
 */

#pragma once

#include <string>

namespace helix {

/**
 * @brief Register XML components from ui_xml/ directory
 *
 * Registers the components that must exist before their first use and
 * installs the loader for the rest.
 * Must be called after AssetManager initialization and theme init.
 */
void register_xml_components();

/**
 * @brief Resolve the HSV picker's consts (sv_size, hue_height, swatch_size) for
 *        the default display's current resolution
 *
 * register_xml_components() runs this at boot; the consts do not follow a later
 * theme_manager_refresh_layout_constants().
 */
void refresh_color_picker_constants();

/**
 * @brief Register each XML component the first time anything names it
 *
 * Installs the engine's component loader: a tag, extends= base, lv_xml_create()
 * or lv_xml_component_get_scope() naming an unregistered component registers
 * ui_xml/<name>.xml or ui_xml/components/<name>.xml, layout variant applied.
 * register_xml_components() calls it; a test fixture that skips that calls it
 * directly.
 */
void register_xml_on_first_use();

/**
 * @brief Remove the first-use loader
 *
 * Process exit only: teardown then looks components up without loading any.
 * A printer switch keeps it, since the next session builds its UI through it.
 */
void stop_xml_on_first_use();

/**
 * @brief Mark a component whose scope C++ extends after it registers
 *
 * Call it from code that pushes constants into a component scope. Neither a
 * resize (unregister_idle_xml_components) nor a hot reload replaces a marked
 * component with a fresh copy from its file, which would lose them.
 */
void keep_xml_component_registered(const char* name);

/// True for a component marked by keep_xml_component_registered(), and for globals.
bool is_xml_component_cpp_extended(const std::string& name);

/**
 * @brief Drop first-use components no live widget is built from
 *
 * A component's px tokens and its layout variant are fixed when it registers.
 * After a resize or layout change, unregistering the idle ones lets their next
 * use register them again at the new geometry. A component with live
 * instances, constants or a subject registered from C++, or a style another
 * scope borrowed keeps its registration.
 */
void unregister_idle_xml_components();

/**
 * @brief Deinitialize XML-related subjects
 *
 * Must be called during shutdown before lv_deinit().
 * Called by StaticPanelRegistry.
 */
void deinit_xml_subjects();

} // namespace helix
