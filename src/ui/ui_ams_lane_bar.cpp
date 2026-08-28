// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_bar.h"

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "ams_lane_state.h"
#include "ams_state.h"
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
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix;

// ============================================================================
// Per-widget user data (managed via static registry for safe shutdown)
// ============================================================================

/**
 * @brief User data stored on each ams_lane_bar widget.
 *
 * Contains the slot index, the two decorations layered over the base
 * LaneState render (last_state, is_active — see apply_lane_border()), and
 * observer handles. Managed via a static registry rather than lv_obj
 * user_data, matching ui_ams_slot.cpp: that field can already carry other
 * payload on generated collections.
 */
struct LaneBarData {
    int slot_index = -1;
    int fill_pct = 0; ///< Last known fill percent (0-100), from slot_fill.

    /// Base render state from the lane_state subject. Border decoration reads
    /// this alongside is_active so the active observer never has to guess
    /// what the base border should fall back to.
    helix::ui::LaneState last_state = helix::ui::LaneState::Empty;
    bool is_active = false; ///< From slot_active_loaded — decoration only.

    // Widget object and named children (see ams_draw::create_slot_column()).
    lv_obj_t* root = nullptr; ///< == container; ghost opacity lands here.
    lv_obj_t* bar_bg = nullptr;
    lv_obj_t* bar_fill = nullptr;
    lv_obj_t* status_line = nullptr;

    // RAII observer handles - automatically removed when this struct is destroyed.
    ObserverGuard lane_state_observer;
    ObserverGuard color_observer;
    ObserverGuard fill_observer;
    ObserverGuard active_loaded_observer;

    // slot_active_loaded is a static-array (singleton-lifetime) subject, so
    // this token is always the empty (always-alive) contract — see
    // AmsState::get_slot_active_loaded_subject(int, SubjectLifetime&). Held
    // anyway for call-site symmetry with the project's dynamic-subject
    // pattern and reset ordering (#705).
    SubjectLifetime active_loaded_lifetime;
};

// Static registry mapping lv_obj_t* -> LaneBarData*. Used for safe cleanup
// during lv_deinit() when user_data may be unreliable (mirrors s_slot_registry
// in ui_ams_slot.cpp).
static std::unordered_map<lv_obj_t*, LaneBarData*> s_lane_bar_registry;

static LaneBarData* get_lane_bar_data(lv_obj_t* obj) {
    auto it = s_lane_bar_registry.find(obj);
    return (it != s_lane_bar_registry.end()) ? it->second : nullptr;
}

static void register_lane_bar_data(lv_obj_t* obj, LaneBarData* data) {
    s_lane_bar_registry[obj] = data;
}

/**
 * @brief Unregister and cleanup lane bar data (normal widget deletion path).
 */
static void unregister_lane_bar_data(lv_obj_t* obj) {
    auto it = s_lane_bar_registry.find(obj);
    if (it != s_lane_bar_registry.end()) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        std::unique_ptr<LaneBarData> data(it->second);
        if (data) {
            // Reset the dynamic-subject lifetime BEFORE its observer so the
            // observer's weak_ptr is already expired (#705 ordering).
            data->active_loaded_lifetime.reset();
            data->lane_state_observer.reset();
            data->color_observer.reset();
            data->fill_observer.reset();
            data->active_loaded_observer.reset();
        }
        s_lane_bar_registry.erase(it);
    }
}

/**
 * @brief Pre-deinit cleanup: release all lane bar data while widgets are still alive.
 *
 * Called via StaticSubjectRegistry BEFORE lv_deinit(), mirroring
 * cleanup_all_slot_data() in ui_ams_slot.cpp.
 */
static void cleanup_all_lane_bar_data() {
    for (auto& [obj, data] : s_lane_bar_registry) {
        if (!data)
            continue;
        data->active_loaded_lifetime.reset();
        data->lane_state_observer.release();
        data->color_observer.release();
        data->fill_observer.release();
        data->active_loaded_observer.release();
        delete data;
    }
    s_lane_bar_registry.clear();
    spdlog::debug("[AmsLaneBar] Pre-deinit cleanup: all lane bar data released");
}

// ============================================================================
// Rendering
// ============================================================================

/**
 * @brief Border is layered from TWO independent decorations over the base
 * state: the LaneState border (Present/Ghosted: 1px muted at 50%; Empty: none
 * — bar_bg is hidden) and the active-loaded override (2px text at 80%) on
 * top. Owning both here — keyed on last_state/is_active — means the
 * active-loaded observer firing on its own can never clobber (or be
 * clobbered by) a later lane_state/fill re-paint, which a single call site
 * writing raw border values from each observer could not guarantee.
 */
static void apply_lane_border(LaneBarData* d) {
    if (!d || !d->bar_bg)
        return;
    if (d->last_state == helix::ui::LaneState::Empty)
        return; // bar_bg is hidden; nothing to draw.

    if (d->is_active) {
        lv_obj_set_style_border_width(d->bar_bg, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(d->bar_bg, theme_manager_get_color("text"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(d->bar_bg, LV_OPA_80, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(d->bar_bg, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(d->bar_bg, theme_manager_get_color("text_muted"),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_opa(d->bar_bg, LV_OPA_50, LV_PART_MAIN);
    }
}

/// THE bar rendering. One switch, no opacity arithmetic at the call site.
static void apply_lane_state(LaneBarData* d, helix::ui::LaneState state) {
    if (!d || !d->bar_bg || !d->bar_fill)
        return;
    d->last_state = state;

    // Empty draws nothing at all. The widget stays in the layout so the lane
    // remains countable, but neither the outline nor the fill is painted.
    if (state == helix::ui::LaneState::Empty) {
        lv_obj_add_flag(d->bar_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->bar_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(d->root, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    lv_obj_remove_flag(d->bar_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(d->bar_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(d->bar_fill, LV_PCT(d->fill_pct));
    lv_obj_align(d->bar_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    apply_lane_border(d);

    // Ghost goes on the ROOT so every child dims together. This is the whole
    // mechanism: a per-element opacity is what made the previous attempt
    // unreadable, and it is what makes reversing #1071 safe.
    lv_obj_set_style_opa(d->root,
                         state == helix::ui::LaneState::Ghosted
                             ? static_cast<lv_opa_t>(theme_manager_get_spacing("ghost_opacity"))
                             : LV_OPA_COVER,
                         LV_PART_MAIN);
}

/// Active-loaded is a decoration laid OVER the base state, not an alternative
/// to it (a blocked lane still has filament; an active lane is still
/// Present) — so this touches only the border, never bar_fill or root opa.
static void apply_active_decoration(LaneBarData* d, bool active) {
    if (!d)
        return;
    d->is_active = active;
    apply_lane_border(d);
}

/// Filament color for the fill gradient. Mirrors the (soon-to-be-retired)
/// style_slot_bar()'s fill styling in ams_drawing_utils.cpp so the visual
/// does not regress when the overview/mini-status panels switch over.
static void apply_lane_color(LaneBarData* d, int color_int) {
    if (!d || !d->bar_fill)
        return;
    lv_color_t base_color = lv_color_hex(static_cast<uint32_t>(color_int));
    lv_color_t light_color = ams_draw::lighten_color(base_color, 50);
    lv_obj_set_style_bg_color(d->bar_fill, light_color, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(d->bar_fill, base_color, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(d->bar_fill, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->bar_fill, LV_OPA_COVER, LV_PART_MAIN);
}

// ============================================================================
// Widget Event Handler (for cleanup)
// ============================================================================

static void ams_lane_bar_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_obj_t* obj = lv_event_get_target_obj(e);
        if (!obj) {
            return;
        }
        unregister_lane_bar_data(obj);
    }
}

// ============================================================================
// Observers
// ============================================================================

/**
 * @brief Setup observers for the widget's current slot_index.
 *
 * Resolves AmsState::get_slot_lane_state_subject(), get_slot_color_subject(),
 * get_slot_fill_subject() and get_slot_active_loaded_subject() and observes
 * each with observe_int_sync<lv_obj_t>. All four are static-array
 * (singleton-lifetime) subjects (ams_state.cpp) — only the active_loaded
 * accessor offers a token'd overload, so only that observer carries a
 * SubjectLifetime; it is always the empty (always-alive) contract, held for
 * symmetry with the project's dynamic-subject pattern.
 */
static void setup_lane_bar_observers(LaneBarData* data) {
    if (data->slot_index < 0 || data->slot_index >= AmsState::MAX_SLOTS) {
        spdlog::warn("[AmsLaneBar] Invalid slot index {}, skipping observers", data->slot_index);
        return;
    }

    using helix::ui::observe_int_sync;
    AmsState& state = AmsState::instance();

    lv_subject_t* lane_state_subject = state.get_slot_lane_state_subject(data->slot_index);
    lv_subject_t* color_subject = state.get_slot_color_subject(data->slot_index);
    lv_subject_t* fill_subject = state.get_slot_fill_subject(data->slot_index);
    data->active_loaded_lifetime.reset();
    lv_subject_t* active_loaded_subject =
        state.get_slot_active_loaded_subject(data->slot_index, data->active_loaded_lifetime);

    // Capture the root object (not the data pointer) to avoid use-after-free
    // when a deferred callback runs after widget deletion — the registry
    // lookup is the validity check (same pattern as ui_ams_slot.cpp, #83).
    lv_obj_t* obj = data->root;

    if (lane_state_subject) {
        data->lane_state_observer =
            observe_int_sync<lv_obj_t>(lane_state_subject, obj, [](lv_obj_t* o, int state_int) {
                auto* d = get_lane_bar_data(o);
                if (d)
                    apply_lane_state(d, static_cast<helix::ui::LaneState>(state_int));
            });
    }
    if (color_subject) {
        data->color_observer =
            observe_int_sync<lv_obj_t>(color_subject, obj, [](lv_obj_t* o, int color_int) {
                auto* d = get_lane_bar_data(o);
                if (d)
                    apply_lane_color(d, color_int);
            });
    }
    if (fill_subject) {
        // pct < 0 means "no data" -> leave the current render untouched.
        data->fill_observer =
            observe_int_sync<lv_obj_t>(fill_subject, obj, [](lv_obj_t* o, int pct) {
                auto* d = get_lane_bar_data(o);
                if (!d || pct < 0)
                    return;
                d->fill_pct = std::clamp(pct, 0, 100);
                apply_lane_state(d, d->last_state);
            });
    }
    if (active_loaded_subject) {
        data->active_loaded_observer = observe_int_sync<lv_obj_t>(
            active_loaded_subject, obj,
            [](lv_obj_t* o, int active) {
                auto* d = get_lane_bar_data(o);
                if (d)
                    apply_active_decoration(d, active != 0);
            },
            data->active_loaded_lifetime);
    }

    // Trigger initial paint from current subject values. Fill and state are
    // read before the first apply_lane_state() so the initial fill height is
    // correct on the very first paint, not just from the second observer fire.
    if (fill_subject) {
        int pct = lv_subject_get_int(fill_subject);
        if (pct >= 0) {
            data->fill_pct = std::clamp(pct, 0, 100);
        }
    }
    if (lane_state_subject) {
        apply_lane_state(data,
                         static_cast<helix::ui::LaneState>(lv_subject_get_int(lane_state_subject)));
    }
    if (color_subject) {
        apply_lane_color(data, lv_subject_get_int(color_subject));
    }
    if (active_loaded_subject) {
        apply_active_decoration(data, lv_subject_get_int(active_loaded_subject) != 0);
    }

    spdlog::trace("[AmsLaneBar] Created observers for slot {}", data->slot_index);
}

// ============================================================================
// XML Handlers
// ============================================================================

static void* ams_lane_bar_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);

    // Compact vertical bar, sized entirely from spacing tokens. Consumers
    // (Tasks 5/6) can override bar_bg/bar_fill/status_line width directly by
    // name, the same way the overview/mini-status panels already resize the
    // create_slot_column() columns they build today.
    int32_t bar_radius = theme_manager_get_spacing("border_radius_small");
    int32_t bar_width = theme_manager_get_spacing("space_lg");
    int32_t bar_height = bar_width * 3;

    ams_draw::SlotColumn col = ams_draw::create_slot_column(static_cast<lv_obj_t*>(parent),
                                                            bar_width, bar_height, bar_radius);
    if (!col.container || !col.bar_bg || !col.bar_fill) {
        spdlog::error("[AmsLaneBar] create_slot_column failed to build the bar structure");
        return col.container;
    }

    lv_obj_set_name(col.bar_bg, "bar_bg");
    lv_obj_set_name(col.bar_fill, "bar_fill");
    if (col.status_line) {
        lv_obj_set_name(col.status_line, "status_line");
        // No error subject is wired to this widget yet (out of Task 4's
        // scope — see ams_lane_bar_row.xml's consumer, a later task). Default
        // to hidden, matching style_slot_bar()'s "non-error: hidden" baseline.
        lv_obj_add_flag(col.status_line, LV_OBJ_FLAG_HIDDEN);
    }

    auto data_ptr = std::make_unique<LaneBarData>();
    data_ptr->slot_index = -1; // Set by xml_apply when slot_index attr is parsed.
    data_ptr->root = col.container;
    data_ptr->bar_bg = col.bar_bg;
    data_ptr->bar_fill = col.bar_fill;
    data_ptr->status_line = col.status_line;

    lv_obj_t* obj = col.container;
    register_lane_bar_data(obj, data_ptr.release());
    lv_obj_add_event_cb(obj, ams_lane_bar_event_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[AmsLaneBar] Created widget from XML");
    return obj;
}

static void ams_lane_bar_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj) {
        spdlog::error("[AmsLaneBar] NULL object in xml_apply");
        return;
    }

    lv_xml_obj_apply(state, attrs);

    auto* data = get_lane_bar_data(obj);
    if (!data) {
        spdlog::error("[AmsLaneBar] No user data in xml_apply");
        return;
    }

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "slot_index") == 0) {
            int new_index = atoi(value);
            if (new_index != data->slot_index) {
                data->active_loaded_lifetime.reset();
                data->lane_state_observer.reset();
                data->color_observer.reset();
                data->fill_observer.reset();
                data->active_loaded_observer.reset();

                data->slot_index = new_index;

                setup_lane_bar_observers(data);

                spdlog::debug("[AmsLaneBar] Set slot_index={}", data->slot_index);
            }
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_ams_lane_bar_register(void) {
    lv_xml_register_widget("ams_lane_bar", ams_lane_bar_xml_create, ams_lane_bar_xml_apply);

    // Self-register cleanup — ensures lane bar data is released before
    // lv_deinit() so that lv_subject_deinit() can safely remove observers
    // from live widgets (mirrors ui_ams_slot_register()).
    StaticSubjectRegistry::instance().register_deinit("AmsLaneBarWidgets",
                                                      cleanup_all_lane_bar_data);

    spdlog::info("[AmsLaneBar] Registered ams_lane_bar widget with XML system");
}
