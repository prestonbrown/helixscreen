// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// Reaches TempGraphOverlay's private entry points for tests. The static XML
/// callbacks resolve the global overlay, so a case wires that object to its
/// own printer state and service and drives the same entry points a tap does.

#include "ui_overlay_temp_graph.h"

#include <optional>
#include <string>

class TempGraphOverlayTestAccess {
  public:
    static void set_deps(TempGraphOverlay& o, helix::PrinterState* state,
                         TemperatureService* service) {
        o.printer_state_ = state;
        o.temp_control_panel_ = service;
    }
    static void set_mode(TempGraphOverlay& o, TempGraphOverlay::Mode mode) {
        o.mode_ = mode;
    }
    /// The custom-entry keypad's opening click (the XML callback ignores its
    /// event, so a null one exercises the production path).
    static void custom_clicked(TempGraphOverlay& o) {
        (void)o;
        TempGraphOverlay::on_temp_graph_custom_clicked(nullptr);
    }
    /// The keypad's confirm, delivered the way ui_component_keypad does.
    static void keypad_confirm(TempGraphOverlay& o, float value) {
        TempGraphOverlay::keypad_value_cb(value, &o.keypad_ctx_);
    }
    /// A tap on the nozzle strip's preset button @p name ("preset_1" is the
    /// first material slot), sending @p deg as that button's target.
    static void preset_clicked(TempGraphOverlay& o, const char* name, int deg, lv_obj_t* parent) {
        const int idx = name[std::string(name).size() - 1] - '0';
        o.preset_data_[static_cast<size_t>(idx)] = {&o, deg};
        click_named(name, parent);
    }
    /// A tap on a button named @p name, whatever that name is.
    static void click_named(const char* name, lv_obj_t* parent) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_set_name(btn, name);
        lv_obj_add_event_cb(btn, TempGraphOverlay::on_temp_graph_preset_clicked, LV_EVENT_CLICKED,
                            nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_delete(btn);
    }
    static void discover(TempGraphOverlay& o) {
        o.discover_series();
    }
    /// {has_target} of the series named @p klipper_name, or nullopt if absent.
    static std::optional<bool> series_has_target(TempGraphOverlay& o,
                                                 const std::string& klipper_name) {
        for (const auto& s : o.series_)
            if (s.klipper_name == klipper_name)
                return s.has_target;
        return std::nullopt;
    }
};
