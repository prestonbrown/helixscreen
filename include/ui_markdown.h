// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lv_markdown_style.h"
#include "lvgl/lvgl.h"

/**
 * @file ui_markdown.h
 * @brief Markdown viewer XML widget with theme-aware styling and subject binding
 *
 * Wraps the lv_markdown library as an XML-usable widget. Supports:
 * - Theme-aware rendering (colors from design tokens)
 * - Subject binding via bind_text attribute
 * - Static text via text attribute
 * - Automatic cleanup via RAII user data
 *
 * Usage in XML:
 *   <ui_markdown bind_text="update_release_notes" width="100%"/>
 *   <ui_markdown text="# Hello\nSome **bold** text" width="100%"/>
 *
 * The widget uses LV_SIZE_CONTENT for height, so it grows to fit content.
 * Wrap in a scrollable container for long content.
 */

/**
 * @brief Initialize the ui_markdown custom widget
 *
 * Registers the <ui_markdown> XML widget with LVGL's XML parser.
 * Must be called after lv_xml_init() and after theme is initialized.
 */
void ui_markdown_init();

/**
 * @brief True when @p obj is a <ui_markdown> widget
 *
 * lv_markdown keeps its heap state in user_data and frees it by reading the slot
 * back on delete, so a teardown path that nulls user_data across a tree must skip
 * these widgets (see Modal::disarm_tree).
 */
// NAMESPACE_OK: beside this widget's global C API
bool ui_markdown_owns_user_data(const lv_obj_t* obj);

/**
 * @brief Populate a lv_markdown_style_t from the current theme tokens
 *
 * Maps responsive font tokens (font_body, font_body_bold, font_heading,
 * font_xl, font_small) and color tokens (text, text_subtle, text_muted,
 * primary, secondary) onto the lv_markdown style so the rendered markdown
 * matches the app's look across breakpoints and light/dark themes.
 *
 * Exposed for unit-testing the token wiring; production callers should
 * use the <ui_markdown> XML widget, which builds and applies the style
 * internally.
 *
 * @param style Output struct; overwritten in full.
 */
void ui_markdown_get_theme_style(lv_markdown_style_t& style);
