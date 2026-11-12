// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "heater_temp_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_service.h"

#include <spdlog/spdlog.h>

using namespace helix;

namespace helix {

const HeaterTempWidget::Config& nozzle_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "temperature",
        "nozzle_icon_glyph",
        "[NozzleTempWidget]",
        TempGraphOverlay::Mode::Nozzle,
        [](PrinterState& p) { return p.temperature_state().get_active_extruder_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_active_extruder_target_subject(); },
        HeaterType::Nozzle};
    return cfg;
}

const HeaterTempWidget::Config& bed_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "bed_temperature",
        "bed_icon_glyph",
        "[BedTempWidget]",
        TempGraphOverlay::Mode::Bed,
        [](PrinterState& p) { return p.temperature_state().get_bed_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_bed_target_subject(); },
        HeaterType::Bed};
    return cfg;
}

const HeaterTempWidget::Config& chamber_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "chamber_temperature",
        "chamber_icon_glyph",
        "[ChamberTempWidget]",
        TempGraphOverlay::Mode::Chamber,
        [](PrinterState& p) { return p.temperature_state().get_chamber_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_chamber_target_subject(); },
        HeaterType::Chamber};
    return cfg;
}

// Factory + XML-callback registration. Each heater registers its own XML
// callback name against the shared HeaterTempWidget::clicked_cb.
static void register_heater_temp_widget(const char* widget_id, const char* xml_callback,
                                        const HeaterTempWidget::Config& (*config)()) {
    register_widget_factory(widget_id, [config](const std::string& instance_id) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TemperatureService>();
        return std::make_unique<HeaterTempWidget>(ps, tcp, config(), instance_id);
    });
    lv_xml_register_event_cb(nullptr, xml_callback, HeaterTempWidget::clicked_cb);
}

void register_temperature_widget() {
    register_heater_temp_widget("temperature", "temp_clicked_cb", nozzle_temp_config);
}

void register_bed_temperature_widget() {
    register_heater_temp_widget("bed_temperature", "bed_temp_clicked_cb", bed_temp_config);
}

void register_chamber_temperature_widget() {
    register_heater_temp_widget("chamber_temperature", "chamber_temp_clicked_cb",
                                chamber_temp_config);
}

std::string heater_full_page_function(const HeaterTempWidget::Config& config) {
    return std::string("temp:") + config.widget_id;
}

} // namespace helix

// The three heaters draw the same shape, so one worst-case budget covers them.
// temp_display draws the unit as its own label beside the value, so the budget
// carries it too: a value measured without the unit is narrower than the row
// that renders. Below 100 the reading carries a decimal, which makes "88.8" the
// widest current. No label is drawn. The nozzle glyph carries a tool digit
// whenever a second tool appears, which can happen after this tile was sized,
// so it is always budgeted. The glyph pulses while heating, so it is never
// scaled.
HeaterTempWidget::HeaterTempWidget(PrinterState& printer_state, TemperatureService* temp_panel,
                                   const Config& config, const std::string& instance_id)
    : TiledPanelWidget(instance_id.empty() ? std::string(config.widget_id) : instance_id,
                       TileSizing::Content{"88.8 / 888\u00B0C", "88.8\u00B0C", "", true,
                                           config.heater == HeaterType::Nozzle ? "8" : "",
                                           /*label_always_drawn=*/false, TileSizing::IconBox::Glyph,
                                           /*icon_animates=*/true}),
      printer_state_(printer_state), temp_control_panel_(temp_panel), cfg_(config),
      instance_id_(instance_id.empty() ? std::string(config.widget_id) : instance_id),
      full_page_(
          sizing_, heater_full_page_function(config),
          []() -> OverlayBase& { return get_global_temp_graph_overlay(); },
          [mode = config.mode](OverlayBase&, lv_obj_t* container) {
              return get_global_temp_graph_overlay().embed_in(mode, container);
          }) {}

HeaterTempWidget::~HeaterTempWidget() {
    detach();
}

void HeaterTempWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Bind the heating icon animator. The binder owns its own temperature
    // observers, so this widget keeps none of its own just to feed an icon tint.
    icon_binder_.bind(widget_obj_, printer_state_, cfg_.heater);
    full_page_.attach(widget_obj);
    install_delete_hook(widget_obj);

    spdlog::debug("{} Attached", cfg_.log_tag);
}

void HeaterTempWidget::detach() {
    lifetime_.invalidate();
    uninstall_delete_hook();
    full_page_.detach();
    icon_binder_.unbind();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("{} Detached", cfg_.log_tag);
}

void HeaterTempWidget::on_hooked_root_deleted() {
    full_page_.on_tile_deleted();
}

void HeaterTempWidget::on_activate() {
    full_page_.activate();
}

void HeaterTempWidget::on_deactivate() {
    full_page_.deactivate();
}

void HeaterTempWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    TiledPanelWidget::on_size_changed(colspan, rowspan, width_px, height_px);
    full_page_.set_placement(fills_extra_page(), page_index());
}

void HeaterTempWidget::handle_temp_clicked() {
    // A full page already shows the controls the tap would open.
    if (full_page_.is_full_page()) {
        return;
    }
    if (jump_to_full_page(full_page_.function())) {
        return;
    }
    spdlog::info("{} Temperature icon clicked - opening temp graph overlay", cfg_.log_tag);
    get_global_temp_graph_overlay().open(cfg_.mode, parent_screen_);
}

void HeaterTempWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HeaterTempWidget] clicked_cb");
    auto* self = panel_widget_from_event<HeaterTempWidget>(e);
    if (self) {
        self->record_interaction();
        self->handle_temp_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
