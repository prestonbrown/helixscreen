// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_spinner.h
 * @brief Custom spinner component with responsive sizing
 *
 * Provides a semantic <spinner> XML widget with consistent styling and
 * responsive sizing that adapts to screen breakpoints.
 *
 * Usage in XML:
 *   <spinner size="lg"/>   - Large (modal overlays, major operations)
 *   <spinner size="md"/>   - Medium (loading states)
 *   <spinner size="sm"/>   - Small (inline status indicators)
 *   <spinner size="xs"/>   - Extra small (compact UI)
 *
 * Size values are defined in globals.xml with _small/_medium/_large variants
 * and are auto-registered by the responsive spacing system at runtime.
 */

/**
 * @brief Initialize the spinner custom widget
 *
 * Registers the <spinner> XML widget with LVGL's XML parser.
 * Must be called after lv_xml_init() and after globals.xml is loaded.
 */
void ui_spinner_init();

/**
 * @brief Hold every spinner still (remote-control freeze) or let them run again
 *
 * A spinner restarts its animations whenever it is drawn without them; while frozen it
 * stays still instead, so a frozen capture is reproducible. Unfrozen spinners restart on
 * their next draw.
 */
namespace helix::ui {
void spinner_set_frozen(bool frozen);
} // namespace helix::ui
