// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_temp_graph.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_heater_config.h"
#include "ui_nav.h"
#include "ui_next_tick.h"
#include "ui_swatch.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_manager.h"
#include "preset_materials.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "static_panel_registry.h"
#include "target_tool.h"
#include "temp_graph_tooltip.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "temperature_sensor_types.h"
#include "temperature_service.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>
#include <utility>

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
// Visibility snapshot (consumed by home graph card "follow" mode)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
struct VisibilitySnapshot {
    std::string printer_name; ///< Tagged so a printer switch invalidates the snapshot.
    std::vector<std::string> klipper_names;
};
std::optional<VisibilitySnapshot> s_visibility_snapshot;

/**
 * The user's chip toggles, remembered across a deactivate/reactivate cycle.
 *
 * This cannot live on the overlay: on_deactivate() clears series_ (and
 * suspend_active() calls on_deactivate() when the app backgrounds), so by the
 * time on_activate() runs again there is no per-series state left to read a
 * previous visibility off. Tagged with the printer AND the mode the toggles
 * were made in — a different printer, or opening the overlay as the Bed view
 * after toggling chips in the Nozzle view, falls back to that mode's defaults
 * rather than restoring a selection that was never about this view.
 */
struct VisibilityOverride {
    std::string printer_name;
    int mode = -1;
    std::vector<std::pair<std::string, bool>> per_series;
};
std::optional<VisibilityOverride> s_visibility_override;

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

TempGraphOverlay::~TempGraphOverlay() {
    controller_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// OverlayBase interface
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::init_subjects() {
    {
        // mode_ is set by open() before this first runs; seeding the subject
        // with the current value avoids a transient wrong-mode frame between
        // XML create and the first lv_subject_set_int in open().
        UI_MANAGED_SUBJECT_INT(mode_subject_, static_cast<int>(mode_), "temp_graph_mode",
                               subjects_);
    }
}

void TempGraphOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_temp_graph_preset_clicked", on_temp_graph_preset_clicked},
        {"on_temp_graph_custom_clicked", on_temp_graph_custom_clicked},
    });
}

lv_obj_t* TempGraphOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    chip_row_ = helix::ui::find_required(overlay_root_, "chip_row", get_name());
    graph_container_ = helix::ui::find_required(overlay_root_, "graph_container", get_name());
    nozzle_strip_ = helix::ui::find_required(overlay_root_, "nozzle_control_strip", get_name());
    bed_strip_ = helix::ui::find_required(overlay_root_, "bed_control_strip", get_name());
    chamber_strip_ = helix::ui::find_required(overlay_root_, "chamber_control_strip", get_name());

    return overlay_root_;
}

void TempGraphOverlay::on_activate() {
    OverlayBase::on_activate();

    // Resolve dependencies
    printer_state_ = &get_printer_state();
    temp_control_panel_ =
        helix::PanelWidgetManager::instance().shared_resource<TemperatureService>();

    // Thermal tint for the heater glyphs, one per control strip plus the
    // chamber card's second header (compact beside diagnostics, xl without).
    // Each binder owns its own observers, so this needs no hook into the
    // graph/series machinery below.
    nozzle_icon_binder_.bind(overlay_root_, *printer_state_, helix::HeaterType::Nozzle);
    bed_icon_binder_.bind(overlay_root_, *printer_state_, helix::HeaterType::Bed);
    chamber_icon_binder_.bind(overlay_root_, *printer_state_, helix::HeaterType::Chamber);
    chamber_hero_icon_binder_.bind(overlay_root_, *printer_state_, helix::HeaterType::Chamber,
                                   "chamber_hero_icon_glyph");

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
            helix::ui::run_next_tick([old]() { delete old; });
        }

        helix::TempGraphControllerConfig cfg;
        // Default point_count (UI_TEMP_GRAPH_DEFAULT_POINTS = 400 points at one
        // per 3 s = a 20 min window) — the overlay is the detailed full-screen view
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

    // Tap-to-caption is opt-in per graph instance (temp_graph_tooltip.h): only
    // this full-screen overlay enables it. The home-panel mini graph already
    // uses a tap to OPEN this overlay (temp_graph_widget.cpp), so enabling the
    // tooltip there would collide with that gesture.
    if (controller_ && controller_->is_valid()) {
        ui_temp_graph_set_tooltip_enabled(controller_->graph(), true);
    }

    // Map series IDs back from controller
    if (controller_ && controller_->is_valid()) {
        for (auto& s : series_) {
            s.series_id = controller_->series_id_for(s.klipper_name);
        }
    }

    // Visibility: lay down this mode's defaults, then re-apply the user's chip
    // toggles over the top when they were made on this printer in this mode.
    // Order matters — the defaults establish a baseline for any series that
    // appeared since the toggles were recorded (a sensor discovered late, a
    // second tool), and the override only touches names it actually knows.
    apply_default_visibility();
    if (s_visibility_override && s_visibility_override->mode == static_cast<int>(mode_) &&
        s_visibility_override->printer_name == current_printer_name()) {
        for (auto& s : series_) {
            for (const auto& [name, vis] : s_visibility_override->per_series) {
                if (name == s.klipper_name) {
                    s.visible = vis;
                    break;
                }
            }
        }
        publish_visibility_snapshot();
        spdlog::debug("[TempGraphOverlay] Restored {} saved chip toggles",
                      s_visibility_override->per_series.size());
    }
    create_chips();
    configure_control_strip();

    spdlog::debug("[TempGraphOverlay] Activated with {} series, mode={}", series_.size(),
                  static_cast<int>(mode_));
}

void TempGraphOverlay::on_deactivating(DeactivateReason) {
    // Mirror the bind() calls in on_activate() — the icon widgets themselves
    // survive (cached_overlay_ persists across pushes), but printer_state_ and
    // the underlying subject lifetimes are only guaranteed valid while active.
    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();
    chamber_icon_binder_.unbind();
    chamber_hero_icon_binder_.unbind();

    // Clear any pinned caption before the controller (and its graph) are torn
    // down below.
    if (controller_ && controller_->is_valid()) {
        helix::temp_graph_internal::temp_graph_tooltip_clear(controller_->graph());
    }

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

void TempGraphOverlay::on_ui_destroyed() {
    chip_row_ = nullptr;
    graph_container_ = nullptr;
    nozzle_strip_ = nullptr;
    bed_strip_ = nullptr;
    chamber_strip_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::open(Mode mode, lv_obj_t* parent_screen,
                            const std::string& nozzle_extruder) {
    mode_ = mode;
    if (!nozzle_extruder.empty()) {
        // The caller showed this tool: the card, and every surface sharing the
        // target, opens on it.
        helix::ui::TargetTool::instance().pick_extruder(nozzle_extruder);
    }

    show(parent_screen);
}

void TempGraphOverlay::before_show() {
    // The pairing must survive a navbar panel switch.
    helix::nav::register_overlay(overlay_root_, this, true);

    // Sync the declarative mode subject on every open: the caller may have chosen
    // a different mode than last time. XML bindings (strip visibility via
    // bind_flag_if_not_eq, graph_outer width via temp_graph_full_width
    // subject_expr) refire and reflow the overlay.
    lv_subject_set_int(&mode_subject_, static_cast<int>(mode_));
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
        s.color = helix::kTempGraphSeriesColors[color_idx++ % helix::kTempGraphPaletteSize];
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
            s.color = helix::kTempGraphSeriesColors[color_idx++ % helix::kTempGraphPaletteSize];
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
        s.color = helix::kTempGraphSeriesColors[color_idx++ % helix::kTempGraphPaletteSize];
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
            const std::string& klipper = temp_state.chamber_temperature_source();
            if (!klipper.empty()) {
                SeriesInfo s;
                s.display_name = lv_tr("Chamber");
                s.heater_name = "chamber";
                s.klipper_name = klipper;
                s.color = helix::kTempGraphSeriesColors[color_idx++ % helix::kTempGraphPaletteSize];
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
        s.color = helix::kTempGraphSeriesColors[color_idx++ % helix::kTempGraphPaletteSize];
        s.has_target = helix::sensors::klipper_object_has_target(sensor.klipper_name);
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
        lv_obj_set_style_border_width(dot, 0, 0);
        helix::ui::apply_swatch_color(dot, lv_color_to_u32(s.color) & 0xFFFFFF, "");
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
        if (controller_ && controller_->is_valid() && s.series_id != SeriesId::None) {
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
    if (controller_ && controller_->is_valid() && s.series_id != SeriesId::None) {
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

    // Persist the whole toggle set outside the overlay. on_deactivate() clears
    // series_, so without this the next activation — including the one that
    // resuming from background performs — has nothing to restore from.
    VisibilityOverride ov;
    ov.printer_name = current_printer_name();
    ov.mode = static_cast<int>(mode_);
    ov.per_series.reserve(series_.size());
    for (const auto& si : series_) {
        ov.per_series.emplace_back(si.klipper_name, si.visible);
    }
    s_visibility_override = std::move(ov);

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
    // DECLARATIVE: strip visibility and graph_outer width are driven from XML
    // by the temp_graph_mode subject (bind_flag_if_not_eq per strip; graph width
    // via the temp_graph_full_width subject_expr + orthogonal bind_style_if
    // pairs on graph_outer_container). This function is now the DATA half only:
    // preset values, callback user_data, and the chamber-async-clamp that can't
    // move to XML because it depends on the fetched chamber max_temp.

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

    // Get preset config from TemperatureService
    if (!temp_control_panel_)
        return;
    auto& heater = temp_control_panel_->heater(heater_type);

    // Configure preset values for the callback. Index 0 is "Off"; index i>=1 is
    // user preset slot i-1 (helix::presets).
    //
    // Only the first kTempGraphVisiblePresets slots are surfaced here: this
    // overlay's preset strip has no room for a fourth button. That is a layout
    // constraint, not an oversight — see the constant's definition. Loop over
    // kTempGraphVisiblePresets, never kPresetCount, or this overruns the
    // buttons that actually exist in temp_graph_overlay.xml.
    int preset_values[kMaxPresets] = {};
    preset_values[0] = heater.config.presets.off;
    for (int i = 0; i < kTempGraphVisiblePresets; ++i) {
        preset_values[i + 1] = heater.config.presets.material[static_cast<size_t>(i)];
    }

    // Store preset values indexed by name suffix for lookup in callback
    // (cannot use lv_obj_set_user_data — ui_button owns that slot, L069)
    for (int i = 0; i < kMaxPresets; ++i) {
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
        static const char* kChamberPresetNames[kMaxPresets] = {
            "chamber_preset_off", "chamber_preset_1", "chamber_preset_2", "chamber_preset_3"};
        for (int i = 0; i < kMaxPresets; ++i) {
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

    if (preset_idx < 0 || preset_idx >= kMaxPresets)
        return;
    auto& data = self->preset_data_[preset_idx];

    helix::HeaterType type;
    if (!mode_to_heater_type(self->mode_, type))
        return;

    spdlog::debug("[TempGraphOverlay] Preset clicked: {}°C for heater {}", data.preset_value,
                  static_cast<int>(type));

    // A nozzle preset goes to the target tool the card displays. The service's
    // nozzle mirror tracks the machine's active tool, so the optimistic local
    // set_heater is only correct when the target is mounted.
    auto& target = helix::ui::TargetTool::instance();
    const bool nozzle_pick_non_active = type == helix::HeaterType::Nozzle && !target.mounted();

    if (!nozzle_pick_non_active) {
        // Update local state
        self->temp_control_panel_->set_heater(
            type, self->temp_control_panel_->heater(type).current,
            helix::ui::temperature::degrees_to_deci(data.preset_value));
    }

    // Send via the controller — it resolves the klipper name internally (chamber
    // never sends a stale HEATER=chamber) and shows the standard error toast.
    if (helix::TemperatureController* c = self->temp_control_panel_->controller()) {
        if (nozzle_pick_non_active) {
            c->set_target(target.extruder_name(), static_cast<double>(data.preset_value),
                          {.toast = true});
        } else {
            c->set_target(type, static_cast<double>(data.preset_value), {.toast = true});
        }
        // A nozzle material preset heats the bed with it, unless "Material
        // presets heat the bed and chamber" is off. Off (index 0) touches the
        // nozzle alone.
        if (type == helix::HeaterType::Nozzle && preset_idx > 0) {
            if (const auto temps = helix::presets::slot_temps(preset_idx - 1)) {
                if (const auto bed = helix::presets::heater_targets(temps->bed).bed) {
                    c->set_target(helix::HeaterType::Bed, static_cast<double>(*bed),
                                  {.toast = true});
                }
            }
        }
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

    // Capture the target's extruder NOW. Showing the keypad stacks it on top
    // of this overlay, which deactivates the overlay before the user confirms,
    // and the confirm must still send to the tool the user was looking at,
    // not to whoever the machine considers active by then.
    overlay.keypad_ctx_ = {&overlay, type,
                           type == helix::HeaterType::Nozzle
                               ? helix::ui::TargetTool::instance().extruder_name()
                               : std::string()};

    // The service answers through the shared keypad-ceiling authority
    // (configured max over the heater default, async fetch triggered inside);
    // no controller → the heater's static config range.
    const float max_value =
        overlay.temp_control_panel_->custom_keypad_max(type, heater.config.keypad_range.max);

    // Seed the keypad. Chamber uses the effective target (heater target when
    // Heating, fan target when Maintaining) so the keypad pre-fills the value the
    // overlay already shows. The raw heater target reads 0 during M141 maintain
    // mode and would otherwise seed 0. A nozzle seeds from the target tool's
    // own target, not the service's active-tool mirror.
    int seed_deci = heater.target;
    if (type == helix::HeaterType::Nozzle) {
        if (lv_subject_t* t = lv_xml_get_subject(nullptr, "target_nozzle_target")) {
            seed_deci = lv_subject_get_int(t);
        }
    } else if (type == helix::HeaterType::Chamber && overlay.printer_state_) {
        if (auto* subj = overlay.printer_state_->temperature_state()
                             .get_chamber_effective_target_subject()) {
            seed_deci = lv_subject_get_int(subj);
        }
    }

    // The card shows the target tool, which need not be the active one: name
    // its extruder.
    std::string title = helix::ui::temperature::heater_keypad_title(type);
    if (type == helix::HeaterType::Nozzle && overlay.printer_state_) {
        const auto& extruders = overlay.printer_state_->temperature_state().extruders();
        const auto it = extruders.find(helix::ui::TargetTool::instance().extruder_name());
        if (it != extruders.end() && !it->second.display_name.empty()) {
            title = it->second.display_name;
        }
    }
    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(helix::ui::temperature::deci_to_degrees(seed_deci)),
        .min_value = heater.config.keypad_range.min,
        .max_value = max_value,
        .title_label = title.c_str(),
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = keypad_value_cb,
        .user_data = &overlay.keypad_ctx_,
    };

    ui_keypad_show(&keypad_config);
}

void TempGraphOverlay::keypad_value_cb(float value, void* user_data) {
    auto* ctx = static_cast<KeypadCtx*>(user_data);
    if (!ctx || !ctx->overlay || !ctx->overlay->temp_control_panel_)
        return;

    int temp = static_cast<int>(value);

    spdlog::debug("[TempGraphOverlay] Custom temperature: {}°C for heater {}", temp,
                  static_cast<int>(ctx->type));

    // Send via the controller — resolves the klipper name internally and shows
    // the standard error toast on failure. A nozzle sends by the klipper name
    // captured when the keypad opened: the target tool's.
    if (helix::TemperatureController* c = ctx->overlay->temp_control_panel_->controller()) {
        if (ctx->type == helix::HeaterType::Nozzle && !ctx->klipper_name.empty()) {
            c->set_target(ctx->klipper_name, static_cast<double>(temp), {.toast = true});
        } else {
            c->set_target(ctx->type, static_cast<double>(temp), {.toast = true});
        }
    }
}

/// Open the graph on @p mode's heater over the screen the tapped row is on.
static void open_from_heater_row(lv_event_t* e, TempGraphOverlay::Mode mode) {
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (row) {
        get_global_temp_graph_overlay().open(mode, lv_obj_get_screen(row));
    }
}

void helix::ui::register_heater_row_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_heater_row_nozzle", [](lv_event_t* e) {
        open_from_heater_row(e, TempGraphOverlay::Mode::Nozzle);
    });
    lv_xml_register_event_cb(nullptr, "on_heater_row_bed", [](lv_event_t* e) {
        open_from_heater_row(e, TempGraphOverlay::Mode::Bed);
    });
    lv_xml_register_event_cb(nullptr, "on_heater_row_chamber", [](lv_event_t* e) {
        open_from_heater_row(e, TempGraphOverlay::Mode::Chamber);
    });
}
