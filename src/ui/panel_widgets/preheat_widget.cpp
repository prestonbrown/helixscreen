// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preheat_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_overlay_temp_graph.h"
#include "ui_split_button.h"

#include "app_globals.h"
#include "config.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_executor.h"
#include "material_settings_manager.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "preset_materials.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <set>
#include <utility>

// Preset material identity comes from helix::presets (backed by
// MaterialSettingsManager), NOT a local literal table. A hardcoded copy here is
// what made this widget show/apply PLA while the filament panel — which reads
// the user's configured presets — showed the material the user actually
// assigned to the slot.
static constexpr int PRESET_COUNT = helix::presets::PRESET_COUNT;

// Static instance for callback dispatch (one preheat widget at a time)
static helix::PreheatWidget* s_active_instance = nullptr;

namespace helix {

void register_preheat_widget() {
    register_widget_factory("preheat", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<PreheatWidget>(ps);
    });

    lv_xml_register_event_cb(nullptr, "preheat_widget_apply_cb", PreheatWidget::preheat_apply_cb);
    lv_xml_register_event_cb(nullptr, "preheat_widget_changed_cb",
                             PreheatWidget::preheat_changed_cb);
    lv_xml_register_event_cb(nullptr, "preheat_nozzle_tap_cb", PreheatWidget::nozzle_tap_cb);
    lv_xml_register_event_cb(nullptr, "preheat_bed_tap_cb", PreheatWidget::bed_tap_cb);
    lv_xml_register_event_cb(nullptr, "preheat_tool_target_cb", PreheatWidget::tool_target_cb);
}

PreheatWidget::PreheatWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

PreheatWidget::~PreheatWidget() {
    detach();
}

void PreheatWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config_.contains("material_index") && config_["material_index"].is_number_integer()) {
        selected_material_ = config_["material_index"].get<int>();
        if (selected_material_ < 0 || selected_material_ >= PRESET_COUNT) {
            selected_material_ = 0;
        }
    }
}

void PreheatWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_active_instance = this;

    split_btn_ = lv_obj_find_by_name(widget_obj_, "preheat_split_btn");
    if (split_btn_) {
        lv_obj_set_user_data(split_btn_, this);
        ui_split_button_set_selected(split_btn_, static_cast<uint32_t>(selected_material_));
        update_button_label();
    }

    // Clamp tool_target_ to prevent a stale index when the printer's extruder
    // count changes between sessions. PanelWidget instances are recycled across
    // home-panel rebuilds, so tool_target_ outlives any one attach.
    if (tool_target_ >= ToolState::instance().extruder_count()) {
        tool_target_ = -1; // Reset to "all" if stale
    }

    // Update label when widget resizes (e.g. breakpoint change, initial layout)
    lv_obj_add_event_cb(
        widget_obj_,
        [](lv_event_t* e) {
            (void)e;
            if (s_active_instance) {
                s_active_instance->update_button_label();
            }
        },
        LV_EVENT_SIZE_CHANGED, nullptr);

    // Tool target button - only visible when the printer has more than one
    // nozzle to aim a preheat at. Counts extruders, not tools: an AMS expands
    // ToolState's tool list to one entry per filament lane, and every lane on a
    // single-hotend printer shares the one heater, so "T0 / T1 / T2 / T3" would
    // offer four names for the same thing.
    tool_target_btn_ = lv_obj_find_by_name(widget_obj_, "preheat_tool_target");
    tool_target_label_ = lv_obj_find_by_name(widget_obj_, "tool_target_label");
    if (tool_target_btn_) {
        if (!ToolState::instance().has_multiple_extruders()) {
            lv_obj_add_flag(tool_target_btn_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(tool_target_btn_, LV_OBJ_FLAG_HIDDEN);
            update_tool_target_label();
        }
    }

    // Set user_data on temp tap rows for callback recovery
    lv_obj_t* nozzle_row = lv_obj_find_by_name(widget_obj_, "preheat_nozzle_row");
    if (nozzle_row)
        lv_obj_set_user_data(nozzle_row, this);
    lv_obj_t* bed_row = lv_obj_find_by_name(widget_obj_, "preheat_bed_row");
    if (bed_row)
        lv_obj_set_user_data(bed_row, this);

    // Tint the two heater glyphs. The numbers beside them already carry the
    // 4-state thermal color, so leaving the icons flat made them disagree
    // inside a single row. Bound from widget_obj_ — the tile holds exactly one
    // of each glyph name.
    nozzle_icon_binder_.bind(widget_obj_, printer_state_, helix::HeaterType::Nozzle);
    bed_icon_binder_.bind(widget_obj_, printer_state_, helix::HeaterType::Bed);

    // Observe heater targets to toggle preheat/cooldown mode
    using helix::ui::observe_int_sync;
    extruder_target_obs_ = observe_int_sync<PreheatWidget>(
        printer_state_.get_active_extruder_target_subject(), this,
        [](PreheatWidget* self, int target) {
            self->cached_extruder_target_ = target;
            self->update_heater_state();
        },
        printer_state_.get_subjects_lifetime());
    bed_target_obs_ = observe_int_sync<PreheatWidget>(
        printer_state_.get_bed_target_subject(bed_target_lifetime_), this,
        [](PreheatWidget* self, int target) {
            self->cached_bed_target_ = target;
            self->update_heater_state();
        },
        bed_target_lifetime_);

    spdlog::debug("[PreheatWidget] Attached (material={}, tool_target={})",
                  presets::name(selected_material_), tool_target_);
}

void PreheatWidget::detach() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }

    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();

    // Applying [L073]: subjects are alive during detach, use reset()
    extruder_target_obs_.reset();
    bed_target_lifetime_.reset();
    bed_target_obs_.reset();

    if (split_btn_) {
        lv_obj_set_user_data(split_btn_, nullptr);
        split_btn_ = nullptr;
    }

    tool_target_btn_ = nullptr;
    tool_target_label_ = nullptr;

    lv_obj_t* nozzle_row =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "preheat_nozzle_row") : nullptr;
    if (nozzle_row)
        lv_obj_set_user_data(nozzle_row, nullptr);
    lv_obj_t* bed_row = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "preheat_bed_row") : nullptr;
    if (bed_row)
        lv_obj_set_user_data(bed_row, nullptr);

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[PreheatWidget] Detached");
}

PreheatWidget::PreheatTargets PreheatWidget::targets_for_slot(int slot) {
    // Slot identity comes from the user's configured presets, and find_material()
    // folds in any MaterialSettingsManager temperature override for it.
    auto mat = filament::find_material(presets::name(slot));
    if (!mat) {
        return {};
    }
    return {mat->nozzle_recommended(), mat->bed_temp};
}

std::string PreheatWidget::label_for_slot(int slot, bool heaters_active, int32_t width_px) {
    if (heaters_active) {
        return "Cool Down";
    }

    const std::string material_name = presets::name(slot);
    const PreheatTargets t = targets_for_slot(slot);

    char label[64];
    if (width_px > 0 && width_px < 280) {
        // Narrow (2-col): just material name
        std::snprintf(label, sizeof(label), "%s", material_name.c_str());
    } else if (t.nozzle > 0 && t.bed > 0) {
        // Wide (3-col+): material + target temps
        std::snprintf(label, sizeof(label), "Preheat %s (%d/%d)", material_name.c_str(), t.nozzle,
                      t.bed);
    } else {
        std::snprintf(label, sizeof(label), "Preheat %s", material_name.c_str());
    }
    return label;
}

void PreheatWidget::update_button_label() {
    if (!split_btn_)
        return;

    const int32_t w = widget_obj_ ? lv_obj_get_width(widget_obj_) : 0;
    ui_split_button_set_text(split_btn_,
                             label_for_slot(selected_material_, heaters_active_, w).c_str());
}

void PreheatWidget::update_heater_state() {
    bool active = (cached_extruder_target_ > 0 || cached_bed_target_ > 0);
    if (active != heaters_active_) {
        heaters_active_ = active;
        update_button_label();
        spdlog::debug("[PreheatWidget] Heaters {} (extruder_target={}, bed_target={})",
                      active ? "active" : "inactive", cached_extruder_target_, cached_bed_target_);
    }
}

void PreheatWidget::update_tool_target_label() {
    if (!tool_target_label_)
        return;

    char label[16];
    if (tool_target_ == -1) {
        // The number of nozzles "All" will heat, which is what
        // collect_preheat_heaters() resolves to once lanes sharing a heater
        // collapse, not the lane count.
        std::snprintf(label, sizeof(label), "All (%d)", ToolState::instance().extruder_count());
    } else {
        std::snprintf(label, sizeof(label), "T%d", tool_target_);
    }
    lv_label_set_text(tool_target_label_, label);
}

void PreheatWidget::handle_selection_changed() {
    if (!split_btn_)
        return;

    uint32_t idx = ui_split_button_get_selected(split_btn_);
    if (idx < static_cast<uint32_t>(PRESET_COUNT)) {
        selected_material_ = static_cast<int>(idx);
        update_button_label();

        // Persist selection
        nlohmann::json new_config = config_;
        new_config["material_index"] = selected_material_;
        save_widget_config(new_config);
        config_ = new_config;

        spdlog::info("[PreheatWidget] Material changed to {}", presets::name(selected_material_));
    }
}

void PreheatWidget::set_temperatures(int nozzle, int bed) {
    auto* c = get_temperature_controller();
    if (!c)
        return;
    c->set_target(helix::HeaterType::Nozzle, static_cast<double>(nozzle),
                  {.toast = true, .on_success = [nozzle]() {
                       NOTIFY_SUCCESS(lv_tr("Nozzle target set to {}°C"), nozzle);
                   }});
    c->set_target(helix::HeaterType::Bed, static_cast<double>(bed),
                  {.toast = true, .on_success = [bed]() {
                       NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), bed);
                   }});
}

void PreheatWidget::handle_apply() {
    if (heaters_active_) {
        handle_cooldown();
        return;
    }

    const std::string material_name = presets::name(selected_material_);
    auto mat = filament::find_material(material_name);
    if (!mat) {
        spdlog::error("[PreheatWidget] Material '{}' not found", material_name);
        return;
    }

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[PreheatWidget] No IMoonrakerAPI available");
        return;
    }

    // Check for custom preheat macro
    const auto* ovr = MaterialSettingsManager::instance().get_override(material_name);
    if (ovr && ovr->preheat_macro && !ovr->preheat_macro->empty()) {
        bool handles_heating = ovr->macro_handles_heating.value_or(true);

        if (!handles_heating) {
            // Macro is additive — set temps first
            const PreheatTargets t = targets_for_slot(selected_material_);
            set_temperatures(t.nozzle, t.bed);
        }

        // Execute the macro
        MacroParamResult no_params;
        execute_macro_gcode(api, *ovr->preheat_macro, no_params, "[PreheatWidget]");

        spdlog::info("[PreheatWidget] Preheat {} via macro '{}' (handles_heating={})",
                     material_name, *ovr->preheat_macro, handles_heating);
        return;
    }

    // Default path: set temperatures. Same derivation the label uses, so the
    // caption and the applied targets can never disagree.
    const PreheatTargets t = targets_for_slot(selected_material_);
    int nozzle = t.nozzle;
    int bed = t.bed;

    // The multi path only earns its per-heater fan-out when the printer has
    // more than one nozzle. A single hotend behind an AMS takes the plain path,
    // which routes through TemperatureController's Nozzle heater.
    if (ToolState::instance().has_multiple_extruders()) {
        set_temperatures_multi(nozzle, bed);
    } else {
        set_temperatures(nozzle, bed);
    }

    spdlog::info("[PreheatWidget] Preheat {} applied (nozzle={}°C, bed={}°C, tool_target={})",
                 material_name, nozzle, bed, tool_target_);
}

void PreheatWidget::handle_cooldown() {
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[PreheatWidget] No IMoonrakerAPI available for cooldown");
        return;
    }

    // Use configured cooldown macro (user-overridable in settings.json)
    auto* cfg = Config::get_instance();
    MacroConfig default_cooldown{"Cool Down", "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\n"
                                              "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0"};
    auto cooldown = cfg ? cfg->get_macro("cooldown", default_cooldown) : default_cooldown;

    spdlog::info("[PreheatWidget] Cooldown requested - executing: {}", cooldown.gcode);
    api->execute_gcode(
        cooldown.gcode, []() { NOTIFY_SUCCESS(lv_tr("Heaters off")); },
        [](const MoonrakerError& error) {
            NOTIFY_ERROR(lv_tr("Failed to cool down: {}"), error.user_message());
        });
}

void PreheatWidget::handle_nozzle_tap() {
    spdlog::info("[PreheatWidget] Nozzle tapped - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void PreheatWidget::handle_bed_tap() {
    spdlog::info("[PreheatWidget] Bed tapped - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

// ============================================================================
// Multi-tool preheat logic
// ============================================================================

std::vector<std::string> PreheatWidget::collect_preheat_heaters(const std::vector<ToolInfo>& tools,
                                                                int tool_target) {
    std::vector<std::string> heaters;

    if (tool_target == -1) {
        // Heat every distinct heater, in tool order. Duplicates collapse:
        // set_ams_topology() expands the tool list to one entry per filament
        // lane, and every lane on a single-hotend printer resolves to the same
        // heater. Without the collapse a 4-lane AMS sent the same target four
        // times and the confirmation toast reported "all 4 tools".
        std::set<std::string> seen;
        for (const auto& tool : tools) {
            if (!tool.extruder_name && !tool.heater_name) {
                continue; // Skip tools with no valid heater
            }
            std::string heater = tool.effective_heater();
            if (seen.insert(heater).second) {
                heaters.push_back(std::move(heater));
            }
        }
    } else if (tool_target >= 0 && tool_target < static_cast<int>(tools.size())) {
        const auto& tool = tools[tool_target];
        if (tool.extruder_name || tool.heater_name) {
            heaters.push_back(tool.effective_heater());
        }
    }

    return heaters;
}

void PreheatWidget::set_temperatures_multi(int nozzle, int bed) {
    auto* c = get_temperature_controller();
    if (!c)
        return;
    const auto& tools = ToolState::instance().tools();
    auto heaters = collect_preheat_heaters(tools, tool_target_);

    for (const auto& heater : heaters) {
        spdlog::debug("[PreheatWidget] Setting {} to {}°C", heater, nozzle);
        c->set_target(
            heater, static_cast<double>(nozzle),
            {.toast = false,
             .on_success =
                 [heater, nozzle]() {
                     spdlog::info("[PreheatWidget] {} target set to {}°C", heater, nozzle);
                 },
             .on_error =
                 [heater](const MoonrakerError& error) {
                     NOTIFY_ERROR(lv_tr("Failed to set {} temp: {}"), heater, error.user_message());
                 }});
    }

    c->set_target(helix::HeaterType::Bed, static_cast<double>(bed),
                  {.toast = true, .on_success = [bed]() {
                       NOTIFY_SUCCESS(lv_tr("Bed target set to {}°C"), bed);
                   }});

    int tool_count = static_cast<int>(heaters.size());
    if (tool_target_ == -1) {
        NOTIFY_SUCCESS(lv_tr("Preheat: all {} tools + bed set"), tool_count);
    } else {
        NOTIFY_SUCCESS(lv_tr("Preheat: T{} + bed set"), tool_target_);
    }
}

void PreheatWidget::cycle_tool_target() {
    // Cycles over nozzles, not lanes: tool_target_ indexes ToolState's tool
    // list, whose first extruder_count() entries carry the distinct extruders.
    int tool_count = ToolState::instance().extruder_count();
    if (tool_count <= 1) {
        tool_target_ = -1; // Single nozzle, always "all"
        return;
    }

    if (tool_target_ == -1) {
        tool_target_ = 0;
    } else if (tool_target_ + 1 >= tool_count) {
        tool_target_ = -1;
    } else {
        tool_target_++;
    }

    spdlog::info("[PreheatWidget] Tool target cycled to {}", tool_target_);
}

void PreheatWidget::tool_target_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] tool_target_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->cycle_tool_target();
        s_active_instance->update_tool_target_label();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// Static callbacks
void PreheatWidget::preheat_apply_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] preheat_apply_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->record_interaction();
        s_active_instance->handle_apply();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::preheat_changed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] preheat_changed_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_selection_changed();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::nozzle_tap_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] nozzle_tap_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_nozzle_tap();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::bed_tap_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] bed_tap_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_bed_tap();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
