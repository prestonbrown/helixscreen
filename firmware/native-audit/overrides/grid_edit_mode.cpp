// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_edit_mode.h"

#include "ui_fonts.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_widget_catalog_overlay.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "grid_layout.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace helix {

// MDI icon_xmark glyph (U+F0156)
static constexpr const char* ICON_XMARK = "\xF3\xB0\x85\x96";

// Safe deferred deletion for objects that may be children of a container
// about to be cleaned via lv_obj_clean(). Reparenting to lv_layer_top()
// removes the object from the container's child list, preventing double-free
// when lv_obj_clean runs before the async delete fires.  Hiding prevents
// the reparented object from being visible in lv_layer_top().
static void safe_deferred_delete(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_parent(obj, lv_layer_top());
    lv_obj_delete_async(obj);
}

// Drag visual constants
static constexpr int GHOST_BORDER_WIDTH = 2;
static constexpr lv_opa_t GHOST_BORDER_OPA = LV_OPA_50;
static constexpr int PREVIEW_BORDER_WIDTH = 3;
static constexpr lv_opa_t DRAG_SHADOW_OPA = LV_OPA_40;
static constexpr int DRAG_SHADOW_WIDTH = 12;
static constexpr int DRAG_SHADOW_OFS = 4;

// Resize edge detection: 18px inside + 18px outside the widget edge = 36px total
static constexpr int EDGE_HIT_INWARD = 18;
static constexpr int EDGE_HIT_MARGIN = 18;

// disable_widget_clicks_recursive() is in ui_utils.h (helix::ui namespace)
using helix::ui::disable_widget_clicks_recursive;

GridEditMode::~GridEditMode() {
    if (active_) {
        exit();
    }
}

void GridEditMode::enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index) {
    if (active_) {
        spdlog::debug("[GridEditMode] Already active, ignoring enter()");
        return;
    }
    active_ = true;
    container_ = container;
    config_ = config;
    page_index_ = page_index;
    lv_subject_set_int(&get_home_edit_mode_subject(), 1);

    // Disable clickability on all widget descendants so their individual
    // click handlers and press animations don't fire during edit mode.
    disable_widget_clicks_recursive(container_);

    // Create dots overlay (event shield + visual grid dots)
    create_dots_overlay();

    // Sync config grid positions from actual screen positions.
    // populate_widgets() may have been called many times during startup
    // (hardware gates firing), and config positions may not match the
    // final visual layout. This ensures config reflects reality.
    sync_config_from_screen();

    spdlog::info("[GridEditMode] Entered edit mode (page {})", page_index_);
}

void GridEditMode::exit() {
    if (!active_) {
        return;
    }
    active_ = false;

    // Null ALL overlay/widget pointers WITHOUT deleting.  rebuild_cb_
    // calls populate_widgets() → lv_obj_clean(container) which destroys
    // every container child.  Deleting them individually here first is
    // redundant and dangerous: this path runs inside indev_proc_release,
    // and synchronous lv_obj_delete during input processing can corrupt
    // LVGL's child list iteration → SIGSEGV in lv_obj_get_parent.
    //
    // Also skip cleanup_drag_state() destroy calls for the same reason;
    // just clear the drag/resize flags directly.
    drag_pending_ = false;
    if (dragging_ && selected_) {
        lv_obj_remove_flag(selected_, LV_OBJ_FLAG_FLOATING);
    }
    dragging_ = false;
    resizing_ = false;
    drag_cfg_idx_ = -1;
    drag_orig_col_ = -1;
    drag_orig_row_ = -1;
    drag_orig_colspan_ = 1;
    drag_orig_rowspan_ = 1;
    drag_ghost_ = nullptr;
    snap_preview_ = nullptr;
    snap_preview_col_ = -1;
    snap_preview_row_ = -1;
    selected_ = nullptr;
    configure_btn_ = nullptr;
    remove_btn_ = nullptr;
    selection_overlay_ = nullptr;
    dots_overlay_ = nullptr;
    delete_page_btn_ = nullptr;

    lv_subject_set_int(&get_home_edit_mode_subject(), 0);

    if (config_) {
        config_->save();
    }
    // Defer the rebuild to the next timer tick so lv_obj_clean runs outside
    // indev_proc_release — synchronous deletion during input processing
    // corrupts LVGL's child list iteration (#814).
    auto save = save_cb_;
    schedule_deferred_rebuild([save]() {
        if (save)
            save();
    });

    container_ = nullptr;
    config_ = nullptr;
    spdlog::info("[GridEditMode] Exited edit mode");
}

void GridEditMode::select_widget(lv_obj_t* widget) {
    if (!active_) {
        return;
    }
    if (widget == selected_) {
        return;
    }
    destroy_selection_chrome();
    selected_ = widget;
    if (widget && container_) {
        create_selection_chrome(widget);
    }
    spdlog::debug("[GridEditMode] Selected widget: {}", static_cast<void*>(widget));
}

void GridEditMode::handle_click(lv_event_t* /*e*/) {
    if (!active_ || !container_) {
        return;
    }

    // Get click point in screen coordinates
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Ensure layout is up-to-date (enter() may have just modified the container)
    lv_obj_update_layout(container_);

    // Hit-test child widgets (skip floating overlays like dots_overlay_ and selection_overlay_)
    lv_obj_t* hit = nullptr;
    uint32_t child_count = lv_obj_get_child_count(container_);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(i));
        if (!child) {
            continue;
        }
        // Skip our overlay objects
        if (child == dots_overlay_ || child == selection_overlay_) {
            continue;
        }
        // Skip floating objects (they are overlays, not grid widgets)
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }
        // Skip decoration objects (merged card backgrounds have no name)
        if (!lv_obj_get_name(child)) {
            continue;
        }

        lv_area_t coords;
        lv_obj_get_coords(child, &coords);
        if (point.x >= coords.x1 && point.x <= coords.x2 && point.y >= coords.y1 &&
            point.y <= coords.y2) {
            hit = child;
            // Keep searching — later children are drawn on top, so prefer last match
        }
    }

    if (hit) {
        lv_area_t hit_area;
        lv_obj_get_coords(hit, &hit_area);
        const char* wname = lv_obj_get_name(hit);
        spdlog::debug(
            "[GridEditMode] Hit widget '{}': screen=({},{})→({},{}) size={}x{} click=({},{}) "
            "state=0x{:x} clickable={} pressed={} floating={}",
            wname ? wname : "?", hit_area.x1, hit_area.y1, hit_area.x2, hit_area.y2,
            hit_area.x2 - hit_area.x1, hit_area.y2 - hit_area.y1, point.x, point.y,
            static_cast<uint32_t>(lv_obj_get_state(hit)),
            lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE),
            (lv_obj_get_state(hit) & LV_STATE_PRESSED) != 0,
            lv_obj_has_flag(hit, LV_OBJ_FLAG_FLOATING));

        // Log widget position BEFORE select
        lv_area_t pre_coords;
        lv_obj_get_coords(hit, &pre_coords);

        select_widget(hit);

        // Log widget position AFTER select (chrome created)
        lv_area_t post_coords;
        lv_obj_get_coords(hit, &post_coords);
        if (pre_coords.x1 != post_coords.x1 || pre_coords.y1 != post_coords.y1) {
            spdlog::warn("[GridEditMode] WIDGET SHIFTED after select! "
                         "before=({},{}) after=({},{}) delta=({},{})",
                         pre_coords.x1, pre_coords.y1, post_coords.x1, post_coords.y1,
                         post_coords.x1 - pre_coords.x1, post_coords.y1 - pre_coords.y1);
        } else {
            spdlog::debug("[GridEditMode] Widget position stable after select: ({},{})",
                          post_coords.x1, post_coords.y1);
        }
    } else {
        // Before deselecting, check if the click is within the edge resize zone
        // of the currently selected widget. If so, keep the selection — the user
        // is clicking in the outer 18px margin to start a resize drag.
        bool in_edge_zone = false;
        if (selected_) {
            lv_area_t sel_area;
            lv_obj_get_coords(selected_, &sel_area);
            if (point.x >= sel_area.x1 - EDGE_HIT_MARGIN &&
                point.x <= sel_area.x2 + EDGE_HIT_MARGIN &&
                point.y >= sel_area.y1 - EDGE_HIT_MARGIN &&
                point.y <= sel_area.y2 + EDGE_HIT_MARGIN) {
                in_edge_zone = true;
                spdlog::debug("[GridEditMode] handle_click: no widget at ({},{}) but within "
                              "edge zone of selected widget — keeping selection",
                              point.x, point.y);
            }
        }
        if (!in_edge_zone) {
            spdlog::debug("[GridEditMode] handle_click: no widget at ({},{}) — {} children checked",
                          point.x, point.y, child_count);
            select_widget(nullptr);
        }
    }
}

void GridEditMode::create_selection_chrome(lv_obj_t* widget) {
    if (!container_) {
        return;
    }

    // Get widget coordinates (screen-absolute)
    lv_area_t widget_area;
    lv_obj_get_coords(widget, &widget_area);

    // Get container's outer coords (selection overlay is positioned relative to these,
    // since it uses LV_OBJ_FLAG_FLOATING which ignores content padding)
    lv_area_t container_area;
    lv_obj_get_coords(container_, &container_area);

    // LVGL adds parent padding even for FLOATING objects (see lv_obj_move_to),
    // so subtract it to get the correct screen position.
    int pad_left = lv_obj_get_style_space_left(container_, LV_PART_MAIN);
    int pad_top = lv_obj_get_style_space_top(container_, LV_PART_MAIN);
    int rel_x1 = widget_area.x1 - container_area.x1 - pad_left;
    int rel_y1 = widget_area.y1 - container_area.y1 - pad_top;
    int widget_w = widget_area.x2 - widget_area.x1;
    int widget_h = widget_area.y2 - widget_area.y1;

    spdlog::debug("[GridEditMode] Chrome coords: widget_screen=({},{})→({},{}) "
                  "container_screen=({},{}) pad=({},{}) rel=({},{}) size={}x{}",
                  widget_area.x1, widget_area.y1, widget_area.x2, widget_area.y2, container_area.x1,
                  container_area.y1, pad_left, pad_top, rel_x1, rel_y1, widget_w, widget_h);

    // Create floating overlay container for selection chrome.
    // The overlay itself draws the connecting border (rounded, muted).
    lv_color_t accent = theme_get_accent_color();
    int radius = theme_manager_get_spacing("border_radius");

    selection_overlay_ = lv_obj_create(container_);
    lv_obj_set_pos(selection_overlay_, rel_x1, rel_y1);
    lv_obj_set_size(selection_overlay_, widget_w, widget_h);
    lv_obj_add_flag(selection_overlay_, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(selection_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    // Let touch events pass through to the container for long-press drag detection.
    // The X button child is independently clickable.
    lv_obj_remove_flag(selection_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(selection_overlay_, LV_OPA_TRANSP, 0);
    // No overlay border — edge bars provide the visual for resizable widgets,
    // non-resizable get just corner brackets with no connecting lines.
    lv_obj_set_style_border_width(selection_overlay_, 0, 0);
    lv_obj_set_style_radius(selection_overlay_, radius, 0);
    lv_obj_set_style_pad_all(selection_overlay_, 0, 0);
    // Corner bracket styling: two bars per corner forming a square L-bracket
    constexpr int BAR_LEN = 16;
    constexpr int BAR_THICK = 3;

    int bar_count = 0;
    auto make_bar = [&](int x, int y, int w, int h) {
        lv_obj_t* bar = lv_obj_create(selection_overlay_);
        lv_obj_set_pos(bar, x, y);
        lv_obj_set_size(bar, w, h);
        lv_obj_set_style_bg_color(bar, accent, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        bar_count++;
    };

    // Top-left
    make_bar(0, 0, BAR_LEN, BAR_THICK);
    make_bar(0, 0, BAR_THICK, BAR_LEN);
    // Top-right
    make_bar(widget_w - BAR_LEN, 0, BAR_LEN, BAR_THICK);
    make_bar(widget_w - BAR_THICK, 0, BAR_THICK, BAR_LEN);
    // Bottom-left
    make_bar(0, widget_h - BAR_THICK, BAR_LEN, BAR_THICK);
    make_bar(0, widget_h - BAR_LEN, BAR_THICK, BAR_LEN);
    // Bottom-right
    make_bar(widget_w - BAR_LEN, widget_h - BAR_THICK, BAR_LEN, BAR_THICK);
    make_bar(widget_w - BAR_THICK, widget_h - BAR_LEN, BAR_THICK, BAR_LEN);

    // Add resize edge handles for scalable widgets: thinner, lower opacity than corners
    if (is_selected_widget_resizable()) {
        constexpr int EDGE_THICK = 1;
        constexpr int HANDLE_INSET = BAR_LEN + 4; // Start after corner brackets

        auto make_edge_bar = [&](int x, int y, int w, int h) {
            lv_obj_t* bar = lv_obj_create(selection_overlay_);
            lv_obj_set_pos(bar, x, y);
            lv_obj_set_size(bar, w, h);
            lv_obj_set_style_bg_color(bar, accent, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_20, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 0, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            bar_count++;
        };

        // Right edge handle
        if (widget_h > 2 * HANDLE_INSET) {
            make_edge_bar(widget_w - EDGE_THICK, HANDLE_INSET, EDGE_THICK,
                          widget_h - 2 * HANDLE_INSET);
        }

        // Left edge handle
        if (widget_h > 2 * HANDLE_INSET) {
            make_edge_bar(0, HANDLE_INSET, EDGE_THICK, widget_h - 2 * HANDLE_INSET);
        }

        // Bottom edge handle
        if (widget_w > 2 * HANDLE_INSET) {
            make_edge_bar(HANDLE_INSET, widget_h - EDGE_THICK, widget_w - 2 * HANDLE_INSET,
                          EDGE_THICK);
        }

        // Top edge handle
        if (widget_w > 2 * HANDLE_INSET) {
            make_edge_bar(HANDLE_INSET, 0, widget_w - 2 * HANDLE_INSET, EDGE_THICK);
        }
    }

    // Pulse animation on corner brackets ONLY (not edge bars) when animations enabled.
    // Corner brackets are the first 8 children (2 bars x 4 corners).
    int corner_bar_count = 8; // Always 8 corner bracket bars
    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, selection_overlay_);
        lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&anim, 1000);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_playback_duration(&anim, 1000);
        lv_anim_set_exec_cb(&anim, [](void* obj, int32_t val) {
            auto* overlay = static_cast<lv_obj_t*>(obj);
            // Only pulse the first 8 children (corner brackets), skip edge bars
            uint32_t count = std::min<uint32_t>(lv_obj_get_child_count(overlay), 8u); // AUDIT OVERRIDE: int32_t=long on Xtensa
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t* child = lv_obj_get_child(overlay, static_cast<int32_t>(i));
                lv_obj_set_style_bg_opa(child, static_cast<lv_opa_t>(val), 0);
            }
        });
        lv_anim_start(&anim);
    }

    // Trash removal button — positioned on the container (not the overlay) so it
    // isn't clipped by the overlay or widget bounds. Uses FLOATING positioning
    // relative to the container's content area.
    // Chrome button size/icon scale responsively — a fixed 36/24 pair dominates
    // the 480x272 micro viewport.
    lv_subject_t* chrome_bp_subj = theme_manager_get_breakpoint_subject();
    const UiBreakpoint chrome_bp =
        chrome_bp_subj ? as_breakpoint(lv_subject_get_int(chrome_bp_subj)) : UiBreakpoint::Medium;
    int BTN_SIZE = 36;
    const lv_font_t* chrome_icon_font = &mdi_icons_24;
    switch (chrome_bp) {
    case UiBreakpoint::Micro:
        BTN_SIZE = 24;
        chrome_icon_font = &mdi_icons_16;
        break;
    case UiBreakpoint::Tiny:
        BTN_SIZE = 28;
        chrome_icon_font = &mdi_icons_16;
        break;
    case UiBreakpoint::Small:
        BTN_SIZE = 32;
        chrome_icon_font = &mdi_icons_24;
        break;
    case UiBreakpoint::Medium:
        BTN_SIZE = 36;
        chrome_icon_font = &mdi_icons_24;
        break;
    case UiBreakpoint::Large:
        BTN_SIZE = 40;
        chrome_icon_font = &mdi_icons_24;
        break;
    case UiBreakpoint::XLarge:
        BTN_SIZE = 48;
        chrome_icon_font = &mdi_icons_32;
        break;
    case UiBreakpoint::XXLarge:
        BTN_SIZE = 56;
        chrome_icon_font = &mdi_icons_32;
        break;
    }
    const int BTN_OVERHANG = BTN_SIZE / 4; // 25% shift outside widget bounds
    remove_btn_ = lv_obj_create(container_);
    lv_obj_t* x_btn = remove_btn_;
    lv_obj_add_flag(x_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(x_btn, rel_x1 + widget_w - BTN_SIZE + BTN_OVERHANG, rel_y1 - BTN_OVERHANG);
    lv_obj_set_size(x_btn, BTN_SIZE, BTN_SIZE);
    lv_obj_set_style_radius(x_btn, LV_RADIUS_CIRCLE, 0);
    lv_color_t btn_bg = theme_manager_get_color("text");
    lv_obj_set_style_bg_color(x_btn, btn_bg, 0);
    lv_obj_set_style_bg_opa(x_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(x_btn, 0, 0);
    lv_obj_set_style_pad_all(x_btn, 0, 0);
    lv_obj_add_flag(x_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(x_btn, LV_OBJ_FLAG_SCROLLABLE);

    // Trash icon with contrast color for readability on the text-colored bg
    lv_obj_t* x_label = lv_label_create(x_btn);
    lv_label_set_text(x_label, ICON_TRASH);
    lv_obj_set_style_text_font(x_label, chrome_icon_font, 0);
    lv_obj_set_style_text_color(x_label, theme_manager_get_contrast_color(btn_bg), 0);
    lv_obj_center(x_label);

    // (X) button click handler — exception: dynamic overlay chrome uses lv_obj_add_event_cb
    lv_obj_add_event_cb(
        x_btn,
        [](lv_event_t* ev) {
            auto* self = static_cast<GridEditMode*>(lv_event_get_user_data(ev));
            self->remove_selected_widget();
        },
        LV_EVENT_CLICKED, this);

    // Configure button — upper-left corner, mirroring trash in upper-right.
    // Only shown for widgets that advertise has_edit_configure().
    {
        auto* raw = lv_obj_get_user_data(widget);
        auto* pw = raw ? static_cast<PanelWidget*>(raw) : nullptr;
        if (pw && pw->has_edit_configure()) {
            configure_btn_ = lv_obj_create(container_);
            lv_obj_t* cfg_btn = configure_btn_;
            lv_obj_add_flag(cfg_btn, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_pos(cfg_btn, rel_x1 - BTN_OVERHANG, rel_y1 - BTN_OVERHANG);
            lv_obj_set_size(cfg_btn, BTN_SIZE, BTN_SIZE);
            lv_obj_set_style_radius(cfg_btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(cfg_btn, btn_bg, 0);
            lv_obj_set_style_bg_opa(cfg_btn, LV_OPA_50, 0);
            lv_obj_set_style_border_width(cfg_btn, 0, 0);
            lv_obj_set_style_pad_all(cfg_btn, 0, 0);
            lv_obj_add_flag(cfg_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(cfg_btn, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* cfg_label = lv_label_create(cfg_btn);
            lv_label_set_text(cfg_label, ICON_SETTINGS);
            lv_obj_set_style_text_font(cfg_label, chrome_icon_font, 0);
            lv_obj_set_style_text_color(cfg_label, theme_manager_get_contrast_color(btn_bg), 0);
            lv_obj_center(cfg_label);

            lv_obj_add_event_cb(
                cfg_btn,
                [](lv_event_t* ev) {
                    auto* self = static_cast<GridEditMode*>(lv_event_get_user_data(ev));
                    self->configure_selected_widget();
                },
                LV_EVENT_CLICKED, this);
        }
    }

    // Verify: where did the overlay actually end up on screen?
    lv_obj_update_layout(selection_overlay_);
    lv_area_t overlay_area;
    lv_obj_get_coords(selection_overlay_, &overlay_area);
    spdlog::debug("[GridEditMode] Chrome verify: overlay_screen=({},{})→({},{}) "
                  "widget_screen=({},{})→({},{}) delta=({},{})",
                  overlay_area.x1, overlay_area.y1, overlay_area.x2, overlay_area.y2,
                  widget_area.x1, widget_area.y1, widget_area.x2, widget_area.y2,
                  overlay_area.x1 - widget_area.x1, overlay_area.y1 - widget_area.y1);
}

void GridEditMode::destroy_selection_chrome() {
    if (!configure_btn_ && !remove_btn_ && !selection_overlay_) {
        return;
    }
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();
    // Reparent to lv_layer_top() + async delete.  This is called during
    // input event processing where synchronous lv_obj_delete corrupts
    // LVGL's child iteration.  Reparenting ensures lv_obj_clean(container_)
    // in a subsequent rebuild won't double-free these objects.
    if (configure_btn_) {
        safe_deferred_delete(configure_btn_);
        configure_btn_ = nullptr;
    }
    if (remove_btn_) {
        safe_deferred_delete(remove_btn_);
        remove_btn_ = nullptr;
    }
    if (selection_overlay_) {
        safe_deferred_delete(selection_overlay_);
        selection_overlay_ = nullptr;
    }
}

int GridEditMode::find_config_index_for_widget(lv_obj_t* widget) const {
    if (!widget || !container_ || !config_) {
        return -1;
    }

    // Look up the widget's name (set by populate_widgets via lv_obj_set_name)
    const char* name = lv_obj_get_name(widget);
    if (!name || name[0] == '\0') {
        spdlog::debug("[GridEditMode] find_config: widget {} has no name",
                      static_cast<void*>(widget));
        return -1;
    }

    // Find the config entry with this widget ID
    const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == name) {
            return static_cast<int>(i);
        }
    }

    spdlog::debug("[GridEditMode] find_config: no config entry for name '{}'", name);
    return -1;
}

void GridEditMode::sync_config_from_screen() {
    if (!container_ || !config_) {
        return;
    }

    lv_obj_update_layout(container_);

    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    if (cw <= 0 || ch <= 0) {
        return;
    }

    auto& mut_entries = config_->page_entries_mut(static_cast<size_t>(page_index_));
    bool any_changed = false;

    // Walk container children and match by name (widget_id) set during populate_widgets.
    uint32_t total_children = lv_obj_get_child_count(container_);
    for (uint32_t i = 0; i < total_children; ++i) {
        lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(i));
        if (!child || child == dots_overlay_ || child == selection_overlay_) {
            continue;
        }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }

        // Look up config entry by widget name
        const char* name = lv_obj_get_name(child);
        if (!name || name[0] == '\0') {
            continue;
        }

        auto it = std::find_if(mut_entries.begin(), mut_entries.end(),
                               [name](const PanelWidgetEntry& e) { return e.id == name; });
        if (it == mut_entries.end()) {
            continue;
        }

        // Use the widget's top-left corner (center of the widget's first cell)
        lv_area_t widget_area;
        lv_obj_get_coords(child, &widget_area);

        // Compute which cell the top-left corner falls in
        int cell_w = cw / ncols;
        int cell_h = ch / nrows;
        int cx = widget_area.x1 + cell_w / 2; // center of first cell
        int cy = widget_area.y1 + cell_h / 2;

        auto [col, row] =
            screen_to_grid_cell(cx, cy, content_area.x1, content_area.y1, cw, ch, ncols, nrows);

        if (it->col != col || it->row != row) {
            spdlog::debug("[GridEditMode] sync_config: '{}' config ({},{}) -> screen ({},{})",
                          it->id, it->col, it->row, col, row);
            it->col = col;
            it->row = row;
            any_changed = true;
        }
    }

    if (any_changed) {
        config_->save();
        spdlog::info("[GridEditMode] Synced config positions from screen layout");
    }
}

void GridEditMode::schedule_deferred_rebuild(std::function<void()> post_rebuild) {
    if (!rebuild_cb_) {
        if (post_rebuild)
            post_rebuild();
        return;
    }
    // Heap-allocate the callbacks so lv_async_call can carry them as void*.
    // The deferred execution runs on the next lv_timer_handler tick, safely
    // outside any indev_proc_release call stack (#814).
    // The LifetimeToken ensures the callback is a no-op if GridEditMode is
    // destroyed before the async call fires (e.g., switch_printer teardown).
    struct Ctx {
        helix::LifetimeToken tok;
        GridEditMode* self;
        RebuildCallback rebuild;
        std::function<void()> post;
    };
    auto* ctx = new Ctx{lifetime_.token(), this, rebuild_cb_, std::move(post_rebuild)};
    lv_async_call(
        [](void* data) {
            auto* c = static_cast<Ctx*>(data);
            if (c->tok.expired()) {
                delete c;
                return;
            }
            c->rebuild();
            // rebuild_cb_ calls lv_obj_clean(container_), destroying all container
            // children. If a user tap between schedule and firing re-created chrome
            // via select_widget(), those chrome objects were just freed — null the
            // cached pointers so the post callback's select_widget() sees a clean
            // slate and doesn't run destroy_selection_chrome() on freed memory.
            c->self->selected_ = nullptr;
            c->self->selection_overlay_ = nullptr;
            c->self->remove_btn_ = nullptr;
            c->self->configure_btn_ = nullptr;
            c->self->delete_page_btn_ = nullptr;
            c->self->dots_overlay_ = nullptr;
            lv_indev_t* indev = lv_indev_active();
            if (indev) {
                lv_indev_reset(indev, nullptr);
            }
            if (c->post)
                c->post();
            delete c;
        },
        ctx);
}

void GridEditMode::remove_selected_widget() {
    if (!selected_ || !config_) {
        spdlog::warn("[GridEditMode] remove_selected_widget: no selection or config");
        return;
    }

    int config_index = find_config_index_for_widget(selected_);
    if (config_index < 0) {
        spdlog::warn("[GridEditMode] Selected widget not found in config");
        select_widget(nullptr);
        return;
    }

    const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));
    spdlog::info("[GridEditMode] Removing widget '{}' (config index {})",
                 entries[static_cast<size_t>(config_index)].id, config_index);

    // Copy by value — delete_entry() and page_entries_mut() modify the vector
    // that entries references, invalidating any references into it (#736)
    const auto widget_id = entries[static_cast<size_t>(config_index)].id;
    if (widget_id.find(':') != std::string::npos) {
        config_->delete_entry(widget_id);
    } else {
        // Use page-scoped entry access instead of set_enabled() which only operates on page 0
        config_
            ->page_entries_mut(static_cast<size_t>(page_index_))[static_cast<size_t>(config_index)]
            .enabled = false;
    }

    // Deselect before rebuild. Null out overlay pointers since
    // lv_obj_clean in the rebuild will delete them.
    select_widget(nullptr);
    delete_page_btn_ = nullptr;
    dots_overlay_ = nullptr;

    // Save config and defer rebuild to next timer tick (#814)
    config_->save();
    schedule_deferred_rebuild([this]() {
        if (active_) {
            create_dots_overlay();
        }
    });
}

void GridEditMode::configure_selected_widget() {
    if (!selected_) {
        return;
    }

    auto* raw = lv_obj_get_user_data(selected_);
    if (!raw) {
        return;
    }

    auto* widget = static_cast<PanelWidget*>(raw);
    if (!widget->on_edit_configure()) {
        return;
    }

    // Save widget ID so we can re-select it after rebuild
    const char* name = lv_obj_get_name(selected_);
    std::string widget_id = name ? name : "";

    spdlog::info("[GridEditMode] Widget '{}' configured, rebuilding", widget_id);

    // Deselect and rebuild
    select_widget(nullptr);
    delete_page_btn_ = nullptr;
    dots_overlay_ = nullptr;
    configure_btn_ = nullptr;
    remove_btn_ = nullptr;

    schedule_deferred_rebuild([this, widget_id]() {
        if (active_ && container_) {
            disable_widget_clicks_recursive(container_);
            create_dots_overlay();

            // Re-select the widget by finding it in the rebuilt container.
            // Force layout first so the widget has valid screen coordinates.
            if (!widget_id.empty()) {
                lv_obj_update_layout(container_);
                lv_obj_t* new_widget = lv_obj_find_by_name(container_, widget_id.c_str());
                if (new_widget) {
                    select_widget(new_widget);
                }
            }
        }
    });
}

// ---------------------------------------------------------------------------
// screen_to_grid_cell
// ---------------------------------------------------------------------------

std::pair<int, int> GridEditMode::screen_to_grid_cell(int screen_x, int screen_y, int container_x,
                                                      int container_y, int container_w,
                                                      int container_h, int ncols, int nrows) {
    // Convert screen coordinates to container-relative
    int rel_x = screen_x - container_x;
    int rel_y = screen_y - container_y;

    // Compute cell indices
    int col = (rel_x * ncols) / container_w;
    int row = (rel_y * nrows) / container_h;

    // Clamp to valid range
    col = std::clamp(col, 0, ncols - 1);
    row = std::clamp(row, 0, nrows - 1);

    return {col, row};
}

// ---------------------------------------------------------------------------
// clamp_span
// ---------------------------------------------------------------------------

std::pair<int, int> GridEditMode::clamp_span(const std::string& widget_id, int desired_colspan,
                                             int desired_rowspan) {
    const auto* def = find_widget_def(widget_id);
    if (!def) {
        // Unknown widget — default to 1x1
        return {std::max(desired_colspan, 1), std::max(desired_rowspan, 1)};
    }

    int min_c = def->effective_min_colspan();
    int max_c = def->effective_max_colspan();
    int min_r = def->effective_min_rowspan();
    int max_r = def->effective_max_rowspan();

    int clamped_c = std::clamp(desired_colspan, min_c, max_c);
    int clamped_r = std::clamp(desired_rowspan, min_r, max_r);

    return {clamped_c, clamped_r};
}

// ---------------------------------------------------------------------------
// Resize helpers
// ---------------------------------------------------------------------------

GridEditMode::ResizeEdge GridEditMode::detect_resize_edge(int px, int py,
                                                          const lv_area_t& widget_area) const {
    // Check proximity to each edge (INWARD inside, OUTWARD outside)
    bool near_right =
        (px >= widget_area.x2 - EDGE_HIT_INWARD && px <= widget_area.x2 + EDGE_HIT_MARGIN);
    bool near_left =
        (px >= widget_area.x1 - EDGE_HIT_MARGIN && px <= widget_area.x1 + EDGE_HIT_INWARD);
    bool near_bottom =
        (py >= widget_area.y2 - EDGE_HIT_INWARD && py <= widget_area.y2 + EDGE_HIT_MARGIN);
    bool near_top =
        (py >= widget_area.y1 - EDGE_HIT_MARGIN && py <= widget_area.y1 + EDGE_HIT_INWARD);

    // Must be within widget bounds on the perpendicular axis (with outward tolerance)
    bool within_x =
        (px >= widget_area.x1 - EDGE_HIT_MARGIN && px <= widget_area.x2 + EDGE_HIT_MARGIN);
    bool within_y =
        (py >= widget_area.y1 - EDGE_HIT_MARGIN && py <= widget_area.y2 + EDGE_HIT_MARGIN);

    // Collect candidate edges with their perpendicular distance
    struct Candidate {
        ResizeEdge edge;
        int dist; // Perpendicular distance to the edge (lower = closer)
    };
    Candidate candidates[4];
    int n = 0;

    if (near_right && within_y) {
        candidates[n++] = {ResizeEdge::Right, std::abs(px - widget_area.x2)};
    }
    if (near_left && within_y) {
        candidates[n++] = {ResizeEdge::Left, std::abs(px - widget_area.x1)};
    }
    if (near_bottom && within_x) {
        candidates[n++] = {ResizeEdge::Bottom, std::abs(py - widget_area.y2)};
    }
    if (near_top && within_x) {
        candidates[n++] = {ResizeEdge::Top, std::abs(py - widget_area.y1)};
    }

    if (n == 0) {
        return ResizeEdge::None;
    }

    // Pick the closest edge (corner disambiguation)
    int best = 0;
    for (int i = 1; i < n; ++i) {
        if (candidates[i].dist < candidates[best].dist) {
            best = i;
        }
    }
    return candidates[best].edge;
}

int GridEditMode::round_to_grid_cell(int px, int content_origin, int content_size, int ncells) {
    float cell_size = static_cast<float>(content_size) / ncells;
    float fractional = (px - content_origin) / cell_size;
    return std::clamp(static_cast<int>(std::round(fractional)), 0, ncells);
}

GridEditMode::ResizeResult GridEditMode::compute_resize_result(ResizeEdge edge, int orig_col,
                                                               int orig_row, int orig_colspan,
                                                               int orig_rowspan, int new_edge_cell,
                                                               int ncells) {
    ResizeResult r;
    r.col = orig_col;
    r.row = orig_row;
    r.colspan = orig_colspan;
    r.rowspan = orig_rowspan;

    // Clamp new_edge_cell to valid range
    new_edge_cell = std::clamp(new_edge_cell, 0, ncells);

    switch (edge) {
    case ResizeEdge::Right: {
        int new_colspan = new_edge_cell - orig_col;
        r.colspan = std::max(new_colspan, 1);
        r.rowspan = orig_rowspan;
        break;
    }
    case ResizeEdge::Left: {
        int right_edge = orig_col + orig_colspan;
        int new_col = std::min(new_edge_cell, right_edge - 1);
        new_col = std::max(new_col, 0);
        r.col = new_col;
        r.colspan = right_edge - new_col;
        r.rowspan = orig_rowspan;
        break;
    }
    case ResizeEdge::Bottom: {
        int new_rowspan = new_edge_cell - orig_row;
        r.rowspan = std::max(new_rowspan, 1);
        r.colspan = orig_colspan;
        break;
    }
    case ResizeEdge::Top: {
        int bottom_edge = orig_row + orig_rowspan;
        int new_row = std::min(new_edge_cell, bottom_edge - 1);
        new_row = std::max(new_row, 0);
        r.row = new_row;
        r.rowspan = bottom_edge - new_row;
        r.colspan = orig_colspan;
        break;
    }
    case ResizeEdge::None:
        break;
    }

    return r;
}

bool GridEditMode::is_selected_widget_resizable() const {
    if (!selected_ || !config_) {
        return false;
    }
    int cfg_idx = find_config_index_for_widget(selected_);
    if (cfg_idx < 0) {
        return false;
    }
    const auto& entry =
        config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(cfg_idx)];
    const auto* def = find_widget_def(entry.id);
    return def && def->is_scalable();
}

// ---------------------------------------------------------------------------
// Drag lifecycle — public entry points
// ---------------------------------------------------------------------------

void GridEditMode::handle_long_press(lv_event_t* e) {
    if (!active_ || !container_) {
        return;
    }

    spdlog::debug("[GridEditMode] handle_long_press: selected_={}", (void*)selected_);

    // If no widget is selected, try to select the one under the finger
    if (!selected_) {
        handle_click(e); // Select widget under finger (if any)
    }

    // If still no widget selected, check if we're on empty area for catalog
    if (!selected_) {
        lv_indev_t* indev = lv_indev_active();
        if (!indev) {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        // Long-press on empty area — open the widget catalog
        lv_area_t content_area;
        lv_obj_get_content_coords(container_, &content_area);
        int cw = content_area.x2 - content_area.x1;
        int ch = content_area.y2 - content_area.y1;

        lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
        UiBreakpoint breakpoint =
            bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
        int ncols = GridLayout::get_cols(breakpoint);
        int nrows = GridLayout::get_rows(breakpoint);

        auto [col, row] = screen_to_grid_cell(point.x, point.y, content_area.x1, content_area.y1,
                                              cw, ch, ncols, nrows);
        catalog_origin_col_ = col;
        catalog_origin_row_ = row;

        lv_obj_t* screen = lv_obj_get_screen(container_);
        if (screen) {
            open_widget_catalog(screen);
        }
        return;
    }

    drag_pending_ = false; // Long-press bypasses threshold
    handle_drag_start(e);
}

void GridEditMode::handle_pressing(lv_event_t* e) {
    (void)e;
    if (!active_) {
        return;
    }
    if (resizing_) {
        handle_resize_move(e);
        return;
    }
    if (dragging_) {
        handle_drag_move(e);
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    // First press frame: select widget and start watching for drag threshold
    if (!drag_pending_ && !selected_) {
        handle_click(e);
        if (selected_) {
            drag_pending_ = true;
            press_origin_ = pt;
        }
        return;
    }

    // If already selected but pressing on a different widget, re-select.
    // Keep selection if the press is within the edge resize zone (18px margin).
    if (!drag_pending_ && selected_) {
        lv_area_t sel_area;
        lv_obj_get_coords(selected_, &sel_area);
        bool outside_edge_zone =
            (pt.x < sel_area.x1 - EDGE_HIT_MARGIN || pt.x > sel_area.x2 + EDGE_HIT_MARGIN ||
             pt.y < sel_area.y1 - EDGE_HIT_MARGIN || pt.y > sel_area.y2 + EDGE_HIT_MARGIN);
        if (outside_edge_zone) {
            handle_click(e);
        }
        if (selected_) {
            drag_pending_ = true;
            press_origin_ = pt;
        }
        return;
    }

    // Drag threshold check: start real drag when finger moves enough
    if (drag_pending_ && selected_) {
        int dx = pt.x - press_origin_.x;
        int dy = pt.y - press_origin_.y;
        if (dx * dx + dy * dy > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX) {
            drag_pending_ = false;
            handle_drag_start(e);
        }
    }
}

void GridEditMode::handle_released(lv_event_t* e) {
    if (!active_) {
        return;
    }
    // Clear drag pending (finger released before threshold = just a tap/select)
    drag_pending_ = false;

    if (resizing_) {
        handle_resize_end(e);
        return;
    }
    if (dragging_) {
        handle_drag_end(e);
    }
}

// ---------------------------------------------------------------------------
// Drag start
// ---------------------------------------------------------------------------

void GridEditMode::handle_drag_start(lv_event_t* /*e*/) {
    spdlog::debug("[GridEditMode] handle_drag_start: dragging_={} resizing_={} selected_={}",
                  dragging_, resizing_, (void*)selected_);
    if (dragging_ || resizing_ || !selected_) {
        return;
    }

    // Verify pointer is on the selected widget
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        spdlog::debug("[GridEditMode] handle_drag_start: no active indev");
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t sel_area;
    lv_obj_get_coords(selected_, &sel_area);
    spdlog::debug("[GridEditMode] handle_drag_start: point=({},{}) sel=({},{})→({},{}) margin={}",
                  point.x, point.y, sel_area.x1, sel_area.y1, sel_area.x2, sel_area.y2,
                  EDGE_HIT_MARGIN);
    if (point.x < sel_area.x1 - EDGE_HIT_MARGIN || point.x > sel_area.x2 + EDGE_HIT_MARGIN ||
        point.y < sel_area.y1 - EDGE_HIT_MARGIN || point.y > sel_area.y2 + EDGE_HIT_MARGIN) {
        spdlog::debug("[GridEditMode] handle_drag_start: point not on selected widget");
        return; // Long-press not on selected widget
    }

    // Look up config entry for the selected widget
    int cfg_idx = find_config_index_for_widget(selected_);
    if (cfg_idx < 0) {
        spdlog::warn("[GridEditMode] Drag start: widget not in config");
        return;
    }

    const auto& entry =
        config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(cfg_idx)];
    spdlog::debug("[GridEditMode] handle_drag_start: cfg_idx={} id='{}' col={} row={} "
                  "colspan={} rowspan={} has_grid={}",
                  cfg_idx, entry.id, entry.col, entry.row, entry.colspan, entry.rowspan,
                  entry.has_grid_position());
    drag_cfg_idx_ = cfg_idx;
    drag_orig_col_ = entry.col;
    drag_orig_row_ = entry.row;
    drag_orig_colspan_ = entry.colspan;
    drag_orig_rowspan_ = entry.rowspan;

    // Check if the INITIAL press point (not current pointer) was near an edge.
    // By the time drag threshold is met, the pointer may have moved beyond the
    // edge tolerance — use press_origin_ which is where the finger first touched.
    ResizeEdge edge = detect_resize_edge(press_origin_.x, press_origin_.y, sel_area);
    if (edge != ResizeEdge::None && is_selected_widget_resizable()) {
        // Start resize mode instead of drag
        resizing_ = true;
        resize_edge_ = edge;

        // Hide selection chrome during resize (will rebuild after)
        destroy_selection_chrome();

        // Show initial resize preview at current widget size
        {
            lv_area_t content_area;
            lv_obj_get_content_coords(container_, &content_area);
            int cw = content_area.x2 - content_area.x1;
            int ch = content_area.y2 - content_area.y1;
            lv_subject_t* bp = theme_manager_get_breakpoint_subject();
            UiBreakpoint bp_val =
                bp ? as_breakpoint(lv_subject_get_int(bp)) : UiBreakpoint::Medium; /* MEDIUM */
            float cell_w = static_cast<float>(cw) / GridLayout::get_cols(bp_val);
            float cell_h = static_cast<float>(ch) / GridLayout::get_rows(bp_val);
            update_resize_preview_px(static_cast<int>(drag_orig_col_ * cell_w),
                                     static_cast<int>(drag_orig_row_ * cell_h),
                                     static_cast<int>(drag_orig_colspan_ * cell_w),
                                     static_cast<int>(drag_orig_rowspan_ * cell_h), true);
        }

        spdlog::info("[GridEditMode] Resize started: widget '{}' at ({},{}) span {}x{} edge={}",
                     entry.id, drag_orig_col_, drag_orig_row_, drag_orig_colspan_,
                     drag_orig_rowspan_, static_cast<int>(edge));
        return;
    }

    // Record drag offset: distance from pointer to widget top-left
    drag_offset_.x = point.x - sel_area.x1;
    drag_offset_.y = point.y - sel_area.y1;

    // Keep selection chrome visible during drag — it moves with the widget
    // in handle_drag_move(). No ghost outline at the origin needed.

    // Make widget float above the grid so it can be freely positioned.
    // Compensate position: FLOATING changes coordinate reference frame
    // from content area to parent outer coords + padding. Without compensation,
    // the widget visually shifts by the container's padding amount.
    lv_area_t cont_area;
    lv_obj_get_coords(container_, &cont_area);
    int pad_left = lv_obj_get_style_space_left(container_, LV_PART_MAIN);
    int pad_top = lv_obj_get_style_space_top(container_, LV_PART_MAIN);

    lv_obj_add_flag(selected_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(selected_, sel_area.x1 - cont_area.x1 - pad_left,
                   sel_area.y1 - cont_area.y1 - pad_top);

    dragging_ = true;
    snap_preview_col_ = -1;
    snap_preview_row_ = -1;

    spdlog::info("[GridEditMode] Drag started: widget '{}' from ({},{}) span {}x{}", entry.id,
                 drag_orig_col_, drag_orig_row_, drag_orig_colspan_, drag_orig_rowspan_);
}

// ---------------------------------------------------------------------------
// Drag move
// ---------------------------------------------------------------------------

void GridEditMode::handle_drag_move(lv_event_t* /*e*/) {
    if (!selected_ || !container_) {
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Move the widget to follow the pointer (adjusted by drag offset)
    lv_area_t container_area;
    lv_obj_get_coords(container_, &container_area);
    int new_x = point.x - drag_offset_.x - container_area.x1;
    int new_y = point.y - drag_offset_.y - container_area.y1;
    lv_obj_set_pos(selected_, new_x, new_y);

    // Move selection chrome overlay to track the widget.
    // Recompute from the widget's actual screen coords (same math as create_selection_chrome).
    if (selection_overlay_) {
        lv_obj_update_layout(selected_);
        lv_area_t widget_area;
        lv_obj_get_coords(selected_, &widget_area);
        int pad_left = lv_obj_get_style_space_left(container_, LV_PART_MAIN);
        int pad_top = lv_obj_get_style_space_top(container_, LV_PART_MAIN);
        int chrome_x = widget_area.x1 - container_area.x1 - pad_left;
        int chrome_y = widget_area.y1 - container_area.y1 - pad_top;
        lv_obj_set_pos(selection_overlay_, chrome_x, chrome_y);
    }

    // Compute target grid cell from the widget's top-left position.
    // The widget follows the pointer with drag_offset_ preserving the grab point,
    // so using top-left gives natural anchored behavior — the widget stays under
    // the finger exactly where grabbed, and the snap target reflects that.
    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    // Widget top-left in screen coords (pointer minus grab offset)
    int widget_left = point.x - drag_offset_.x;
    int widget_top = point.y - drag_offset_.y;

    // Round the top-left to the nearest grid cell for snap target
    int target_col = round_to_grid_cell(widget_left, content_area.x1, cw, ncols);
    int target_row = round_to_grid_cell(widget_top, content_area.y1, ch, nrows);

    // Clamp so the widget span doesn't extend past the grid
    target_col = std::min(target_col, ncols - drag_orig_colspan_);
    target_row = std::min(target_row, nrows - drag_orig_rowspan_);

    // Only update preview if target cell changed
    if (target_col == snap_preview_col_ && target_row == snap_preview_row_) {
        return;
    }

    // Skip if hovering over the original position
    if (target_col == drag_orig_col_ && target_row == drag_orig_row_) {
        destroy_snap_preview();
        snap_preview_col_ = target_col;
        snap_preview_row_ = target_row;
        return;
    }

    // Check placement validity: build a temporary grid with only VISIBLE widgets
    // (hardware-gated widgets may be enabled in config but not placed on screen)
    GridLayout temp_grid(breakpoint);
    const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));
    std::string dragged_id;
    if (drag_cfg_idx_ >= 0 && static_cast<size_t>(drag_cfg_idx_) < entries.size()) {
        dragged_id = entries[static_cast<size_t>(drag_cfg_idx_)].id;
    }

    // Collect IDs of widgets actually visible on screen
    std::unordered_set<std::string> visible_ids;
    uint32_t nchildren = lv_obj_get_child_count(container_);
    for (uint32_t ci = 0; ci < nchildren; ++ci) {
        lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(ci));
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }
        const char* cname = lv_obj_get_name(child);
        if (cname && cname[0] != '\0') {
            visible_ids.insert(cname);
        }
    }

    // Occupant at target position (reject drop on occupied cells)
    std::string occupant_id;

    for (const auto& entry : entries) {
        if (!entry.enabled || !entry.has_grid_position()) {
            continue;
        }
        if (entry.id == dragged_id) {
            continue; // Skip the widget being dragged
        }
        if (visible_ids.find(entry.id) == visible_ids.end()) {
            continue; // Skip hardware-gated widgets not on screen
        }
        temp_grid.place({entry.id, entry.col, entry.row, entry.colspan, entry.rowspan});

        // Check if this entry occupies the target cell
        if (target_col >= entry.col && target_col < entry.col + entry.colspan &&
            target_row >= entry.row && target_row < entry.row + entry.rowspan) {
            occupant_id = entry.id;
        }
    }

    bool valid = false;
    if (occupant_id.empty()) {
        // Target is empty — check if the dragged widget fits (bounds + collision)
        valid = (target_col + drag_orig_colspan_ <= ncols) &&
                (target_row + drag_orig_rowspan_ <= nrows) &&
                temp_grid.can_place(target_col, target_row, drag_orig_colspan_, drag_orig_rowspan_);
    } else {
        // Target occupied — reject drop (no swapping for now)
        valid = false;
    }

    update_snap_preview(target_col, target_row, drag_orig_colspan_, drag_orig_rowspan_, valid);
    snap_preview_col_ = target_col;
    snap_preview_row_ = target_row;
}

// ---------------------------------------------------------------------------
// Drag end
// ---------------------------------------------------------------------------

void GridEditMode::handle_drag_end(lv_event_t* /*e*/) {
    if (!selected_ || !container_ || !config_) {
        cleanup_drag_state();
        return;
    }

    // Use the last snap preview position — this is what the user saw highlighted.
    // Recomputing from the release point can differ (finger moves during release).
    int target_col = snap_preview_col_;
    int target_row = snap_preview_row_;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;

    bool did_move = false;

    spdlog::debug("[GridEditMode] Drag end: target=({},{}) orig=({},{})", target_col, target_row,
                  drag_orig_col_, drag_orig_row_);

    // Don't allow drop if no preview was shown or on the same position
    if (target_col >= 0 && target_row >= 0 &&
        (target_col != drag_orig_col_ || target_row != drag_orig_row_)) {
        int cfg_idx = drag_cfg_idx_;
        spdlog::debug("[GridEditMode] Drag end: cfg_idx={} selected_={}", cfg_idx,
                      (void*)selected_);
        if (cfg_idx >= 0) {
            auto& entries = config_->page_entries_mut(static_cast<size_t>(page_index_));
            auto& dragged_entry = entries[static_cast<size_t>(cfg_idx)];

            // Collect IDs of widgets actually visible on screen (not hardware-gated)
            std::unordered_set<std::string> visible_ids;
            uint32_t nchildren = lv_obj_get_child_count(container_);
            for (uint32_t ci = 0; ci < nchildren; ++ci) {
                lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(ci));
                if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
                    continue;
                }
                const char* cname = lv_obj_get_name(child);
                if (cname && cname[0] != '\0') {
                    visible_ids.insert(cname);
                }
            }

            // Check for occupant at target (only visible widgets)
            int occupant_cfg_idx = -1;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (!entries[i].enabled || !entries[i].has_grid_position()) {
                    continue;
                }
                if (static_cast<int>(i) == cfg_idx) {
                    continue;
                }
                if (visible_ids.find(entries[i].id) == visible_ids.end()) {
                    continue; // Skip hardware-gated widgets not on screen
                }
                if (target_col >= entries[i].col &&
                    target_col < entries[i].col + entries[i].colspan &&
                    target_row >= entries[i].row &&
                    target_row < entries[i].row + entries[i].rowspan) {
                    occupant_cfg_idx = static_cast<int>(i);
                    break;
                }
            }

            spdlog::debug("[GridEditMode] Drag end: occupant_cfg_idx={}", occupant_cfg_idx);

            if (occupant_cfg_idx >= 0) {
                // Target occupied — reject drop (swap logic disabled for now)
                // TODO: re-enable swap when UX is polished
                spdlog::debug("[GridEditMode] Drag end: target occupied by '{}', rejecting drop",
                              entries[static_cast<size_t>(occupant_cfg_idx)].id);
            } else {
                // Empty cell — check bounds and collision (only visible widgets)
                GridLayout temp_grid(breakpoint);

                for (const auto& entry : entries) {
                    if (!entry.enabled || !entry.has_grid_position()) {
                        continue;
                    }
                    if (entry.id == dragged_entry.id) {
                        continue;
                    }
                    if (visible_ids.find(entry.id) == visible_ids.end()) {
                        continue; // Skip hardware-gated widgets not on screen
                    }
                    temp_grid.place({entry.id, entry.col, entry.row, entry.colspan, entry.rowspan});
                }
                bool ok = temp_grid.can_place(target_col, target_row, drag_orig_colspan_,
                                              drag_orig_rowspan_);
                spdlog::debug("[GridEditMode] Drag end: can_place({},{} {}x{})={}", target_col,
                              target_row, drag_orig_colspan_, drag_orig_rowspan_, ok);
                if (ok) {
                    spdlog::info("[GridEditMode] Moving '{}' from ({},{}) to ({},{})",
                                 dragged_entry.id, drag_orig_col_, drag_orig_row_, target_col,
                                 target_row);
                    dragged_entry.col = target_col;
                    dragged_entry.row = target_row;
                    did_move = true;
                }
            }
        }
    }

    if (!did_move) {
        spdlog::debug("[GridEditMode] Drag cancelled, snapping back to ({},{})", drag_orig_col_,
                      drag_orig_row_);
    }

    // Clean up visual state before rebuild
    lv_obj_t* was_selected = selected_;
    std::string moved_id;
    if (drag_cfg_idx_ >= 0) {
        moved_id =
            config_
                ->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(drag_cfg_idx_)]
                .id;
    }
    cleanup_drag_state();

    if (did_move) {
        // Deselect before rebuild. The rebuild (lv_obj_clean) will destroy
        // selection_overlay_ and dots_overlay_ since they're container children.
        // Null them out first to prevent dangling pointer access.
        selected_ = nullptr;
        selection_overlay_ = nullptr;
        remove_btn_ = nullptr;
        configure_btn_ = nullptr;
        delete_page_btn_ = nullptr;
        dots_overlay_ = nullptr;
        config_->save();
        spdlog::debug("[GridEditMode] Config saved, scheduling deferred rebuild...");
        schedule_deferred_rebuild([this, moved_id]() {
            spdlog::debug("[GridEditMode] Rebuild complete, recreating dots overlay");
            if (active_) {
                create_dots_overlay();
            }
            // Re-select the moved widget after rebuild (layout must be
            // recalculated first so widget coords are valid for chrome placement)
            if (container_) {
                lv_obj_update_layout(container_);
                for (uint32_t i = 0; i < lv_obj_get_child_count(container_); ++i) {
                    lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(i));
                    if (!child) {
                        continue;
                    }
                    const char* cname = lv_obj_get_name(child);
                    if (cname && moved_id == cname) {
                        select_widget(child);
                        break;
                    }
                }
            }
        });
    } else {
        // Re-select to show chrome again (widget snaps back via grid layout)
        selected_ = nullptr;
        // Force LVGL to recalculate positions
        lv_obj_invalidate(container_);
        lv_obj_update_layout(container_);

        // Validate was_selected is still a live child of container_ before
        // re-selecting — a concurrent rebuild could have freed it.
        bool still_valid = false;
        if (was_selected && container_) {
            uint32_t n = lv_obj_get_child_count(container_);
            for (uint32_t i = 0; i < n; ++i) {
                if (lv_obj_get_child(container_, static_cast<int32_t>(i)) == was_selected) {
                    still_valid = true;
                    break;
                }
            }
        }
        if (still_valid) {
            select_widget(was_selected);
        }
    }
}

// ---------------------------------------------------------------------------
// Resize move
// ---------------------------------------------------------------------------

void GridEditMode::handle_resize_move(lv_event_t* /*e*/) {
    if (!selected_ || !container_ || !config_) {
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Get container content area for coordinate mapping
    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    float cell_w = static_cast<float>(cw) / ncols;
    float cell_h = static_cast<float>(ch) / nrows;

    // Original widget pixel bounds (from grid config)
    int orig_x1 = content_area.x1 + static_cast<int>(drag_orig_col_ * cell_w);
    int orig_y1 = content_area.y1 + static_cast<int>(drag_orig_row_ * cell_h);
    int orig_x2 =
        content_area.x1 + static_cast<int>((drag_orig_col_ + drag_orig_colspan_) * cell_w);
    int orig_y2 =
        content_area.y1 + static_cast<int>((drag_orig_row_ + drag_orig_rowspan_) * cell_h);

    // Compute pixel preview bounds based on which edge is being dragged.
    // The dragged edge follows the pointer; the opposite edge stays fixed.
    int preview_x1 = orig_x1;
    int preview_y1 = orig_y1;
    int preview_x2 = orig_x2;
    int preview_y2 = orig_y2;

    // Minimum size in pixels (1 cell)
    int min_w = static_cast<int>(cell_w);
    int min_h = static_cast<int>(cell_h);

    switch (resize_edge_) {
    case ResizeEdge::Right:
        preview_x2 = std::clamp<int32_t>(point.x, preview_x1 + min_w, content_area.x2);
        break;
    case ResizeEdge::Left:
        preview_x1 = std::clamp<int32_t>(point.x, content_area.x1, preview_x2 - min_w);
        break;
    case ResizeEdge::Bottom:
        preview_y2 = std::clamp<int32_t>(point.y, preview_y1 + min_h, content_area.y2);
        break;
    case ResizeEdge::Top:
        preview_y1 = std::clamp<int32_t>(point.y, content_area.y1, preview_y2 - min_h);
        break;
    case ResizeEdge::None:
        return;
    }

    // Round current pixel position to grid to check validity
    int ncells_axis =
        (resize_edge_ == ResizeEdge::Left || resize_edge_ == ResizeEdge::Right) ? ncols : nrows;
    int edge_cell;
    if (resize_edge_ == ResizeEdge::Right) {
        edge_cell = round_to_grid_cell(preview_x2, content_area.x1, cw, ncols);
    } else if (resize_edge_ == ResizeEdge::Left) {
        edge_cell = round_to_grid_cell(preview_x1, content_area.x1, cw, ncols);
    } else if (resize_edge_ == ResizeEdge::Bottom) {
        edge_cell = round_to_grid_cell(preview_y2, content_area.y1, ch, nrows);
    } else {
        edge_cell = round_to_grid_cell(preview_y1, content_area.y1, ch, nrows);
    }

    auto result =
        compute_resize_result(resize_edge_, drag_orig_col_, drag_orig_row_, drag_orig_colspan_,
                              drag_orig_rowspan_, edge_cell, ncells_axis);

    // Apply registry min/max clamping and detect if at limit
    int cfg_idx = find_config_index_for_widget(selected_);
    if (cfg_idx < 0) {
        return;
    }
    const auto& entry =
        config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(cfg_idx)];
    int pre_clamp_c = result.colspan;
    int pre_clamp_r = result.rowspan;
    auto [clamped_c, clamped_r] = clamp_span(entry.id, result.colspan, result.rowspan);
    result.colspan = clamped_c;
    result.rowspan = clamped_r;

    // Re-anchor origin for top/left edges so the opposite edge stays fixed
    if (resize_edge_ == ResizeEdge::Top) {
        result.row = drag_orig_row_ + drag_orig_rowspan_ - result.rowspan;
    } else if (resize_edge_ == ResizeEdge::Left) {
        result.col = drag_orig_col_ + drag_orig_colspan_ - result.colspan;
    }

    // Detect if the dragged axis hit its max constraint
    bool at_limit = false;
    if (resize_edge_ == ResizeEdge::Right || resize_edge_ == ResizeEdge::Left) {
        at_limit = (pre_clamp_c != clamped_c); // Colspan was clamped
    } else if (resize_edge_ == ResizeEdge::Bottom || resize_edge_ == ResizeEdge::Top) {
        at_limit = (pre_clamp_r != clamped_r); // Rowspan was clamped
    }

    // Check collision with other widgets
    GridLayout temp_grid(breakpoint);
    const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));
    for (const auto& e : entries) {
        if (!e.enabled || !e.has_grid_position()) {
            continue;
        }
        if (e.id == entry.id) {
            continue;
        }
        temp_grid.place({e.id, e.col, e.row, e.colspan, e.rowspan});
    }
    bool valid = temp_grid.can_place(result.col, result.row, result.colspan, result.rowspan);

    // Convert preview coords to container-relative (content area relative)
    int px = preview_x1 - content_area.x1;
    int py = preview_y1 - content_area.y1;
    int pw = preview_x2 - preview_x1;
    int ph = preview_y2 - preview_y1;

    // Pixel-following preview: danger color when at max size or invalid placement
    update_resize_preview_px(px, py, pw, ph, valid && !at_limit);

    // Grid-snapped preview (shows where widget will land on release)
    update_snap_preview(result.col, result.row, result.colspan, result.rowspan, valid);

    spdlog::debug("[GridEditMode] Resize preview: px=({},{} {}x{}) → grid ({},{} {}x{}) valid={}",
                  px, py, pw, ph, result.col, result.row, result.colspan, result.rowspan, valid);
}

// ---------------------------------------------------------------------------
// Resize end
// ---------------------------------------------------------------------------

void GridEditMode::handle_resize_end(lv_event_t* /*e*/) {
    if (!selected_ || !container_ || !config_) {
        resizing_ = false;
        helix::ui::safe_delete_deferred(resize_preview_);
        return;
    }

    // Get the pointer's final position and compute the rounded grid result
    lv_indev_t* indev = lv_indev_active();
    lv_point_t point = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &point);
    }

    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    // Round the dragged edge to the nearest grid cell boundary
    int edge_cell;
    int ncells_axis;
    if (resize_edge_ == ResizeEdge::Right) {
        edge_cell = round_to_grid_cell(point.x, content_area.x1, cw, ncols);
        ncells_axis = ncols;
    } else if (resize_edge_ == ResizeEdge::Left) {
        edge_cell = round_to_grid_cell(point.x, content_area.x1, cw, ncols);
        ncells_axis = ncols;
    } else if (resize_edge_ == ResizeEdge::Bottom) {
        edge_cell = round_to_grid_cell(point.y, content_area.y1, ch, nrows);
        ncells_axis = nrows;
    } else {
        edge_cell = round_to_grid_cell(point.y, content_area.y1, ch, nrows);
        ncells_axis = nrows;
    }

    auto result =
        compute_resize_result(resize_edge_, drag_orig_col_, drag_orig_row_, drag_orig_colspan_,
                              drag_orig_rowspan_, edge_cell, ncells_axis);

    // Apply registry clamping
    int cfg_idx = find_config_index_for_widget(selected_);
    bool did_resize = false;

    if (cfg_idx >= 0) {
        const auto& entry =
            config_->page_entries(static_cast<size_t>(page_index_))[static_cast<size_t>(cfg_idx)];
        auto [clamped_c, clamped_r] = clamp_span(entry.id, result.colspan, result.rowspan);
        result.colspan = clamped_c;
        result.rowspan = clamped_r;

        // Re-anchor origin for top/left edges so the opposite edge stays fixed
        if (resize_edge_ == ResizeEdge::Top) {
            result.row = drag_orig_row_ + drag_orig_rowspan_ - result.rowspan;
        } else if (resize_edge_ == ResizeEdge::Left) {
            result.col = drag_orig_col_ + drag_orig_colspan_ - result.colspan;
        }

        // Reject if the span didn't actually change — this prevents a top/left
        // edge drag at max size from silently moving the widget origin instead
        // of snapping back (the user saw a red/at-limit preview and expects no change).
        bool span_changed =
            (result.colspan != drag_orig_colspan_ || result.rowspan != drag_orig_rowspan_);
        bool changed =
            (result.col != drag_orig_col_ || result.row != drag_orig_row_ || span_changed);

        if (changed && span_changed) {
            // Validate against other widgets
            GridLayout temp_grid(breakpoint);
            const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));
            for (const auto& e : entries) {
                if (!e.enabled || !e.has_grid_position()) {
                    continue;
                }
                if (e.id == entry.id) {
                    continue;
                }
                temp_grid.place({e.id, e.col, e.row, e.colspan, e.rowspan});
            }

            if (temp_grid.can_place(result.col, result.row, result.colspan, result.rowspan)) {
                did_resize = true;
            }
        }
    }

    if (did_resize) {
        commit_resize_with_snap(result);
    } else {
        // Snap back: clean up previews and restore selection
        lv_obj_t* was_selected = selected_;
        helix::ui::safe_delete_deferred(resize_preview_);
        destroy_snap_preview();
        resizing_ = false;
        resize_edge_ = ResizeEdge::None;
        drag_orig_col_ = -1;
        drag_orig_row_ = -1;
        drag_orig_colspan_ = 1;
        drag_orig_rowspan_ = 1;

        selected_ = nullptr;
        if (container_) {
            lv_obj_update_layout(container_);

            // Validate was_selected is still a live child of container_
            bool still_valid = false;
            if (was_selected) {
                uint32_t n = lv_obj_get_child_count(container_);
                for (uint32_t i = 0; i < n; ++i) {
                    if (lv_obj_get_child(container_, static_cast<int32_t>(i)) == was_selected) {
                        still_valid = true;
                        break;
                    }
                }
            }
            if (still_valid) {
                select_widget(was_selected);
            }
        }
    }
}

void GridEditMode::update_resize_preview_px(int x, int y, int w, int h, bool valid) {
    if (!container_) {
        return;
    }

    if (!resize_preview_) {
        // Pixel-following preview: thin border, no fill. The grid-snapped
        // snap_preview_ provides the primary visual indicator of landing position.
        resize_preview_ = lv_obj_create(container_);
        lv_obj_add_flag(resize_preview_, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(resize_preview_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(resize_preview_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(resize_preview_, 2, 0);
        lv_obj_set_style_radius(resize_preview_, 8, 0);
        lv_obj_set_style_pad_all(resize_preview_, 0, 0);
        lv_obj_set_style_bg_opa(resize_preview_, LV_OPA_TRANSP, 0);
    }

    lv_obj_set_pos(resize_preview_, x, y);
    lv_obj_set_size(resize_preview_, w, h);

    lv_color_t color =
        valid ? theme_get_accent_color() : ThemeManager::instance().get_color("danger");
    lv_obj_set_style_border_color(resize_preview_, color, 0);
    lv_obj_set_style_border_opa(resize_preview_, LV_OPA_40, 0);
}

void GridEditMode::commit_resize_with_snap(const ResizeResult& result) {
    if (!container_ || !config_) {
        return;
    }

    int cfg_idx = find_config_index_for_widget(selected_);
    if (cfg_idx < 0) {
        return;
    }

    // Compute the final pixel position for the snap animation target
    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);
    float cell_w = static_cast<float>(cw) / ncols;
    float cell_h = static_cast<float>(ch) / nrows;

    int target_x = static_cast<int>(result.col * cell_w);
    int target_y = static_cast<int>(result.row * cell_h);
    int target_w = static_cast<int>(result.colspan * cell_w);
    int target_h = static_cast<int>(result.rowspan * cell_h);

    // Update config
    auto& entries = config_->page_entries_mut(static_cast<size_t>(page_index_));
    auto& entry = entries[static_cast<size_t>(cfg_idx)];
    std::string resized_id = entry.id;

    spdlog::info("[GridEditMode] Resized '{}' from ({},{} {}x{}) to ({},{} {}x{})", entry.id,
                 drag_orig_col_, drag_orig_row_, drag_orig_colspan_, drag_orig_rowspan_, result.col,
                 result.row, result.colspan, result.rowspan);

    entry.col = result.col;
    entry.row = result.row;
    entry.colspan = result.colspan;
    entry.rowspan = result.rowspan;

    // Clean up resize state (before animation or rebuild)
    resizing_ = false;
    resize_edge_ = ResizeEdge::None;
    drag_orig_col_ = -1;
    drag_orig_row_ = -1;
    drag_orig_colspan_ = 1;
    drag_orig_rowspan_ = 1;

    // Prepare rebuild context for deferred execution
    auto do_rebuild = [this, resized_id]() {
        selected_ = nullptr;
        selection_overlay_ = nullptr;
        remove_btn_ = nullptr;
        configure_btn_ = nullptr;
        delete_page_btn_ = nullptr;
        dots_overlay_ = nullptr;
        snap_preview_ = nullptr;
        config_->save();
        schedule_deferred_rebuild([this, resized_id]() {
            if (active_ && container_) {
                create_dots_overlay();
            }
            // Re-select the resized widget after rebuild (layout must be
            // recalculated first so widget coords are valid for chrome placement)
            if (!container_) {
                return;
            }
            lv_obj_update_layout(container_);
            for (uint32_t i = 0; i < lv_obj_get_child_count(container_); ++i) {
                lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(i));
                if (!child) {
                    continue;
                }
                const char* cname = lv_obj_get_name(child);
                if (cname && resized_id == cname) {
                    select_widget(child);
                    break;
                }
            }
        });
    };

    // Animate preview to final grid position, then rebuild on completion.
    // The rebuild destroys all container children, so it MUST NOT run while
    // the animation is still in flight (the preview is a container child).
    if (resize_preview_ && DisplaySettingsManager::instance().get_animations_enabled()) {
        struct SnapData {
            lv_obj_t* preview;
            int target_x, target_y, target_w, target_h;
            int start_x, start_y, start_w, start_h;
            GridEditMode* self;
            std::string resized_id;
        };

        auto* data = new SnapData();
        data->preview = resize_preview_;
        data->target_x = target_x;
        data->target_y = target_y;
        data->target_w = target_w;
        data->target_h = target_h;
        data->start_x = lv_obj_get_x(resize_preview_);
        data->start_y = lv_obj_get_y(resize_preview_);
        data->start_w = lv_obj_get_width(resize_preview_);
        data->start_h = lv_obj_get_height(resize_preview_);
        data->self = this;
        data->resized_id = resized_id;

        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, data);
        lv_anim_set_values(&anim, 0, 255);
        lv_anim_set_duration(&anim, 150);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, [](void* var, int32_t val) {
            auto* d = static_cast<SnapData*>(var);
            int t = val; // 0..255
            int x = d->start_x + (d->target_x - d->start_x) * t / 255;
            int y = d->start_y + (d->target_y - d->start_y) * t / 255;
            int w = d->start_w + (d->target_w - d->start_w) * t / 255;
            int h = d->start_h + (d->target_h - d->start_h) * t / 255;
            lv_obj_set_pos(d->preview, x, y);
            lv_obj_set_size(d->preview, w, h);
        });
        lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
            auto* d = static_cast<SnapData*>(a->var);
            auto* self = d->self;
            std::string rid = std::move(d->resized_id);
            // Preview will be destroyed by the rebuild (it's a container child).
            // No need to manually delete it.
            delete d;
            // Now safe to rebuild — animation is complete
            self->selected_ = nullptr;
            self->selection_overlay_ = nullptr;
            self->remove_btn_ = nullptr;
            self->configure_btn_ = nullptr;
            self->delete_page_btn_ = nullptr;
            self->dots_overlay_ = nullptr;
            self->snap_preview_ = nullptr;
            self->config_->save();
            self->schedule_deferred_rebuild([self, rid]() {
                if (self->active_ && self->container_) {
                    self->create_dots_overlay();
                }
                // Re-select the resized widget after rebuild
                if (!self->container_) {
                    return;
                }
                lv_obj_update_layout(self->container_);
                for (uint32_t i = 0; i < lv_obj_get_child_count(self->container_); ++i) {
                    lv_obj_t* child = lv_obj_get_child(self->container_, static_cast<int32_t>(i));
                    if (!child) {
                        continue;
                    }
                    const char* cname = lv_obj_get_name(child);
                    if (cname && rid == cname) {
                        self->select_widget(child);
                        break;
                    }
                }
            });
        });
        lv_anim_start(&anim);

        resize_preview_ = nullptr; // Ownership transferred to animation
    } else {
        // No animation: clean up and rebuild immediately
        helix::ui::safe_delete_deferred(resize_preview_);
        do_rebuild();
    }
}

// ---------------------------------------------------------------------------
// Drag visual helpers
// ---------------------------------------------------------------------------

void GridEditMode::create_drag_ghost(int col, int row, int colspan, int rowspan) {
    if (!container_) {
        return;
    }

    // Compute cell pixel positions from container content area
    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    int cell_w = cw / ncols;
    int cell_h = ch / nrows;

    int ghost_x = col * cell_w;
    int ghost_y = row * cell_h;
    int ghost_w = colspan * cell_w;
    int ghost_h = rowspan * cell_h;

    drag_ghost_ = lv_obj_create(container_);
    lv_obj_set_pos(drag_ghost_, ghost_x, ghost_y);
    lv_obj_set_size(drag_ghost_, ghost_w, ghost_h);
    lv_obj_add_flag(drag_ghost_, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(drag_ghost_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(drag_ghost_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(drag_ghost_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drag_ghost_, GHOST_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(drag_ghost_, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(drag_ghost_, GHOST_BORDER_OPA, 0);
    lv_obj_set_style_radius(drag_ghost_, 8, 0);
    lv_obj_set_style_pad_all(drag_ghost_, 0, 0);

    spdlog::debug("[GridEditMode] Created drag ghost at ({},{}) {}x{}", col, row, colspan, rowspan);
}

void GridEditMode::destroy_drag_ghost() {
    if (drag_ghost_) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        safe_deferred_delete(drag_ghost_);
        drag_ghost_ = nullptr;
    }
}

void GridEditMode::update_snap_preview(int col, int row, int colspan, int rowspan, bool valid) {
    destroy_snap_preview();
    if (!container_) {
        return;
    }

    lv_area_t content_area;
    lv_obj_get_content_coords(container_, &content_area);
    int cw = content_area.x2 - content_area.x1;
    int ch = content_area.y2 - content_area.y1;

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    int cell_w = cw / ncols;
    int cell_h = ch / nrows;

    int px = col * cell_w;
    int py = row * cell_h;
    int pw = colspan * cell_w;
    int ph = rowspan * cell_h;

    snap_preview_ = lv_obj_create(container_);
    lv_obj_set_pos(snap_preview_, px, py);
    lv_obj_set_size(snap_preview_, pw, ph);
    lv_obj_add_flag(snap_preview_, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(snap_preview_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(snap_preview_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(snap_preview_, PREVIEW_BORDER_WIDTH, 0);
    lv_obj_set_style_radius(snap_preview_, 8, 0);
    lv_obj_set_style_pad_all(snap_preview_, 0, 0);

    if (valid) {
        lv_obj_set_style_bg_color(snap_preview_, theme_get_accent_color(), 0);
        lv_obj_set_style_bg_opa(snap_preview_, LV_OPA_10, 0);
        lv_obj_set_style_border_color(snap_preview_, theme_get_accent_color(), 0);
        lv_obj_set_style_border_opa(snap_preview_, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_color(snap_preview_, ThemeManager::instance().get_color("danger"), 0);
        lv_obj_set_style_bg_opa(snap_preview_, LV_OPA_10, 0);
        lv_obj_set_style_border_color(snap_preview_, ThemeManager::instance().get_color("danger"),
                                      0);
        lv_obj_set_style_border_opa(snap_preview_, LV_OPA_50, 0);
    }
}

void GridEditMode::destroy_snap_preview() {
    if (snap_preview_) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        safe_deferred_delete(snap_preview_);
        snap_preview_ = nullptr;
    }
    snap_preview_col_ = -1;
    snap_preview_row_ = -1;
}

void GridEditMode::cleanup_drag_state() {
    drag_pending_ = false;
    if (!dragging_ && !resizing_) {
        return;
    }

    // Remove floating flag from the widget (only for drag, not resize)
    if (dragging_ && selected_) {
        lv_obj_remove_flag(selected_, LV_OBJ_FLAG_FLOATING);
    }

    destroy_drag_ghost();
    destroy_snap_preview();

    dragging_ = false;
    resizing_ = false;
    drag_cfg_idx_ = -1;
    drag_orig_col_ = -1;
    drag_orig_row_ = -1;
    drag_orig_colspan_ = 1;
    drag_orig_rowspan_ = 1;
    drag_offset_ = {0, 0};
    if (resize_preview_) {
        safe_deferred_delete(resize_preview_);
        resize_preview_ = nullptr;
    }
}

void GridEditMode::create_dots_overlay() {
    if (!container_)
        return;

    // Get current breakpoint for grid dimensions
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj))
                                      : UiBreakpoint::Medium; // Default MEDIUM
    int ncols = GridLayout::get_cols(breakpoint);
    int nrows = GridLayout::get_rows(breakpoint);

    // Create transparent overlay that floats above grid children.
    // This overlay serves two purposes:
    // 1. Draws grid intersection dots for visual feedback
    // 2. Acts as an event shield — absorbs ALL touches so widgets underneath
    //    never receive click/press events during edit mode. Events bubble up
    //    to the container where our drag/click handlers process them.
    dots_overlay_ = lv_obj_create(container_);
    lv_obj_set_size(dots_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(dots_overlay_, LV_OBJ_FLAG_FLOATING);
    // CLICKABLE + EVENT_BUBBLE makes this a genuine event shield: as the
    // top-most full-grid child it becomes the hit target for every touch, so
    // no widget underneath ever receives a press/click while editing (only
    // sizing and moving, handled geometrically by the container's bubbled
    // press/click handlers). A non-clickable overlay would let touches fall
    // through to widgets that re-add CLICKABLE on async updates (e.g. the AMS
    // mini-status), launching them mid-edit. Chrome buttons (configure/remove/
    // delete-page) are created after this overlay, so they stay on top.
    lv_obj_add_flag(dots_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dots_overlay_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(dots_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(dots_overlay_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots_overlay_, 0, 0);
    lv_obj_set_style_pad_all(dots_overlay_, 0, 0);

    // Get container content area dimensions
    lv_area_t area;
    lv_obj_get_content_coords(container_, &area);
    int w = area.x2 - area.x1;
    int h = area.y2 - area.y1;

    if (w <= 0 || h <= 0) {
        spdlog::warn("[GridEditMode] Container content area {}x{}, skipping dots", w, h);
        return;
    }

    constexpr int DOT_SIZE = 4;
    constexpr int DOT_HALF = DOT_SIZE / 2;
    // Use contrast text color so dots are visible on both light and dark backgrounds
    lv_color_t screen_bg = ThemeManager::instance().current_palette().screen_bg;
    lv_color_t dot_color = theme_manager_get_contrast_color(screen_bg);

    // Place a dot at each grid intersection (ncols+1 x nrows+1 points)
    for (int r = 0; r <= nrows; ++r) {
        for (int c = 0; c <= ncols; ++c) {
            lv_obj_t* dot = lv_obj_create(dots_overlay_);
            lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, dot_color, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

            int x = (c * w / ncols) - DOT_HALF;
            int y = (r * h / nrows) - DOT_HALF;
            lv_obj_set_pos(dot, x, y);
        }
    }

    // Create "Delete Page" button — hidden for the main page, shown for secondary pages
    delete_page_btn_ = nullptr;
    if (config_ && static_cast<size_t>(page_index_) != config_->main_page_index() &&
        config_->page_count() > 1) {
        constexpr int DEL_BTN_SIZE = 40;
        constexpr int DEL_BTN_MARGIN = 8;

        delete_page_btn_ = lv_obj_create(dots_overlay_);
        lv_obj_set_size(delete_page_btn_, DEL_BTN_SIZE, DEL_BTN_SIZE);
        lv_obj_set_style_radius(delete_page_btn_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(delete_page_btn_,
                                  ThemeManager::instance().current_palette().danger, 0);
        lv_obj_set_style_bg_opa(delete_page_btn_, LV_OPA_80, 0);
        lv_obj_set_style_border_width(delete_page_btn_, 0, 0);
        lv_obj_set_style_pad_all(delete_page_btn_, 0, 0);
        lv_obj_add_flag(delete_page_btn_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(delete_page_btn_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(delete_page_btn_, w - DEL_BTN_SIZE - DEL_BTN_MARGIN, DEL_BTN_MARGIN);

        // Trash icon label
        lv_obj_t* icon = lv_label_create(delete_page_btn_);
        lv_label_set_text(icon, ICON_TRASH);
        lv_obj_set_style_text_font(icon, &mdi_icons_16, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        lv_obj_center(icon);

        // Click handler
        lv_obj_add_event_cb(
            delete_page_btn_,
            [](lv_event_t* ev) {
                auto* self = static_cast<GridEditMode*>(lv_event_get_user_data(ev));
                if (self && self->delete_page_cb_) {
                    self->delete_page_cb_();
                }
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[GridEditMode] Created dots overlay: {}x{} grid, {}x{} area (page {})", ncols,
                  nrows, w, h, page_index_);
}

void GridEditMode::destroy_dots_overlay() {
    if (dots_overlay_) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        // delete_page_btn_ is a child of dots_overlay_ — will be deleted with it
        delete_page_btn_ = nullptr;
        safe_deferred_delete(dots_overlay_);
        dots_overlay_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Hit-test: check if screen coordinates land on any grid widget
// ---------------------------------------------------------------------------

bool GridEditMode::hit_test_any_widget(int screen_x, int screen_y) const {
    if (!container_) {
        return false;
    }
    uint32_t child_count = lv_obj_get_child_count(container_);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(i));
        if (!child) {
            continue;
        }
        if (child == dots_overlay_ || child == selection_overlay_) {
            continue;
        }
        if (child == drag_ghost_ || child == snap_preview_) {
            continue;
        }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }
        lv_area_t coords;
        lv_obj_get_coords(child, &coords);
        if (screen_x >= coords.x1 && screen_x <= coords.x2 && screen_y >= coords.y1 &&
            screen_y <= coords.y2) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Widget catalog integration
// ---------------------------------------------------------------------------

void GridEditMode::open_widget_catalog(lv_obj_t* screen) {
    if (!config_) {
        spdlog::warn("[GridEditMode] Cannot open catalog: no config");
        return;
    }

    spdlog::info("[GridEditMode] Opening widget catalog (origin cell: {}, {})", catalog_origin_col_,
                 catalog_origin_row_);

    catalog_open_ = true;

    // Hide dots overlay while catalog is open — it sits on top of the screen
    // and absorbs all touch events, preventing interaction with the catalog.
    if (dots_overlay_) {
        lv_obj_add_flag(dots_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    WidgetCatalogOverlay::show(
        screen, *config_,
        [this](const std::string& widget_id) { place_widget_from_catalog(widget_id); },
        [this]() {
            catalog_open_ = false;
            // Restore dots overlay when catalog closes
            if (dots_overlay_) {
                lv_obj_remove_flag(dots_overlay_, LV_OBJ_FLAG_HIDDEN);
            }
        });
}

void GridEditMode::place_widget_from_catalog(const std::string& widget_id) {
    if (!config_ || !container_) {
        spdlog::warn("[GridEditMode] place_widget_from_catalog: no config or container");
        return;
    }

    const auto* def = find_widget_def(widget_id);
    if (!def) {
        spdlog::warn("[GridEditMode] Unknown widget ID: {}", widget_id);
        return;
    }

    int colspan = def->colspan;
    int rowspan = def->rowspan;

    // Determine current breakpoint and build a temporary grid
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint =
        bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;

    GridLayout temp_grid(breakpoint);
    const auto& entries = config_->page_entries(static_cast<size_t>(page_index_));

    // Only include widgets actually visible on screen (not hardware-gated invisible ones)
    std::unordered_set<std::string> visible_ids;
    uint32_t nchildren = lv_obj_get_child_count(container_);
    for (uint32_t ci = 0; ci < nchildren; ++ci) {
        lv_obj_t* child = lv_obj_get_child(container_, static_cast<int32_t>(ci));
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
            continue;
        }
        const char* cname = lv_obj_get_name(child);
        if (cname && cname[0] != '\0') {
            visible_ids.insert(cname);
        }
    }

    for (const auto& entry : entries) {
        if (!entry.enabled || !entry.has_grid_position()) {
            continue;
        }
        if (visible_ids.find(entry.id) == visible_ids.end()) {
            continue;
        }
        temp_grid.place({entry.id, entry.col, entry.row, entry.colspan, entry.rowspan});
    }

    int place_col = -1;
    int place_row = -1;

    // Try the catalog origin cell first
    if (catalog_origin_col_ >= 0 && catalog_origin_row_ >= 0 &&
        temp_grid.can_place(catalog_origin_col_, catalog_origin_row_, colspan, rowspan)) {
        place_col = catalog_origin_col_;
        place_row = catalog_origin_row_;
    } else {
        // Fall back to first available position
        auto pos = temp_grid.find_available(colspan, rowspan);
        if (pos) {
            place_col = pos->first;
            place_row = pos->second;
        }
    }

    // If default size doesn't fit, try progressively smaller sizes down to the minimum
    if (place_col < 0 || place_row < 0) {
        int min_c = def->effective_min_colspan();
        int min_r = def->effective_min_rowspan();

        if (min_c < colspan || min_r < rowspan) {
            // Try shrinking rowspan first (wider but shorter), then colspan
            for (int try_r = rowspan; try_r >= min_r && place_col < 0; --try_r) {
                for (int try_c = colspan; try_c >= min_c && place_col < 0; --try_c) {
                    if (try_c == colspan && try_r == rowspan)
                        continue; // Already tried

                    if (catalog_origin_col_ >= 0 && catalog_origin_row_ >= 0 &&
                        temp_grid.can_place(catalog_origin_col_, catalog_origin_row_, try_c,
                                            try_r)) {
                        place_col = catalog_origin_col_;
                        place_row = catalog_origin_row_;
                        colspan = try_c;
                        rowspan = try_r;
                    } else {
                        auto pos = temp_grid.find_available(try_c, try_r);
                        if (pos) {
                            place_col = pos->first;
                            place_row = pos->second;
                            colspan = try_c;
                            rowspan = try_r;
                        }
                    }
                }
            }

            if (place_col >= 0) {
                spdlog::info("[GridEditMode] Widget '{}' shrunk to {}x{} to fit", widget_id,
                             colspan, rowspan);
            }
        }
    }

    if (place_col < 0 || place_row < 0) {
        spdlog::warn("[GridEditMode] No available grid position for widget '{}' ({}x{})", widget_id,
                     colspan, rowspan);
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            "Not enough room for this widget. Rearrange or remove widgets to make space.");
        return;
    }

    // Enable the widget in config with the computed grid position.
    // Find the entry by ID — it should exist as disabled.
    auto& mutable_entries = config_->page_entries_mut(static_cast<size_t>(page_index_));
    bool found = false;
    for (auto& entry : mutable_entries) {
        if (entry.id == widget_id) {
            entry.enabled = true;
            entry.col = place_col;
            entry.row = place_row;
            entry.colspan = colspan;
            entry.rowspan = rowspan;
            found = true;
            break;
        }
    }

    if (!found) {
        // Widget not in this page's entries — add a new entry.
        // This happens for multi-instance widgets (user-created) and for any widget
        // being placed on a non-default page (pages beyond page 0 start empty).
        mutable_entries.push_back({widget_id, true, {}, place_col, place_row, colspan, rowspan});
        spdlog::info("[GridEditMode] Created new entry '{}' on page {}", widget_id, page_index_);
    }

    spdlog::info("[GridEditMode] Placed widget '{}' at ({},{}) {}x{}", widget_id, place_col,
                 place_row, colspan, rowspan);

    // Reset catalog origin
    catalog_origin_col_ = -1;
    catalog_origin_row_ = -1;

    // Deselect, save, and rebuild
    select_widget(nullptr);
    delete_page_btn_ = nullptr;
    dots_overlay_ = nullptr;
    config_->save();
    schedule_deferred_rebuild([this, widget_id]() {
        if (active_ && container_) {
            create_dots_overlay();
        }
        // Select the newly added widget so its chrome is visible immediately.
        // Force layout update first — the widget was just created by the rebuild
        // so LVGL hasn't calculated its position yet (coordinates would be 0,0).
        if (active_ && container_) {
            lv_obj_update_layout(container_);
            lv_obj_t* new_widget = lv_obj_find_by_name(container_, widget_id.c_str());
            if (new_widget) {
                select_widget(new_widget);
            }
        }
    });
}

} // namespace helix
