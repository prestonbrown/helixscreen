// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_widget.h"

#include "ui_overlay_temp_graph.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

void register_temp_graph_widget() {
    register_widget_factory(
        "temp_graph", [](const std::string& id) { return std::make_unique<TempGraphWidget>(id); });

    lv_xml_register_event_cb(nullptr, "on_temp_graph_widget_clicked",
                             TempGraphWidget::on_temp_graph_widget_clicked);
}

} // namespace helix

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

TempGraphWidget::TempGraphWidget(const std::string& instance_id) : instance_id_(instance_id) {
    spdlog::debug("[TempGraphWidget] Created instance '{}'", instance_id_);
}

TempGraphWidget::~TempGraphWidget() {
    if (widget_obj_) {
        detach();
    }
}

// ============================================================================
// PanelWidget interface
// ============================================================================

std::string TempGraphWidget::get_component_name() const {
    return "panel_widget_temp_graph";
}

void TempGraphWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    follow_overlay_ = config.is_object() && config.value("follow_overlay", false);
}

void TempGraphWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Store self-pointer for click callback routing
    lv_obj_set_user_data(widget_obj_, this);

    // Build default config if not yet configured
    if (config_.empty() || !config_.contains("sensors")) {
        build_default_config();
    }

    TempGraphControllerConfig ctrl_config;
    ctrl_config.point_count = 300; // 5-minute window at 1Hz (matches mini graph)
    ctrl_config.axis_size = "xs";
    ctrl_config.initial_features = features_for_size(current_colspan_, current_rowspan_);
    // Uses default TempGraphScaleParams (same as mini graph and overlay)
    ctrl_config.series = build_series_from_config();
    applied_visibility_signature_ = current_visibility_signature();

    controller_ = std::make_unique<TempGraphController>(widget_obj_, std::move(ctrl_config));

    // Match container bg to card (chart styling handled by controller)
    if (controller_ && controller_->is_valid()) {
        lv_obj_set_style_bg_color(widget_obj_, theme_manager_get_color("card_bg"), 0);
        lv_obj_set_style_bg_opa(widget_obj_, LV_OPA_COVER, 0);
    }

    spdlog::debug("[TempGraphWidget] Attached '{}' ({}x{})", instance_id_, current_colspan_,
                  current_rowspan_);
}

void TempGraphWidget::detach() {
    controller_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempGraphWidget] Detached '{}'", instance_id_);
}

void TempGraphWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                      int /*height_px*/) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;

    if (controller_) {
        uint32_t features = features_for_size(colspan, rowspan);
        controller_->set_features(features);
        spdlog::debug("[TempGraphWidget] '{}' resized to {}x{}, features=0x{:x}", instance_id_,
                      colspan, rowspan, features);
    }
}

void TempGraphWidget::on_activate() {
    // Follow mode: rebuild if the overlay's visibility snapshot drifted while
    // the user was on the full-screen graph.
    if (!follow_overlay_ || !widget_obj_ || !parent_screen_)
        return;
    if (current_visibility_signature() == applied_visibility_signature_)
        return;

    auto* saved_widget = widget_obj_;
    auto* saved_parent = parent_screen_;
    detach();
    attach(saved_widget, saved_parent);
}

void TempGraphWidget::on_deactivate() {}

bool TempGraphWidget::on_edit_configure() {
    auto* saved_widget = widget_obj_;
    auto* saved_parent = parent_screen_;

    config_modal_ = std::make_unique<TempGraphConfigModal>(
        config_, [this, saved_widget, saved_parent](const nlohmann::json& new_config) {
            config_ = new_config;
            save_widget_config(config_);
            detach();
            attach(saved_widget, saved_parent);
        });
    config_modal_->show(lv_screen_active());
    return true;
}

// ============================================================================
// Feature mapping
// ============================================================================

uint32_t TempGraphWidget::features_for_size(int colspan, int rowspan) {
    // Gradients always enabled — the draw callback auto-disables when >3 series visible
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_GRADIENTS;

    if (colspan >= 2 || rowspan >= 2) {
        // Medium: add target lines
        features |= TEMP_GRAPH_FEATURE_TARGET_LINES;
    }

    if (rowspan >= 2) {
        // Tall: add legend chips (enough vertical space to not obscure data)
        features |= TEMP_GRAPH_FEATURE_LEGEND;
    }

    if (rowspan >= 2) {
        // Tall: add Y-axis labels
        features |= TEMP_GRAPH_FEATURE_Y_AXIS;
    }

    if (colspan >= 3) {
        // Wide (3+): add X-axis time labels (too crowded at 2x)
        features |= TEMP_GRAPH_FEATURE_X_AXIS;
    }

    if (colspan >= 3 && rowspan >= 2) {
        // Extra large: add readouts
        features |= TEMP_GRAPH_FEATURE_READOUTS;
    }

    return features;
}

// ============================================================================
// Series config builder
// ============================================================================

std::vector<TempGraphSeriesSpec> TempGraphWidget::build_series_from_config() const {
    std::vector<TempGraphSeriesSpec> specs;
    if (!config_.contains("sensors"))
        return specs;

    // Follow mode overrides per-sensor `enabled` with the overlay's last
    // visibility set; falls back to config flags until the overlay is opened.
    auto snapshot = get_temp_graph_visibility_snapshot();
    const bool use_snapshot = follow_overlay_ && snapshot.has_value();

    int color_idx = 0;
    for (const auto& entry : config_["sensors"]) {
        if (!entry.contains("name"))
            continue;

        const std::string klipper_name = entry["name"].get<std::string>();
        bool enabled;
        if (use_snapshot) {
            enabled = std::find(snapshot->begin(), snapshot->end(), klipper_name) !=
                      snapshot->end();
        } else {
            enabled = entry.value("enabled", true);
        }
        if (!enabled)
            continue;

        TempGraphSeriesSpec spec;
        spec.klipper_name = klipper_name;
        if (entry.contains("color")) {
            spec.color = lv_color_hex(entry["color"].get<uint32_t>());
        } else {
            spec.color = TEMP_GRAPH_SERIES_COLORS[color_idx % TEMP_GRAPH_PALETTE_SIZE];
        }
        color_idx++;

        // Heaters have targets, sensors generally don't
        spec.show_target =
            (spec.klipper_name == "extruder" || spec.klipper_name.find("extruder") == 0 ||
             spec.klipper_name == "heater_bed" || spec.klipper_name == "chamber");
        specs.push_back(std::move(spec));
    }
    return specs;
}

std::string TempGraphWidget::current_visibility_signature() const {
    // Compute from the same data path that produces the series so a signature
    // change implies a real render delta — using the raw snapshot here would
    // flag drift even when names absent from this card's config get filtered
    // back out by build_series_from_config().
    auto specs = build_series_from_config();
    std::vector<std::string> names;
    names.reserve(specs.size());
    for (const auto& s : specs) {
        names.push_back(s.klipper_name);
    }
    std::sort(names.begin(), names.end());

    std::string sig;
    for (const auto& n : names) {
        sig += n;
        sig += ',';
    }
    return sig;
}

// ============================================================================
// Default config
// ============================================================================

void TempGraphWidget::build_default_config() {
    nlohmann::json sensors = nlohmann::json::array();

    // Always include extruder and bed
    sensors.push_back({{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}});
    sensors.push_back({{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}});

    // Check for chamber
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        sensors.push_back({{"name", "chamber"}, {"enabled", false}, {"color", 0xA3BE8C}});
    }

    // Add discovered auxiliary sensors (disabled by default)
    int color_idx = 3; // Start after nozzle/bed/chamber colors
    auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
    auto discovered = sensor_mgr.get_sensors_sorted();
    for (const auto& sensor : discovered) {
        if (!sensor.enabled)
            continue;
        uint32_t color_hex = 0;
        lv_color_t c = TEMP_GRAPH_SERIES_COLORS[color_idx % TEMP_GRAPH_PALETTE_SIZE];
        color_hex = (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
                    static_cast<uint32_t>(c.blue);
        color_idx++;

        sensors.push_back({
            {"name", sensor.klipper_name},
            {"enabled", false},
            {"color", color_hex},
        });
    }

    config_["sensors"] = sensors;
    spdlog::debug("[TempGraphWidget] Built default config with {} sensors for '{}'", sensors.size(),
                  instance_id_);
}

// ============================================================================
// TempGraphConfigModal
// ============================================================================

TempGraphWidget::TempGraphConfigModal::TempGraphConfigModal(const nlohmann::json& config,
                                                            SaveCallback on_save)
    : config_(config), on_save_(std::move(on_save)) {}

void TempGraphWidget::TempGraphConfigModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    populate_follow_toggle();
    populate_sensor_list();

    spdlog::debug("[TempGraphConfigModal] Opened with {} sensor rows", rows_.size());
}

void TempGraphWidget::TempGraphConfigModal::on_ok() {
    // Collect current toggle/color states back into config
    nlohmann::json sensors = nlohmann::json::array();
    for (auto& row : rows_) {
        // Read current switch state
        if (row.sw) {
            row.enabled = lv_obj_has_state(row.sw, LV_STATE_CHECKED);
        }

        lv_color_t c = TEMP_GRAPH_SERIES_COLORS[row.color_idx % TEMP_GRAPH_PALETTE_SIZE];
        uint32_t color_hex = (static_cast<uint32_t>(c.red) << 16) |
                             (static_cast<uint32_t>(c.green) << 8) | static_cast<uint32_t>(c.blue);

        sensors.push_back({
            {"name", row.name},
            {"enabled", row.enabled},
            {"color", color_hex},
        });
    }

    nlohmann::json new_config = config_;
    new_config["sensors"] = sensors;
    new_config["follow_overlay"] =
        follow_switch_ && lv_obj_has_state(follow_switch_, LV_STATE_CHECKED);

    if (on_save_) {
        on_save_(new_config);
    }

    spdlog::info("[TempGraphConfigModal] Saved config with {} sensors (follow={})",
                 sensors.size(), new_config["follow_overlay"].get<bool>());
    hide();
}

std::string
TempGraphWidget::TempGraphConfigModal::sensor_display_name(const std::string& klipper_name) {
    if (klipper_name == "extruder")
        return "Nozzle";
    if (klipper_name == "heater_bed")
        return "Bed";
    if (klipper_name == "chamber")
        return "Chamber";

    // Strip common prefixes for auxiliary sensors
    std::string display = klipper_name;
    const char* prefixes[] = {"temperature_sensor ", "temperature_fan "};
    for (const char* prefix : prefixes) {
        if (display.find(prefix) == 0) {
            display = display.substr(strlen(prefix));
            break;
        }
    }

    // Capitalize first letter
    if (!display.empty()) {
        display[0] = static_cast<char>(toupper(static_cast<unsigned char>(display[0])));
    }

    // Replace underscores with spaces
    for (auto& ch : display) {
        if (ch == '_')
            ch = ' ';
    }

    return display;
}

void TempGraphWidget::TempGraphConfigModal::populate_follow_toggle() {
    lv_obj_t* list = find_widget("sensor_list");
    if (!list)
        return;

    // Row: [title + hint stacked] [switch]
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);

    lv_obj_t* col = lv_obj_create(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(col);
    lv_label_set_text(title, lv_tr("Follow graph screen"));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t* hint = lv_label_create(col);
    lv_label_set_text(hint, lv_tr("Show whatever curves you last had visible on the full graph."));
    lv_obj_set_style_text_color(hint, theme_manager_get_color("text_muted"), 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    // Toggle switch
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 24);
    if (config_.is_object() && config_.value("follow_overlay", false))
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    follow_switch_ = sw;
}

void TempGraphWidget::TempGraphConfigModal::populate_sensor_list() {
    lv_obj_t* list = find_widget("sensor_list");
    if (!list) {
        spdlog::warn("[TempGraphConfigModal] sensor_list container not found");
        return;
    }

    rows_.clear();

    if (!config_.contains("sensors"))
        return;

    const auto& sensors = config_["sensors"];
    for (size_t i = 0; i < sensors.size(); ++i) {
        const auto& entry = sensors[i];
        if (!entry.contains("name"))
            continue;

        SensorRow row;
        row.name = entry["name"].get<std::string>();
        row.display = sensor_display_name(row.name);
        row.enabled = entry.value("enabled", true);

        // Find the matching color index from the palette
        if (entry.contains("color")) {
            uint32_t cfg_hex = entry["color"].get<uint32_t>();
            lv_color_t cfg_color = lv_color_hex(cfg_hex);
            row.color_idx = static_cast<int>(i) % TEMP_GRAPH_PALETTE_SIZE; // default
            for (int ci = 0; ci < TEMP_GRAPH_PALETTE_SIZE; ++ci) {
                if (TEMP_GRAPH_SERIES_COLORS[ci].red == cfg_color.red &&
                    TEMP_GRAPH_SERIES_COLORS[ci].green == cfg_color.green &&
                    TEMP_GRAPH_SERIES_COLORS[ci].blue == cfg_color.blue) {
                    row.color_idx = ci;
                    break;
                }
            }
        } else {
            row.color_idx = static_cast<int>(i) % TEMP_GRAPH_PALETTE_SIZE;
        }

        // Build the row container: [color swatch] [name label] [spacer] [switch]
        lv_obj_t* row_obj = lv_obj_create(list);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row_obj, 6, 0);
        lv_obj_set_style_pad_gap(row_obj, 12, 0);

        // Color swatch — small colored square, clickable to cycle
        lv_obj_t* swatch = lv_obj_create(row_obj);
        lv_obj_set_size(swatch, 24, 24);
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(
            swatch, TEMP_GRAPH_SERIES_COLORS[row.color_idx % TEMP_GRAPH_PALETTE_SIZE], 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        row.swatch = swatch;

        // Store row index in user data for the click callback
        lv_obj_set_user_data(swatch, reinterpret_cast<void*>(static_cast<uintptr_t>(rows_.size())));
        lv_obj_add_event_cb(swatch, color_swatch_clicked, LV_EVENT_CLICKED, this);

        // Sensor name label
        lv_obj_t* label = lv_label_create(row_obj);
        lv_label_set_text(label, row.display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);

        // Toggle switch
        lv_obj_t* sw = lv_switch_create(row_obj);
        lv_obj_set_size(sw, 44, 24);
        if (row.enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        row.sw = sw;

        rows_.push_back(std::move(row));
    }
}

void TempGraphWidget::TempGraphConfigModal::color_swatch_clicked(lv_event_t* e) {
    auto* modal = static_cast<TempGraphConfigModal*>(lv_event_get_user_data(e));
    auto* swatch = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(swatch)));

    if (!modal || idx >= modal->rows_.size())
        return;

    auto& row = modal->rows_[idx];
    row.color_idx = (row.color_idx + 1) % TEMP_GRAPH_PALETTE_SIZE;
    lv_obj_set_style_bg_color(swatch,
                              TEMP_GRAPH_SERIES_COLORS[row.color_idx % TEMP_GRAPH_PALETTE_SIZE], 0);

    spdlog::debug("[TempGraphConfigModal] Cycled '{}' to color index {}", row.name, row.color_idx);
}

// ============================================================================
// Click callback
// ============================================================================

void TempGraphWidget::on_temp_graph_widget_clicked(lv_event_t* e) {
    auto* self = panel_widget_from_event<TempGraphWidget>(e);
    if (!self || !self->parent_screen_)
        return;

    spdlog::debug("[TempGraphWidget] Clicked '{}', opening overlay", self->instance_id_);
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly, self->parent_screen_);
}
