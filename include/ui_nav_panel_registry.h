// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_nav.h"
#include "ui_widget_ref.h"

#include <array>
#include <functional>

class PanelBase; // NAMESPACE_OK: PanelBase is a global-scope class (ui_panel_base.h)

namespace helix::ui {

/**
 * @brief The main panels of the navbar: their widgets, instances and lazy builder
 *
 * Panel widgets are held as WidgetRefs, so a slot clears itself when LVGL deletes
 * its widget. Indices are PanelId values; out-of-range indices read as empty.
 */
class PanelRegistry {
  public:
    static constexpr int kCount = static_cast<int>(PanelId::Count);

    /// Fill every slot from @p panels (kCount entries).
    void set_widgets(lv_obj_t* const* panels);

    /// The widget in slot @p idx, or nullptr.
    [[nodiscard]] lv_obj_t* widget(int idx) const;

    /// Put @p widget in slot @p idx and return the widget it displaced, or nullptr.
    /// The displaced widget is not freed. The slot is no longer stale.
    lv_obj_t* replace_widget(int idx, lv_obj_t* widget);

    /// Mark every built panel but @p keep stale: built for a layout that no
    /// longer holds, and due a rebuild before it is next shown.
    void mark_stale_except(int keep);

    [[nodiscard]] bool is_stale(int idx) const;

    void clear_stale(int idx);

    /// The slot holding @p obj, or -1.
    [[nodiscard]] int index_of(const lv_obj_t* obj) const;

    [[nodiscard]] bool is_main_panel(const lv_obj_t* obj) const {
        return index_of(obj) >= 0;
    }

    /// Show the panel in slot @p idx and hide every other panel. An index with
    /// no panel (-1 for none) hides them all.
    void show_only(int idx);

    void set_instance(int idx, PanelBase* panel);

    /// The C++ panel registered for slot @p idx, or nullptr.
    [[nodiscard]] PanelBase* instance(int idx) const;

    /// The slot @p panel is registered in, or PanelId::Count.
    [[nodiscard]] PanelId find(const PanelBase* panel) const;

    /// Register the builder for panels instantiated on first navigation (ESP32
    /// firmware, where only the home panel exists at boot). The builder must fill
    /// the slot and register the instance. Empty to disable.
    void set_deferred_builder(std::function<void(int)> builder);

    /// Whether ensure_built(idx) would build anything.
    [[nodiscard]] bool needs_build(int idx) const;

    /// Build panel @p idx now if its slot is empty and a builder is set. Guarded
    /// against re-entrancy.
    void ensure_built(int idx);

    /// Forget every instance; the widgets are left to their owners.
    void clear_instances();

    /// Forget widgets and instances; the builder stays.
    void reset();

  private:
    WidgetRef widgets_[kCount];
    std::array<PanelBase*, kCount> instances_ = {};
    std::array<bool, kCount> stale_ = {};
    std::function<void(int)> deferred_builder_;
    bool building_ = false;
};

} // namespace helix::ui
