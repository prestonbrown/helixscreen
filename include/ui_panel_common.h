// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

/**
 * @file ui_panel_common.h
 * @brief Common helper utilities for panel setup to reduce boilerplate
 *
 * Provides reusable functions for standard panel setup patterns:
 * - Header bar configuration with responsive height
 * - Content area padding (responsive vertical, fixed horizontal)
 * - Resize callback registration
 * - Standard back button event handlers
 */

// ============================================================================
// HEADER BAR SETUP
// ============================================================================

/**
 * @brief Setup header bar with responsive height
 *
 * Finds the header bar widget by name within the panel and configures it
 * for responsive height based on screen size.
 *
 * @param panel Panel object containing the header bar
 * @param parent_screen Parent screen object for measuring screen height
 * @param header_name Name of the header bar widget (e.g., "motion_header")
 * @return Pointer to header bar widget if found, nullptr otherwise
 */
lv_obj_t* ui_panel_setup_header(lv_obj_t* panel, lv_obj_t* parent_screen, const char* header_name);

// ============================================================================
// CONTENT PADDING SETUP
// ============================================================================

/**
 * @brief Setup responsive padding for content area
 *
 * Configures content area with responsive vertical padding (varies by screen size)
 * and responsive horizontal padding using the space_md token.
 *
 * Pattern used across all panels:
 * - Vertical (top/bottom): space_lg (12/16/20px at small/medium/large)
 * - Horizontal (left/right): space_md (8/10/12px at small/medium/large)
 *
 * @param panel Panel object containing the content area
 * @param parent_screen Parent screen object for measuring screen height
 * @param content_name Name of the content area widget (e.g., "motion_content")
 * @return Pointer to content area widget if found, nullptr otherwise
 */
lv_obj_t* ui_panel_setup_content_padding(lv_obj_t* panel, lv_obj_t* parent_screen,
                                         const char* content_name);

// ============================================================================
// RESIZE CALLBACK SETUP
// ============================================================================

/**
 * @brief Context for panel resize callbacks
 *
 * Stores panel state needed for resize operations. Pass this to
 * ui_panel_setup_resize_callback() to automatically handle content padding
 * updates on window resize.
 */
struct ui_panel_resize_context_t {
    lv_obj_t* panel;          ///< Panel object
    lv_obj_t* parent_screen;  ///< Parent screen object
    const char* content_name; ///< Name of content area widget
};

/**
 * @brief Setup standard resize callback for content padding
 *
 * Registers a resize callback that automatically updates content padding
 * when the window is resized. The context object must remain valid for
 * the lifetime of the panel.
 *
 * Pattern: Each panel has a static resize context and callback that updates
 * vertical padding responsively while keeping horizontal padding constant.
 *
 * @param context Pointer to resize context (must be static/persistent)
 */
void ui_panel_setup_resize_callback(ui_panel_resize_context_t* context);

// ============================================================================
// OVERLAY PANEL SETUP (For panels using overlay_panel.xml wrapper)
// ============================================================================

namespace helix::ui {
/**
 * @brief Create an XML component that is hidden for its whole construction
 *
 * A widget built during lv_xml_create() can force a layout pass (a bound
 * dropdown scrolls to its selection). If the root is visible then, its full
 * area is invalidated and the next refresh redraws it even though the caller
 * hides it on return, so an overlay open pays for one full-screen render
 * before NavigationManager::push_overlay() shows it and pays for another.
 * Building under a hidden holder keeps every invalidation of the new tree off
 * the display.
 *
 * @return The component root, hidden and parented to @p parent, or nullptr
 */
lv_obj_t* create_xml_hidden(lv_obj_t* parent, const char* component, const char** attrs = nullptr);

/**
 * @brief Create an XML component that appears already at its final size
 *
 * LVGL sizes children before their parents, so a new tree's first layout sees
 * width:100% parents still 0 wide: a wrapping label is one glyph a line and
 * hundreds of px tall, and height:content carries that up the tree. A visible
 * root invalidates every one of those sizes, most of the screen for a toast.
 * Laid out while hidden, only the final area is redrawn.
 *
 * @return The component root, visible and parented to @p parent, or nullptr
 */
lv_obj_t* create_xml_laid_out(lv_obj_t* parent, const char* component,
                              const char** attrs = nullptr);
} // namespace helix::ui

/**
 * @brief Standard setup for overlay panels using overlay_panel.xml wrapper
 *
 * Overlay panels use the overlay_panel.xml component which provides:
 * - Integrated header_bar with back button (wired via XML event_cb)
 * - Right-aligned positioning
 * - Content area with responsive padding
 *
 * NOTE: Back button wiring is handled by header_bar.xml via XML event_cb.
 * Do NOT add C++ event handlers for back buttons - it causes double navigation.
 *
 * @param panel Overlay panel root object
 * @param parent_screen Parent screen for measuring dimensions
 * @param header_name Name of header_bar widget (default: "overlay_header")
 * @param content_name Name of content area (default: "overlay_content")
 */
void ui_overlay_panel_setup_standard(lv_obj_t* panel, lv_obj_t* parent_screen,
                                     const char* header_name = "overlay_header",
                                     const char* content_name = "overlay_content");

/**
 * @brief Wire action button in overlay panel header_bar
 *
 * Finds the action button within the header_bar and wires it to the provided
 * callback. Used for confirm/save/action buttons in overlay panels.
 *
 * @param panel Overlay panel root object
 * @param callback Event callback for button click
 * @param header_name Name of header_bar widget (default: "overlay_header")
 * @param user_data Optional user data to pass to callback (default: nullptr)
 * @return Pointer to action button if found, nullptr otherwise
 */
lv_obj_t* ui_overlay_panel_wire_action_button(lv_obj_t* panel, lv_event_cb_t callback,
                                              const char* header_name = "overlay_header",
                                              void* user_data = nullptr);
