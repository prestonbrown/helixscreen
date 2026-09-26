// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_spool.h"

#include "ui_observer_guard.h"
#include "ui_spool_canvas.h"
#include "ui_update_queue.h"

#include "ams_lane_state.h"
#include "ams_state.h"
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

    ams_draw::SpoolVisual sv; ///< Layer handles (3D canvas or flat rings + placeholder + dot).

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
    ams_draw::spool_visual_set_color(d->sv, lv_color_hex(static_cast<uint32_t>(color_int)));
}

/**
 * @brief Apply a fill percent in the display_fill_pct encoding.
 *
 * pct < 0 means "no data": leave the current render (and fill_level) untouched
 * so a startup skeleton does not blank a lane that already rendered a value.
 */
static void apply_fill_pct(LaneSpoolData* d, int pct) {
    if (!d || pct < 0)
        return;
    pct = std::clamp(pct, 0, 100);
    d->fill_level = static_cast<float>(pct) / 100.0f;
    ams_draw::spool_visual_set_fill(d->sv, d->fill_level);
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
    data_ptr->sv = ams_draw::create_spool_visual(root, spool_size);
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

float ams_lane_spool_get_fill_level(lv_obj_t* spool) {
    auto* data = get_lane_spool_data(spool);
    return data ? data->fill_level : 1.0f;
}

void ams_lane_spool_set_fill_level(lv_obj_t* spool, float fill_level) {
    auto* data = get_lane_spool_data(spool);
    if (!data)
        return;
    data->fill_level = std::clamp(fill_level, 0.0f, 1.0f);
    ams_draw::spool_visual_set_fill(data->sv, data->fill_level);
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
