// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"
#include "ui_observer_guard.h"

#include "ams_error.h"
#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace helix {

class PrinterState;
class ToolSwitcherTestAccess;

class ToolSwitcherWidget : public PanelWidget {
  public:
    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "tool_switcher";
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_overlay_open() const override {
        return picker_.is_visible();
    }

    // Static instance tracker for callbacks from static event handlers
    static ToolSwitcherWidget* s_active_instance;

  private:
    friend class ToolSwitcherTestAccess;

    /// Single-select list of the printer's tools, raised by a tap on the compact
    /// tile. Picking a row issues the change; a tap outside it chooses nothing.
    class ToolPicker : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ToolPicker)

      public:
        explicit ToolPicker(ToolSwitcherWidget& owner) : owner_(owner) {}

      protected:
        const char* xml_component_name() const override {
            return "tool_switcher_picker";
        }
        void on_created(lv_obj_t* backdrop) override;

      private:
        ToolSwitcherWidget& owner_;
    };

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    ToolPicker picker_{*this};
    /// Compact-mode tool label. Held so the print gate can grey it without
    /// re-running the whole rebuild; nulled by rebuild_pills() and detach().
    lv_obj_t* compact_label_ = nullptr;

    // Physical size the widget was last granted, cached for observer paths
    // (tool_count_observer_, on_active_tool_changed()) that fire later and
    // need to know which layout is currently built without a size to read.
    int current_width_px_ = 0;
    int current_height_px_ = 0;

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;
    ObserverGuard print_state_observer_;

    std::vector<lv_obj_t*> pill_buttons_;

    // Grid layout descriptors for multi-row pill layout. LVGL stores these
    // pointers (no copy), so the backing arrays must outlive the layout.
    std::vector<int32_t> grid_col_dsc_;
    std::vector<int32_t> grid_row_dsc_;

    // tool_switcher_container, cached so the native SIZE_CHANGED hook (below)
    // can watch and clean up the same object across rebuild_pills()/
    // rebuild_compact() calls (they only replace its children, never the
    // container object itself).
    lv_obj_t* size_watch_container_ = nullptr;

    // size_watch_container_'s own size the last time the SIZE_CHANGED hook
    // drove a rebuild, cached so a re-fire with an unchanged size is a no-op
    // instead of a redundant rebuild. rebuild_pills()'s row-count decision is
    // a pure function of (w, h, tool list) here, so an unchanged size always
    // reproduces the same layout — this is also what keeps a same-size
    // re-fire from looping.
    int grid_settled_w_px_ = -1;
    int grid_settled_h_px_ = -1;

    // Re-entrancy guard: rebuild_pills()/rebuild_compact() call
    // lv_obj_update_layout()/lv_obj_set_layout() on size_watch_container_,
    // which can re-dispatch LV_EVENT_SIZE_CHANGED on it before this call
    // returns.
    bool in_grid_size_refresh_ = false;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void rebuild_pills();
    void rebuild_compact();
    void show_tool_picker();
    void handle_tool_selected(int tool_index);

    // Layout predicates over the cached granted size — shared by the readers
    // that fire from on_size_changed() itself and the ones that fire later
    // from observers (tool_count_observer_, on_active_tool_changed()).
    bool is_compact_size() const;
    bool is_narrow_tall_size() const;
    void on_active_tool_changed(int tool_index);

    /**
     * @brief Whether a running print blocks a tool change, and why.
     *
     * The same question every other filament surface asks — the filament panel,
     * the AMS sidebar and the AMS context menu all route through
     * helix::ui::print_blocks_filament_op(), the mirror of
     * AmsSubscriptionBackend::refuse_if_printing(). PRINTING always refuses;
     * PAUSED refuses only on a backend whose filament macro homes itself
     * (AD5X IFS). A tool change on a shared-toolhead AMS *is* a filament op, so
     * it gets the same gate rather than a policy of its own.
     *
     * @return AmsErrorHelper::print_active() when blocked, success() otherwise.
     */
    [[nodiscard]] AmsError tool_change_refusal() const;

    /// Grey the pills / compact label whenever tool_change_refusal() refuses.
    /// Called from both rebuild paths and from the print-state observer, so a
    /// recycled instance re-applies it on attach as well as on state change.
    void refresh_print_gating();

    /// Issue the change and report any refusal. Static so the confirmation
    /// modal's stateless event callback shares the one on_error path.
    static void dispatch_tool_change(int tool_index);

    // Native SIZE_CHANGED hook on size_watch_container_ (tool_switcher_container)
    // — same mechanism as UiClogMeter::resize_arc()/UiBufferMeter::resize(),
    // and watching the same object rebuild_pills() self-measures, not an
    // ancestor of it (see attach()'s comment for why that distinction
    // matters). PanelWidgetManager activates the grid layout only after
    // every widget's on_size_changed() has already run
    // (panel_widget_manager.cpp:901-903, deliberately — see #983), so the
    // container still reports its pre-grid, whole-content-box size the first
    // time rebuild_pills() measures it. This fires once the grid settles the
    // container to its real cell-derived size and re-drives the same
    // rebuild against the now-correct geometry.
    static void on_widget_size_changed(lv_event_t* e);
    void rebuild_for_settled_grid_size();

  public:
    static void tool_pill_cb(lv_event_t* e);
    static void tool_compact_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
