// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_icon_binder.h"
#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace helix {

struct ToolInfo;
class PrinterState;
class PreheatWidgetTestAccess;

class PreheatWidget : public PanelWidget {
  public:
    explicit PreheatWidget(PrinterState& printer_state);
    ~PreheatWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "preheat";
    }

    /// Nozzle/bed targets this widget would apply for preset slot `slot`.
    /// Pure function of the user's configured preset materials + the filament
    /// database, so it is unit-testable without an attached widget.
    struct PreheatTargets {
        int nozzle = 0;
        int bed = 0;
    };
    static PreheatTargets targets_for_slot(int slot);

    /// The split-button caption for preset slot `slot`. `width_px` selects the
    /// narrow (name only) vs wide (name + temps) form; 0 means unknown width.
    /// Extracted from update_button_label() so a test can assert the rendered
    /// text follows a reassigned preset slot.
    static std::string label_for_slot(int slot, bool heaters_active, int32_t width_px);

  private:
    friend class PreheatWidgetTestAccess;

    PrinterState& printer_state_;
    nlohmann::json config_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* split_btn_ = nullptr;
    lv_obj_t* tool_target_btn_ = nullptr;
    lv_obj_t* tool_target_label_ = nullptr;

    int selected_material_ = 0; // index into helix::presets slots, not a fixed material
    bool heaters_active_ = false;

    // Thermal tint for the two heater glyphs. Each binder owns its own animator
    // and temperature observers, so the tile's numbers and icons agree.
    helix::ui::HeaterIconBinder nozzle_icon_binder_;
    helix::ui::HeaterIconBinder bed_icon_binder_;

    // Observers for heater target temperatures
    ObserverGuard extruder_target_obs_;
    SubjectLifetime bed_target_lifetime_;
    ObserverGuard bed_target_obs_;
    int cached_extruder_target_ = 0;
    int cached_bed_target_ = 0;

    int tool_target_ = -1; // -1 = all tools, 0..N = specific tool index

    void handle_apply();
    void handle_cooldown();
    void handle_selection_changed();
    void update_button_label();
    void update_heater_state();
    void update_tool_target_label();
    void set_temperatures(int nozzle, int bed);
    void set_temperatures_multi(int nozzle, int bed);
    void cycle_tool_target();

    void handle_nozzle_tap();
    void handle_bed_tap();

  public:
    /// Collect the heater names that should be heated for a given tool target.
    /// @param tools Vector of ToolInfo from ToolState::tools()
    /// @param tool_target -1 for all tools, 0..N for a specific tool index
    /// @return Vector of heater names (via effective_heater()), skipping tools with no heater
    static std::vector<std::string> collect_preheat_heaters(const std::vector<ToolInfo>& tools,
                                                            int tool_target);

    static void preheat_apply_cb(lv_event_t* e);
    static void preheat_changed_cb(lv_event_t* e);
    static void tool_target_cb(lv_event_t* e);
    static void nozzle_tap_cb(lv_event_t* e);
    static void bed_tap_cb(lv_event_t* e);
};

} // namespace helix
