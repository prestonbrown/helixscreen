// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_temp_graph.h"

#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_heater_config.h"
#include "ui_nav_manager.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_sensor_types.h"
#include "temperature_service.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Map overlay Mode to HeaterType for temperature control.
 * Returns false for GraphOnly mode (no heater controls).
 */
static bool mode_to_heater_type(TempGraphOverlay::Mode mode, helix::HeaterType& out) {
    switch (mode) {
    case TempGraphOverlay::Mode::Nozzle:
        out = helix::HeaterType::Nozzle;
        return true;
    case TempGraphOverlay::Mode::Bed:
        out = helix::HeaterType::Bed;
        return true;
    case TempGraphOverlay::Mode::Chamber:
        out = helix::HeaterType::Chamber;
        return true;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Global instance
// ─────────────────────────────────────────────────────────────────────────────

static std::unique_ptr<TempGraphOverlay> g_temp_graph_overlay;

TempGraphOverlay& get_global_temp_graph_overlay() {
    if (!g_temp_graph_overlay) {
        g_temp_graph_overlay = std::make_unique<TempGraphOverlay>();
        StaticPanelRegistry::instance().register_destroy("TempGraphOverlay",
                                                         []() { g_temp_graph_overlay.reset(); });
    }
    return *g_temp_graph_overlay;
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility snapshot (consumed by home graph card "follow" mode)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
struct VisibilitySnapshot {
    std::string printer_name; ///< Tagged so a printer switch invalidates the snapshot.
    std::vector<std::string> klipper_names;
};
std::optional<VisibilitySnapshot> s_visibility_snapshot;

std::string current_printer_name() {
    auto* subj = ::get_printer_state().get_active_printer_name_subject();
    if (!subj)
        return {};
    const char* s = lv_subject_get_string(subj);
    return s ? std::string(s) : std::string();
}
} // namespace

std::optional<std::vector<std::string>> get_temp_graph_visibility_snapshot() {
    if (!s_visibility_snapshot)
        return std::nullopt;
    if (s_visibility_snapshot->printer_name != current_printer_name())
        return std::nullopt;
    return s_visibility_snapshot->klipper_names;
}

namespace helix::test_access {
void set_temp_graph_visibility_snapshot(std::optional<std::vector<std::string>> snapshot) {
    if (snapshot) {
        s_visibility_snapshot = VisibilitySnapshot{current_printer_name(), std::move(*snapshot)};
    } else {
        s_visibility_snapshot.reset();
    }
}
} // namespace helix::test_access

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

TempGraphOverlay::TempGraphOverlay() = default;

TempGraphOverlay::~TempGraphOverlay() {
    controller_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// OverlayBase interface
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::init_subjects() {
    init_subjects_guarded([]() {});
}

void TempGraphOverlay::register_callbacks() {
    // Callbacks registered in xml_registration.cpp at startup (before XML parsing)
}

lv_obj_t* TempGraphOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "temp_graph_overlay")) {
        return nullptr;
    }

    chip_row_ = lv_obj_find_by_name(overlay_root_, "chip_row");
    graph_container_ = lv_obj_find_by_name(overlay_root_, "graph_container");
    nozzle_strip_ = lv_obj_find_by_name(overlay_root_, "nozzle_control_strip");
    bed_strip_ = lv_obj_find_by_name(overlay_root_, "bed_control_strip");
    chamber_strip_ = lv_obj_find_by_name(overlay_root_, "chamber_control_strip");
    extruder_selector_row_ = lv_obj_find_by_name(overlay_root_, "extruder_selector_row");
    graph_outer_ = lv_obj_find_by_name(overlay_root_, "graph_outer_container");

    return overlay_root_;
}

void TempGraphOverlay::on_activate() {
    OverlayBase::on_activate();

    // Resolve dependencies
    printer_state_ = &get_printer_state();
    temp_control_panel_ =
        helix::PanelWidgetManager::instance().shared_resource<TemperatureService>();

    // Discover series metadata (populates series_ with display info)
    discover_series();

    // Build TempGraphSeriesSpec vector from discovered series
    std::vector<helix::TempGraphSeriesSpec> specs;
    specs.reserve(series_.size());
    for (const auto& s : series_) {
        specs.push_back({s.klipper_name, s.color, s.has_target, s.display_name});
    }

    // Create controller (handles graph creation, observers, history, auto-range)
    if (graph_container_) {
        // Detach observers synchronously then defer memory deallocation.
        // Synchronous detach prevents use-after-free when deferred delete
        // runs after LVGL objects are freed (#726). Deferred delete avoids
        // re-entrant drain() corruption (#696).
        if (controller_) {
            controller_->detach();
            auto* old = controller_.release();
            lv_async_call([](void* p) { delete static_cast<helix::TempGraphController*>(p); }, old);
        }

        helix::TempGraphControllerConfig cfg;
        // Default point_count (1200 = 20 min) — overlay is the detailed full-screen view
        cfg.axis_size = "sm";
        cfg.initial_features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                               TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS |
                               TEMP_GRAPH_FEATURE_GRADIENTS | TEMP_GRAPH_FEATURE_TARGET_HISTORY;
        cfg.series = std::move(specs);
        controller_ = std::make_unique<helix::TempGraphController>(graph_container_, cfg);

        // Set cached_graph_bg for gradient rendering (chart sizing handled by controller)
        if (controller_->is_valid()) {
            controller_->graph()->cached_graph_bg = theme_manager_get_color("card_bg");
        }
    }

    // Map series IDs back from controller
    if (controller_ && controller_->is_valid()) {
        for (auto& s : series_) {
            s.series_id = controller_->series_id_for(s.klipper_name);
        }
    }

    // Apply mode-specific visibility and chips (every activation)
    apply_default_visibility();
    create_chips();
    configure_control_strip();

    spdlog::debug("[TempGraphOverlay] Activated with {} series, mode={}", series_.size(),
                  static_cast<int>(mode_));
}

void TempGraphOverlay::on_deactivate() {
    OverlayBase::on_deactivate();

    // Destroy controller (tears down observers, destroys graph)
    controller_.reset();

    // Clear series
    series_.clear();

    // Clear chip row
    if (chip_row_) {
        helix::ui::safe_clean_children(chip_row_);
    }

    spdlog::debug("[TempGraphOverlay] Deactivated");
}

void TempGraphOverlay::cleanup() {
    controller_.reset();
    series_.clear();
    OverlayBase::cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::open(Mode mode, lv_obj_t* parent_screen) {
    mode_ = mode;

    // Lazy create
    if (!cached_overlay_ && parent_screen) {
        if (!are_subjects_initialized()) {
            init_subjects();
        }

        cached_overlay_ = create(parent_screen);
        if (!cached_overlay_) {
            spdlog::error("[TempGraphOverlay] Failed to create overlay from XML");
            NOTIFY_ERROR(lv_tr("Failed to open temperature graph"));
            return;
        }

        NavigationManager::instance().register_overlay_instance(cached_overlay_, this, true);
        spdlog::info("[TempGraphOverlay] Overlay created");
    }

    if (cached_overlay_) {
        NavigationManager::instance().push_overlay(cached_overlay_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Series discovery
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::discover_series() {
    series_.clear();
    int color_idx = 0;

    if (!printer_state_)
        return;

    const auto& temp_state = printer_state_->temperature_state();

    // 1. Nozzle(s)
    const auto& extruders = temp_state.extruders();
    if (extruders.empty()) {
        // Fallback: always add at least one nozzle
        SeriesInfo s;
        s.display_name = lv_tr("Nozzle");
        s.heater_name = "extruder";
        s.klipper_name = "extruder";
        s.color = helix::TEMP_GRAPH_SERIES_COLORS[color_idx++ % helix::TEMP_GRAPH_PALETTE_SIZE];
        s.has_target = true;
        s.is_dynamic = false;
        series_.push_back(std::move(s));
    } else {
        // Sort extruders by name for consistent ordering
        std::vector<const helix::ExtruderInfo*> sorted_extruders;
        for (const auto& [name, info] : extruders) {
            sorted_extruders.push_back(&info);
        }
        std::sort(sorted_extruders.begin(), sorted_extruders.end(),
                  [](const auto* a, const auto* b) { return a->name < b->name; });

        for (const auto* ext : sorted_extruders) {
            SeriesInfo s;
            s.display_name = ext->display_name;
            s.heater_name = ext->name;
            s.klipper_name = ext->name;
            s.color = helix::TEMP_GRAPH_SERIES_COLORS[color_idx++ % helix::TEMP_GRAPH_PALETTE_SIZE];
            s.has_target = true;
            s.is_dynamic = (extruders.size() > 1); // Dynamic if multi-extruder
            series_.push_back(std::move(s));
        }
    }

    // 2. Bed
    {
        SeriesInfo s;
        s.display_name = lv_tr("Bed");
        s.heater_name = "heater_bed";
        s.klipper_name = "heater_bed";
        s.color = helix::TEMP_GRAPH_SERIES_COLORS[color_idx++ % helix::TEMP_GRAPH_PALETTE_SIZE];
        s.has_target = true;
        s.is_dynamic = false;
        series_.push_back(std::move(s));
    }

    // 3. Chamber (if present). Prefer the heater klipper name when a heater
    // exists (enables target control); fall back to the sensor klipper name
    // so sensor-only setups still graph a live series.
    {
        lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
        if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
            const std::string& heater = temp_state.chamber_heater_name();
            const std::string& sensor = temp_state.chamber_sensor_name();
            const std::string& klipper = !heater.empty() ? heater : sensor;
            if (!klipper.empty()) {
                SeriesInfo s;
                s.display_name = lv_tr("Chamber");
                s.heater_name = "chamber";
                s.klipper_name = klipper;
                s.color =
                    helix::TEMP_GRAPH_SERIES_COLORS[color_idx++ % helix::TEMP_GRAPH_PALETTE_SIZE];
                s.has_target = !heater.empty();
                s.is_dynamic = false;
                series_.push_back(std::move(s));
            }
        }
    }

    // 4. Custom sensors from TemperatureSensorManager
    auto& sensor_mgr = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = sensor_mgr.get_sensors_sorted();
    for (const auto& sensor : sensors) {
        // Skip sensors with chamber role (already handled above)
        if (sensor.role == helix::sensors::TemperatureSensorRole::CHAMBER)
            continue;
        // Skip diagnostic sensors — not user-facing for the default graph
        if (sensor.role == helix::sensors::TemperatureSensorRole::MCU ||
            sensor.role == helix::sensors::TemperatureSensorRole::HOST ||
            sensor.role == helix::sensors::TemperatureSensorRole::STEPPER_DRIVER)
            continue;
        // Skip disabled sensors
        if (!sensor.enabled)
            continue;

        SeriesInfo s;
        s.display_name = sensor.display_name;
        s.heater_name = sensor.klipper_name; // May not have history
        s.klipper_name = sensor.klipper_name;
        s.color = helix::TEMP_GRAPH_SERIES_COLORS[color_idx++ % helix::TEMP_GRAPH_PALETTE_SIZE];
        s.has_target = (sensor.type == helix::sensors::TemperatureSensorType::TEMPERATURE_FAN);
        s.is_dynamic = true;
        series_.push_back(std::move(s));
    }

    spdlog::debug("[TempGraphOverlay] Discovered {} series", series_.size());
}

void TempGraphOverlay::apply_default_visibility() {
    // GraphOnly defaults to the core heaters (nozzle(s), bed, chamber if present).
    // Heater-specific modes show only their primary sensor. Users can toggle
    // additional sensors on via the chip row.
    for (auto& s : series_) {
        switch (mode_) {
        case Mode::GraphOnly:
            s.visible = (s.heater_name.find("extruder") == 0) || (s.heater_name == "heater_bed") ||
                        (s.heater_name == "chamber");
            break;
        case Mode::Nozzle:
            // Match any extruder (extruder, extruder1, etc.)
            s.visible = (s.heater_name.find("extruder") == 0);
            break;
        case Mode::Bed:
            s.visible = (s.heater_name == "heater_bed");
            break;
        case Mode::Chamber:
            s.visible = (s.heater_name == "chamber");
            break;
        }
    }
    publish_visibility_snapshot();
}

void TempGraphOverlay::publish_visibility_snapshot() const {
    std::vector<std::string> visible;
    visible.reserve(series_.size());
    for (const auto& s : series_) {
        if (s.visible) {
            visible.push_back(s.klipper_name);
        }
    }
    s_visibility_snapshot = VisibilitySnapshot{current_printer_name(), std::move(visible)};
}

// ─────────────────────────────────────────────────────────────────────────────
// Chip creation
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::create_chips() {
    if (!chip_row_)
        return;

    helix::ui::safe_clean_children(chip_row_);

    for (size_t i = 0; i < series_.size(); ++i) {
        auto& s = series_[i];

        // Create a chip button: colored dot + label
        lv_obj_t* chip = lv_obj_create(chip_row_);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_style_pad_ver(chip, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(chip, theme_manager_get_color("elevated_bg"), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, theme_manager_get_color("border"), 0);
        lv_obj_set_style_shadow_width(chip, 4, 0);
        lv_obj_set_style_shadow_opa(chip, LV_OPA_20, 0);
        lv_obj_set_style_shadow_offset_y(chip, 2, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(chip, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

        // Color dot
        lv_obj_t* dot = lv_obj_create(chip);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, s.color, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(
            dot, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Label — strip redundant " Temperature" suffix for chip brevity
        std::string chip_label = s.display_name;
        const std::string suffix = " Temperature";
        if (chip_label.size() > suffix.size() &&
            chip_label.compare(chip_label.size() - suffix.size(), suffix.size(), suffix) == 0) {
            chip_label.erase(chip_label.size() - suffix.size());
        }
        lv_obj_t* label = lv_label_create(chip);
        lv_label_set_text(label, chip_label.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store index as user data for click handler
        lv_obj_set_user_data(chip, reinterpret_cast<void*>(i));
        // Exception: programmatic widget, can't use XML event_cb
        lv_obj_add_event_cb(chip, on_chip_clicked, LV_EVENT_CLICKED, this);

        s.chip = chip;

        // Apply initial visibility state (set by apply_default_visibility)
        update_chip_style(i);
        if (controller_ && controller_->is_valid() && s.series_id >= 0) {
            ui_temp_graph_show_series(controller_->graph(), s.series_id, s.visible);
        }
    }
}

void TempGraphOverlay::on_chip_clicked(lv_event_t* e) {
    auto* self = static_cast<TempGraphOverlay*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!self || !target)
        return;

    auto idx = reinterpret_cast<size_t>(lv_obj_get_user_data(target));
    if (idx < self->series_.size()) {
        self->toggle_series_visibility(idx);
    }
}

void TempGraphOverlay::toggle_series_visibility(size_t series_idx) {
    if (series_idx >= series_.size())
        return;
    auto& s = series_[series_idx];

    s.visible = !s.visible;
    if (controller_ && controller_->is_valid() && s.series_id >= 0) {
        auto* graph = controller_->graph();
        ui_temp_graph_show_series(graph, s.series_id, s.visible);
        if (s.has_target) {
            // Mirror chip state: target trace visibility tracks the actuals
            // visibility. The buffer's 0-sentinel handles off-period gaps via
            // the segmenter, so we don't gate on current target value — historical
            // positive-target samples remain visible when the heater is currently off.
            ui_temp_graph_show_target(graph, s.series_id, s.visible);
        }
    }
    update_chip_style(series_idx);
    publish_visibility_snapshot();

    spdlog::debug("[TempGraphOverlay] {} series '{}' (idx={})", s.visible ? "Showed" : "Hid",
                  s.display_name, series_idx);
}

void TempGraphOverlay::update_chip_style(size_t series_idx) {
    if (series_idx >= series_.size())
        return;
    auto& s = series_[series_idx];
    if (!s.chip)
        return;

    lv_opa_t opa = s.visible ? LV_OPA_COVER : LV_OPA_40;
    lv_obj_set_style_opa(s.chip, opa, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Control strip
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::configure_control_strip() {
    // Hide all strips first
    if (nozzle_strip_)
        lv_obj_add_flag(nozzle_strip_, LV_OBJ_FLAG_HIDDEN);
    if (bed_strip_)
        lv_obj_add_flag(bed_strip_, LV_OBJ_FLAG_HIDDEN);
    if (chamber_strip_)
        lv_obj_add_flag(chamber_strip_, LV_OBJ_FLAG_HIDDEN);

    if (mode_ == Mode::GraphOnly) {
        // Graph takes full width — no right column
        if (graph_outer_)
            lv_obj_set_width(graph_outer_, lv_pct(100));
        return;
    }

    // Graph gets 66% width with control column visible
    if (graph_outer_)
        lv_obj_set_width(graph_outer_, lv_pct(66));

    // Determine which strip to show and heater type
    helix::HeaterType heater_type;
    if (!mode_to_heater_type(mode_, heater_type))
        return;

    lv_obj_t* active_strip = nullptr;
    switch (mode_) {
    case Mode::Nozzle:
        active_strip = nozzle_strip_;
        break;
    case Mode::Bed:
        active_strip = bed_strip_;
        break;
    case Mode::Chamber:
        active_strip = chamber_strip_;
        break;
    default:
        break;
    }

    if (!active_strip)
        return;
    lv_obj_remove_flag(active_strip, LV_OBJ_FLAG_HIDDEN);

    // Get preset config from TemperatureService
    if (!temp_control_panel_)
        return;
    auto& heater = temp_control_panel_->heater(heater_type);

    // Configure preset values for the callback
    int preset_values[] = {heater.config.presets.off, heater.config.presets.pla,
                           heater.config.presets.petg, heater.config.presets.abs};

    // Store preset values indexed by name suffix for lookup in callback
    // (cannot use lv_obj_set_user_data — ui_button owns that slot, L069)
    for (int i = 0; i < MAX_PRESETS; ++i) {
        preset_data_[i] = {this, preset_values[i]};
    }

    // Chamber: clamp the overlay's own presets to the configured max_temp so a
    // preset above the ceiling (e.g. 60°C on a 50°C chamber) is hidden — the
    // overlay has its own preset grid separate from the chamber panel's.
    if (mode_ == Mode::Chamber) {
        // The controller owns the chamber's configured ceiling. Trigger its async
        // fetch (no-op once known), then clamp each preset against it.
        helix::TemperatureController* c = temp_control_panel_->controller();
        if (c)
            c->ensure_limits(helix::HeaterType::Chamber);
        static const char* kChamberPresetNames[MAX_PRESETS] = {
            "chamber_preset_off", "chamber_preset_1", "chamber_preset_2", "chamber_preset_3"};
        for (int i = 0; i < MAX_PRESETS; ++i) {
            lv_obj_t* btn = lv_obj_find_by_name(active_strip, kChamberPresetNames[i]);
            if (!btn) {
                continue;
            }
            // No controller → default to showing all presets.
            bool show = !c || c->preset_visible(heater_type, preset_values[i]);
            if (show) {
                lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Extruder selector: show only in nozzle mode with multiple extruders
    if (extruder_selector_row_) {
        auto& temp_state = printer_state_->temperature_state();
        if (mode_ == Mode::Nozzle && temp_state.extruder_count() > 1) {
            lv_obj_remove_flag(extruder_selector_row_, LV_OBJ_FLAG_HIDDEN);
            rebuild_extruder_selector();
        } else {
            lv_obj_add_flag(extruder_selector_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset / Custom callbacks
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::on_temp_graph_preset_clicked(lv_event_t* e) {
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!btn)
        return;

    // Derive preset index from button name (L069: ui_button owns user_data)
    const char* name = lv_obj_get_name(btn);
    if (!name)
        return;

    auto* self = &get_global_temp_graph_overlay();
    if (!self->temp_control_panel_)
        return;

    // Map button name suffix to preset index
    int preset_idx = -1;
    std::string name_str(name);
    if (name_str.find("preset_off") != std::string::npos)
        preset_idx = 0;
    else if (name_str.find("preset_1") != std::string::npos)
        preset_idx = 1;
    else if (name_str.find("preset_2") != std::string::npos)
        preset_idx = 2;
    else if (name_str.find("preset_3") != std::string::npos)
        preset_idx = 3;

    if (preset_idx < 0 || preset_idx >= MAX_PRESETS)
        return;
    auto& data = self->preset_data_[preset_idx];

    helix::HeaterType type;
    if (!mode_to_heater_type(self->mode_, type))
        return;

    spdlog::debug("[TempGraphOverlay] Preset clicked: {}°C for heater {}", data.preset_value,
                  static_cast<int>(type));

    // Update local state
    self->temp_control_panel_->set_heater(
        type, self->temp_control_panel_->heater(type).current,
        helix::ui::temperature::degrees_to_deci(data.preset_value));

    // Send via the controller — it resolves the klipper name internally (chamber
    // never sends a stale HEATER=chamber) and shows the standard error toast.
    if (helix::TemperatureController* c = self->temp_control_panel_->controller()) {
        c->set_target(type, static_cast<double>(data.preset_value), {.toast = true});
    }
}

void TempGraphOverlay::on_temp_graph_custom_clicked(lv_event_t* e) {
    (void)e;
    auto& overlay = get_global_temp_graph_overlay();
    if (!overlay.temp_control_panel_)
        return;

    helix::HeaterType type;
    if (!mode_to_heater_type(overlay.mode_, type))
        return;

    auto& heater = overlay.temp_control_panel_->heater(type);

    // The controller owns the effective keypad ceiling (configured max clamped to
    // the heater default). Trigger its async limit fetch (no-op once known /
    // non-chamber), then read the range from it. No controller → fall back to the
    // heater's static config range.
    float max_value = heater.config.keypad_range.max;
    if (helix::TemperatureController* c = overlay.temp_control_panel_->controller()) {
        c->ensure_limits(type);
        max_value = c->keypad_range(type).max;
    }

    // Store context for keypad callback (static because keypad outlives this scope).
    // No lifetime token needed — the overlay is a global singleton that outlives the keypad.
    static struct KeypadCtxStatic {
        TempGraphOverlay* overlay = nullptr;
        helix::HeaterType type{};
    } s_keypad_ctx;
    s_keypad_ctx.overlay = &overlay;
    s_keypad_ctx.type = type;

    // Seed the keypad. Chamber uses the effective target (heater target when
    // Heating, fan target when Maintaining) so the keypad pre-fills the value the
    // overlay already shows. The raw heater target reads 0 during M141 maintain
    // mode and would otherwise seed 0. Other heaters seed from their raw target.
    int seed_deci = heater.target;
    if (type == helix::HeaterType::Chamber && overlay.printer_state_) {
        if (auto* subj = overlay.printer_state_->get_chamber_effective_target_subject()) {
            seed_deci = lv_subject_get_int(subj);
        }
    }

    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(seed_deci)),
        .min_value = heater.config.keypad_range.min,
        .max_value = max_value,
        .title_label = heater.config.title,
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = keypad_value_cb,
        .user_data = &s_keypad_ctx,
    };

    ui_keypad_show(&keypad_config);
}

void TempGraphOverlay::keypad_value_cb(float value, void* user_data) {
    struct KeypadCtx {
        TempGraphOverlay* overlay;
        helix::HeaterType type;
    };
    auto* ctx = static_cast<KeypadCtx*>(user_data);
    if (!ctx || !ctx->overlay || !ctx->overlay->temp_control_panel_)
        return;

    int temp = static_cast<int>(value);

    spdlog::debug("[TempGraphOverlay] Custom temperature: {}°C for heater {}", temp,
                  static_cast<int>(ctx->type));

    // Send via the controller — resolves the klipper name internally and shows
    // the standard error toast on failure.
    if (helix::TemperatureController* c = ctx->overlay->temp_control_panel_->controller()) {
        c->set_target(ctx->type, static_cast<double>(temp), {.toast = true});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Extruder selector
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::rebuild_extruder_selector() {
    if (!extruder_selector_row_ || !printer_state_)
        return;

    // Called from lifetime_.defer in on_extruder_selected (#80). Use
    // safe_clean_children so the deletion is scheduled via lv_obj_delete_async
    // outside UpdateQueue::process_pending() — prevents event-list corruption
    // (#776).
    lv_obj_update_layout(extruder_selector_row_);
    helix::ui::safe_clean_children(extruder_selector_row_);

    auto& temp_state = printer_state_->temperature_state();
    const auto& extruders = temp_state.extruders();

    // Sort for consistent ordering
    std::vector<const helix::ExtruderInfo*> sorted;
    for (const auto& [name, info] : extruders) {
        sorted.push_back(&info);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->name < b->name; });

    // Touch-friendly pill sizing (esp. Tiny breakpoint / 480x320): the four tool
    // pills share the narrow 33% control column. Grow each pill to fill the row
    // equally and give it a real minimum touch height plus a body-size digit,
    // instead of shrinking to the glyph (was font_small + content-size, ~18px
    // tall and near-untappable). Mirrors tool_switcher_widget's pill sizing.
    const int32_t pill_min_h = theme_manager_get_spacing("button_height_sm");
    const int32_t pill_pad_ver = theme_manager_get_spacing("space_xxs");
    const int32_t pill_pad_hor = theme_manager_get_spacing("space_xs");
    // Rounded-rectangle radius + outline width, matching the preset buttons and
    // dropdowns (both are breakpoint-aware theme consts, same values used in XML
    // as #border_radius / #border_width).
    const int32_t pill_radius = theme_manager_get_spacing("border_radius");
    const int32_t pill_border_w = theme_manager_get_spacing("border_width");

    for (const auto* ext : sorted) {
        lv_obj_t* btn = lv_obj_create(extruder_selector_row_);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(btn, 1); // share the column width equally across pills
        lv_obj_set_style_min_height(btn, pill_min_h, 0);
        lv_obj_set_style_pad_ver(btn, pill_pad_ver, 0);
        lv_obj_set_style_pad_hor(btn, pill_pad_hor, 0);
        // Rounded rectangle rather than a full-circle pill.
        lv_obj_set_style_radius(btn, pill_radius, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        bool is_active = (ext->name == active_extruder_name_);
        lv_obj_set_style_bg_color(
            btn,
            is_active ? theme_manager_get_color("primary") : theme_manager_get_color("card_bg"), 0);
        // Inactive pills get an outline like the "Off" ghost button; the active
        // pill stays a solid primary fill with no border.
        lv_obj_set_style_border_width(btn, is_active ? 0 : pill_border_w, 0);
        if (!is_active) {
            lv_obj_set_style_border_color(btn, theme_manager_get_color("border"), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        }

        lv_obj_t* label = lv_label_create(btn);
        // Compact pill label: show only the trailing number from "Nozzle N".
        // Saves horizontal space so 4+ pills fit without clipping; the full
        // "Nozzle N" wording still appears in status messages and the heater
        // icon caption.
        auto space_pos = ext->display_name.find_last_of(' ');
        std::string pill_text = (space_pos != std::string::npos)
                                    ? ext->display_name.substr(space_pos + 1)
                                    : ext->display_name;
        lv_label_set_text(label, pill_text.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
        lv_obj_set_style_text_color(
            label,
            is_active ? theme_manager_get_color("on_primary") : theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_center(label); // center the digit within the grown pill

        // Store name as obj name for lookup in callback
        lv_obj_set_name(btn, ext->name.c_str());
        // Exception: programmatic widget, can't use XML event_cb
        lv_obj_add_event_cb(btn, on_extruder_selected, LV_EVENT_CLICKED, this);
    }
}

void TempGraphOverlay::on_extruder_selected(lv_event_t* e) {
    auto* self = static_cast<TempGraphOverlay*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!self || !target)
        return;

    const char* name = lv_obj_get_name(target);
    if (!name)
        return;

    self->active_extruder_name_ = name;
    self->printer_state_->set_active_extruder(name);

    // Defer rebuild (#80) AND use safe_clean_children in rebuild_extruder_selector
    // (#776): lifetime_.defer moves work off the click stack so we don't delete
    // the clicked chip mid-event; rebuild_extruder_selector's safe_clean_children
    // then escapes UpdateQueue::process_pending() so sync deletion can't corrupt
    // LVGL's event linked list.
    self->lifetime_.defer("rebuild_extruder_selector",
                          [self]() { self->rebuild_extruder_selector(); });

    spdlog::debug("[TempGraphOverlay] Selected extruder: {}", name);
}
