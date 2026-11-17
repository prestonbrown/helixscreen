// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_panel_registry.h"

#include "ui_panel_base.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
bool in_range(int idx) {
    return idx >= 0 && idx < PanelRegistry::kCount;
}
} // namespace

void PanelRegistry::set_widgets(lv_obj_t* const* panels) {
    for (int i = 0; i < kCount; i++) {
        widgets_[i] = panels[i];
    }
}

lv_obj_t* PanelRegistry::widget(int idx) const {
    return in_range(idx) ? widgets_[idx].get() : nullptr;
}

lv_obj_t* PanelRegistry::replace_widget(int idx, lv_obj_t* widget) {
    if (!in_range(idx)) {
        return nullptr;
    }
    lv_obj_t* displaced = widgets_[idx].get();
    widgets_[idx] = widget;
    apply_content_cap(idx);
    return displaced;
}

int PanelRegistry::index_of(const lv_obj_t* obj) const {
    if (!obj) {
        return -1; // an empty slot is not a panel
    }
    for (int i = 0; i < kCount; i++) {
        if (widgets_[i].get() == obj) {
            return i;
        }
    }
    return -1;
}

void PanelRegistry::show_only(int idx) {
    for (int i = 0; i < kCount; i++) {
        if (widgets_[i]) {
            if (i == idx) {
                lv_obj_remove_flag(widgets_[i], LV_OBJ_FLAG_HIDDEN);
                apply_content_cap(i);
            } else {
                lv_obj_add_flag(widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void PanelRegistry::set_instance(int idx, PanelBase* panel) {
    if (in_range(idx)) {
        instances_[idx] = panel;
        apply_content_cap(idx);
    }
}

void PanelRegistry::apply_content_cap(int idx) {
    lv_obj_t* panel = widget(idx);
    const PanelBase* inst = instance(idx);
    if (!panel || !inst || lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_t* container = lv_obj_get_parent(panel);
    if (container_.get() != container) {
        container_ = container;
        container_base_pad_right_ = content_cap_base_pad(container);
    }
    helix::apply_content_cap(container, container_base_pad_right_, content_capped(inst));
}

void PanelRegistry::reapply_content_caps() {
    for (int i = 0; i < kCount; i++) {
        apply_content_cap(i); // acts only for the panel shown
    }
}

PanelBase* PanelRegistry::instance(int idx) const {
    return in_range(idx) ? instances_[idx] : nullptr;
}

PanelId PanelRegistry::find(const PanelBase* panel) const {
    if (!panel) {
        return PanelId::Count;
    }
    for (int i = 0; i < kCount; i++) {
        if (instances_[i] == panel) {
            return static_cast<PanelId>(i);
        }
    }
    return PanelId::Count;
}

void PanelRegistry::set_deferred_builder(std::function<void(int)> builder) {
    deferred_builder_ = std::move(builder);
}

bool PanelRegistry::needs_build(int idx) const {
    return in_range(idx) && !widgets_[idx] && deferred_builder_;
}

void PanelRegistry::ensure_built(int idx) {
    if (!needs_build(idx))
        return; // out of range, already built, or desktop (all-resident, nothing deferred)
    if (building_)
        return; // re-entrancy guard (nav runs single-threaded; belt-and-suspenders)
    building_ = true;
    spdlog::info("[NavigationManager] Building deferred panel {} on first navigation", idx);
    deferred_builder_(idx); // creates + setup + registers widget/instance
    building_ = false;
}

void PanelRegistry::clear_instances() {
    instances_.fill(nullptr);
}

void PanelRegistry::reset() {
    container_ = nullptr;
    for (int i = 0; i < kCount; i++) {
        widgets_[i] = nullptr;
    }
    clear_instances();
}

} // namespace helix::ui
