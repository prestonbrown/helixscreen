// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_panel_registry.h"

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
    stale_.fill(false);
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
    stale_[idx] = false;
    return displaced;
}

void PanelRegistry::mark_stale_except(int keep) {
    for (int i = 0; i < kCount; i++) {
        stale_[i] = i != keep && widgets_[i].get() != nullptr;
    }
}

bool PanelRegistry::is_stale(int idx) const {
    return in_range(idx) && stale_[idx];
}

void PanelRegistry::clear_stale(int idx) {
    if (in_range(idx)) {
        stale_[idx] = false;
    }
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
            } else {
                lv_obj_add_flag(widgets_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void PanelRegistry::set_instance(int idx, PanelBase* panel) {
    if (in_range(idx)) {
        instances_[idx] = panel;
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
    for (int i = 0; i < kCount; i++) {
        widgets_[i] = nullptr;
    }
    stale_.fill(false);
    clear_instances();
}

} // namespace helix::ui
