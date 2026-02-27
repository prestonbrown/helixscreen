// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_manager.h"

#include "ui_ams_mini_status.h"

#include "config.h"
#include "observer_factory.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace helix {

PanelWidgetManager& PanelWidgetManager::instance() {
    static PanelWidgetManager instance;
    return instance;
}

void PanelWidgetManager::clear_shared_resources() {
    shared_resources_.clear();
}

void PanelWidgetManager::init_widget_subjects() {
    if (widget_subjects_initialized_) {
        return;
    }

    // Register all widget factories explicitly (avoids SIOF from file-scope statics)
    init_widget_registrations();

    for (const auto& def : get_all_widget_defs()) {
        if (def.init_subjects) {
            spdlog::debug("[PanelWidgetManager] Initializing subjects for widget '{}'", def.id);
            def.init_subjects();
        }
    }

    widget_subjects_initialized_ = true;
    spdlog::debug("[PanelWidgetManager] Widget subjects initialized");
}

void PanelWidgetManager::register_rebuild_callback(const std::string& panel_id,
                                                   RebuildCallback cb) {
    rebuild_callbacks_[panel_id] = std::move(cb);
}

void PanelWidgetManager::unregister_rebuild_callback(const std::string& panel_id) {
    rebuild_callbacks_.erase(panel_id);
}

void PanelWidgetManager::notify_config_changed(const std::string& panel_id) {
    auto it = rebuild_callbacks_.find(panel_id);
    if (it != rebuild_callbacks_.end()) {
        it->second();
    }
}

static PanelWidgetConfig& get_widget_config(const std::string& panel_id) {
    // Per-panel config instances cached by panel ID
    static std::unordered_map<std::string, PanelWidgetConfig> configs;
    auto it = configs.find(panel_id);
    if (it == configs.end()) {
        it = configs.emplace(panel_id, PanelWidgetConfig(panel_id, *Config::get_instance())).first;
    }
    // Always reload to pick up changes from settings overlay
    it->second.load();
    return it->second;
}

std::vector<std::unique_ptr<PanelWidget>>
PanelWidgetManager::populate_widgets(const std::string& panel_id, lv_obj_t* container) {
    if (!container) {
        spdlog::debug("[PanelWidgetManager] populate_widgets: null container for '{}'", panel_id);
        return {};
    }

    // Clear existing children (for repopulation)
    lv_obj_clean(container);

    auto& widget_config = get_widget_config(panel_id);

    // Resolved widget slot: holds the widget ID, resolved XML component name,
    // per-widget config, and optionally a pre-created PanelWidget instance.
    struct WidgetSlot {
        std::string widget_id;
        std::string component_name;
        nlohmann::json config;
        std::unique_ptr<PanelWidget> instance; // nullptr for pure-XML widgets
    };

    // Collect enabled + hardware-available widgets
    std::vector<WidgetSlot> enabled_widgets;
    for (const auto& entry : widget_config.entries()) {
        if (!entry.enabled) {
            continue;
        }

        // Check hardware gate — skip widgets whose hardware isn't present.
        // Gates are defined in PanelWidgetDef::hardware_gate_subject and checked
        // here instead of XML bind_flag_if_eq to avoid orphaned dividers.
        const auto* def = find_widget_def(entry.id);
        if (def && def->hardware_gate_subject) {
            lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
            if (gate && lv_subject_get_int(gate) == 0) {
                continue;
            }
        }

        WidgetSlot slot;
        slot.widget_id = entry.id;
        slot.config = entry.config;

        // If this widget has a factory, create the instance early so it can
        // resolve the XML component name (e.g. carousel vs stack mode).
        if (def && def->factory) {
            slot.instance = def->factory();
            if (slot.instance) {
                slot.instance->set_config(entry.config);
                slot.component_name = slot.instance->get_component_name();
            } else {
                slot.component_name = "panel_widget_" + entry.id;
            }
        } else {
            slot.component_name = "panel_widget_" + entry.id;
        }

        enabled_widgets.push_back(std::move(slot));
    }

    // If firmware_restart is NOT already in the list (user disabled it),
    // conditionally inject it as the LAST widget when Klipper is NOT READY.
    // This ensures the restart button is always reachable during shutdown, error,
    // or startup (e.g., stuck trying to connect to an MCU).
    bool has_firmware_restart = false;
    for (const auto& slot : enabled_widgets) {
        if (slot.widget_id == "firmware_restart") {
            has_firmware_restart = true;
            break;
        }
    }
    if (!has_firmware_restart) {
        lv_subject_t* klippy = lv_xml_get_subject(nullptr, "klippy_state");
        if (klippy) {
            int state = lv_subject_get_int(klippy);
            if (state != static_cast<int>(KlippyState::READY)) {
                const char* state_names[] = {"READY", "STARTUP", "SHUTDOWN", "ERROR"};
                const char* name = (state >= 0 && state <= 3) ? state_names[state] : "UNKNOWN";
                WidgetSlot slot;
                slot.widget_id = "firmware_restart";
                slot.component_name = "panel_widget_firmware_restart";
                enabled_widgets.push_back(std::move(slot));
                spdlog::debug("[PanelWidgetManager] Injected firmware_restart (Klipper {})", name);
            }
        }
    }

    if (enabled_widgets.empty()) {
        return {};
    }

    // Smart row layout:
    //   1-4 widgets  -> 1 row
    //   5-8 widgets  -> 2 rows, first row has 4
    //   9-10 widgets -> 2 rows, first row has 5
    size_t total = enabled_widgets.size();
    size_t first_row_count;
    if (total <= 4) {
        first_row_count = total; // Single row
    } else if (total <= 8) {
        first_row_count = 4; // 2 rows: 4 + remainder
    } else {
        first_row_count = 5; // 2 rows: 5 + remainder
    }

    std::vector<std::unique_ptr<PanelWidget>> result;

    auto create_row = [&](size_t start, size_t count) {
        lv_obj_t* row = lv_obj_create(container);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool first = true;
        for (size_t i = start; i < start + count && i < enabled_widgets.size(); ++i) {
            // Add divider between widgets (not before first)
            if (!first) {
                const char* div_attrs[] = {"height", "80%", nullptr, nullptr};
                lv_xml_create(row, "divider_vertical", div_attrs);
            }

            auto& slot = enabled_widgets[i];
            auto* widget =
                static_cast<lv_obj_t*>(lv_xml_create(row, slot.component_name.c_str(), nullptr));
            if (widget) {
                first = false;
                spdlog::debug("[PanelWidgetManager] Created widget: {} (component: {})",
                              slot.widget_id, slot.component_name);

                // Attach the pre-created PanelWidget instance if present
                if (slot.instance) {
                    slot.instance->attach(widget, lv_scr_act());
                    slot.instance->set_row_density(count);
                    result.push_back(std::move(slot.instance));
                }

                // Propagate row density to AMS mini status (pure XML widget, no PanelWidget)
                if (slot.widget_id == "ams") {
                    lv_obj_t* ams_child = lv_obj_get_child(widget, 0);
                    if (ams_child && ui_ams_mini_status_is_valid(ams_child)) {
                        ui_ams_mini_status_set_row_density(ams_child, static_cast<int>(count));
                    }
                }
            } else {
                spdlog::warn("[PanelWidgetManager] Failed to create widget: {} (component: {})",
                             slot.widget_id, slot.component_name);
            }
        }
    };

    // Create first row
    create_row(0, first_row_count);

    // Create second row if needed
    if (total > first_row_count) {
        create_row(first_row_count, total - first_row_count);
    }

    spdlog::debug("[PanelWidgetManager] Populated {} widgets ({} with factories) for '{}'", total,
                  result.size(), panel_id);

    return result;
}

void PanelWidgetManager::setup_gate_observers(const std::string& panel_id,
                                              RebuildCallback rebuild_cb) {
    using helix::ui::observe_int_sync;

    // Observers must be destroyed BEFORE timers — observer callbacks
    // capture &timer references into rebuild_timers_
    gate_observers_.erase(panel_id);
    rebuild_timers_.erase(panel_id);
    auto& observers = gate_observers_[panel_id];
    auto& timer = rebuild_timers_.emplace(panel_id, ui::CoalescedTimer(1)).first->second;

    // Collect unique gate subject names from the widget registry
    std::vector<const char*> gate_names;
    for (const auto& def : get_all_widget_defs()) {
        if (def.hardware_gate_subject) {
            // Avoid duplicates
            bool found = false;
            for (const auto* existing : gate_names) {
                if (std::strcmp(existing, def.hardware_gate_subject) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                gate_names.push_back(def.hardware_gate_subject);
            }
        }
    }

    // Also observe klippy_state for firmware_restart conditional injection
    gate_names.push_back("klippy_state");

    for (const auto* name : gate_names) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        if (!subject) {
            spdlog::trace("[PanelWidgetManager] Gate subject '{}' not registered yet", name);
            continue;
        }

        // Use observe_int_sync with PanelWidgetManager as the class template parameter.
        // The callback ignores the value and schedules a coalesced rebuild.
        // Multiple gate subjects changing in the same LVGL tick (common during
        // startup discovery) coalesce into a single rebuild instead of one each.
        observers.push_back(observe_int_sync<PanelWidgetManager>(
            subject, this, [&timer, rebuild_cb](PanelWidgetManager* /*self*/, int /*value*/) {
                timer.schedule(rebuild_cb);
            }));

        spdlog::trace("[PanelWidgetManager] Observing gate subject '{}' for panel '{}'", name,
                      panel_id);
    }

    spdlog::debug("[PanelWidgetManager] Set up {} gate observers for panel '{}'", observers.size(),
                  panel_id);
}

void PanelWidgetManager::clear_gate_observers(const std::string& panel_id) {
    auto it = gate_observers_.find(panel_id);
    if (it != gate_observers_.end()) {
        spdlog::debug("[PanelWidgetManager] Clearing {} gate observers for panel '{}'",
                      it->second.size(), panel_id);
        // Observers must be destroyed BEFORE timers — observer callbacks
        // capture &timer references into rebuild_timers_
        gate_observers_.erase(it);
    }
    rebuild_timers_.erase(panel_id);
}

} // namespace helix
