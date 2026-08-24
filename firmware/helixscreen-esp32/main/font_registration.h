// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Registers the medium-tier (800x480 K-Touch) compiled font faces with
// helix-xml's global font registry under their token names (e.g.
// "noto_sans_26"), matching desktop's AssetManager::register_fonts(). MUST
// be called before theme init — theme_manager_register_responsive_fonts()
// resolves globals.xml font tokens (font_heading_medium, etc.) to these
// names via lv_xml_get_font() and hard-aborts on a miss (audit rule).
void helix_fonts_register(void);

// Re-logs the runtime .bin font-load results (per-face ms / failure). The
// loads happen ~2s into boot, inside the WiFi RF-cal power dip that drops the
// CH340 off USB — the original log lines land in a dead serial window, so
// app_boot calls this at home-panel-up where serial is reliable.
void helix_fonts_log_summary(void);
