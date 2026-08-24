// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_common.h"

#include "ui_component_header_bar.h"
#include "ui_icon.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "display_manager.h"
#include "layout_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

// ============================================================================
// HEADER BAR SETUP
// ============================================================================

lv_obj_t* ui_panel_setup_header(lv_obj_t* panel, lv_obj_t* parent_screen, const char* header_name) {
    if (!panel || !parent_screen || !header_name) {
        spdlog::warn("[PanelCommon] Invalid parameters for header setup");
        return nullptr;
    }

    lv_obj_t* header = lv_obj_find_by_name(panel, header_name);
    if (header) {
        ui_component_header_bar_setup(header, parent_screen);
        spdlog::debug("[PanelCommon] Header '{}' configured for responsive height", header_name);
    } else {
        spdlog::warn("[PanelCommon] Header '{}' not found in panel", header_name);
    }

    return header;
}

// ============================================================================
// CONTENT PADDING SETUP
// ============================================================================

lv_obj_t* ui_panel_setup_content_padding(lv_obj_t* panel, lv_obj_t* parent_screen,
                                         const char* content_name) {
    if (!panel || !parent_screen || !content_name) {
        spdlog::warn("[PanelCommon] Invalid parameters for content padding setup");
        return nullptr;
    }

    lv_obj_t* content = lv_obj_find_by_name(panel, content_name);
    if (content) {
        lv_coord_t vertical_padding = ui_get_header_content_padding();

        // Set vertical padding (top/bottom) responsively, keep horizontal at space_md
        lv_obj_set_style_pad_top(content, vertical_padding, 0);
        lv_obj_set_style_pad_bottom(content, vertical_padding, 0);
        lv_obj_set_style_pad_left(content, theme_manager_get_spacing("space_md"), 0);
        lv_obj_set_style_pad_right(content, theme_manager_get_spacing("space_md"), 0);

        spdlog::debug("[PanelCommon] Content '{}' padding: top/bottom={}px, left/right={}px",
                      content_name, vertical_padding, theme_manager_get_spacing("space_md"));
    } else {
        spdlog::warn("[PanelCommon] Content '{}' not found in panel", content_name);
    }

    return content;
}

// ============================================================================
// RESIZE CALLBACK SETUP
// ============================================================================

// Generic resize callback that uses context to update content padding
static void panel_resize_callback_wrapper(ui_panel_resize_context_t* context) {
    if (!context || !context->panel || !context->parent_screen || !context->content_name) {
        return;
    }

    lv_obj_t* content = lv_obj_find_by_name(context->panel, context->content_name);
    if (content) {
        lv_coord_t vertical_padding = ui_get_header_content_padding();

        // Update vertical padding (top/bottom) responsively, keep horizontal at space_md
        lv_obj_set_style_pad_top(content, vertical_padding, 0);
        lv_obj_set_style_pad_bottom(content, vertical_padding, 0);
        lv_obj_set_style_pad_left(content, theme_manager_get_spacing("space_md"), 0);
        lv_obj_set_style_pad_right(content, theme_manager_get_spacing("space_md"), 0);
    }
}

// Context storage for trampolines (max 8 panels using common resize)
static ui_panel_resize_context_t* trampoline_contexts[8] = {nullptr};
static int trampoline_count = 0;

// Actual trampoline implementations
static void resize_trampoline_impl_0(void) {
    panel_resize_callback_wrapper(trampoline_contexts[0]);
}
static void resize_trampoline_impl_1(void) {
    panel_resize_callback_wrapper(trampoline_contexts[1]);
}
static void resize_trampoline_impl_2(void) {
    panel_resize_callback_wrapper(trampoline_contexts[2]);
}
static void resize_trampoline_impl_3(void) {
    panel_resize_callback_wrapper(trampoline_contexts[3]);
}
static void resize_trampoline_impl_4(void) {
    panel_resize_callback_wrapper(trampoline_contexts[4]);
}
static void resize_trampoline_impl_5(void) {
    panel_resize_callback_wrapper(trampoline_contexts[5]);
}
static void resize_trampoline_impl_6(void) {
    panel_resize_callback_wrapper(trampoline_contexts[6]);
}
static void resize_trampoline_impl_7(void) {
    panel_resize_callback_wrapper(trampoline_contexts[7]);
}

static DisplayManager::ResizeCallback trampoline_funcs[8] = {
    resize_trampoline_impl_0, resize_trampoline_impl_1, resize_trampoline_impl_2,
    resize_trampoline_impl_3, resize_trampoline_impl_4, resize_trampoline_impl_5,
    resize_trampoline_impl_6, resize_trampoline_impl_7,
};

void ui_panel_setup_resize_callback(ui_panel_resize_context_t* context) {
    if (!context) {
        spdlog::warn("[PanelCommon] Invalid resize context");
        return;
    }

    if (trampoline_count >= 8) {
        spdlog::error("[PanelCommon] Too many panels using common resize (max 8)");
        return;
    }

    // Store context and register corresponding trampoline
    trampoline_contexts[trampoline_count] = context;
    if (auto* dm = DisplayManager::instance()) {
        dm->register_resize_callback(trampoline_funcs[trampoline_count]);
    }
    trampoline_count++;

    spdlog::debug("[PanelCommon] Resize callback registered for content '{}'",
                  context->content_name);
}

// ============================================================================
// OVERLAY PANEL SETUP (For panels using overlay_panel.xml wrapper)
// ============================================================================

void ui_overlay_panel_setup_standard(lv_obj_t* panel, lv_obj_t* parent_screen,
                                     const char* header_name, const char* content_name) {
    if (!panel || !parent_screen) {
        spdlog::error("[PanelCommon] Invalid parameters for overlay panel setup");
        return;
    }

    spdlog::debug("[PanelCommon] Setting up overlay panel with header='{}', content='{}'",
                  header_name, content_name);

    // Verify header exists (back button wiring handled by header_bar.xml event_cb)
    lv_obj_t* header = lv_obj_find_by_name(panel, header_name);
    if (!header) {
        spdlog::warn("[PanelCommon] Header '{}' not found in overlay panel", header_name);
    }

    // Apply pressed style to back button for visual touch feedback
    lv_obj_t* back_btn = header ? lv_obj_find_by_name(header, "back_button") : nullptr;
    if (back_btn) {
        static lv_style_t back_pressed;
        static bool inited = false;
        if (!inited) {
            lv_style_init(&back_pressed);
            lv_style_set_opa(&back_pressed, LV_OPA_50);
            inited = true;
        }
        lv_obj_add_style(back_btn, &back_pressed, LV_PART_MAIN | LV_STATE_PRESSED);

        // Portrait overlays drop from the top (ui_set_overlay_geometry +
        // overlay_animate_slide_in), so a left-pointing chevron would point the
        // wrong way out. chevron_up is the exact mirror of `back`. This is the
        // ONLY place this swap runs: every real overlay panel (bed mesh, AMS,
        // motion, power, ...) goes through here via
        // OverlayBase/ui_overlay_panel_setup_standard.
        // ui_component_header_bar.cpp's ui_component_header_bar_setup() is a
        // separate, unreachable path (no caller of ui_panel_setup_header()
        // above exists) — it does not carry this swap.
        //
        // DELIBERATELY on detect_layout_type() (physical), NOT LayoutManager —
        // this must match overlay_animate_slide_in() in ui_nav_manager.cpp,
        // which animates on the physical axis. A --layout override reshapes the
        // XML and the canvas-height decisions, but the slide animation still
        // follows the glass, and the chevron must agree with the slide. #1255.
        if (helix::is_portrait_layout(helix::detect_layout_type(
                lv_obj_get_width(parent_screen), lv_obj_get_height(parent_screen)))) {
            lv_obj_t* icon = lv_obj_get_child(back_btn, 0);
            if (icon) {
                ui_icon_set_source(icon, "chevron_up");
            }
        }
    }

    // 2. Setup content padding (responsive vertical, fixed horizontal)
    // Note: overlay_panel.xml already sets style_pad_all="#space_lg"
    // This is a no-op unless we need to override for specific screen sizes
    lv_obj_t* content = lv_obj_find_by_name(panel, content_name);
    if (content) {
        spdlog::debug("[PanelCommon] Content area '{}' found, padding already set by XML",
                      content_name);
    } else {
        spdlog::warn("[PanelCommon] Content area '{}' not found in overlay panel", content_name);
    }

    spdlog::debug("[PanelCommon] Overlay panel setup complete");
}

lv_obj_t* ui_overlay_panel_wire_action_button(lv_obj_t* panel, lv_event_cb_t callback,
                                              const char* header_name, void* user_data) {
    if (!panel || !callback || !header_name) {
        spdlog::warn("[PanelCommon] Invalid parameters for overlay action button wiring");
        return nullptr;
    }

    // Find header_bar widget
    lv_obj_t* header = lv_obj_find_by_name(panel, header_name);
    if (!header) {
        spdlog::warn("[PanelCommon] Header '{}' not found in overlay panel", header_name);
        return nullptr;
    }

    // Find action_button within header_bar
    lv_obj_t* action_btn = lv_obj_find_by_name(header, "action_button");
    if (!action_btn) {
        spdlog::warn("[PanelCommon] Action button not found in header '{}'", header_name);
        return nullptr;
    }

    // Wire to provided callback with user_data
    // Note: visibility is controlled by XML hide_action_button prop
    lv_obj_add_event_cb(action_btn, callback, LV_EVENT_CLICKED, user_data);
    spdlog::debug("[PanelCommon] Action button wired in header '{}'", header_name);

    return action_btn;
}
