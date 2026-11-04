// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Responsive layout tokens: breakpoint classification of a display, the px
// tokens and overlay widths resolved from it, and the refresh that follows a
// resize or rotation.

#include "ui_breakpoint.h"
#include "ui_switch.h"

#include "asset_manager.h"
#include "display_metrics.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "theme_manager_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace helix;
using helix::theme_detail::runtime;
using helix::theme_detail::scale_px_token;

// Returns the smaller dimension of the display — used for responsive breakpoint
// selection so portrait layouts pick a breakpoint suited to the cramped axis.
// Landscape: typically the height. Portrait: the width. Either way, the short
// dimension is the one the design system has to fit content into.
int32_t responsive_dimension(lv_display_t* display) {
    lv_display_t* d = display ? display : lv_display_get_default();
    if (!d)
        return 600; // safe fallback when no display is available
    int32_t hor = lv_display_get_horizontal_resolution(d);
    int32_t ver = lv_display_get_vertical_resolution(d);
    if (hor <= 0 || ver <= 0)
        return 600;
    return hor < ver ? hor : ver;
}

// Returns the vertical resolution — the second responsive ladder (#1209).
// responsive_dimension() answers "how much room does the cramped axis have";
// this answers "how much room is there to stack things". Landscape and square
// displays have min(w,h) == h, so the two only diverge in portrait.
int32_t responsive_vertical_dimension(lv_display_t* display) {
    lv_display_t* d = display ? display : lv_display_get_default();
    if (!d)
        return 600; // safe fallback when no display is available
    int32_t ver = lv_display_get_vertical_resolution(d);
    return ver > 0 ? ver : 600;
}

// Breakpoint classification lives in ui_breakpoint.h as breakpoint_for() — the
// single canonical ladder shared with theme_manager_get_breakpoint_suffix() and
// FONT_TIERS ordering. Both axes feed the same ladder; only the scalar differs.

namespace helix::theme_detail {

/// Apply the high-DPI UI scale to one authored px token.
///
/// Shared by the responsive resolver and the static registration path. Both
/// must scale: a non-suffixed token is a fixed-size box (icon badges, chips,
/// swatches, column widths) whose contents are scaled fonts, so scaling only
/// the responsive half leaves the glyph overflowing its container.
///
/// Opacities are declared as <px> too and must be left alone — an 0-255 alpha
/// multiplied by 1.578 sails past opaque. `modal_backdrop_opacity` is the only
/// one today; the suffix test keeps a future one safe without another audit.
std::string scale_px_token(const std::string& name, const std::string& value, double scale) {
    if (scale <= 1.0) {
        return value;
    }
    static constexpr const char* kOpacitySuffix = "_opacity";
    const size_t suffix_len = std::strlen(kOpacitySuffix);
    if (name.size() >= suffix_len &&
        name.compare(name.size() - suffix_len, suffix_len, kOpacitySuffix) == 0) {
        return value;
    }
    // Leave anything that is not a bare positive integer alone: percentages and
    // sizing keywords are not lengths to multiply.
    char* end = nullptr;
    const long authored = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || authored <= 0) {
        return value;
    }
    return std::to_string(helix::DisplayMetrics::scaled_px(static_cast<int32_t>(authored), scale));
}

} // namespace helix::theme_detail

/**
 * Get the breakpoint suffix for a given resolution
 *
 * Breakpoints (in px) — ranges come from UI_BREAKPOINT_*_MAX constants:
 *   "_micro"    (≤ MICRO_MAX,   e.g. 272)
 *   "_tiny"     (≤ TINY_MAX,    e.g. 320)
 *   "_small"    (≤ SMALL_MAX,   e.g. 460)
 *   "_medium"   (≤ MEDIUM_MAX,  e.g. 540)
 *   "_large"    (≤ LARGE_MAX,   e.g. 800)
 *   "_xlarge"   (≤ XLARGE_MAX, e.g. 1280)
 *   "_xxlarge"  (> XLARGE_MAX — 1440p / 4K)
 *
 * @param resolution Screen dimension in px — typically responsive_dimension(display)
 *                   so portrait orientations pick a breakpoint matched to the
 *                   cramped axis.
 * @return One of the seven suffix strings above (valid for lv_xml_get_const lookups).
 */
const char* theme_manager_get_breakpoint_suffix(int32_t resolution) {
    return theme_detail::kSizeSuffixes[to_int(breakpoint_for(resolution))];
}

namespace helix::theme_detail {

int tier_for_suffix(const char* suffix) {
    for (int tier = 0; tier < kSizeSuffixCount; ++tier) {
        if (strcmp(suffix, kSizeSuffixes[tier]) == 0)
            return tier;
    }
    return -1;
}

bool has_dynamic_suffix(const std::string& name) {
    auto ends_with = [&](const char* suffix) {
        const size_t len = strlen(suffix);
        return name.size() > len && name.compare(name.size() - len, len, suffix) == 0;
    };
    if (ends_with("_light") || ends_with("_dark"))
        return true;
    for (const char* suffix : kSizeSuffixes) {
        if (ends_with(suffix))
            return true;
    }
    return false;
}

} // namespace helix::theme_detail

// ============================================================================
// Responsive px token resolution — one implementation, two callers
// ============================================================================
// Startup (theme_manager_register_responsive_spacing) and resize
// (theme_manager_refresh_layout_constants) used to carry two copies of the tier
// selection chain. They must never disagree: a token that gets one tier at boot
// and another after a rotation is worse than no responsiveness at all.

// The tokens that size a box vertically, and so must be chosen from how much
// height there is rather than from the cramped axis (#1209). Exact base names,
// deliberately not a `*_height` convention: dialog_content_max is a vertical
// maximum that does not end in _height, and a convention would have to
// understand the _sm/_lg modifiers too.
//
// Adding to this list is how a new height token opts in — see
// docs/devel/UI_CONTRIBUTOR_GUIDE.md § "Adding New Tokens". Horizontal and
// axis-neutral tokens (space_*, widths, square icon/badge sizes) stay on the
// cramped ladder and belong nowhere near here.
static constexpr const char* VERTICAL_AXIS_TOKENS[] = {
    "button_height",
    "button_height_sm",
    "button_height_lg",
    "chamber_preset_h",
    "header_height",
    "wizard_dropdown_height",
    "temp_card_height",
    "dialog_content_max",
    "dialog_content_pinned_max",
    "dialog_content_recovery_max",
    "spinner_lg",
    "header_button_height",
    "belt_tune_hero_h",
};

bool theme_manager_token_uses_vertical_axis(const char* base_name) {
    if (!base_name || base_name[0] == '\0')
        return false;
    for (const char* name : VERTICAL_AXIS_TOKENS) {
        if (strcmp(base_name, name) == 0)
            return true;
    }
    return false;
}

const std::string& helix::theme_manager_pick_px_tier(const PxTierTables& t, const std::string& base,
                                                     const char* suffix) {
    const auto& small_val = t.small.at(base);

    if (strcmp(suffix, "_micro") == 0) {
        auto it = t.micro.find(base);
        if (it != t.micro.end())
            return it->second;
        auto tiny_it = t.tiny.find(base);
        return (tiny_it != t.tiny.end()) ? tiny_it->second : small_val;
    }
    if (strcmp(suffix, "_tiny") == 0) {
        auto it = t.tiny.find(base);
        return (it != t.tiny.end()) ? it->second : small_val;
    }
    if (strcmp(suffix, "_small") == 0)
        return small_val;
    if (strcmp(suffix, "_medium") == 0)
        return t.medium.at(base);
    if (strcmp(suffix, "_large") == 0)
        return t.large.at(base);
    if (strcmp(suffix, "_xlarge") == 0) {
        auto it = t.xlarge.find(base);
        return (it != t.xlarge.end()) ? it->second : t.large.at(base);
    }
    // _xxlarge: fall back to _xlarge, then _large.
    auto it = t.xxlarge.find(base);
    if (it != t.xxlarge.end())
        return it->second;
    auto xl_it = t.xlarge.find(base);
    return (xl_it != t.xlarge.end()) ? xl_it->second : t.large.at(base);
}

std::unordered_map<std::string, std::string>
theme_manager_resolve_px_tokens(lv_display_t* display) {
    // theme_detail::ui_xml_dir(), not the "ui_xml" literal: on ESP-IDF the asset root is a
    // VFS mount, and the literal would scan an empty path AND miss the
    // build-time token-table fast path (its guard string-compares the dir).
    PxTierTables t{
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_micro"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_tiny"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_small"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_medium"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_large"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_xlarge"),
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "px", "_xxlarge"),
    };

    return theme_manager_resolve_px_tables(t, display, true);
}

std::unordered_map<std::string, std::string>
helix::theme_manager_resolve_px_tables(const PxTierTables& t, lv_display_t* display,
                                       bool app_tokens) {
    // Two ladders, one classification function. Landscape and square displays
    // have min(w,h) == h, so these two suffixes are equal there and nothing
    // moves; only portrait geometry sees a difference.
    const char* cramped_suffix = theme_manager_get_breakpoint_suffix(responsive_dimension(display));
    const char* vertical_suffix =
        theme_manager_get_breakpoint_suffix(responsive_vertical_dimension(display));

    // nav_width is the one token with its own ladder: primarily horizontal, but
    // it uses the vertical resolution to separate 480x272 from 480x320 and to
    // keep the strip slim on ultrawide panels.
    lv_display_t* d = display ? display : lv_display_get_default();
    const char* nav_suffix = d ? helix::nav_width_suffix(lv_display_get_horizontal_resolution(d),
                                                         lv_display_get_vertical_resolution(d))
                               : "_medium";

    std::unordered_map<std::string, std::string> resolved;
    for (const auto& [base_name, small_val] : t.small) {
        // _small/_medium/_large is the required triplet; anything short of it is
        // an incomplete set (theme_manager_validate_constant_sets warns) and is
        // left unregistered rather than guessed at.
        if (t.medium.find(base_name) == t.medium.end() || t.large.find(base_name) == t.large.end())
            continue;

        const char* suffix = !app_tokens                ? cramped_suffix
                             : base_name == "nav_width" ? nav_suffix
                             : theme_manager_token_uses_vertical_axis(base_name.c_str())
                                 ? vertical_suffix
                                 : cramped_suffix;

        resolved[base_name] = theme_manager_pick_px_tier(t, base_name, suffix);
    }

    // Apply the high-DPI UI scale to every px token — spacing, padding, and
    // sizes alike. This is the one place both the init and the resize path go
    // through, so the two can never disagree.
    //
    // No double-count with LVGL's own LV_DPX padding: these tokens are plain
    // pixel constants that reach widgets via style attributes, while LV_DPX
    // scales LVGL's internal theme chrome off the display DPI. The two paths
    // are disjoint, and the display DPI is set from the same scale factor (see
    // Application), so the two halves grow in step rather than compounding.
    //
    // active_scale() is 1.0 on every shipping printer, making this loop an
    // exact identity there.
    const double scale = helix::DisplayMetrics::active_scale();
    for (auto& [base_name, value] : resolved) {
        value = scale_px_token(base_name, value, scale);
    }
    return resolved;
}

/**
 * Register responsive spacing tokens from all XML files
 *
 * Auto-discovers all <px name="xxx_small"> elements from all XML files in ui_xml/
 * and registers base tokens by matching xxx_small/xxx_medium/xxx_large triplets.
 * This makes the system fully extensible without C++ code changes.
 *
 * CRITICAL: Base tokens must NOT be pre-defined or responsive overrides will be
 * silently ignored (LVGL ignores duplicate lv_xml_register_const).
 *
 * @param display The LVGL display to get resolution from
 */
void theme_manager_register_responsive_spacing(lv_display_t* display) {
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    // Logging only — the per-token axis choice lives in the resolver below. The
    // cramped axis is what most tokens follow, so it is what the summary reports.
    int32_t resp_res = responsive_dimension(display);
    const char* size_label = responsive_pick(breakpoint_for(resp_res), "MICRO", "TINY", "SMALL",
                                             "MEDIUM", "LARGE", "XLARGE", "XXLARGE");

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::warn("[Theme] Failed to get globals scope for spacing constants");
        return;
    }

    // Auto-discover and resolve every px token — including nav_width, which the
    // resolver gives its own horizontal ladder. Shared with the resize path so
    // the two can never pick different tiers for the same token.
    int registered = 0;
    for (const auto& [base_name, value] : theme_manager_resolve_px_tokens(display)) {
        spdlog::trace("[Theme] Registering spacing {}: selected={}", base_name, value);
        lv_xml_register_const(scope, base_name.c_str(), value.c_str());
        registered++;
    }

    spdlog::trace("[Theme] Responsive spacing: {} (min_dim={}px) - auto-registered {} tokens",
                  size_label, resp_res, registered);

    // ========================================================================
    // Register computed overlay widths (derived from nav_width + gap)
    // ========================================================================
    // nav_width was registered above from its own horizontal ladder. Read it
    // back to compute overlay panel widths.
    const char* nav_width_str = lv_xml_get_const(nullptr, "nav_width");
    int32_t nav_width = nav_width_str ? std::atoi(nav_width_str) : 94; // fallback

    const char* space_lg_str = lv_xml_get_const(nullptr, "space_lg");
    int32_t gap = space_lg_str ? std::atoi(space_lg_str) : 16; // fallback to 16px

    // Two overlay widths, distinguished by what they mean rather than by how
    // much space they leave. See include/overlay_class.h and
    // prestonbrown/helixscreen#1178.
    //   transient layer — the backdrop shows at the leading edge: you opened
    //                     this over something and will return from it.
    //   destination     — occludes the backdrop: a place you park, and whose
    //                     drill-downs are part of it.
    const helix::OverlayWidths widths =
        helix::compute_overlay_widths(hor_res, ver_res, nav_width, gap);

    char transient_str[16];
    char destination_str[16];
    snprintf(transient_str, sizeof(transient_str), "%d", widths.transient);
    snprintf(destination_str, sizeof(destination_str), "%d", widths.destination);

    lv_xml_register_const(scope, "overlay_width_transient", transient_str);
    lv_xml_register_const(scope, "overlay_width_destination", destination_str);

    spdlog::trace("[Theme] Layout: nav_width={}px, gap={}px, overlay transient={}px "
                  "destination={}px",
                  nav_width, gap, widths.transient, widths.destination);
}

void theme_manager_refresh_orientation(lv_display_t* display) {
    // ui_is_portrait is consumed by XML for visual layout (flex flow, strip
    // stacking, <if cond="ui_is_portrait eq 1">). Those decisions must follow a
    // --layout override, so the source of truth is LayoutManager::type() once it
    // is initialized. Before Phase 8b LayoutManager carries only its default
    // STANDARD; detect_layout_type() gives the right physical answer, using the
    // caller's display when provided so a non-default display (tests, a
    // specific refresh target) is not confused with the default. See #1255.
    // ui_is_ultrawide follows the same source for the same reason.
    lv_subject_t* portrait_subject = lv_xml_get_subject(nullptr, "ui_is_portrait");
    lv_subject_t* ultrawide_subject = lv_xml_get_subject(nullptr, "ui_is_ultrawide");
    if (!portrait_subject && !ultrawide_subject) {
        return;
    }

    lv_display_t* disp = display ? display : lv_display_get_default();
    if (!disp && !LayoutManager::instance().is_initialized()) {
        return;
    }
    const int w = disp ? lv_display_get_horizontal_resolution(disp) : 0;
    const int h = disp ? lv_display_get_vertical_resolution(disp) : 0;
    const LayoutType type = app_layout_type(w, h);
    if (portrait_subject) {
        lv_subject_set_int(portrait_subject, is_portrait_layout(type) ? 1 : 0);
    }
    if (ultrawide_subject) {
        lv_subject_set_int(ultrawide_subject, type == LayoutType::ULTRAWIDE ? 1 : 0);
    }
}

namespace {
std::vector<std::pair<int, std::function<void(lv_display_t*)>>>& layout_refresh_hooks() {
    static std::vector<std::pair<int, std::function<void(lv_display_t*)>>> hooks;
    return hooks;
}
} // namespace

int helix::theme_manager_add_layout_refresh_hook(std::function<void(lv_display_t*)> hook) {
    static int next_id = 1;
    const int id = next_id++;
    layout_refresh_hooks().emplace_back(id, std::move(hook));
    return id;
}

void helix::theme_manager_remove_layout_refresh_hook(int id) {
    auto& hooks = layout_refresh_hooks();
    hooks.erase(
        std::remove_if(hooks.begin(), hooks.end(), [id](const auto& h) { return h.first == id; }),
        hooks.end());
}

void theme_manager_refresh_layout_constants(lv_display_t* display) {
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope)
        return;

    // Update every responsive px token for the new size — nav_width included.
    // Same resolver as startup, which is the point: this path used to carry its
    // own copy of the selection chain, and its nav_width write was then
    // overwritten a few lines later by a generic loop that knew nothing about
    // the ultrawide ladder. See theme_manager_resolve_px_tokens().
    int32_t resp_res = responsive_dimension(display);
    for (const auto& [base_name, value] : theme_manager_resolve_px_tokens(display)) {
        lv_xml_set_const(scope, base_name.c_str(), value.c_str());
    }

    // Recalculate overlay widths from updated nav_width and space_lg
    const char* nav_width_str = lv_xml_get_const(nullptr, "nav_width");
    int32_t nav_width = nav_width_str ? std::atoi(nav_width_str) : 94;

    const char* space_lg_str = lv_xml_get_const(nullptr, "space_lg");
    int32_t gap = space_lg_str ? std::atoi(space_lg_str) : 16;

    const helix::OverlayWidths widths =
        helix::compute_overlay_widths(hor_res, ver_res, nav_width, gap);

    char transient_str[16];
    char destination_str[16];
    snprintf(transient_str, sizeof(transient_str), "%d", widths.transient);
    snprintf(destination_str, sizeof(destination_str), "%d", widths.destination);

    lv_xml_update_const(scope, "overlay_width_transient", transient_str);
    lv_xml_update_const(scope, "overlay_width_destination", destination_str);

    // Update breakpoint subject — use shared helper so rotation never
    // downgrades XXLarge to XLarge (previous bug: missing XLARGE_MAX check).
    UiBreakpoint bp = breakpoint_for(resp_res);

    lv_subject_t* bp_subject = lv_xml_get_subject(nullptr, "ui_breakpoint");
    if (bp_subject) {
        lv_subject_set_int(bp_subject, to_int(bp));
    }

    // Rotation swaps the axes, so the vertical tier has to be recomputed too --
    // updating only ui_breakpoint would leave a panel rotated out of portrait
    // still claiming the height it no longer has.
    lv_subject_t* bp_v_subject = lv_xml_get_subject(nullptr, "ui_breakpoint_v");
    if (bp_v_subject) {
        lv_subject_set_int(bp_v_subject,
                           to_int(breakpoint_for(responsive_vertical_dimension(display))));
    }

    // Orientation follows the same axis swap. theme_manager_refresh_orientation
    // consults LayoutManager (override-aware) when it is up, falling back to
    // detect_layout_type() on the early-startup probe path; either way a
    // rotation cannot leave ui_is_portrait disagreeing with the axes it just
    // repointed (#1255).
    theme_manager_refresh_orientation(display);

    // Type has to follow the breakpoint too. The px tokens above moved the
    // boxes; without the two calls below the fonts stayed sized for the startup
    // breakpoint, so a resize rescaled layout but not type (#1210).
    //
    // Order is load-bearing: AssetManager decides which font tiers exist in
    // memory at all, and startup deliberately skips the tiers above its own. If
    // the tokens were re-pointed first, every raised one would name a face that
    // is not registered and get bounced back down to the _large tier by the
    // existence check in theme_manager_register_responsive_fonts().
    AssetManager::register_fonts_for_tier(to_int(bp));
    theme_manager_register_responsive_fonts(display);

    // Switch size presets are the same ladder expressed as plain C++ values,
    // and were likewise chosen once at startup (#1210, Notes).
    ui_switch_init_size_presets(display);

    for (const auto& [id, hook] : layout_refresh_hooks())
        hook(display);

    spdlog::info("[Theme] Responsive state refreshed for {}x{}: nav={}px, "
                 "overlay transient={}px destination={}px (breakpoint={})",
                 hor_res, ver_res, nav_width, widths.transient, widths.destination, to_int(bp));
}

/**
 * Get spacing value from unified space_* system
 *
 * Reads the registered space_* constant value from LVGL's XML constant registry.
 * The value returned is responsive - it depends on what breakpoint was used
 * during theme initialization (small/medium/large).
 *
 * This function is the C++ interface to the unified spacing system, replacing
 * the old hardcoded UI_PADDING_* constants. All spacing in C++ code should now
 * use this function to stay consistent with XML layouts.
 *
 * Available tokens and their responsive values:
 *   space_xxs: 2/3/4px  (small/medium/large)
 *   space_xs:  4/5/6px
 *   space_sm:  6/7/8px
 *   space_md:  8/10/12px
 *   space_lg:  12/16/20px
 *   space_xl:  16/20/24px
 *   space_2xl: 24/32/40px
 *
 * @param token Spacing token name (e.g., "space_lg", "space_md", "space_xs")
 * @return Spacing value in pixels, or 0 if token not found
 *
 * Example:
 *   lv_obj_set_style_pad_all(obj, theme_manager_get_spacing("space_lg"), 0);
 */
int32_t theme_manager_get_spacing(const char* token) {
    if (!token) {
        spdlog::warn("[Theme] theme_manager_get_spacing: NULL token");
        return 0;
    }

    const char* value = lv_xml_get_const_silent(nullptr, token);
    if (!value) {
        if (runtime().current_theme) {
            spdlog::warn("[Theme] Spacing token '{}' not found - is theme initialized?", token);
        } else {
            spdlog::trace("[Theme] Spacing token '{}' not found (theme not initialized)", token);
        }
        return 0;
    }

    return std::atoi(value);
}
