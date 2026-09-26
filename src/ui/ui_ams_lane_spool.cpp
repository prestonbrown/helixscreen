// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_spool.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_observer_guard.h"
#include "ui_spool_canvas.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_lane_state.h"
#include "ams_state.h"
#include "config.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "observer_factory.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix;

// ============================================================================
// Spool visual construction (file-local)
//
// The layered spool graphic the widget renders: pseudo-3D canvas or flat
// concentric rings per /ams/spool_style, plus the dashed empty-lane
// placeholder and the error dot. Lives here rather than ams_draw because the
// ams_lane_spool widget is its only consumer; GHOST_OPA and the error/lane
// badges it composes with stay shared in ams_drawing_utils.
// ============================================================================

/// Widget handles produced by create_spool_visual()
struct SpoolVisual {
    lv_obj_t* container = nullptr;
    bool use_3d = true;
    int32_t spool_size = 0;
    lv_obj_t* canvas = nullptr;            ///< 3D-only: pseudo-3D spool canvas
    lv_obj_t* spool_outer = nullptr;       ///< flat-only: outer flange ring
    lv_obj_t* color_swatch = nullptr;      ///< flat-only: filament color ring
    lv_obj_t* spool_hub = nullptr;         ///< flat-only: center hub
    lv_obj_t* empty_placeholder = nullptr; ///< dashed-circle "empty" placeholder (hidden)
    lv_obj_t* error_indicator = nullptr;   ///< error dot, top-right (hidden)
};

// Draw a dashed circle using segmented arcs (LVGL 9.5 has no dashed border API)
static void draw_dashed_circle_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = static_cast<lv_layer_t*>(lv_event_get_layer(e));

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t cx = coords.x1 + w / 2;
    int32_t cy = coords.y1 + h / 2;
    int32_t radius = LV_MIN(w, h) / 2 - 1;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.radius = static_cast<uint16_t>(radius);
    arc_dsc.width = 2;
    arc_dsc.color = theme_manager_get_color("text_muted");
    arc_dsc.opa = LV_OPA_20;

    // Draw 16 dashes of 15 degrees each with 7.5 degree gaps
    constexpr int DASH_COUNT = 16;
    constexpr int DASH_ANGLE = 15;
    constexpr int GAP_ANGLE = 7; // 16 * (15 + 7) = 352 — 360
    for (int d = 0; d < DASH_COUNT; d++) {
        arc_dsc.start_angle = static_cast<uint16_t>(d * (DASH_ANGLE + GAP_ANGLE));
        arc_dsc.end_angle = static_cast<uint16_t>(arc_dsc.start_angle + DASH_ANGLE);
        lv_draw_arc(layer, &arc_dsc);
    }
}

static bool resolve_3d_spool_style() {
    helix::Config* cfg = helix::Config::get_instance();
    return cfg->get<std::string>("/ams/spool_style", "3d") == "3d";
}

/**
 * @brief Build a spool visual into @p container, honoring /ams/spool_style.
 * @param container Parent to populate (its size is set to
 *        spool_size + AMS_LANE_SPOOL_BADGE_MARGIN_PX, square).
 * @param spool_size Spool graphic size in px; <= 0 uses the "ams_slot_spool_size" token.
 */
static SpoolVisual create_spool_visual(lv_obj_t* container, int32_t spool_size) {
    SpoolVisual sv;
    if (!container)
        return sv;
    sv.container = container;
    sv.use_3d = resolve_3d_spool_style();

    // Spool size is a dedicated responsive token (see ams_panel.xml consts).
    if (spool_size <= 0) {
        spool_size = theme_manager_get_spacing("ams_slot_spool_size");
        if (spool_size <= 0)
            spool_size = theme_manager_get_spacing("space_lg") * 4;
    }
    sv.spool_size = spool_size;

    int32_t container_size = spool_size + helix::ui::AMS_LANE_SPOOL_BADGE_MARGIN_PX; // badge slack
    lv_obj_set_size(container, container_size, container_size);

    if (sv.use_3d) {
        // ====================================================================
        // 3D SPOOL CANVAS (Bambu-style pseudo-3D with gradients + AA)
        // ====================================================================
        lv_obj_t* canvas = ui_spool_canvas_create(container, spool_size);
        if (canvas) {
            lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
            // Prevent flex layout from resizing the canvas
            lv_obj_set_style_min_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_min_height(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_height(canvas, spool_size, LV_PART_MAIN);
            ui_spool_canvas_set_color(canvas, lv_color_hex(helix::AMS_DEFAULT_SLOT_COLOR));
            ui_spool_canvas_set_fill_level(canvas, 1.0f);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
            sv.canvas = canvas;
            lv_obj_set_name(canvas, "spool_graphic");
        }
    } else {
        // ====================================================================
        // FLAT STYLE (skeuomorphic concentric rings)
        // ====================================================================
        int32_t filament_ring_size = spool_size - 8;
        int32_t hub_size = spool_size / 3;

        lv_obj_set_style_radius(container, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(container, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(container, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(container, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(container, lv_color_black(), LV_PART_MAIN);

        // Layer 1: Outer ring (flange - darker shade of filament color)
        lv_obj_t* outer_ring = lv_obj_create(container);
        lv_obj_set_size(outer_ring, spool_size, spool_size);
        lv_obj_align(outer_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(outer_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            outer_ring, ams_draw::darken_color(lv_color_hex(helix::AMS_DEFAULT_SLOT_COLOR), 50),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(outer_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(outer_ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(outer_ring, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(outer_ring, LV_OPA_50, LV_PART_MAIN);
        lv_obj_remove_flag(outer_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(outer_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.spool_outer = outer_ring;
        lv_obj_set_name(outer_ring, "spool_outer");

        // Layer 2: Main filament color ring
        lv_obj_t* filament_ring = lv_obj_create(container);
        lv_obj_set_size(filament_ring, filament_ring_size, filament_ring_size);
        lv_obj_align(filament_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(filament_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(filament_ring, lv_color_hex(helix::AMS_DEFAULT_SLOT_COLOR),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(filament_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(filament_ring, 0, LV_PART_MAIN);
        lv_obj_remove_flag(filament_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(filament_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.color_swatch = filament_ring;
        lv_obj_set_name(filament_ring, "spool_graphic");

        // Layer 3: Center hub
        lv_obj_t* hub = lv_obj_create(container);
        lv_obj_set_size(hub, hub_size, hub_size);
        lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hub, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(hub, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(hub, theme_manager_get_color("ams_hub_border"), LV_PART_MAIN);
        lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hub, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.spool_hub = hub;
        lv_obj_set_name(hub, "spool_hub");
    }

    // Create empty slot placeholder (circle outline with plus icon, initially hidden)
    {
        lv_obj_t* ph = lv_obj_create(container);
        lv_obj_set_size(ph, spool_size - 4, spool_size - 4);
        lv_obj_align(ph, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ph, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ph, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(ph, draw_dashed_circle_cb, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);

        // Plus icon centered in circle to communicate "empty, add filament"
        const char* plus_glyph = helix::ui::icon::lookup_codepoint("plus");
        if (plus_glyph) {
            lv_obj_t* plus = lv_label_create(ph);
            lv_label_set_text(plus, plus_glyph);
            lv_obj_set_style_text_font(plus, &mdi_icons_24, LV_PART_MAIN);
            lv_obj_set_style_text_color(plus, theme_manager_get_color("text_muted"), LV_PART_MAIN);
            lv_obj_set_style_text_opa(plus, LV_OPA_20, LV_PART_MAIN);
            lv_obj_align(plus, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(plus, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        // Named so lv_obj_find_by_name() reaches it — it is the one element that
        // exists in both the flat and 3D branches, which makes it the stable
        // handle for "is this lane rendering as unassigned-empty?" from tests
        // and from `helix-screen ctl`.
        lv_obj_set_name(ph, "empty_placeholder");
        sv.empty_placeholder = ph;
    }

    // Create error indicator dot (top-right of container, initially hidden)
    {
        lv_obj_t* err = lv_obj_create(container);
        lv_obj_set_size(err, 14, 14);
        lv_obj_set_style_radius(err, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(err, theme_manager_get_color("danger"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(err, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(err, 0, LV_PART_MAIN);
        lv_obj_set_align(err, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_style_translate_x(err, -2, LV_PART_MAIN);
        lv_obj_set_style_translate_y(err, 2, LV_PART_MAIN);
        lv_obj_remove_flag(err, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_HIDDEN);
        sv.error_indicator = err;
    }

    return sv;
}

/// Update spool color (3D canvas or flat color_swatch + darkened outer flange)
static void spool_visual_set_color(const SpoolVisual& sv, lv_color_t color) {
    if (sv.use_3d) {
        if (sv.canvas)
            ui_spool_canvas_set_color(sv.canvas, color);
    } else if (sv.color_swatch) {
        lv_obj_set_style_bg_color(sv.color_swatch, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sv.color_swatch, LV_OPA_COVER, LV_PART_MAIN);
        if (sv.spool_outer)
            lv_obj_set_style_bg_color(sv.spool_outer, ams_draw::darken_color(color, 50),
                                      LV_PART_MAIN);
    }
}

/// Update spool fill level 0.0-1.0 (3D canvas fill or flat concentric ring size)
static void spool_visual_set_fill(const SpoolVisual& sv, float fill) {
    if (fill < 0.0f)
        fill = 0.0f;
    if (fill > 1.0f)
        fill = 1.0f;
    if (sv.use_3d) {
        if (sv.canvas)
            ui_spool_canvas_set_fill_level(sv.canvas, fill);
    } else if (sv.color_swatch && sv.container && sv.spool_hub) {
        lv_obj_update_layout(sv.container);
        int32_t spool_w = lv_obj_get_width(sv.container);
        int32_t hub_w = lv_obj_get_width(sv.spool_hub);
        int32_t min_ring = hub_w + 4;
        int32_t max_ring = spool_w - 8;
        int32_t ring_size = min_ring + static_cast<int32_t>((max_ring - min_ring) * fill);
        lv_obj_set_size(sv.color_swatch, ring_size, ring_size);
        lv_obj_align(sv.color_swatch, LV_ALIGN_CENTER, 0, 0);
    }
}

/// Toggle the empty-slot placeholder vs. the spool graphic
static void spool_visual_set_empty(const SpoolVisual& sv, bool empty) {
    auto show = [&](lv_obj_t* o, bool visible) {
        if (!o)
            return;
        if (visible)
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(sv.empty_placeholder, empty);
    show(sv.canvas, !empty);
    show(sv.spool_outer, !empty);
    show(sv.color_swatch, !empty);
    show(sv.spool_hub, !empty);
}

// ============================================================================
// Per-widget user data (managed via static registry for safe shutdown)
// ============================================================================

/**
 * @brief User data stored on each ams_lane_spool widget.
 *
 * The spool layers themselves are built by ams_draw::create_spool_visual()
 * into the widget root; this struct holds the returned handles, the last
 * applied inputs, and the observer set. Registry-managed like ui_ams_slot.cpp
 * and ui_ams_lane_bar.cpp: lv_obj user_data can carry other payload on
 * generated collections, and the registry survives lv_deinit().
 */
struct LaneSpoolData {
    int slot_index = -1;
    float fill_level = 1.0f; ///< Last applied fill (0.0-1.0), from slot_fill.
    bool has_error = false;  ///< From slot_has_error — error dot visibility.
    SlotError::Severity severity = SlotError::Severity::INFO; ///< Error dot color.

    /// Last-applied presentation, so a size rebuild can repaint fresh layers.
    helix::ui::LaneState lane_state = helix::ui::LaneState::Empty;
    uint32_t color_int = 0x808080;

    SpoolVisual sv; ///< Layer handles (3D canvas or flat rings + placeholder + dot).

    // RAII observer handles - automatically removed when this struct is destroyed.
    ObserverGuard lane_state_observer;
    ObserverGuard color_observer;
    ObserverGuard fill_observer;
    ObserverGuard has_error_observer;
    ObserverGuard severity_observer;
};

static std::unordered_map<lv_obj_t*, LaneSpoolData*> s_lane_spool_registry;

static LaneSpoolData* get_lane_spool_data(lv_obj_t* obj) {
    auto it = s_lane_spool_registry.find(obj);
    return (it != s_lane_spool_registry.end()) ? it->second : nullptr;
}

static void register_lane_spool_data(lv_obj_t* obj, LaneSpoolData* data) {
    s_lane_spool_registry[obj] = data;
}

/**
 * @brief Unregister and cleanup spool data (normal widget deletion path).
 */
static void unregister_lane_spool_data(lv_obj_t* obj) {
    auto it = s_lane_spool_registry.find(obj);
    if (it != s_lane_spool_registry.end()) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        std::unique_ptr<LaneSpoolData> data(it->second);
        if (data) {
            // All observed subjects are static-array (singleton lifetime), so
            // reset() is safe on every path here (#579/#705 ordering notes in
            // ui_ams_slot.cpp).
            data->lane_state_observer.reset();
            data->color_observer.reset();
            data->fill_observer.reset();
            data->has_error_observer.reset();
            data->severity_observer.reset();
        }
        s_lane_spool_registry.erase(it);
    }
}

/**
 * @brief Pre-deinit cleanup: release all spool data while widgets are still alive.
 *
 * Called via StaticSubjectRegistry BEFORE lv_deinit(), mirroring
 * cleanup_all_slot_data() in ui_ams_slot.cpp.
 */
static void cleanup_all_lane_spool_data() {
    for (auto& [obj, data] : s_lane_spool_registry) {
        if (!data)
            continue;
        data->lane_state_observer.release();
        data->color_observer.release();
        data->fill_observer.release();
        data->has_error_observer.release();
        data->severity_observer.release();
        delete data;
    }
    s_lane_spool_registry.clear();
    spdlog::debug("[AmsLaneSpool] Pre-deinit cleanup: all lane spool data released");
}

// ============================================================================
// Rendering
// ============================================================================

/**
 * @brief THE lane presentation rule, in spool form.
 *
 * One switch over LaneState, no opacity arithmetic at the call sites:
 * - Empty: the spool graphic is hidden and the dashed placeholder shows, so
 *   the lane stays countable without claiming anything is loaded.
 * - Ghosted: the graphic renders at GHOST_OPA — the dimming is the disclaimer
 *   that says "assigned, not present" (#1071/#1065).
 * - Present: full strength.
 *
 * The placeholder and the error dot sit OUTSIDE the dimming on purpose: an
 * empty lane has nothing to dim, and an error must stay readable even when
 * the lane carrying it is otherwise ghosted.
 */
static void apply_lane_state(LaneSpoolData* d, helix::ui::LaneState state) {
    if (!d)
        return;
    d->lane_state = state;

    const bool show_spool = state != helix::ui::LaneState::Empty;
    const lv_opa_t spool_opa =
        (state == helix::ui::LaneState::Ghosted) ? ams_draw::GHOST_OPA : LV_OPA_COVER;

    auto set_visible = [](lv_obj_t* o, bool visible) {
        if (!o)
            return;
        if (visible)
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    auto set_opa = [](lv_obj_t* o, lv_opa_t opa) {
        if (!o)
            return;
        lv_obj_set_style_opa(o, opa, LV_PART_MAIN);
    };

    set_visible(d->sv.canvas, show_spool);
    set_visible(d->sv.spool_outer, show_spool);
    set_visible(d->sv.color_swatch, show_spool);
    set_visible(d->sv.spool_hub, show_spool);
    set_opa(d->sv.canvas, spool_opa);
    set_opa(d->sv.spool_outer, spool_opa);
    set_opa(d->sv.color_swatch, spool_opa);
    set_visible(d->sv.empty_placeholder, !show_spool);
}

static void apply_color(LaneSpoolData* d, int color_int) {
    if (!d)
        return;
    d->color_int = static_cast<uint32_t>(color_int);
    spool_visual_set_color(d->sv, lv_color_hex(d->color_int));
}

/**
 * @brief Apply a fill percent in the display_fill_pct encoding.
 *
 * pct < 0 means "no data": leave the current render (and fill_level) untouched
 * so a startup skeleton does not blank a lane that already rendered a value.
 *
 * Otherwise the fill renders RAW (0 paints an empty spool) by deliberation,
 * unlike the bar family's ams_draw::floor_fill_pct(): the bar floor exists so
 * a present-but-spent lane keeps a visible sliver, and the spool graphic
 * already gives the lane that visibility on its own — an empty spool is still a
 * drawn spool, not a vanished bar. Painting filament that does not exist
 * would add nothing the graphic lacks. The remaining-percent LABEL beside a
 * strip spool is where exactness is communicated instead.
 */
static void apply_fill_pct(LaneSpoolData* d, int pct) {
    if (!d || pct < 0)
        return;
    pct = std::clamp(pct, 0, 100);
    d->fill_level = static_cast<float>(pct) / 100.0f;
    spool_visual_set_fill(d->sv, d->fill_level);
}

/// Error dot: severity color + visibility (+ pulse when animations allow).
/// Keys on the has_error/error_severity subjects AmsState derives from
/// `status == BLOCKED || slot.error`, so the dot is reactive with no panel
/// refresh.
static void apply_error_decoration(LaneSpoolData* d) {
    if (!d)
        return;
    const bool animate = DisplaySettingsManager::instance().get_animations_enabled();
    ams_draw::update_error_badge(d->sv.error_indicator, d->has_error, d->severity, animate);
}

// ============================================================================
// Widget Event Handler (for cleanup)
// ============================================================================

static void ams_lane_spool_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_obj_t* obj = lv_event_get_target_obj(e);
        if (obj) {
            unregister_lane_spool_data(obj);
        }
    }
}

// ============================================================================
// Observers
// ============================================================================

/**
 * @brief Setup observers for the widget's current slot_index.
 *
 * Resolves AmsState's per-slot subjects (lane_state, color, fill, has_error,
 * error_severity) and observes each with observe_int_sync<lv_obj_t>. All are
 * static-array (singleton-lifetime) subjects, so every observer carries
 * state.get_subjects_lifetime() — the same seam ams_lane_bar observes through.
 */
static void setup_lane_spool_observers(LaneSpoolData* data) {
    if (data->slot_index < 0 || data->slot_index >= AmsState::MAX_SLOTS) {
        spdlog::warn("[AmsLaneSpool] Invalid slot index {}, skipping observers", data->slot_index);
        return;
    }

    using helix::ui::observe_int_sync;
    AmsState& state = AmsState::instance();

    lv_subject_t* lane_state_subject = state.get_slot_lane_state_subject(data->slot_index);
    lv_subject_t* color_subject = state.get_slot_color_subject(data->slot_index);
    lv_subject_t* fill_subject = state.get_slot_fill_subject(data->slot_index);
    lv_subject_t* has_error_subject = state.get_slot_has_error_subject(data->slot_index);
    lv_subject_t* severity_subject = state.get_slot_error_severity_subject(data->slot_index);

    // Capture the root object (not the data pointer) to avoid use-after-free
    // when a deferred callback runs after widget deletion — the registry
    // lookup is the validity check (same pattern as ui_ams_slot.cpp, #83).
    lv_obj_t* obj = data->sv.container;

    if (lane_state_subject) {
        data->lane_state_observer = observe_int_sync<lv_obj_t>(
            lane_state_subject, obj,
            [](lv_obj_t* o, int state_int) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_lane_state(d, static_cast<helix::ui::LaneState>(state_int));
            },
            state.get_subjects_lifetime());
    }
    if (color_subject) {
        data->color_observer = observe_int_sync<lv_obj_t>(
            color_subject, obj,
            [](lv_obj_t* o, int color_int) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_color(d, color_int);
            },
            state.get_subjects_lifetime());
    }
    if (fill_subject) {
        data->fill_observer = observe_int_sync<lv_obj_t>(
            fill_subject, obj,
            [](lv_obj_t* o, int pct) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_fill_pct(d, pct);
            },
            state.get_subjects_lifetime());
    }
    if (severity_subject) {
        // Color before visibility, so a has_error flip never paints one frame
        // with the default color.
        data->severity_observer = observe_int_sync<lv_obj_t>(
            severity_subject, obj,
            [](lv_obj_t* o, int sev) {
                auto* d = get_lane_spool_data(o);
                if (!d)
                    return;
                d->severity = static_cast<SlotError::Severity>(sev);
                apply_error_decoration(d);
            },
            state.get_subjects_lifetime());
    }
    if (has_error_subject) {
        data->has_error_observer = observe_int_sync<lv_obj_t>(
            has_error_subject, obj,
            [](lv_obj_t* o, int has_error) {
                auto* d = get_lane_spool_data(o);
                if (!d)
                    return;
                d->has_error = has_error != 0;
                apply_error_decoration(d);
            },
            state.get_subjects_lifetime());
    }

    // Trigger initial paint from current subject values.
    if (fill_subject) {
        apply_fill_pct(data, lv_subject_get_int(fill_subject));
    }
    if (lane_state_subject) {
        apply_lane_state(data,
                         static_cast<helix::ui::LaneState>(lv_subject_get_int(lane_state_subject)));
    }
    if (color_subject) {
        apply_color(data, lv_subject_get_int(color_subject));
    }
    if (severity_subject) {
        data->severity = static_cast<SlotError::Severity>(lv_subject_get_int(severity_subject));
    }
    if (has_error_subject) {
        data->has_error = lv_subject_get_int(has_error_subject) != 0;
        apply_error_decoration(data);
    }

    spdlog::trace("[AmsLaneSpool] Created observers for slot {}", data->slot_index);
}

// ============================================================================
// XML Handlers
// ============================================================================

static void* ams_lane_spool_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    void* parent = lv_xml_state_get_parent(state);

    // Spool graphic size: the ams_slot_spool_size responsive token, or an
    // explicit spool_size attr for consumers with a measured cell (the
    // mini-status strip). create_spool_visual() sizes the widget root to
    // spool_size + SPOOL_VISUAL_BADGE_MARGIN_PX so the lane badge is not
    // clipped.
    int32_t spool_size = 0;
    for (int i = 0; attrs && attrs[i]; i += 2) {
        if (strcmp(attrs[i], "spool_size") == 0) {
            spool_size = atoi(attrs[i + 1]);
        }
    }

    lv_obj_t* root = ams_draw::create_transparent_container(static_cast<lv_obj_t*>(parent));
    if (!root) {
        spdlog::error("[AmsLaneSpool] failed to create root container");
        return nullptr;
    }

    auto data_ptr = std::make_unique<LaneSpoolData>();
    data_ptr->slot_index = -1; // Set by xml_apply when slot_index attr is parsed.
    data_ptr->sv = create_spool_visual(root, spool_size);
    if (!data_ptr->sv.empty_placeholder) {
        spdlog::error("[AmsLaneSpool] create_spool_visual failed to populate the root");
    }

    LaneSpoolData* data = data_ptr.get();
    register_lane_spool_data(root, data_ptr.release());
    lv_obj_add_event_cb(root, ams_lane_spool_event_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[AmsLaneSpool] Created widget from XML");
    return root;
}

static void ams_lane_spool_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj) {
        spdlog::error("[AmsLaneSpool] NULL object in xml_apply");
        return;
    }

    lv_xml_obj_apply(state, attrs);

    auto* data = get_lane_spool_data(obj);
    if (!data) {
        spdlog::error("[AmsLaneSpool] No user data in xml_apply");
        return;
    }

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "slot_index") == 0) {
            int new_index = atoi(value);
            if (new_index != data->slot_index) {
                data->lane_state_observer.reset();
                data->color_observer.reset();
                data->fill_observer.reset();
                data->has_error_observer.reset();
                data->severity_observer.reset();

                data->slot_index = new_index;

                setup_lane_spool_observers(data);

                spdlog::debug("[AmsLaneSpool] Set slot_index={}", data->slot_index);
            }
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

namespace helix::ui {

void ams_lane_spool_set_index(lv_obj_t* spool, int slot_index) {
    auto* data = get_lane_spool_data(spool);
    if (!data || slot_index == data->slot_index) {
        return;
    }
    data->lane_state_observer.reset();
    data->color_observer.reset();
    data->fill_observer.reset();
    data->has_error_observer.reset();
    data->severity_observer.reset();

    data->slot_index = slot_index;
    setup_lane_spool_observers(data);
}

void ams_lane_spool_set_size(lv_obj_t* spool, int32_t spool_size) {
    auto* data = get_lane_spool_data(spool);
    if (!data || spool_size <= 0)
        return;
    const bool style_3d = resolve_3d_spool_style();
    if (spool_size == data->sv.spool_size && style_3d == data->sv.use_3d)
        return; // already rendered at this size and style

    // Resizing means rebuilding: the layer sizes and the 3D canvas buffer are
    // baked at creation. Deletion stays deferred (this can run inside a queued
    // callback); sv is overwritten before the stale handles could be reused.
    helix::ui::safe_clean_children(spool);
    data->sv = create_spool_visual(spool, spool_size);

    // Repaint the fresh layers with the cached presentation.
    spool_visual_set_fill(data->sv, data->fill_level);
    apply_lane_state(data, data->lane_state);
    apply_color(data, static_cast<int>(data->color_int));
    apply_error_decoration(data);
}

const char* lane_material_text(helix::ui::LaneState state, const char* material) {
    if (state == helix::ui::LaneState::Empty)
        return lv_tr("Empty"); // UI copy, not a material name
    if (!material || material[0] == '\0')
        return "--";
    return material;
}

float ams_lane_spool_get_fill_level(lv_obj_t* spool) {
    auto* data = get_lane_spool_data(spool);
    return data ? data->fill_level : 1.0f;
}

void ams_lane_spool_set_fill_level(lv_obj_t* spool, float fill_level) {
    auto* data = get_lane_spool_data(spool);
    if (!data)
        return;
    data->fill_level = std::clamp(fill_level, 0.0f, 1.0f);
    spool_visual_set_fill(data->sv, data->fill_level);
}

} // namespace helix::ui

void ui_ams_lane_spool_register(void) {
    lv_xml_register_widget("ams_lane_spool", ams_lane_spool_xml_create, ams_lane_spool_xml_apply);

    // Self-register cleanup — ensures spool data is released before lv_deinit()
    // so that lv_subject_deinit() can safely remove observers from live
    // widgets (mirrors ui_ams_slot_register()).
    StaticSubjectRegistry::instance().register_deinit("AmsLaneSpoolWidgets",
                                                      cleanup_all_lane_spool_data);

    spdlog::info("[AmsLaneSpool] Registered ams_lane_spool widget with XML system");
}
