// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_icon_binder.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "heater_type.h"
#include "panel_widget.h"

#include <memory>

class TemperatureService;

namespace helix {
class PrinterState;

class TempStackWidget : public PanelWidget {
  public:
    TempStackWidget(PrinterState& printer_state, TemperatureService* temp_panel);
    ~TempStackWidget() override;

    void set_config(const nlohmann::json& config) override;
    std::string get_component_name() const override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    const char* id() const override {
        return "temp_stack";
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

  private:
    PrinterState& printer_state_;
    TemperatureService* temp_control_panel_;
    nlohmann::json config_;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;

    // Heating icon binders (nozzle/bed/chamber). Each owns its own temperature
    // observers, bound from the narrowest applicable root: widget_obj_ for the
    // stack layout (one row per heater), or the carousel page's own root for
    // carousel mode (stack and carousel are mutually exclusive subtrees under
    // widget_obj_ — see is_carousel_mode() / attach()).
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;
    helix::ui::HeaterIconBinder chamber_icon_binder_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Kept for future deferred-callback use (e.g.
    // long-press handling); no longer feeds icon-animator observers directly —
    // those now live inside the HeaterIconBinder members above. Bundle
    // AX3CKAKB (k1 v0.99.52).
    helix::AsyncLifetimeGuard lifetime_;

    bool is_carousel_mode() const;
    void attach_stack(lv_obj_t* widget_obj);
    void attach_carousel(lv_obj_t* widget_obj);

    void handle_nozzle_clicked();
    void handle_bed_clicked();
    void handle_chamber_clicked();

  public:
    // Public for early XML callback registration (before attach)
    static void temp_stack_nozzle_cb(lv_event_t* e);
    static void temp_stack_bed_cb(lv_event_t* e);
    static void temp_stack_chamber_cb(lv_event_t* e);

    // Carousel page click callback
    static void temp_carousel_page_cb(lv_event_t* e);
};

} // namespace helix
