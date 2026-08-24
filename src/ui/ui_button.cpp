// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_button.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <unordered_set>

using namespace helix;

namespace {

// User data stored on button to track icon/label positions
// NOTE: Magic number required because Modal::wire_button overwrites user_data
// with a Modal* pointer. Without this check, button_delete_cb would try to
// delete a Modal* as if it were UiButtonData*, causing a crash on shutdown.
//
// `id` is a per-button unique value captured when a deferred contrast recompute
// is queued and re-verified when it fires. lv_obj_is_valid() + magic only prove
// the address is *a* live ui_button; under heap address reuse a freed button's
// address can be reallocated to a *different* live ui_button before the deferred
// tick, passing both checks. The id catches that foreign-button case (#924).
struct UiButtonData {
    static constexpr uint32_t MAGIC = 0x42544E31; // "BTN1"
    uint32_t magic{MAGIC};
    uint64_t id{0};     // Defer-time identity token (see note above, #924)
    lv_obj_t* icon;     // Icon widget (or nullptr if none)
    lv_obj_t* label;    // Label widget (always present)
    bool icon_on_right; // true if icon is after text

    // Declared icon placement, recorded at create time. The icon itself may not
    // exist yet: a button with bind_icon but no static icon= attribute has its
    // icon created later, during the apply pass, and that pass has no access to
    // the XML attributes. Without these, a late-created icon always ends up in a
    // row and icon_position="top"/"bottom" is silently ignored.
    bool icon_vertical{false};  // icon_position top/bottom, or layout="column"
    bool icon_on_bottom{false}; // icon comes after the label in a vertical stack

    // op-state (bind_op_state): 0=idle, 1=busy (spinner), 2=done (check glyph).
    // op_spinner is an lv_arc lazily created in the icon slot the first time the
    // button goes busy; it is shown/hidden as state changes so the button width
    // never reflows. op_idle_glyph caches the original icon codepoint so the idle
    // glyph can be restored after busy/done.
    lv_obj_t* op_spinner{nullptr};
    char op_idle_glyph[8]{}; // MDI glyph is 4-byte UTF-8 + NUL; 8 is generous
};

// Monotonic source of per-button identity tokens. Buttons are created on the
// main thread, so a plain non-atomic counter is sufficient.
uint64_t next_button_id() {
    static uint64_t n = 1;
    return n++;
}

// Set of addresses that are currently one of OUR live ui_buttons. A deferred
// contrast recompute captures a raw lv_obj_t*; by the time it fires the button
// may have been freed and its address reused. lv_obj_is_valid() only proves the
// address is *some* live object — it can be a foreign widget whose user_data is
// a small non-pointer sentinel (e.g. 0x2), and dereferencing that as a
// UiButtonData* crashes (#1111). Membership here is the missing precondition:
// only if btn is still a tracked ui_button is its user_data guaranteed to be a
// real UiButtonData safe to dereference. Function-local static (not namespace
// scope) to avoid static-init-order issues; main-thread only (create, delete,
// and queue drain all run on the UI thread), so no locking is needed.
std::unordered_set<const lv_obj_t*>& live_ui_buttons() {
    static std::unordered_set<const lv_obj_t*> s;
    return s;
}

/**
 * @brief Resolve an icon font by XML const name (e.g. icon_font_sm/md/lg)
 *
 * Falls back to mdi_icons_24 if the constant is missing or the font isn't
 * compiled in. Not cached — callers pass one of a small set of names and
 * LVGL's const lookup is already cheap.
 */
static const lv_font_t* resolve_icon_font(const char* xml_const) {
    const lv_font_t* font = nullptr;
    const char* font_name = lv_xml_get_const_silent(nullptr, xml_const);
    if (font_name) {
        font = lv_xml_get_font(nullptr, font_name);
        if (!font) {
            spdlog::error("[ui_button] Font '{}' (from '{}') is not compiled — using fallback",
                          font_name, xml_const);
        }
    } else {
        spdlog::error("[ui_button] Font constant '{}' not found — using fallback", xml_const);
    }
    return font ? font : &mdi_icons_24;
}

static const lv_font_t* s_icon_font = nullptr;
static bool s_icon_font_resolved = false;

/**
 * @brief Get the default button icon font (icon_font_sm).
 *
 * Cached because the same value is used for every standard button. Dropped by
 * ui_button_invalidate_icon_font_cache() when the breakpoint re-points the
 * constant.
 */
static const lv_font_t* get_button_icon_font() {
    if (s_icon_font_resolved) {
        return s_icon_font;
    }
    s_icon_font = resolve_icon_font("icon_font_sm");
    s_icon_font_resolved = true;
    spdlog::debug("[ui_button] Default button icon font — line_height={}px",
                  lv_font_get_line_height(s_icon_font));
    return s_icon_font;
}

/**
 * @brief Check if a font is one of the MDI icon fonts
 *
 * NOTE: Duplicates is_icon_font() in theme_manager.cpp (both are file-local static).
 * If new icon font sizes are added, update both.
 */
static bool is_mdi_icon_font(const lv_font_t* font) {
    if (!font)
        return false;
    return font == &mdi_icons_14 || font == &mdi_icons_16 || font == &mdi_icons_24 ||
           font == &mdi_icons_32 || font == &mdi_icons_48 || font == &mdi_icons_64;
}

// ---------------------------------------------------------------------------
// op-state spinner (bind_op_state busy animation)
//
// A self-contained Material-style indeterminate arc spinner rendered inside the
// button's icon slot. Mirrors ui_spinner.cpp but uses its own file-local
// animation callbacks (lv_anim_delete keys on obj+exec_cb, so the callbacks must
// be distinct from the standalone <spinner> widget's). The arc is sized to the
// icon line-height so the button width never reflows between idle and busy.
// ---------------------------------------------------------------------------
namespace op_spinner_anim {

constexpr uint32_t ROTATION_DURATION_MS = 1568;
constexpr uint32_t SWEEP_DURATION_MS = 667;
constexpr int32_t ARC_MIN_SWEEP = 45;
constexpr int32_t ARC_MAX_SWEEP = 270;

int32_t g_last_start = 0;
int32_t g_last_end = 0;

int32_t clamp_sweep(int32_t angle, int32_t reference, bool angle_is_start) {
    int32_t sweep = angle_is_start ? (reference - angle) : (angle - reference);
    if (sweep < 0)
        sweep += 360;
    if (sweep >= ARC_MIN_SWEEP)
        return angle;

    int32_t clamped = angle_is_start ? (reference - ARC_MIN_SWEEP) : (reference + ARC_MIN_SWEEP);
    if (clamped < 0)
        clamped += 360;
    if (clamped >= 360)
        clamped -= 360;
    return clamped;
}

void start_angle(void* obj, int32_t value) {
    value = clamp_sweep(value, g_last_end, /*angle_is_start=*/true);
    g_last_start = value;
    lv_arc_set_start_angle(static_cast<lv_obj_t*>(obj), static_cast<uint32_t>(value));
}

void end_angle(void* obj, int32_t value) {
    value = clamp_sweep(value, g_last_start, /*angle_is_start=*/false);
    g_last_end = value;
    lv_arc_set_end_angle(static_cast<lv_obj_t*>(obj), static_cast<uint32_t>(value));
}

void rotation(void* obj, int32_t value) {
    lv_arc_set_rotation(static_cast<lv_obj_t*>(obj), value);
}

void start(lv_obj_t* arc) {
    lv_anim_t a_end;
    lv_anim_init(&a_end);
    lv_anim_set_var(&a_end, arc);
    lv_anim_set_exec_cb(&a_end, end_angle);
    lv_anim_set_duration(&a_end, SWEEP_DURATION_MS * 2);
    lv_anim_set_values(&a_end, ARC_MIN_SWEEP, ARC_MIN_SWEEP + 360);
    lv_anim_set_repeat_count(&a_end, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_end, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&a_end, LV_BEZIER_VAL_FLOAT(0.0), LV_BEZIER_VAL_FLOAT(0.0),
                              LV_BEZIER_VAL_FLOAT(0.2), LV_BEZIER_VAL_FLOAT(1.0));
    lv_anim_start(&a_end);

    lv_anim_t a_start;
    lv_anim_init(&a_start);
    lv_anim_set_var(&a_start, arc);
    lv_anim_set_exec_cb(&a_start, start_angle);
    lv_anim_set_duration(&a_start, SWEEP_DURATION_MS * 2);
    lv_anim_set_values(&a_start, 0, 360);
    lv_anim_set_repeat_count(&a_start, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_start, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&a_start, LV_BEZIER_VAL_FLOAT(0.8), LV_BEZIER_VAL_FLOAT(0.0),
                              LV_BEZIER_VAL_FLOAT(1.0), LV_BEZIER_VAL_FLOAT(1.0));
    lv_anim_start(&a_start);

    lv_anim_t a_rot;
    lv_anim_init(&a_rot);
    lv_anim_set_var(&a_rot, arc);
    lv_anim_set_exec_cb(&a_rot, rotation);
    lv_anim_set_duration(&a_rot, ROTATION_DURATION_MS);
    lv_anim_set_values(&a_rot, 270, 270 + 360);
    lv_anim_set_repeat_count(&a_rot, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_rot, lv_anim_path_linear);
    lv_anim_start(&a_rot);
}

void stop(lv_obj_t* arc) {
    lv_anim_delete(arc, start_angle);
    lv_anim_delete(arc, end_angle);
    lv_anim_delete(arc, rotation);
}

} // namespace op_spinner_anim

/**
 * @brief Update button text color for contrast against the button background
 *
 * Handles both internal label/icon (from text= attr) and XML child labels
 * (from layout="column" buttons with text_body/text_small children).
 * Uses theme_manager_get_contrast_color() to pick dark vs light text.
 *
 * @param btn The button widget
 */
void update_button_text_contrast(lv_obj_t* btn) {
    // Check magic to ensure user_data hasn't been overwritten (e.g., by Modal::wire_button)
    UiButtonData* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC) {
        // Normal for buttons whose user_data was repurposed (e.g., Modal::wire_button)
        return;
    }

    bool is_disabled = lv_obj_has_state(btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(btn, is_disabled ? LV_OPA_50 : LV_OPA_COVER, LV_PART_MAIN);
    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(btn, LV_PART_MAIN);
    bool is_ghost = bg_opa < LV_OPA_50;

    // Determine text color based on button type
    lv_color_t text_color;
    if (is_ghost) {
        // Ghost/transparent button - use theme text color
        text_color = theme_manager_get_color("text");
    } else {
        // Solid button - calculate contrast against effective background
        lv_color_t bg = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);

        // Disabled buttons render at 50% opacity, so blend with screen bg
        // to get the effective color for contrast calculation
        if (is_disabled) {
            lv_color_t screen_bg = theme_manager_get_color("screen_bg");
            bg = lv_color_mix(bg, screen_bg, LV_OPA_50);
        }

        text_color = theme_manager_get_contrast_color(bg);
    }

    spdlog::trace("[ui_button] contrast: ghost={} disabled={} text=0x{:06X}", is_ghost, is_disabled,
                  lv_color_to_u32(text_color) & 0xFFFFFF);

    // Text opacity: slightly higher than button opacity (50%) for disabled state
    // to maintain readability while indicating disabled
    lv_opa_t text_opa = is_disabled ? LV_OPA_70 : LV_OPA_COVER;

    // Helper to set contrast color on a widget unconditionally
    auto set_contrast = [&](lv_obj_t* obj) {
        if (!obj)
            return;
        lv_obj_set_style_text_color(obj, text_color, LV_PART_MAIN);
        lv_obj_set_style_text_opa(obj, text_opa, LV_PART_MAIN);
    };

    // Button's own label and icon always get contrast colors -
    // they're internal widgets that must match the button background
    set_contrast(data->label);
    set_contrast(data->icon);

    // Walk XML child labels for "shell" buttons (no text= attr) that use
    // layout="column" with XML children (e.g., filament preset buttons).
    // Skip icon-font children - they manage their own color via the variant
    // system (e.g., nav bar icons with variant="primary"/"secondary")
    // and should not be overridden by button contrast logic.
    if (!data->label && !data->icon) {
        uint32_t count = lv_obj_get_child_count(btn);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(btn, i);
            if (!child)
                continue;
            if (!lv_obj_check_type(child, &lv_label_class))
                continue;
            const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
            if (is_mdi_icon_font(font))
                continue;
            set_contrast(child);
        }
    }
}

// Defer a contrast recompute that survives widget address reuse.
// The button may be freed and its address reused before the deferred tick
// fires. Two independent hazards:
//   1. Reused by a foreign (non-ui_button) widget whose user_data is a small
//      non-pointer sentinel (e.g. 0x2). lv_obj_is_valid() passes (a live object
//      occupies the address) and !d passes, so dereferencing d->magic faults at
//      the sentinel address (#1111). Guard: reject btn unless it is still one of
//      our tracked live ui_buttons — only then is user_data a real UiButtonData.
//   2. Reused by a *different* ui_button. It passes the registry + magic checks,
//      so re-verify the per-button identity token captured at defer time (#924).
void defer_button_contrast_update(lv_obj_t* btn) {
    UiButtonData* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC)
        return;
    const uint64_t gen = data->id;
    helix::ui::queue_update("ui_button::contrast_update", [btn, gen]() {
        // Hazard 1: never dereference user_data unless btn is still a live
        // ui_button we own (guards against foreign-widget address reuse, #1111).
        if (!live_ui_buttons().count(btn))
            return;
        UiButtonData* d = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
        // Hazard 2: same address, different ui_button — identity token differs.
        if (!d || d->magic != UiButtonData::MAGIC || d->id != gen)
            return;
        update_button_text_contrast(btn);
    });
}

/**
 * @brief Event callback for LV_EVENT_STYLE_CHANGED and LV_EVENT_STATE_CHANGED
 *
 * Called when button style changes (e.g., theme update) or state changes
 * (e.g., disabled via bind_state_if_eq). Recalculates and applies text contrast.
 *
 * @param e Event object
 */
void button_style_changed_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target_obj(e);
    // Defer contrast update to avoid setting styles mid-cascade.
    // refresh_children_style sends STYLE_CHANGED recursively; modifying child
    // styles in that handler triggers trans_delete on partially-initialized
    // transitions, crashing in lv_obj_set_local_style_prop (#729).
    defer_button_contrast_update(btn);
}

/**
 * @brief Event callback for LV_EVENT_CLICKED — plays button tap sound
 *
 * Hooked at the component level so ALL <ui_button> instances get audio
 * feedback automatically. SoundManager::play() handles the sounds_enabled
 * and ui_sounds_enabled checks internally, so no gating needed here.
 *
 * @param e Event object (unused)
 */
void button_clicked_sound_cb(lv_event_t* e) {
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (lv_obj_has_flag(btn, LV_OBJ_FLAG_USER_4))
        return; // Suppressed (e.g., sound preview buttons)
    SoundManager::instance().play("button_tap");
}

/**
 * @brief Event callback for LV_EVENT_DELETE
 *
 * Called when button is deleted. Frees the UiButtonData user data.
 *
 * @param e Event object
 */
void button_delete_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target_obj(e);
    // Stop tracking this address before it is freed and possibly reused, so a
    // pending deferred contrast update rejects the stale pointer (#1111).
    live_ui_buttons().erase(btn);
    UiButtonData* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    // Only delete if magic matches - user_data may have been overwritten
    // by Modal::wire_button with a Modal* pointer
    if (data && data->magic == UiButtonData::MAGIC) {
        delete data;
        lv_obj_set_user_data(btn, nullptr);
    }
}

/**
 * @brief Create icon widget for button
 *
 * Creates a font-based MDI icon label inside the button.
 *
 * @param btn Parent button widget
 * @param icon_name Icon name (e.g., "settings", "heat_wave")
 * @return Created icon widget or nullptr on failure
 */
lv_obj_t* create_button_icon(lv_obj_t* btn, const char* icon_name,
                             const char* size_override = nullptr) {
    if (!icon_name || strlen(icon_name) == 0) {
        return nullptr;
    }

    // Lookup icon codepoint
    const char* codepoint = ui_icon::lookup_codepoint(icon_name);
    if (!codepoint) {
        // Try stripping legacy prefix
        const char* stripped = ui_icon::strip_legacy_prefix(icon_name);
        if (stripped != icon_name) {
            codepoint = ui_icon::lookup_codepoint(stripped);
        }
    }

    if (!codepoint) {
        spdlog::warn("[ui_button] Icon '{}' not found", icon_name);
        return nullptr;
    }

    // Pick the icon font. "sm" / empty => default (cached); "md"/"lg" read the
    // matching responsive const so callers can oversize icon-only buttons
    // (e.g. e-stop at micro) without pulling the rest of the UI with them.
    const lv_font_t* font = get_button_icon_font();
    if (size_override && size_override[0] && strcmp(size_override, "sm") != 0) {
        char const_name[32];
        snprintf(const_name, sizeof(const_name), "icon_font_%s", size_override);
        font = resolve_icon_font(const_name);
    }

    // Create icon as lv_label with MDI font
    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, codepoint);
    lv_obj_set_style_text_font(icon, font, LV_PART_MAIN);

    spdlog::trace("[ui_button] Created icon '{}' -> codepoint", icon_name);
    return icon;
}

/**
 * @brief XML create callback for <ui_button> widget
 *
 * Creates a semantic button with:
 * - lv_button as base widget
 * - Shared style based on variant (primary/secondary/danger/ghost/outline)
 * - Optional icon with auto-contrast
 * - Child lv_label with text attribute
 * - LV_EVENT_STYLE_CHANGED handler for auto-contrast updates
 *
 * Attributes:
 * - variant: Button style (primary/secondary/danger/success/tertiary/warning/ghost/outline)
 * - text: Button label text
 * - icon: Optional icon name (e.g., "settings", "heat_wave")
 * - icon_position: "left" (default), "right", "top" or "bottom". Honoured whether
 *   the icon comes from the static icon= attribute or from a bind_icon subject.
 *
 * @param state XML parser state
 * @param attrs XML attributes
 * @return Created button object
 */
void* ui_button_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create button with default height from theme system
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_height(btn, theme_manager_get_spacing("button_height"));

    // If focusable="false", remove from default input group
    // This prevents keyboard Tab navigation focus (and focus ring)
    // Separate from click_focusable which only affects click-based focus
    const char* focusable = lv_xml_get_value_of(attrs, "focusable");
    if (focusable && strcmp(focusable, "false") == 0) {
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_remove_obj(btn);
        }
    }

    // Parse variant attribute (default: primary)
    const char* variant_str = lv_xml_get_value_of(attrs, "variant");
    if (!variant_str) {
        variant_str = "primary";
    }

    // Apply shared style based on variant
    auto& tm = ThemeManager::instance();
    lv_style_t* style = nullptr;
    if (strcmp(variant_str, "primary") == 0) {
        style = tm.get_style(StyleRole::ButtonPrimary);
    } else if (strcmp(variant_str, "secondary") == 0) {
        style = tm.get_style(StyleRole::ButtonSecondary);
    } else if (strcmp(variant_str, "danger") == 0) {
        style = tm.get_style(StyleRole::ButtonDanger);
    } else if (strcmp(variant_str, "success") == 0) {
        style = tm.get_style(StyleRole::ButtonSuccess);
    } else if (strcmp(variant_str, "tertiary") == 0) {
        style = tm.get_style(StyleRole::ButtonTertiary);
    } else if (strcmp(variant_str, "warning") == 0) {
        style = tm.get_style(StyleRole::ButtonWarning);
    } else if (strcmp(variant_str, "ghost") == 0) {
        style = tm.get_style(StyleRole::ButtonGhost);
    } else if (strcmp(variant_str, "transparent") == 0) {
        style = tm.get_style(StyleRole::ButtonTransparent);
    } else if (strcmp(variant_str, "outline") == 0) {
        style = tm.get_style(StyleRole::ButtonOutline);
    } else {
        spdlog::warn("[ui_button] Unknown variant '{}', defaulting to primary", variant_str);
        style = tm.get_style(StyleRole::ButtonPrimary);
    }

    if (style) {
        lv_obj_add_style(btn, style, LV_PART_MAIN);
    }

    // Parse text attribute
    // text="@subject" is handled in apply phase (subject binding) — treat as
    // empty here for label creation but still set has_text=true for layout.
    const char* text = lv_xml_get_value_of(attrs, "text");
    bool text_is_subject = (text && text[0] == '@');
    if (!text) {
        text = "";
    }
    const char* label_text = text_is_subject ? "" : text;

    // Parse translation_tag attribute for i18n support
    const char* translation_tag = lv_xml_get_value_of(attrs, "translation_tag");

    // Parse icon attribute
    const char* icon_name = lv_xml_get_value_of(attrs, "icon");
    // Optional icon size override: "sm" (default), "md", "lg". Maps to
    // icon_font_<size> in globals.xml so icon-only buttons can scale up
    // on small displays without bumping icon_font_sm globally.
    const char* icon_size = lv_xml_get_value_of(attrs, "icon_size");

    // Parse icon_position attribute (default: left)
    // Supported values: "left" (default), "right", "top", "bottom"
    const char* icon_pos_str = lv_xml_get_value_of(attrs, "icon_position");
    bool icon_on_right = (icon_pos_str && strcmp(icon_pos_str, "right") == 0);
    bool icon_on_top = (icon_pos_str && strcmp(icon_pos_str, "top") == 0);
    bool icon_on_bottom = (icon_pos_str && strcmp(icon_pos_str, "bottom") == 0);
    bool vertical_layout = icon_on_top || icon_on_bottom;

    // Parse layout attribute for explicit flex direction on button children
    // Supported values: "row", "column"
    // This enables stacking text + XML children properly
    const char* layout_str = lv_xml_get_value_of(attrs, "layout");
    bool explicit_column = (layout_str && strcmp(layout_str, "column") == 0);
    bool explicit_row = (layout_str && strcmp(layout_str, "row") == 0);
    if (explicit_column) {
        vertical_layout = true;
    }

    // Allocate user data to track icon/label
    UiButtonData* data = new UiButtonData{.magic = UiButtonData::MAGIC,
                                          .id = next_button_id(),
                                          .icon = nullptr,
                                          .label = nullptr,
                                          .icon_on_right = icon_on_right,
                                          .icon_vertical = vertical_layout,
                                          .icon_on_bottom = icon_on_bottom};

    bool has_icon = (icon_name && strlen(icon_name) > 0);
    const char* bind_text_create = lv_xml_get_value_of(attrs, "bind_text");
    bool has_text =
        (text && strlen(text) > 0) || (bind_text_create && strlen(bind_text_create) > 0);

    if (has_icon && has_text) {
        if (vertical_layout) {
            // Icon + text: use vertical flex layout (column)
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            // No pad_row - use pad_top on label to match Motors Off button style
            lv_obj_set_style_pad_row(btn, 0, LV_PART_MAIN);

            if (icon_on_bottom) {
                // Text first, then icon
                data->label = lv_label_create(btn);
                lv_label_set_text(data->label, label_text);
                data->icon = create_button_icon(btn, icon_name, icon_size);
            } else {
                // Icon first (top), then text
                data->icon = create_button_icon(btn, icon_name, icon_size);
                data->label = lv_label_create(btn);
                lv_label_set_text(data->label, label_text);
            }
            // Use small font for vertical layout labels (matches text_small)
            if (data->label) {
                lv_obj_set_style_text_font(data->label, theme_manager_get_font("font_small"),
                                           LV_PART_MAIN);
                lv_obj_set_style_pad_top(data->label, theme_manager_get_spacing("space_xxs"),
                                         LV_PART_MAIN);
            }
        } else {
            // Icon + text: use horizontal flex layout (row)
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(btn, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);

            if (icon_on_right) {
                // Text first, then icon
                data->label = lv_label_create(btn);
                lv_label_set_text(data->label, label_text);
                data->icon = create_button_icon(btn, icon_name, icon_size);
            } else {
                // Icon first (left), then text
                data->icon = create_button_icon(btn, icon_name, icon_size);
                data->label = lv_label_create(btn);
                lv_label_set_text(data->label, label_text);
            }
        }
    } else if (has_icon) {
        // Icon only: center the icon, no label needed
        data->icon = create_button_icon(btn, icon_name, icon_size);
        if (data->icon) {
            lv_obj_center(data->icon);
        }
    } else if (has_text) {
        if (explicit_column) {
            // Text with column layout: set up flex for XML children below
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            data->label = lv_label_create(btn);
            lv_label_set_text(data->label, label_text);
        } else if (explicit_row) {
            // Text with row layout: set up flex for XML children beside
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(btn, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);
            data->label = lv_label_create(btn);
            lv_label_set_text(data->label, label_text);
        } else {
            // Text only without explicit layout: center the label
            data->label = lv_label_create(btn);
            lv_label_set_text(data->label, label_text);
            lv_obj_center(data->label);
        }
    } else if (explicit_column || explicit_row) {
        // No icon, no text, but explicit layout: set up flex for XML children
        if (explicit_column) {
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
        } else {
            lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(btn, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);
        }
    }
    // else: No icon, no text, no layout - leave button empty for XML children

    // Apply translation tag if provided (for i18n hot-reload support)
    if (translation_tag && strlen(translation_tag) > 0 && data->label) {
        lv_label_set_translation_tag(data->label, translation_tag);
    }

    // Store user data on button
    lv_obj_set_user_data(btn, data);
    // Track this address as a live ui_button so deferred work can verify the
    // button hasn't been freed and its address reused (#1111).
    live_ui_buttons().insert(btn);

    // Register event handlers
    lv_obj_add_event_cb(btn, button_style_changed_cb, LV_EVENT_STYLE_CHANGED, nullptr);
    lv_obj_add_event_cb(btn, button_style_changed_cb, LV_EVENT_STATE_CHANGED, nullptr);
    lv_obj_add_event_cb(btn, button_clicked_sound_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, button_delete_cb, LV_EVENT_DELETE, nullptr);

    // Apply initial text contrast for buttons with text=/icon= attrs
    update_button_text_contrast(btn);

    // Defer a second pass for "shell" buttons whose XML children aren't
    // created yet. By the time the async callback fires, children will have
    // their fonts and variant styles applied, so contrast and icon-skip
    // logic works correctly.
    defer_button_contrast_update(btn);

    // Prevent accidental click when scrolling.
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);

    const char* pos_name = icon_on_top      ? "top"
                           : icon_on_bottom ? "bottom"
                           : icon_on_right  ? "right"
                                            : "left";
    const char* layout_name = explicit_column ? "column" : (explicit_row ? "row" : "auto");
    spdlog::trace(
        "[ui_button] Created button variant='{}' text='{}' icon='{}' icon_pos='{}' layout='{}'",
        variant_str, text, icon_name ? icon_name : "", pos_name, layout_name);

    return btn;
}

/**
 * @brief Observer callback for bind_icon subject changes
 *
 * Updates the icon label text when the bound string subject changes.
 * The icon label is stored in user_data, and the button is in user_data of observer.
 *
 * @param observer The LVGL observer
 * @param subject The subject that changed (string subject with icon name)
 */
void icon_subject_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* icon = static_cast<lv_obj_t*>(lv_observer_get_target_obj(observer));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_observer_get_user_data(observer));

    if (!icon) {
        return;
    }

    const char* icon_value = lv_subject_get_string(subject);
    if (!icon_value || strlen(icon_value) == 0) {
        spdlog::trace("[ui_button] bind_icon: empty icon value, keeping current");
        return;
    }

    // Check if value is already a UTF-8 codepoint (starts with high byte 0xF0+)
    // MDI icons are in the Unicode Private Use Area, encoded as 4-byte UTF-8
    // starting with 0xF3 (U+F0000..U+FFFFF range)
    const unsigned char first_byte = static_cast<unsigned char>(icon_value[0]);
    if (first_byte >= 0xF0) {
        // Value is already a codepoint - use directly
        lv_label_set_text(icon, icon_value);
        spdlog::trace("[ui_button] bind_icon: updated to codepoint");
    } else {
        // Value is an icon name - look up the codepoint
        const char* codepoint = ui_icon::lookup_codepoint(icon_value);
        if (!codepoint) {
            // Try stripping legacy prefix
            const char* stripped = ui_icon::strip_legacy_prefix(icon_value);
            if (stripped != icon_value) {
                codepoint = ui_icon::lookup_codepoint(stripped);
            }
        }

        if (codepoint) {
            lv_label_set_text(icon, codepoint);
            spdlog::trace("[ui_button] bind_icon: updated to '{}'", icon_value);
        } else {
            spdlog::warn("[ui_button] Icon '{}' not found during bind_icon update", icon_value);
            return;
        }
    }

    // Re-apply text contrast if we have the button reference
    if (btn) {
        update_button_text_contrast(btn);
    }
}

/**
 * @brief Cleanup callback for the op-state spinner arc
 *
 * Stops the running animations before the arc is freed so the animation engine
 * never dereferences a deleted object. The arc is a child of the button, so this
 * fires automatically when the button (and thus the arc) is deleted.
 */
void op_spinner_delete_cb(lv_event_t* e) {
    op_spinner_anim::stop(lv_event_get_target_obj(e));
}

/**
 * @brief Lazily create the busy spinner arc inside the button's icon slot
 *
 * Sized to the icon line-height and inserted at the icon's position so it
 * occupies the same footprint — toggling between glyph and spinner never reflows
 * the button. Created once and reused; returns the existing arc on later calls.
 */
lv_obj_t* ensure_op_spinner(lv_obj_t* btn, UiButtonData* data) {
    if (data->op_spinner) {
        return data->op_spinner;
    }
    if (!data->icon) {
        return nullptr;
    }

    // Match the icon glyph footprint so width/height stay constant.
    const lv_font_t* icon_font = lv_obj_get_style_text_font(data->icon, LV_PART_MAIN);
    int32_t sz = icon_font ? static_cast<int32_t>(lv_font_get_line_height(icon_font)) : 16;
    int32_t arc_width = sz >= 24 ? 3 : 2;

    lv_obj_t* arc = lv_arc_create(btn);
    lv_obj_set_size(arc, sz, sz);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_obj_set_style_opa(arc, LV_OPA_0, LV_PART_KNOB);
    lv_obj_add_style(arc, ThemeManager::instance().get_style(StyleRole::Spinner),
                     LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_0, LV_PART_MAIN);
    lv_arc_set_angles(arc, 0, op_spinner_anim::ARC_MAX_SWEEP);

    // Occupy the icon's slot in the flex flow.
    int32_t icon_index = lv_obj_get_index(data->icon);
    lv_obj_move_to_index(arc, icon_index);

    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN); // shown only while busy
    lv_obj_add_event_cb(arc, op_spinner_delete_cb, LV_EVENT_DELETE, nullptr);

    data->op_spinner = arc;
    return arc;
}

/**
 * @brief Render an op-state value (0=idle, 1=busy, 2=done) on the button
 */
void apply_op_state(lv_obj_t* btn, UiButtonData* data, int op_state) {
    if (!data || data->magic != UiButtonData::MAGIC || !data->icon) {
        return;
    }

    switch (op_state) {
    case 1: { // busy: hide glyph, show animated spinner
        lv_obj_t* arc = ensure_op_spinner(btn, data);
        lv_obj_add_flag(data->icon, LV_OBJ_FLAG_HIDDEN);
        if (arc) {
            lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
            op_spinner_anim::stop(arc); // restart cleanly if re-entered
            op_spinner_anim::start(arc);
        }
        break;
    }
    case 2: { // done: show check glyph, hide spinner
        if (data->op_spinner) {
            op_spinner_anim::stop(data->op_spinner);
            lv_obj_add_flag(data->op_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        const char* check_cp = ui_icon::lookup_codepoint("check");
        if (check_cp) {
            lv_label_set_text(data->icon, check_cp);
        }
        lv_obj_remove_flag(data->icon, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    case 0:
    default: { // idle: restore original glyph, hide spinner
        if (data->op_spinner) {
            op_spinner_anim::stop(data->op_spinner);
            lv_obj_add_flag(data->op_spinner, LV_OBJ_FLAG_HIDDEN);
        }
        if (data->op_idle_glyph[0] != '\0') {
            lv_label_set_text(data->icon, data->op_idle_glyph);
        }
        lv_obj_remove_flag(data->icon, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    }
    update_button_text_contrast(btn);
}

/**
 * @brief Observer callback for bind_op_state int subject changes
 *
 * The int subject value (0/1/2) drives the icon-slot rendering. The button is in
 * the observer's user_data, its UiButtonData in user_data of the button.
 */
void op_state_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_observer_get_user_data(observer));
    if (!btn) {
        return;
    }
    UiButtonData* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    apply_op_state(btn, data, lv_subject_get_int(subject));
}

/**
 * @brief XML apply callback for <ui_button> widget
 *
 * Delegates to standard object parser for base properties (align, hidden, etc.)
 * Also sets derived names for icon/label children if the button has a name.
 *
 * @param state XML parser state
 * @param attrs XML attributes
 */
void ui_button_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);

    void* item = lv_xml_state_get_item(state);
    lv_obj_t* btn = static_cast<lv_obj_t*>(item);
    UiButtonData* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));

    // Text binding convention:
    //   text="literal"       → static text (set during create, no action here)
    //   text="@subject_name" → '@' prefix triggers reactive subject binding
    //   bind_text="subject"  → LVGL-standard attribute, always a subject (no '@' needed)
    // Both paths share the same binding logic below.

    // Determine subject name from text="@..." or bind_text="..."
    const char* subject_name = nullptr;
    const char* fmt_attr = nullptr;
    const char* text_val = lv_xml_get_value_of(attrs, "text");
    const char* bind_text_val = lv_xml_get_value_of(attrs, "bind_text");

    if (bind_text_val && bind_text_val[0] != '\0') {
        // bind_text is always a subject name (LVGL standard)
        // Strip '@' prefix if present (for consistency with text="@subject" convention)
        subject_name = (bind_text_val[0] == '@') ? bind_text_val + 1 : bind_text_val;
        fmt_attr = lv_xml_get_value_of(attrs, "bind_text-fmt");
    } else if (text_val && text_val[0] == '@') {
        // text="@subject" — strip '@' prefix
        subject_name = text_val + 1;
        fmt_attr = lv_xml_get_value_of(attrs, "text-fmt");
    }

    if (subject_name && subject_name[0] != '\0' && data && data->magic == UiButtonData::MAGIC) {
        if (!data->label) {
            data->label = lv_label_create(btn);
            lv_obj_center(data->label);
        }

        lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
        if (subject) {
            if (fmt_attr) {
                fmt_attr = lv_strdup(fmt_attr);
                lv_obj_add_event_cb(data->label, lv_event_free_user_data_cb, LV_EVENT_DELETE,
                                    const_cast<char*>(fmt_attr));
            }
            lv_label_bind_text(data->label, subject, fmt_attr);

            // When subject changes, LVGL only redraws the label area — the
            // button background needs a full repaint. Defer invalidation via
            // lv_async_call so it runs after the observer chain completes
            // (invalidating mid-observer causes wrong style state).
            lv_subject_add_observer_obj(
                subject,
                [](lv_observer_t* obs, lv_subject_t*) {
                    lv_obj_t* parent_btn = static_cast<lv_obj_t*>(lv_observer_get_target_obj(obs));
                    if (parent_btn) {
                        helix::ui::async_call(
                            parent_btn,
                            [](void* ud) { lv_obj_invalidate(static_cast<lv_obj_t*>(ud)); },
                            parent_btn);
                    }
                },
                btn, nullptr);

            update_button_text_contrast(btn);
            spdlog::trace("[ui_button] Bound label to subject '{}'", subject_name);
        } else {
            spdlog::warn("[ui_button] Subject '{}' not found for text binding", subject_name);
            lv_label_set_text(data->label, subject_name);
            update_button_text_contrast(btn);
        }
    }

    // Handle bind_icon - bind the internal icon to a string subject
    const char* bind_icon = lv_xml_get_value_of(attrs, "bind_icon");
    if (bind_icon && data && data->magic == UiButtonData::MAGIC) {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_icon);
        if (subject) {
            // If button has no icon yet, create one now
            if (!data->icon) {
                data->icon = lv_label_create(btn);
                lv_obj_set_style_text_font(data->icon, get_button_icon_font(), LV_PART_MAIN);

                // Position icon appropriately
                if (data->label) {
                    // Icon + text: set up flex layout matching the position the
                    // button declared at create time. Mirrors the static-icon
                    // layout in ui_button_create().
                    // Clear any centering on the label (it was centered when text-only)
                    lv_obj_set_align(data->label, LV_ALIGN_DEFAULT);
                    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                          LV_FLEX_ALIGN_CENTER);

                    if (data->icon_vertical) {
                        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
                        // No pad_row - the label carries pad_top instead
                        lv_obj_set_style_pad_row(btn, 0, LV_PART_MAIN);
                        lv_obj_move_to_index(data->icon, data->icon_on_bottom ? -1 : 0);
                        // Small font for vertical layout labels (matches text_small)
                        lv_obj_set_style_text_font(
                            data->label, theme_manager_get_font("font_small"), LV_PART_MAIN);
                        lv_obj_set_style_pad_top(
                            data->label, theme_manager_get_spacing("space_xxs"), LV_PART_MAIN);
                    } else {
                        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
                        lv_obj_set_style_pad_column(btn, theme_manager_get_spacing("space_xs"),
                                                    LV_PART_MAIN);
                        // Position icon before or after text based on icon_on_right
                        lv_obj_move_to_index(data->icon, data->icon_on_right ? -1 : 0);
                    }
                    spdlog::trace("[ui_button] bind_icon: created icon with {} flex layout",
                                  data->icon_vertical ? "column" : "row");
                } else {
                    // Icon only: center it
                    lv_obj_center(data->icon);
                    spdlog::trace("[ui_button] bind_icon: created centered icon-only");
                }
            }

            // Get initial icon value from subject and set icon text
            // Value can be either an icon name ("light") or raw codepoint ("\xF3\xB0\x8C\xB6")
            const char* icon_value = lv_subject_get_string(subject);
            if (icon_value && strlen(icon_value) > 0) {
                const unsigned char first_byte = static_cast<unsigned char>(icon_value[0]);
                if (first_byte >= 0xF0) {
                    // Value is already a codepoint - use directly
                    lv_label_set_text(data->icon, icon_value);
                } else {
                    // Value is an icon name - look up the codepoint
                    const char* codepoint = ui_icon::lookup_codepoint(icon_value);
                    if (!codepoint) {
                        // Try stripping legacy prefix
                        const char* stripped = ui_icon::strip_legacy_prefix(icon_value);
                        if (stripped != icon_value) {
                            codepoint = ui_icon::lookup_codepoint(stripped);
                        }
                    }
                    if (codepoint) {
                        lv_label_set_text(data->icon, codepoint);
                    } else {
                        spdlog::warn("[ui_button] Icon '{}' not found for bind_icon", icon_value);
                    }
                }
            }

            // Add observer to update icon when subject changes
            lv_subject_add_observer_obj(subject, icon_subject_observer_cb, data->icon, btn);

            // Re-apply contrast after adding icon
            update_button_text_contrast(btn);
            spdlog::trace("[ui_button] Bound icon to subject '{}'", bind_icon);
        } else {
            spdlog::warn("[ui_button] Subject '{}' not found for bind_icon", bind_icon);
        }
    }

    // Handle bind_op_state - drive idle/busy/done rendering from an int subject.
    // 0=idle (original icon glyph), 1=busy (animated spinner in the icon slot),
    // 2=done ("check" glyph). Requires an icon to render into.
    const char* bind_op_state = lv_xml_get_value_of(attrs, "bind_op_state");
    if (bind_op_state && bind_op_state[0] != '\0' && data && data->magic == UiButtonData::MAGIC) {
        if (!data->icon) {
            spdlog::warn("[ui_button] bind_op_state '{}' ignored — button has no icon",
                         bind_op_state);
        } else {
            // Cache the current (idle) glyph so it can be restored after busy/done.
            const char* current_glyph = lv_label_get_text(data->icon);
            if (current_glyph) {
                lv_strlcpy(data->op_idle_glyph, current_glyph, sizeof(data->op_idle_glyph));
            }

            lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_op_state);
            if (subject) {
                // Render the initial state, then react to changes. Observer is
                // object-tied (auto-removed when the button is deleted), matching
                // the bind_icon precedent above.
                apply_op_state(btn, data, lv_subject_get_int(subject));
                lv_subject_add_observer_obj(subject, op_state_observer_cb, btn, btn);
                spdlog::trace("[ui_button] Bound op_state to subject '{}'", bind_op_state);
            } else {
                spdlog::warn("[ui_button] Subject '{}' not found for bind_op_state", bind_op_state);
            }
        }
    }

    // Handle long_mode - set label text truncation mode (dots, clip, scroll, etc.)
    const char* long_mode = lv_xml_get_value_of(attrs, "long_mode");
    if (long_mode && data && data->magic == UiButtonData::MAGIC && data->label) {
        lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_WRAP; // default
        if (strcmp(long_mode, "dots") == 0)
            mode = LV_LABEL_LONG_MODE_DOTS;
        else if (strcmp(long_mode, "clip") == 0)
            mode = LV_LABEL_LONG_MODE_CLIP;
        else if (strcmp(long_mode, "scroll") == 0)
            mode = LV_LABEL_LONG_MODE_SCROLL;
        else if (strcmp(long_mode, "scroll_circular") == 0)
            mode = LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
        lv_label_set_long_mode(data->label, mode);
        // Label must have a bounded width for long_mode to take effect
        lv_obj_set_width(data->label, lv_pct(100));
        // Center text within the expanded label
        lv_obj_set_style_text_align(data->label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        spdlog::trace("[ui_button] Set long_mode to '{}'", long_mode);
    }

    // If button has a name, give icon a derived name so it can be found
    const char* btn_name = lv_obj_get_name(btn);
    if (btn_name && strlen(btn_name) > 0) {
        if (data && data->magic == UiButtonData::MAGIC && data->icon) {
            // Set icon name as "{button_name}_icon"
            char icon_name[128];
            snprintf(icon_name, sizeof(icon_name), "%s_icon", btn_name);
            lv_obj_set_name(data->icon, icon_name);
            spdlog::trace("[ui_button] Set icon name to '{}'", icon_name);
        }
    }

    // Attribute-level analog of the `<bind_flag_if_eq subject="ui_breakpoint"
    // flag="hidden" ref_value="N"/>` child pattern used on plain XML objects —
    // but targeting the internal label instead of the button itself, which XML
    // child bindings can't reach. Lets `ui_button` collapse to icon-only at a
    // specific breakpoint without parallel button instances.
    const char* label_hidden_bp = lv_xml_get_value_of(attrs, "label_hidden_if_bp_eq");
    if (label_hidden_bp && data && data->magic == UiButtonData::MAGIC && data->label) {
        lv_subject_t* bp_subject = lv_xml_get_subject(&state->scope, "ui_breakpoint");
        if (bp_subject) {
            intptr_t ref_val = static_cast<intptr_t>(atoi(label_hidden_bp));
            lv_subject_add_observer_obj(
                bp_subject,
                [](lv_observer_t* o, lv_subject_t* s) {
                    auto* target = static_cast<lv_obj_t*>(lv_observer_get_target_obj(o));
                    intptr_t ref = reinterpret_cast<intptr_t>(lv_observer_get_user_data(o));
                    ui_button_set_label_hidden(target, lv_subject_get_int(s) == ref);
                },
                btn, reinterpret_cast<void*>(ref_val));
            spdlog::trace("[ui_button] label_hidden_if_bp_eq={} bound to ui_breakpoint",
                          static_cast<int>(ref_val));
        } else {
            spdlog::warn("[ui_button] label_hidden_if_bp_eq set but ui_breakpoint subject "
                         "not found");
        }
    }
}

} // namespace

void ui_button_init() {
    lv_xml_register_widget("ui_button", ui_button_create, ui_button_apply);
    spdlog::trace("[ui_button] Registered semantic button widget");
}

void ui_button_invalidate_icon_font_cache() {
    s_icon_font_resolved = false;
}

void ui_button_set_text(lv_obj_t* btn, const char* text) {
    if (!btn || !text) {
        return;
    }
    auto* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC || !data->label) {
        return;
    }
    lv_label_set_text(data->label, text);
    // Label invalidation only redraws the label area — force the whole button
    // to repaint so the background behind the label is consistent
    lv_obj_invalidate(btn);
}

void ui_button_set_icon(lv_obj_t* btn, const char* icon_name) {
    if (!btn || !icon_name) {
        return;
    }
    auto* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC || !data->icon) {
        return;
    }

    const char* codepoint = ui_icon::lookup_codepoint(icon_name);
    if (!codepoint) {
        const char* stripped = ui_icon::strip_legacy_prefix(icon_name);
        if (stripped != icon_name) {
            codepoint = ui_icon::lookup_codepoint(stripped);
        }
    }
    if (!codepoint) {
        spdlog::warn("[ui_button] Icon '{}' not found", icon_name);
        return;
    }

    lv_label_set_text(data->icon, codepoint);
    lv_obj_invalidate(btn);
}

void ui_button_set_label_hidden(lv_obj_t* btn, bool hidden) {
    if (!btn) {
        return;
    }
    auto* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC || !data->label) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(data->label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(data->label, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* ui_button_get_icon(lv_obj_t* btn) {
    if (!btn) {
        return nullptr;
    }
    auto* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC) {
        return nullptr;
    }
    return data->icon;
}

lv_obj_t* ui_button_get_op_spinner(lv_obj_t* btn) {
    if (!btn) {
        return nullptr;
    }
    auto* data = static_cast<UiButtonData*>(lv_obj_get_user_data(btn));
    if (!data || data->magic != UiButtonData::MAGIC) {
        return nullptr;
    }
    return data->op_spinner;
}
