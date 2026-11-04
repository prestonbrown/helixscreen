// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Declarations shared between the src/ui/theme_*.cpp files that implement
// theme_manager.h. Nothing here is public API: callers outside those files go
// through include/theme_manager.h.

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace helix::theme_detail {

/// Canonical ui_xml directory for token discovery, resolved once through the
/// asset-root seam. On CWD-less targets (ESP-IDF VFS) this is an absolute path
/// under the mount the firmware configured via helix::set_asset_root().
const char* ui_xml_dir();

/// Set how old a file's last change must be, when token discovery parses it,
/// before its stat is trusted to detect the next change; returns the previous
/// window. A file changed more recently is re-parsed on every lookup, since a
/// same-size rewrite inside one timestamp tick leaves its stat unchanged. A
/// negative window trusts a file the moment it is written, which is how a test
/// pins parse reuse on files it just created.
int64_t set_token_racy_window_ns(int64_t ns);

/// The size ladder in tier order (index == UiBreakpoint tier, Micro..XXLarge).
/// Every place that names a tier suffix derives from this one list.
inline constexpr const char* kSizeSuffixes[] = {"_micro", "_tiny",   "_small",  "_medium",
                                                "_large", "_xlarge", "_xxlarge"};
inline constexpr int kSizeSuffixCount = sizeof(kSizeSuffixes) / sizeof(kSizeSuffixes[0]);

/// Tier number of a size suffix, or -1 when `suffix` is not on the ladder.
int tier_for_suffix(const char* suffix);

/// True when `name` ends in a size suffix, `_light` or `_dark`: a token that has
/// a variant-selected base name, so it is not registered as a static constant.
/// A name that is only the suffix does not count.
bool has_dynamic_suffix(const std::string& name);

/// Apply the high-DPI UI scale to one authored px token. Opacities (`*_opacity`)
/// and anything that is not a bare positive integer pass through unchanged.
/// Shared by the responsive resolver and the static-constant registration so a
/// fixed-size box and the scaled glyph inside it cannot disagree.
std::string scale_px_token(const std::string& name, const std::string& value, double scale);

/// Core theme state: which display, which mode, which theme, and what the
/// repeat guard in theme_manager_init() last built. Main-thread only.
struct ThemeRuntime {
    lv_theme_t* current_theme = nullptr;
    bool dark = true;
    lv_display_t* display = nullptr;
    helix::ThemeData active_theme;

    // Repeat-guard state for theme_manager_init(): the display pointer, its
    // resolution and the mode of the last FULL rebuild, plus a count of
    // rebuilds. A repeat call for an unchanged target skips the whole
    // registration pass, which otherwise re-parses ui_xml/ from scratch.
    bool fully_initialized = false;
    int32_t init_h_res = 0;
    int32_t init_v_res = 0;
    int full_init_count = 0;
};

ThemeRuntime& runtime();

/**
 * @brief 16-color semantic palette for theme initialization (internal use)
 */
struct theme_palette_t {
    lv_color_t screen_bg;   // 0: Main app background
    lv_color_t overlay_bg;  // 1: Sidebar/panel background
    lv_color_t card_bg;     // 2: Card surfaces
    lv_color_t elevated_bg; // 3: Elevated/control surfaces (buttons, inputs)
    lv_color_t border;      // 4: Borders and dividers
    lv_color_t text;        // 5: Primary text
    lv_color_t text_muted;  // 6: Secondary text
    lv_color_t text_subtle; // 7: Hint/tertiary text
    lv_color_t primary;     // 8: Primary accent
    lv_color_t secondary;   // 9: Secondary accent
    lv_color_t tertiary;    // 10: Tertiary accent
    lv_color_t info;        // 11: Info states
    lv_color_t success;     // 12: Success states
    lv_color_t warning;     // 13: Warning states
    lv_color_t danger;      // 14: Error/danger states
    lv_color_t focus;       // 15: Focus ring color
};

/// Palette of the active theme for the current mode, falling back to whichever
/// mode the theme supports.
const helix::ModePalette& get_current_mode_palette();

theme_palette_t build_palette_from_mode(const helix::ModePalette& mode_palette);

/// Rebuild the ThemeManager palettes from the active theme and sync its mode.
void resync_palette_manager(bool is_dark);

/// Create the helix_theme (over lv_theme_default) and its shared styles.
lv_theme_t* theme_init_lvgl(lv_display_t* display, const theme_palette_t* palette, bool is_dark,
                            const lv_font_t* base_font);

/// Re-point the ThemeManager palettes and the handle styles at the active
/// theme without rebuilding the LVGL theme.
void theme_update_colors(bool is_dark);

/// XML constant registration into `scope` (null is the global scope). Constants
/// are first-wins, so callers register theme-dependent values before the static
/// ones discovered in ui_xml/.
void register_semantic_colors(lv_xml_component_scope_t* scope, const helix::ThemeData& theme,
                              bool dark_mode);
void register_theme_properties(lv_xml_component_scope_t* scope, const helix::ThemeData& theme,
                               bool dark_mode);
void register_static_constants(lv_xml_component_scope_t* scope);
void register_object_colors(lv_xml_component_scope_t* scope);
void register_color_pairs(lv_xml_component_scope_t* scope, bool dark_mode);

/// Replace the colour-swap maps used to recolour inline-styled containers:
/// every surface and border colour of `old_palette` maps to its `new_palette`
/// counterpart. A null `old_palette` leaves both maps empty.
void set_swap_maps(const helix::ModePalette* old_palette, const helix::ModePalette& new_palette);

/// The subjects the theme publishes to XML. deinit() tears them all down before
/// lv_deinit() so no widget deletion fires a stale observer callback.
struct ThemeSubjects {
    static constexpr size_t kSwatchCount = 16;
    static constexpr size_t kSwatchBufSize = 32;

    /// Monotonic generation counter; observers re-read colours when it moves.
    lv_subject_t changed{};
    int32_t generation = 0;
    bool changed_ready = false;

    /// Cramped-axis tier (0=MICRO..6=XXLARGE) for reactive responsive visibility.
    lv_subject_t breakpoint{};
    bool breakpoint_ready = false;

    /// Vertical-axis tier, for height-aware layout that ui_breakpoint cannot
    /// express because its tier follows the cramped axis.
    lv_subject_t breakpoint_v{};
    bool breakpoint_v_ready = false;

    /// 1 for any portrait class, 0 otherwise. A tier cannot answer "is this
    /// portrait": 480x800 and 800x480 can land on the same one.
    lv_subject_t is_portrait{};
    bool is_portrait_ready = false;

    /// 1 for the ultrawide class (wider than 2.5:1), 0 otherwise. Its tier
    /// follows the short axis, so ui_breakpoint calls a 1920x440 panel small.
    lv_subject_t is_ultrawide{};
    bool is_ultrawide_ready = false;

    /// Theme-editor swatch descriptions, bound by name from XML.
    lv_subject_t swatch_desc[kSwatchCount]{};
    char swatch_bufs[kSwatchCount][kSwatchBufSize]{};
    bool swatch_ready = false;

    void deinit();
};

ThemeSubjects& subjects();

} // namespace helix::theme_detail
