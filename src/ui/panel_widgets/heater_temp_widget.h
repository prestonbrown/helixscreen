// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_icon_binder.h"
#include "ui_overlay_temp_graph.h"

#include "async_lifetime_guard.h"
#include "heater_type.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

class TemperatureService;

namespace helix {
class PrinterState;
}

namespace helix {

// A single-heater temperature tile for the home panel (nozzle, bed, or chamber).
//
// All three heaters render the same 1x1 tile — icon + current/target readout, a
// heating animation driven by current-vs-target, and a tap that opens the temp
// graph overlay focused on that heater. They differ only in *data*: which
// subjects to observe, which icon/button names the XML uses, and which overlay
// mode to open. That difference is captured by Config, so there is exactly one
// implementation instead of three near-identical copies.
class HeaterTempWidget : public TiledPanelWidget {
  public:
    // Resolves the per-heater subjects from PrinterState. Captureless lambdas
    // convert to these plain function pointers, so each Config is a trivial,
    // copyable value (no std::function, no heap).
    using SubjectGetter = lv_subject_t* (*)(PrinterState&);

    struct Config {
        const char* widget_id;   // PanelWidget id() (e.g. "bed_temperature")
        const char* button_name; // ui_button name in the XML component
        const char* icon_name;   // icon glyph name (exercised by tests; icon lookup
                                 // itself now goes through HeaterIconBinder's own
                                 // default_icon_name(heater))
        const char* log_tag;     // spdlog prefix, e.g. "[BedTemperatureWidget]"
        TempGraphOverlay::Mode mode;
        SubjectGetter temp_getter;   // current temperature subject (exercised by tests)
        SubjectGetter target_getter; // target temperature subject (exercised by tests)
        HeaterType heater;           // which heater's subjects the icon binder observes
    };

    HeaterTempWidget(PrinterState& printer_state, TemperatureService* temp_panel,
                     const Config& config);
    ~HeaterTempWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return cfg_.widget_id;
    }

    // Shared XML event callback. All three heater components register their
    // distinct callback names (temp_clicked_cb / bed_temp_clicked_cb /
    // chamber_temp_clicked_cb) against this one function — the bound widget is
    // recovered from per-callback user_data, so it knows which heater it is.
    static void clicked_cb(lv_event_t* e);

  private:
    PrinterState& printer_state_;
    TemperatureService* temp_control_panel_;
    Config cfg_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* temp_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    helix::ui::HeaterIconBinder icon_binder_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void handle_temp_clicked();
};

// Per-heater configs — single source of truth shared by the widget factories
// (panel_widget_registry) and unit tests.
const HeaterTempWidget::Config& nozzle_temp_config();
const HeaterTempWidget::Config& bed_temp_config();
const HeaterTempWidget::Config& chamber_temp_config();

} // namespace helix
