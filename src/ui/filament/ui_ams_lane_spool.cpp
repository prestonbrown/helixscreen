// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_spool.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_observer_guard.h"
#include "ui_spool_canvas.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_lane_state.h"
#include "ams_state.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/static_subject_registry.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "widget_data_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace helix;

// ============================================================================
// Spool visual construction (file-local)
//
// The layered spool graphic the widget renders: a spool canvas seen from the
// side or face-on per /ams/spool_style, plus the dashed empty-lane
// placeholder and the error dot. Lives here rather than ams_draw because the
// ams_lane_spool widget is its only consumer; kGhostOpa and the error/lane
// badges it composes with stay shared in ams_drawing_utils.
// ============================================================================

/// Widget handles produced by create_spool_visual()
struct SpoolVisual {
    lv_obj_t* container = nullptr;
    bool use_3d = true;
    int32_t spool_size = 0;
    lv_obj_t* glow = nullptr;              ///< current-lane glow layer, behind the spool (hidden)
    lv_obj_t* canvas = nullptr;            ///< spool canvas: side view (3D) or face-on (flat)
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
    constexpr int kDashCount = 16;
    constexpr int kDashAngle = 15;
    constexpr int kGapAngle = 7; // 16 * (15 + 7) = 352 - 360
    for (int d = 0; d < kDashCount; d++) {
        arc_dsc.start_angle = static_cast<uint16_t>(d * (kDashAngle + kGapAngle));
        arc_dsc.end_angle = static_cast<uint16_t>(arc_dsc.start_angle + kDashAngle);
        lv_draw_arc(layer, &arc_dsc);
    }
}

bool helix::ui::ams_spool_style_3d() {
    helix::Config* cfg = helix::Config::get_instance();
    return cfg->get<std::string>("/ams/spool_style", "3d") == "3d";
}

/**
 * @brief Build a spool visual into @p container, honoring /ams/spool_style.
 * @param container Parent to populate (its size is set to
 *        spool_size + kAmsLaneSpoolBadgeMarginPx, square).
 * @param spool_size Spool graphic size in px; <= 0 uses the "ams_slot_spool_size" token.
 */
static SpoolVisual create_spool_visual(lv_obj_t* container, int32_t spool_size) {
    SpoolVisual sv;
    if (!container)
        return sv;
    sv.container = container;
    sv.use_3d = helix::ui::ams_spool_style_3d();

    // Spool size is a dedicated responsive token (see ams_panel.xml consts).
    if (spool_size <= 0) {
        spool_size = theme_manager_get_spacing("ams_slot_spool_size");
        if (spool_size <= 0)
            spool_size = theme_manager_get_spacing("space_lg") * 4;
    }
    sv.spool_size = spool_size;

    int32_t container_size = spool_size + helix::ui::kAmsLaneSpoolBadgeMarginPx; // badge slack
    lv_obj_set_size(container, container_size, container_size);

    // Created first so it draws behind every spool layer.
    sv.glow = helix::ui::spool_glow_create(container);
    if (sv.glow) {
        lv_obj_set_size(sv.glow, spool_size, spool_size);
        lv_obj_align(sv.glow, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(sv.glow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_name(sv.glow, "spool_glow");
    }

    lv_obj_t* canvas = ui_spool_canvas_create(container, spool_size);
    if (canvas) {
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
        // Prevent flex layout from resizing the canvas
        lv_obj_set_style_min_width(canvas, spool_size, LV_PART_MAIN);
        lv_obj_set_style_min_height(canvas, spool_size, LV_PART_MAIN);
        lv_obj_set_style_max_width(canvas, spool_size, LV_PART_MAIN);
        lv_obj_set_style_max_height(canvas, spool_size, LV_PART_MAIN);
        if (!sv.use_3d)
            helix::ui::spool_canvas_set_view(canvas, helix::ui::SpoolView::Front);
        ui_spool_canvas_set_color(canvas, lv_color_hex(helix::kAmsDefaultSlotColor));
        ui_spool_canvas_set_fill_level(canvas, 1.0f);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.canvas = canvas;
        lv_obj_set_name(canvas, "spool_graphic");
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
        // Named so lv_obj_find_by_name() reaches it - it is the one element that
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

/// Update the spool's filament: its colour, multi-colour split and finish, as
/// every swatch paints them.
static void spool_visual_set_color(const SpoolVisual& sv, uint32_t rgb,
                                   const std::string& multi_color_hexes,
                                   helix::ui::SwatchFinish finish) {
    if (sv.canvas)
        helix::ui::spool_canvas_set_filament(sv.canvas, rgb, multi_color_hexes, finish);
}

/// Update spool fill level 0.0-1.0
static void spool_visual_set_fill(const SpoolVisual& sv, float fill) {
    if (sv.canvas)
        ui_spool_canvas_set_fill_level(sv.canvas, LV_CLAMP(0.0f, fill, 1.0f));
}

// ============================================================================
// Per-widget user data (managed via static registry for safe shutdown)
// ============================================================================

/**
 * @brief User data stored on each ams_lane_spool widget.
 *
 * The spool layers themselves are built by create_spool_visual() below
 * into the widget root; this struct holds the returned handles, the last
 * applied inputs, and the observer set. Registry-managed like ui_ams_slot.cpp
 * and ui_ams_lane_bar.cpp: lv_obj user_data can carry other payload on
 * generated collections, and the registry survives lv_deinit().
 */
struct LaneSpoolData {
    int slot_index = -1;
    int backend_index = 0;
    float fill_level = 1.0f; ///< Last applied fill (0.0-1.0), from slot_fill.
    bool has_error = false;  ///< From slot_has_error - error dot visibility.
    SlotError::Severity severity = SlotError::Severity::INFO; ///< Error dot color.

    /// Last-applied presentation, so a size rebuild can repaint fresh layers.
    helix::ui::LaneState lane_state = helix::ui::LaneState::Empty;
    uint32_t color_int = 0x808080;
    helix::ui::SpoolHighlight highlight = helix::ui::SpoolHighlight::None;

    SpoolVisual sv; ///< Layer handles (3D canvas or flat rings + placeholder + dot).

    /// Death signal for a secondary backend's dynamic subjects; empty for
    /// backend 0, whose observers carry AmsState's subjects lifetime instead.
    SubjectLifetime lifetime;

    // RAII observer handles - automatically removed when this struct is destroyed.
    ObserverGuard lane_state_observer;
    ObserverGuard color_observer;
    ObserverGuard paint_observer; ///< the slot's multi-colour list and finish
    ObserverGuard fill_observer;
    ObserverGuard has_error_observer;
    ObserverGuard severity_observer;
    ObserverGuard theme_observer; ///< a lit glow re-reads the accent
};

static helix::WidgetDataRegistry<LaneSpoolData> s_lane_spool_registry;

static LaneSpoolData* get_lane_spool_data(lv_obj_t* obj) {
    return s_lane_spool_registry.get(obj);
}

static void register_lane_spool_data(lv_obj_t* obj, LaneSpoolData* data) {
    s_lane_spool_registry.set(obj, data);
}

/**
 * @brief Unregister and cleanup spool data (normal widget deletion path).
 */
static void unregister_lane_spool_data(lv_obj_t* obj) {
    if (LaneSpoolData* registered = s_lane_spool_registry.get(obj)) {
        std::unique_ptr<LaneSpoolData> data(registered);
        if (data) {
            // Expire a secondary backend's token before its observers (#705
            // ordering, as in ui_ams_slot.cpp).
            data->lifetime.reset();
            data->lane_state_observer.reset();
            data->color_observer.reset();
            data->paint_observer.reset();
            data->fill_observer.reset();
            data->has_error_observer.reset();
            data->severity_observer.reset();
            data->theme_observer.reset();
        }
        s_lane_spool_registry.take(obj);
    }
}

/**
 * @brief Teardown cleanup: drop all spool data while widgets are still alive.
 *
 * Runs at every printer switch and before lv_deinit(); same rule as
 * cleanup_all_slot_data() in ui_ams_slot.cpp. The theme subject outlives a
 * switch, so an observer left on it would stay attached for the rest of the process.
 */
static void cleanup_all_lane_spool_data() {
    s_lane_spool_registry.for_each([](lv_obj_t* /*obj*/, LaneSpoolData* data) {
        if (!data)
            return;
        data->lifetime.reset();
        data->lane_state_observer.reset();
        data->color_observer.reset();
        data->paint_observer.reset();
        data->fill_observer.reset();
        data->has_error_observer.reset();
        data->severity_observer.reset();
        data->theme_observer.reset();
        delete data;
    });
    s_lane_spool_registry.clear();
    spdlog::debug("[AmsLaneSpool] Pre-deinit cleanup: all lane spool data released");
}

// ============================================================================
// Rendering
// ============================================================================

static void glow_opa_anim_cb(void* obj, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

/// Current-lane glow: bound to the spool's silhouette (cached, shared), shown
/// while the lane is highlighted and holds a spool. Pulse animates only the
/// layer's opacity, so no frame repaints or re-blurs anything; a running pulse
/// is left alone, so repeated applies never restart it.
static void apply_highlight(LaneSpoolData* d) {
    lv_obj_t* glow = d->sv.glow;
    if (!glow)
        return;
    using helix::ui::SpoolHighlight;
    const bool on =
        d->highlight != SpoolHighlight::None && d->lane_state != helix::ui::LaneState::Empty;
    const bool pulse = on && d->highlight == SpoolHighlight::Pulse;
    const bool pulsing = lv_anim_get(glow, glow_opa_anim_cb) != nullptr;
    if (pulsing && !pulse) {
        lv_anim_delete(glow, glow_opa_anim_cb);
        lv_obj_set_style_opa(glow, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (!on) {
        helix::ui::spool_glow_clear(glow);
        lv_obj_add_flag(glow, LV_OBJ_FLAG_HIDDEN);
        helix::ui::refresh_overhang_chain(glow);
        return;
    }
    helix::ui::spool_glow_paint(glow, d->sv.canvas, d->sv.spool_size);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_HIDDEN);
    helix::ui::refresh_overhang_chain(glow);
    if (pulse && !pulsing) {
        constexpr int32_t kPulseDimOpa = LV_OPA_40;
        constexpr uint32_t kPulseDurationMs = 600;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, glow);
        lv_anim_set_values(&anim, kPulseDimOpa, LV_OPA_COVER);
        lv_anim_set_duration(&anim, kPulseDurationMs);
        lv_anim_set_reverse_duration(&anim, kPulseDurationMs);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&anim, glow_opa_anim_cb);
        lv_anim_start(&anim);
    }
}

/**
 * @brief THE lane presentation rule, in spool form.
 *
 * One switch over LaneState, no opacity arithmetic at the call sites:
 * - Empty: the spool graphic is hidden and the dashed placeholder shows, so
 *   the lane stays countable without claiming anything is loaded.
 * - Ghosted: the graphic renders at kGhostOpa - the dimming is the disclaimer
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
        (state == helix::ui::LaneState::Ghosted) ? ams_draw::kGhostOpa : LV_OPA_COVER;

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
    set_opa(d->sv.canvas, spool_opa);
    set_visible(d->sv.empty_placeholder, !show_spool);
    apply_highlight(d);
}

static void apply_color(LaneSpoolData* d, int color_int) {
    if (!d)
        return;
    d->color_int = static_cast<uint32_t>(color_int);
    // The colour subject carries the primary colour; the split and finish ride
    // on the slot it names.
    std::string multi;
    helix::ui::SwatchFinish finish = helix::ui::SwatchFinish::Plain;
    if (AmsBackend* backend = AmsState::instance().get_backend(d->backend_index)) {
        const SlotInfo slot = backend->get_slot_info(d->slot_index);
        multi = slot.multi_color_hexes;
        finish = helix::ui::swatch_finish_of(slot);
    }
    spool_visual_set_color(d->sv, d->color_int, multi, finish);
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
 * already gives the lane that visibility on its own - an empty spool is still a
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
 * @brief Setup observers for the widget's current slot and backend.
 *
 * Resolves AmsState's per-slot subjects (lane_state, color, fill, has_error,
 * error_severity) for data->backend_index and observes each with
 * observe<int>.
 */
static void setup_lane_spool_observers(LaneSpoolData* data) {
    if (data->slot_index < 0) {
        spdlog::warn("[AmsLaneSpool] Invalid slot index {}, skipping observers", data->slot_index);
        return;
    }
    if (data->slot_index >= AmsState::kMaxSlots) {
        // AmsState warns once for the whole overflow; this slot stays unbound.
        spdlog::debug("[AmsLaneSpool] Slot {} is past the subject ceiling, left unbound",
                      data->slot_index);
        return;
    }

    using helix::ui::observe;
    AmsState& state = AmsState::instance();

    // Backend 0's subjects are static and die with AmsState's subjects; a
    // secondary backend's are dynamic and die with its lifetime token. Each
    // accessor below sets the same token, so one member carries it.
    const int b = data->backend_index;
    const int slot = data->slot_index;
    data->lifetime.reset();
    lv_subject_t* lane_state_subject = state.get_slot_lane_state_subject(b, slot, data->lifetime);
    lv_subject_t* color_subject = state.get_slot_color_subject(b, slot, data->lifetime);
    lv_subject_t* paint_subject = state.get_slot_paint_subject(b, slot, data->lifetime);
    lv_subject_t* fill_subject = state.get_slot_fill_subject(b, slot, data->lifetime);
    lv_subject_t* has_error_subject = state.get_slot_has_error_subject(b, slot, data->lifetime);
    lv_subject_t* severity_subject = state.get_slot_error_severity_subject(b, slot, data->lifetime);
    const SubjectLifetime lifetime = b == 0 ? state.get_subjects_lifetime() : data->lifetime;

    // Capture the root object (not the data pointer) to avoid use-after-free
    // when a deferred callback runs after widget deletion - the registry
    // lookup is the validity check (same pattern as ui_ams_slot.cpp, #83).
    lv_obj_t* obj = data->sv.container;

    if (lane_state_subject) {
        data->lane_state_observer = observe<int>(
            lane_state_subject, obj,
            [](lv_obj_t* o, int state_int) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_lane_state(d, static_cast<helix::ui::LaneState>(state_int));
            },
            lifetime);
    }
    if (color_subject) {
        data->color_observer = observe<int>(
            color_subject, obj,
            [](lv_obj_t* o, int color_int) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_color(d, color_int);
            },
            lifetime);
    }
    if (paint_subject) {
        // The split and finish can change under an unchanged colour.
        data->paint_observer = observe<int>(
            paint_subject, obj,
            [](lv_obj_t* o, int /*paint_key*/) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_color(d, static_cast<int>(d->color_int));
            },
            lifetime);
    }
    if (fill_subject) {
        data->fill_observer = observe<int>(
            fill_subject, obj,
            [](lv_obj_t* o, int pct) {
                auto* d = get_lane_spool_data(o);
                if (d)
                    apply_fill_pct(d, pct);
            },
            lifetime);
    }
    if (severity_subject) {
        // Color before visibility, so a has_error flip never paints one frame
        // with the default color.
        data->severity_observer = observe<int>(
            severity_subject, obj,
            [](lv_obj_t* o, int sev) {
                auto* d = get_lane_spool_data(o);
                if (!d)
                    return;
                d->severity = static_cast<SlotError::Severity>(sev);
                apply_error_decoration(d);
            },
            lifetime);
    }
    if (has_error_subject) {
        data->has_error_observer = observe<int>(
            has_error_subject, obj,
            [](lv_obj_t* o, int has_error) {
                auto* d = get_lane_spool_data(o);
                if (!d)
                    return;
                d->has_error = has_error != 0;
                apply_error_decoration(d);
            },
            lifetime);
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
    // The current-lane glow spills past the spool canvas and this root.
    helix::ui::pass_child_overhang(root);

    auto data_ptr = std::make_unique<LaneSpoolData>();
    data_ptr->slot_index = -1; // Set by xml_apply when slot_index attr is parsed.
    data_ptr->sv = create_spool_visual(root, spool_size);
    if (!data_ptr->sv.empty_placeholder) {
        spdlog::error("[AmsLaneSpool] create_spool_visual failed to populate the root");
    }

    LaneSpoolData* data = data_ptr.get();
    register_lane_spool_data(root, data_ptr.release());
    lv_obj_add_event_cb(root, ams_lane_spool_event_cb, LV_EVENT_DELETE, nullptr);

    if (lv_subject_t* theme = theme_manager_get_changed_subject()) {
        data->theme_observer = helix::ui::observe<int>(
            theme, root,
            [](lv_obj_t* o, int /*generation*/) {
                auto* d = get_lane_spool_data(o);
                if (d && d->highlight != helix::ui::SpoolHighlight::None && d->sv.glow &&
                    !lv_obj_has_flag(d->sv.glow, LV_OBJ_FLAG_HIDDEN))
                    helix::ui::spool_glow_paint(d->sv.glow, d->sv.canvas, d->sv.spool_size);
            },
            subject_never_freed());
    }

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
                data->paint_observer.reset();
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

void ams_lane_spool_set_index(lv_obj_t* spool, int slot_index, int backend_index) {
    auto* data = get_lane_spool_data(spool);
    if (!data || (slot_index == data->slot_index && backend_index == data->backend_index)) {
        return;
    }
    data->lifetime.reset();
    data->lane_state_observer.reset();
    data->color_observer.reset();
    data->paint_observer.reset();
    data->fill_observer.reset();
    data->has_error_observer.reset();
    data->severity_observer.reset();

    data->slot_index = slot_index;
    data->backend_index = backend_index;
    setup_lane_spool_observers(data);
}

void ams_lane_spool_set_size(lv_obj_t* spool, int32_t spool_size) {
    auto* data = get_lane_spool_data(spool);
    if (!data || spool_size <= 0)
        return;
    const bool style_3d = helix::ui::ams_spool_style_3d();
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

void ams_lane_spool_set_highlight(lv_obj_t* spool, SpoolHighlight highlight) {
    auto* data = get_lane_spool_data(spool);
    if (!data || highlight == data->highlight)
        return;
    data->highlight = highlight;
    apply_highlight(data);
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

    // Self-register cleanup - ensures spool data is released before lv_deinit()
    // so that lv_subject_deinit() can safely remove observers from live
    // widgets (mirrors ui_ams_slot_register()).
    StaticSubjectRegistry::instance().register_deinit(
        "AmsLaneSpoolWidgets", cleanup_all_lane_spool_data,
        StaticSubjectRegistry::Lifetime::ProcessRunOnSwitch);

    spdlog::info("[AmsLaneSpool] Registered ams_lane_spool widget with XML system");
}
