// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_progress_arc.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_style.h"

#include <spdlog/spdlog.h>

#include <unordered_map>

namespace helix::ui {

namespace {

constexpr const char* kComponentScopeName = "helix_progress_arc";
constexpr const char* kTierStyleNames[] = {"arc_w_4", "arc_w_6", "arc_w_8", "arc_w_10", "arc_w_12"};
constexpr int kTierCount = 5;

struct AttachContext {
    lv_obj_t* arc;
    lv_subject_t* tier_subject;
    bool owns_subject; // if true, free tier_subject on arc destruction
};

// Per-arc context registry — avoids storing AttachContext* on lv_obj user_data
// (which other LVGL/widget machinery may also use, see [L069]). Keyed by arc
// pointer; entries removed on the arc's LV_EVENT_DELETE.
static std::unordered_map<lv_obj_t*, AttachContext*>& registry() {
    static std::unordered_map<lv_obj_t*, AttachContext*> m;
    return m;
}

void resize_and_publish(lv_obj_t* parent, AttachContext* ctx) {
    if (!ctx || !ctx->arc || !parent)
        return;
    lv_obj_update_layout(parent);
    int ph = lv_obj_get_content_height(parent);
    int pw = lv_obj_get_content_width(parent);
    int dim = ph < pw ? ph : pw;
    if (dim <= 0)
        return;
    if (lv_obj_get_width(ctx->arc) != dim || lv_obj_get_height(ctx->arc) != dim) {
        lv_obj_set_size(ctx->arc, dim, dim);
    }
    int tier = progress_arc_thickness_tier_for(dim);
    // lv_subject_set_int is a no-op when value is unchanged.
    lv_subject_set_int(ctx->tier_subject, tier);
}

void on_parent_size_changed(lv_event_t* e) {
    auto* parent = lv_event_get_current_target_obj(e);
    auto* ctx = static_cast<AttachContext*>(lv_event_get_user_data(e));
    resize_and_publish(parent, ctx);
}

void on_arc_deleted(lv_event_t* e) {
    auto* ctx = static_cast<AttachContext*>(lv_event_get_user_data(e));
    if (ctx) {
        registry().erase(ctx->arc);
        if (ctx->owns_subject && ctx->tier_subject) {
            lv_subject_deinit(ctx->tier_subject);
            delete ctx->tier_subject;
        }
        delete ctx;
    }
}

} // namespace

int progress_arc_thickness_tier_for(int diameter_px) {
    if (diameter_px < 80)
        return 0; // 4px  — Tiny/Micro
    if (diameter_px < 120)
        return 1; // 6px
    if (diameter_px < 180)
        return 2; // 8px  — Medium default
    if (diameter_px < 240)
        return 3; // 10px
    return 4;     // 12px — XXLarge / user-resized big
}

void attach_progress_arc(lv_obj_t* arc, lv_obj_t* parent, lv_subject_t* tier_subject) {
    if (!arc || !tier_subject) {
        spdlog::warn("[progress_arc] attach: null arc or subject");
        return;
    }
    if (!parent) {
        parent = lv_obj_get_parent(arc);
    }
    if (!parent) {
        spdlog::warn("[progress_arc] attach: arc has no parent");
        return;
    }

    // Bind the 5 thickness styles to the tier subject. Styles live in the
    // helix_progress_arc component scope; lookup is deterministic by name.
    // If the scope isn't registered (e.g., in LVGLTestFixture-based unit
    // tests that don't load XML), skip the binding but still wire the
    // resize behavior — the arc stays at LVGL's default stroke instead of
    // throwing the whole feature away.
    auto* scope = lv_xml_component_get_scope(kComponentScopeName);
    if (scope) {
        for (int tier = 0; tier < kTierCount; ++tier) {
            auto* xs = lv_xml_get_style_by_name(scope, kTierStyleNames[tier]);
            if (!xs) {
                spdlog::warn("[progress_arc] style '{}' missing from component scope",
                             kTierStyleNames[tier]);
                continue;
            }
            // Same style applies to LV_PART_MAIN AND LV_PART_INDICATOR — both
            // edges of the arc track must share the width. (XML parts="..."
            // attr does this in one line; here we hand-roll because the
            // binding is programmatic.)
            lv_obj_bind_style(arc, &xs->style, LV_PART_MAIN, tier_subject, tier);
            lv_obj_bind_style(arc, &xs->style, LV_PART_INDICATOR, tier_subject, tier);
        }
    } else {
        spdlog::warn("[progress_arc] component scope '{}' not registered — "
                     "ensure helix_progress_arc.xml is registered at startup. "
                     "Skipping bind_style; arc will use LVGL default stroke.",
                     kComponentScopeName);
    }

    // Per-arc context survives until the arc is deleted.
    auto* ctx = new AttachContext{arc, tier_subject, /*owns_subject=*/false};
    registry()[arc] = ctx;
    lv_obj_add_event_cb(arc, on_arc_deleted, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(parent, on_parent_size_changed, LV_EVENT_SIZE_CHANGED, ctx);

    // Seed initial sizing + tier so the arc renders correctly before any
    // SIZE_CHANGED event fires.
    resize_and_publish(parent, ctx);
}

void refresh_progress_arc(lv_obj_t* arc) {
    auto& m = registry();
    auto it = m.find(arc);
    if (it == m.end())
        return;
    AttachContext* ctx = it->second;
    if (!ctx)
        return;
    resize_and_publish(lv_obj_get_parent(arc), ctx);
}

lv_subject_t* attach_progress_arc_owned(lv_obj_t* arc, lv_obj_t* parent) {
    auto* subject = new lv_subject_t{};
    lv_subject_init_int(subject, 2); // tier 2 (medium / 8px) until first resize
    attach_progress_arc(arc, parent, subject);
    // Flip ownership flag on the now-registered context.
    auto it = registry().find(arc);
    if (it != registry().end() && it->second) {
        it->second->owns_subject = true;
    }
    return subject;
}

} // namespace helix::ui
