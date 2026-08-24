// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_icon.h
 * @brief Font-based Material Design Icons with semantic sizing and coloring
 *
 * @pattern Semantic variants (text/muted/primary/danger/etc.) with shared reactive styles
 * @threading Main thread only
 * @gotchas Must call ui_icon_register_widget() BEFORE loading icon.xml
 */

#pragma once

#include "lvgl/lvgl.h"

/**
 * Register the custom icon widget with LVGL's XML system.
 *
 * This enables the <icon> XML component to create font-based Material Design
 * Icons using lv_label with MDI font glyphs. The widget provides semantic
 * property handling for easy theming:
 *
 * Properties:
 *   - src: Icon short name (e.g., "home", "wifi", "settings")
 *   - size: Semantic size string - "xs", "sm", "md", "lg", "xl"
 *   - variant: Color variant - "text", "muted", "primary", "secondary",
 *              "tertiary", "disabled", "success", "warning", "danger",
 *              "info", "none"
 *   - color: Custom color override (e.g., "0xFF0000", "#FF0000")
 *   - clickable: Boolean to enable/disable click events ("true" or "false")
 *
 * Size mapping (uses mdi_icons_* fonts):
 *   xs: 16px (mdi_icons_16)
 *   sm: 24px (mdi_icons_24)
 *   md: 32px (mdi_icons_32)
 *   lg: 48px (mdi_icons_48)
 *   xl: 64px (mdi_icons_64)
 *
 * Variant mapping (uses shared styles from theme_core for reactive theming):
 *   text:      Primary text color (100% opacity)
 *   muted:     De-emphasized text color (100% opacity)
 *   primary:   Accent/brand color (100% opacity)
 *   secondary: Secondary accent color (100% opacity)
 *   tertiary:  Tertiary/subtle color (100% opacity)
 *   disabled:  Primary text color (50% opacity)
 *   success:   Success/green color (100% opacity)
 *   warning:   Warning/amber color (100% opacity)
 *   danger:    Danger/error/red color (100% opacity)
 *   info:      Info/blue color (100% opacity)
 *   none:      Same as "text" (default)
 *
 * Call once at application startup, BEFORE registering XML components.
 *
 * Example initialization order:
 *   ui_icon_register_widget();  // <-- Register before icon.xml
 *   lv_xml_register_component_from_file("A:ui_xml/icon.xml");
 */
void ui_icon_register_widget();

/**
 * Drop the memoized icon_font_* lookups so the next icon re-reads them.
 *
 * Each <icon size="..."> resolves its MDI face once per size from the
 * icon_font_* XML constant and keeps it for the life of the process. Those
 * constants are responsive: theme_manager_register_responsive_fonts() re-points
 * them at every breakpoint (icon_font_sm is mdi_icons_16 at micro and
 * mdi_icons_48 at xxlarge), so a cache filled at one breakpoint hands every
 * later icon a face sized for a screen that is no longer there (#1210).
 *
 * Called from theme_manager_register_responsive_fonts(); nothing else needs it.
 * Icons that already exist keep the face they were built with — the lv_font_t
 * objects are static .rodata and are never freed.
 */
void ui_icon_invalidate_font_cache();

/**
 * Change the icon source at runtime.
 *
 * @param icon       Icon widget created by ui_icon_register_widget()
 * @param icon_name  Icon short name (e.g., "home", "wifi")
 *                   Legacy "mat_*_img" names are also supported for transition.
 */
void ui_icon_set_source(lv_obj_t* icon, const char* icon_name);

/**
 * Change the icon size at runtime.
 *
 * @param icon      Icon widget
 * @param size_str  Size string: "xs", "sm", "md", "lg", or "xl"
 */
void ui_icon_set_size(lv_obj_t* icon, const char* size_str);

/**
 * Change the icon color variant at runtime.
 *
 * @param icon         Icon widget
 * @param variant_str  Variant string: "text", "muted", "primary", "secondary",
 *                     "tertiary", "disabled", "success", "warning", "danger",
 *                     "info", or "none"
 */
void ui_icon_set_variant(lv_obj_t* icon, const char* variant_str);

/**
 * Set custom color for icon at runtime.
 *
 * @param icon   Icon widget
 * @param color  LVGL color value
 * @param opa    Opacity (0-255, use LV_OPA_COVER for full opacity)
 */
void ui_icon_set_color(lv_obj_t* icon, lv_color_t color, lv_opa_t opa);

/**
 * Set clickable state for icon at runtime.
 *
 * When clickable is enabled, the icon can receive click events and be used
 * as an interactive element. Icons are non-clickable by default (they inherit
 * from lv_label which has no click flag set).
 *
 * @param icon      Icon widget
 * @param clickable true to enable click events, false to disable
 */
void ui_icon_set_clickable(lv_obj_t* icon, bool clickable);
