// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Typography: the responsive font tokens (font_body and friends) for the active
// breakpoint, the icon-font predicate, and the font lookups the rest of the UI
// calls.

#include "ui_breakpoint.h"
#include "ui_button.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_split_button.h"

#include "display_metrics.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "theme_manager_internal.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif

using namespace helix;
using helix::theme_detail::runtime;

/**
 * Register responsive font tokens from all XML files
 *
 * Auto-discovers all <string name="xxx_small"> elements from all XML files in ui_xml/
 * and registers base tokens by matching xxx_small/xxx_medium/xxx_large triplets.
 * This makes the system fully extensible without C++ code changes.
 *
 * @param display The LVGL display to get resolution from
 */
void theme_manager_register_responsive_fonts(lv_display_t* display) {
    // Use the smaller dimension — the cramped axis is the design constraint
    // regardless of orientation (landscape: usually height; portrait: width).
    int32_t resp_res = responsive_dimension(display);
    const char* size_suffix = theme_manager_get_breakpoint_suffix(resp_res);
    const char* size_label = responsive_pick(breakpoint_for(resp_res), "MICRO", "TINY", "SMALL",
                                             "MEDIUM", "LARGE", "XLARGE", "XXLARGE");

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::warn("[Theme] Failed to get globals scope for font constants");
        return;
    }

    // Auto-discover all string tokens from all XML files (including optional _micro, _tiny,
    // _xlarge, and _xxlarge)
    auto micro_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_micro");
    auto tiny_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_tiny");
    auto small_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_small");
    auto medium_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_medium");
    auto large_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_large");
    auto xlarge_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_xlarge");
    auto xxlarge_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "string", "_xxlarge");

    int registered = 0;
    for (const auto& [base_name, small_val] : small_tokens) {
        // Verify _small/_medium/_large triplet exists (required)
        auto medium_it = medium_tokens.find(base_name);
        auto large_it = large_tokens.find(base_name);

        if (medium_it != medium_tokens.end() && large_it != large_tokens.end()) {
            // Select appropriate variant based on breakpoint. Also track which
            // suffix actually supplied the value so we can tier-classify a
            // missing-font miss below.
            const char* value = nullptr;
            const char* selected_suffix = nullptr;
            if (strcmp(size_suffix, "_micro") == 0) {
                auto micro_it = micro_tokens.find(base_name);
                if (micro_it != micro_tokens.end()) {
                    value = micro_it->second.c_str();
                    selected_suffix = "_micro";
                } else {
                    auto tiny_it = tiny_tokens.find(base_name);
                    if (tiny_it != tiny_tokens.end()) {
                        value = tiny_it->second.c_str();
                        selected_suffix = "_tiny";
                    } else {
                        value = small_val.c_str();
                        selected_suffix = "_small";
                    }
                }
            } else if (strcmp(size_suffix, "_tiny") == 0) {
                // Use _tiny if available, otherwise fall back to _small
                auto tiny_it = tiny_tokens.find(base_name);
                if (tiny_it != tiny_tokens.end()) {
                    value = tiny_it->second.c_str();
                    selected_suffix = "_tiny";
                } else {
                    value = small_val.c_str();
                    selected_suffix = "_small";
                }
            } else if (strcmp(size_suffix, "_small") == 0) {
                value = small_val.c_str();
                selected_suffix = "_small";
            } else if (strcmp(size_suffix, "_medium") == 0) {
                value = medium_it->second.c_str();
                selected_suffix = "_medium";
            } else if (strcmp(size_suffix, "_large") == 0) {
                value = large_it->second.c_str();
                selected_suffix = "_large";
            } else if (strcmp(size_suffix, "_xlarge") == 0) {
                auto xlarge_it = xlarge_tokens.find(base_name);
                if (xlarge_it != xlarge_tokens.end()) {
                    value = xlarge_it->second.c_str();
                    selected_suffix = "_xlarge";
                } else {
                    value = large_it->second.c_str();
                    selected_suffix = "_large";
                }
            } else {
                // _xxlarge: use xxlarge if available, fall back to _xlarge, then _large
                auto xxlarge_it = xxlarge_tokens.find(base_name);
                if (xxlarge_it != xxlarge_tokens.end()) {
                    value = xxlarge_it->second.c_str();
                    selected_suffix = "_xxlarge";
                } else {
                    auto xlarge_it = xlarge_tokens.find(base_name);
                    if (xlarge_it != xlarge_tokens.end()) {
                        value = xlarge_it->second.c_str();
                        selected_suffix = "_xlarge";
                    } else {
                        value = large_it->second.c_str();
                        selected_suffix = "_large";
                    }
                }
            }

            // Only apply font existence check to actual font constants.
            // Other string constants (e.g. icon_size_xlarge = "xl") are not
            // font names and must be registered as-is.
            bool is_font_constant =
                (base_name.rfind("font_", 0) == 0) || (base_name.rfind("icon_font_", 0) == 0);

            // High-DPI UI scale: the tier picks the face, then the scale steps
            // it up to the nearest larger one so type grows with the layout
            // rather than being left behind in an oversized box. The scaled
            // name is adopted only when it is actually linked, so a build that
            // pruned the larger faces simply keeps its tier font. Storage must
            // outlive the registration below, since `value` is a borrowed
            // pointer into the token maps.
            std::string scaled_font_storage;
            if (is_font_constant) {
                const double ui_scale = helix::DisplayMetrics::active_scale();
                if (ui_scale > 1.0) {
                    scaled_font_storage = helix::DisplayMetrics::scaled_font_name(value, ui_scale);
                    if (scaled_font_storage != value &&
                        lv_xml_get_font_silent(scope, scaled_font_storage.c_str()) != nullptr) {
                        value = scaled_font_storage.c_str();
                    }
                }
            }

            // Verify the selected font is actually linked. If not, fall back to
            // _large (guaranteed present by the triplet check above) and emit
            // tier-aware diagnostics: warn when the miss falls within this
            // platform's compiled tier range (build bug), stay silent when it's
            // above the max tier (expected pruning).
            if (is_font_constant && lv_xml_get_font_silent(scope, value) == nullptr) {
                int tier = helix::theme_detail::tier_for_suffix(selected_suffix);
                if (tier >= 0 && tier <= HELIX_MAX_FONT_TIER) {
                    spdlog::warn("[Theme] Font '{}' expected for tier '{}' but not linked "
                                 "(build bug?) — falling back to _large",
                                 value, selected_suffix);
                } else {
                    spdlog::trace("[Theme] Font '{}' pruned for tier '{}' (max tier {}) — "
                                  "falling back to _large",
                                  value, selected_suffix, HELIX_MAX_FONT_TIER);
                }
                const char* fallback = large_it->second.c_str();
                if (lv_xml_get_font_silent(scope, fallback) == nullptr) {
                    spdlog::error("[Theme] Fallback font '{}' for '{}' also not linked — "
                                  "skipping registration",
                                  fallback, base_name);
                    continue;
                }
                value = fallback;
                selected_suffix = "_large";
            }

            spdlog::trace("[Theme] Registering font {}: selected={} ({})", base_name, value,
                          selected_suffix);
            // set, not register: lv_xml_register_const() is first-write-wins,
            // so on the second pass (a runtime breakpoint change, or a theme
            // reload) it silently keeps the startup value. These base tokens
            // have no globals.xml declaration to protect — they exist only
            // because this function derives them — so overwriting is correct,
            // and on the first pass lv_xml_set_const() registers them (#1210).
            lv_xml_set_const(scope, base_name.c_str(), value);
            registered++;
        }
    }

    spdlog::trace("[Theme] Responsive fonts: {} (min_dim={}px) - auto-registered {} tokens",
                  size_label, resp_res, registered);

    // Three widgets memoize icon_font_* for the life of the process, and the
    // loop above just re-pointed those constants. Without this, the first
    // <icon size="sm"> ever built pins the face for every icon that follows, so
    // a breakpoint change resizes type everywhere except the icons (#1210).
    helix::ui::icon::invalidate_font_cache();
    ui_button_invalidate_icon_font_cache();
    ui_split_button_invalidate_icon_font_cache();
}

// A base font token (font_body) carries no size suffix after the font_
// prefix; theme_manager_register_responsive_fonts re-points it at the tier's
// variant, so it renders at a real size on every display. The plugin XML
// policy and the canvas font resolver both ask this one question.
// NAMESPACE_OK: joins this header's global theme_manager_* free-function API
bool theme_manager_font_token_is_base(const char* token) {
    // The suffix test runs on the name after font_: font_small is the base
    // token "small", not the _small variant of a token named "font".
    if (!token || strncmp(token, "font_", 5) != 0 || token[5] == '\0')
        return false;
    const char* name = token + 5;
    const size_t len = strlen(name);
    for (const char* suffix : helix::theme_detail::kSizeSuffixes) {
        const size_t slen = strlen(suffix);
        if (len > slen && strcmp(name + len - slen, suffix) == 0)
            return false;
    }
    return true;
}

namespace helix::ui {

bool is_icon_font(const lv_font_t* font) {
    if (!font)
        return false;
    if (font == &mdi_icons_14 || font == &mdi_icons_16 || font == &mdi_icons_24 ||
        font == &mdi_icons_32 || font == &mdi_icons_48 || font == &mdi_icons_64)
        return true;
        // Faces above 64px are linked only where mk/fonts.mk puts them, so
        // taking the address of one unconditionally fails to link other builds.
#if HELIX_HAS_MDI_ICONS_80
    if (font == &mdi_icons_80)
        return true;
#endif
#if HELIX_HAS_MDI_ICONS_96
    if (font == &mdi_icons_96)
        return true;
#endif
#if HELIX_HAS_MDI_ICONS_128
    if (font == &mdi_icons_128)
        return true;
#endif
    return false;
}

} // namespace helix::ui

/**
 * Get font line height in pixels
 *
 * Returns the total vertical space a line of text will occupy for the given font.
 * This includes ascender, descender, and line gap. Useful for calculating layout
 * heights before widgets are created.
 *
 * @param font Font to query (e.g., theme_manager_get_font("font_heading"), &noto_sans_16)
 * @return Line height in pixels, or 0 if font is NULL
 *
 * Examples:
 *   int32_t heading_h = theme_manager_get_font_height(theme_manager_get_font("font_heading"));
 *   int32_t body_h = theme_manager_get_font_height(theme_manager_get_font("font_body"));
 *   int32_t small_h = theme_manager_get_font_height(theme_manager_get_font("font_small"));
 *
 *   // Calculate total height for multi-line layout
 *   int32_t total = theme_manager_get_font_height(theme_manager_get_font("font_heading")) +
 *                   (theme_manager_get_font_height(theme_manager_get_font("font_body")) * 3) +
 *                   (4 * 8);  // 4 gaps of 8px padding
 */
int32_t theme_manager_get_font_height(const lv_font_t* font) {
    if (!font) {
        spdlog::warn("[Theme] theme_manager_get_font_height: NULL font pointer");
        return 0;
    }

    return lv_font_get_line_height(font);
}

/**
 * Get responsive font by token name
 *
 * Looks up the font token (e.g., "font_small") which was registered during
 * theme init with the appropriate breakpoint variant value (e.g., "noto_sans_16"),
 * then retrieves the actual font pointer.
 *
 * @param token Font token name (e.g., "font_small", "font_body", "font_heading")
 * @return Font pointer, or nullptr if not found
 */
const lv_font_t* theme_manager_get_font(const char* token) {
    if (!token) {
        spdlog::warn("[Theme] theme_manager_get_font: NULL token, using default font");
        return lv_font_get_default();
    }

    // Get the font name from the registered constant (e.g., "font_small" -> "noto_sans_16")
    const char* font_name = lv_xml_get_const_silent(nullptr, token);
    if (!font_name) {
        if (runtime().current_theme) {
            spdlog::warn("[Theme] Font token '{}' not found - falling back to default font", token);
        } else {
            spdlog::trace("[Theme] Font token '{}' not found (theme not initialized)", token);
        }
        return lv_font_get_default();
    }

    // Get the actual font pointer
    const lv_font_t* font = lv_xml_get_font(nullptr, font_name);
    if (!font) {
        spdlog::warn("[Theme] Font '{}' (from token '{}') not registered - falling back to default",
                     font_name, token);
        return lv_font_get_default();
    }

    return font;
}

const char* theme_manager_size_to_font_token(const char* size, const char* default_size) {
    const char* effective_size = size ? size : default_size;
    if (!effective_size) {
        effective_size = "sm"; // Fallback if both are null
    }

    if (strcmp(effective_size, "xs") == 0) {
        return "font_xs";
    } else if (strcmp(effective_size, "sm") == 0) {
        return "font_small";
    } else if (strcmp(effective_size, "md") == 0) {
        return "font_body";
    } else if (strcmp(effective_size, "lg") == 0) {
        return "font_heading";
    } else if (strcmp(effective_size, "xl") == 0) {
        return "font_xl";
    }

    // Unknown size - warn and return default
    spdlog::warn("[Theme] Unknown size '{}', using default '{}'", effective_size, default_size);
    return theme_manager_size_to_font_token(default_size, "sm");
}
