// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_widget.h"

#include "ui_next_tick.h"
#include "ui_overlay_temp_graph.h"
#include "ui_swatch.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "hardware_object_table.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "humidity_sensor_types.h"
#include "json_utils.h"
#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "sort_small.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>
#include <vector>

namespace helix {

void register_temp_graph_widget() {
    register_widget_factory(
        "temp_graph", [](const std::string& id) { return std::make_unique<TempGraphWidget>(id); });

    lv_xml_register_event_cb(nullptr, "on_temp_graph_widget_clicked",
                             TempGraphWidget::on_temp_graph_widget_clicked);
    lv_xml_register_event_cb(nullptr, "temp_graph_series_swatch_cb",
                             TempGraphWidget::config_swatch_cb);
    lv_xml_register_event_cb(nullptr, "temp_graph_series_switch_cb",
                             TempGraphWidget::config_series_switch_cb);
    lv_xml_register_event_cb(nullptr, "temp_graph_follow_switch_cb",
                             TempGraphWidget::config_follow_switch_cb);
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
    // Unconditional: detach() nulls widget_obj_, so gating the cancel on it
    // would leave a queued async pointing at this object after a detach.
    cancel_discovery_rebuild();
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
    follow_overlay_ =
        config.is_object() && helix::json_util::safe_bool(config, "follow_overlay", false);
}

void TempGraphWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Build default config if not yet configured
    if (config_.empty() || !config_.contains("sensors")) {
        build_default_config();
    }

    // Auto-upgrade older configs that pre-date extruder discovery (only the
    // legacy single "extruder" entry was added). Without this, a widget that
    // was first attached before the printer connected stays stuck on a single
    // nozzle even after a multi-tool printer is discovered. New entries are
    // enabled=true so users actually see their nozzles; existing rows are
    // never modified so an explicit disable choice is preserved.
    if (merge_discovered_extruders(config_, true)) {
        save_widget_config(config_);
        spdlog::info("[TempGraphWidget] '{}' auto-upgraded config with discovered extruders",
                     instance_id_);
    }

    // Seed features from the measured 800x480 two-span (colspan=2/rowspan=2)
    // extents — a stand-in until the first real on_size_changed() call
    // supplies this instance's actual pixel size. 233x230 against that panel's
    // bands yields everything but READOUTS, which needs the widest band. The
    // exact mask never reaches the screen: PanelWidgetManager
    // calls on_size_changed() with this instance's real pixel extents
    // immediately after attach() returns, in the same synchronous loop
    // iteration and before any paint (panel_widget_manager.cpp:862-868), so
    // this seed only lives for the width of that one function call.
    constexpr int kSeedWidthPx = 233;
    constexpr int kSeedHeightPx = 230;

    TempGraphControllerConfig ctrl_config;
    ctrl_config.point_count = 300; // 5-minute window at 1Hz (matches mini graph)
    ctrl_config.axis_size = "xs";
    ctrl_config.initial_features = features_for_size(kSeedWidthPx, kSeedHeightPx);
    // Uses default TempGraphScaleParams (same as mini graph and overlay)
    ctrl_config.series = build_series_from_config();
    applied_visibility_signature_ = current_visibility_signature();

    controller_ = std::make_unique<TempGraphController>(widget_obj_, std::move(ctrl_config));
    // Live only while the panel shows it: attach() also runs on rebuilds under
    // an overlay and for pages that are not on screen, and on_activate() resumes.
    if (!active_)
        controller_->pause();

    // Discovery lands after the WebSocket connects, which is later than the
    // startup attach of a dashboard widget. When it adds extruders this config
    // has never seen, the series list itself is short a curve — the controller's
    // own discovery retry only re-resolves series that already exist. Capture
    // the current version so the immediate registration callback is a no-op.
    {
        auto& ps = get_printer_state();
        if (auto* version_subj = ps.temperature_state().get_extruder_version_subject()) {
            int initial_version = lv_subject_get_int(version_subj);
            extruder_version_observer_ = helix::ui::observe<int>(
                version_subj, this,
                [initial_version](TempGraphWidget* self, int version) {
                    if (version == initial_version)
                        return; // Series were built against this version already
                    // A rediscovery that changes the tool count can rename an
                    // extruder ("Nozzle" to "Nozzle 1").
                    self->refresh_series_names();
                    self->schedule_discovery_rebuild();
                },
                ps.get_subjects_lifetime());
        }
    }

    // Bed and chamber are named here, not by the printer layer.
    language_observer_ = helix::ui::observe_language_change(
        this, [](TempGraphWidget* self) { self->refresh_series_names(); });

    // The graph is `merges_into_card = false` — it reads as its own panel, not
    // as one tile in a fused run — so it supplies its own surface. Use the
    // shared Card style rather than painting card_bg directly: the raw fill
    // this used to set had no radius, so it drew square corners wherever it sat
    // against the rounded cards around it.
    lv_obj_add_style(widget_obj_, ThemeManager::instance().get_style(StyleRole::Card),
                     LV_PART_MAIN);

    spdlog::debug("[TempGraphWidget] Attached '{}' (seed {}x{}px)", instance_id_, kSeedWidthPx,
                  kSeedHeightPx);
}

void TempGraphWidget::detach() {
    cancel_discovery_rebuild();
    extruder_version_observer_.reset();
    language_observer_.reset();
    controller_.reset();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempGraphWidget] Detached '{}'", instance_id_);
}

void TempGraphWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    if (controller_) {
        uint32_t features = features_for_size(width_px, height_px);
        controller_->set_features(features);
        spdlog::debug("[TempGraphWidget] '{}' resized to {}x{} ({}x{}px), features=0x{:x}",
                      instance_id_, colspan, rowspan, width_px, height_px, features);
    }
}

void TempGraphWidget::on_activate() {
    active_ = true;
    if (controller_)
        controller_->resume();

    // Follow mode: rebuild if the overlay's visibility snapshot drifted while
    // the user was on the full-screen graph.
    if (!follow_overlay_ || !widget_obj_ || !parent_screen_)
        return;
    if (current_visibility_signature() == applied_visibility_signature_)
        return;

    rebuild_in_place();
}

// Off screen, or under an overlay, the graph takes no live samples: each one
// invalidates the chart, and that redraw merges with whatever the overlay is
// redrawing and drags the panel beneath it into every frame. resume() backfills.
void TempGraphWidget::on_deactivate() {
    active_ = false;
    if (controller_)
        controller_->pause();
}

void TempGraphWidget::refresh_series_names() {
    if (!controller_ || !config_.contains("sensors") || !config_["sensors"].is_array()) {
        return;
    }
    for (const auto& entry : config_["sensors"]) {
        if (entry.contains("name") && entry["name"].is_string()) {
            const std::string name = entry["name"].get<std::string>();
            controller_->set_series_name(name, TempGraphConfigModal::sensor_display_name(name));
        }
    }
}

void TempGraphWidget::rebuild_in_place() {
    // detach() nulls the members, and a panel rebuild between modal-open and
    // save can free the container too (bundle RP293UCW): handles read nullptr then.
    const helix::ui::WeakWidget current_widget(widget_obj_);
    const helix::ui::WeakWidget current_parent(parent_screen_);
    detach();
    if (!current_widget) {
        spdlog::warn("[TempGraphWidget] '{}' rebuild: widget container gone, skipping reattach",
                     instance_id_);
        return;
    }
    attach(current_widget, current_parent);
}

void TempGraphWidget::apply_config_save(const nlohmann::json& new_config) {
    config_ = new_config;
    save_widget_config(config_);
    rebuild_in_place();
}

void TempGraphWidget::schedule_discovery_rebuild() {
    if (discovery_rebuild_pending_) {
        return; // Already queued — collapse the burst into one rebuild
    }
    discovery_rebuild_pending_ = true;
    helix::ui::run_next_tick(discovery_rebuild_guard_.token(),
                             [this]() { run_discovery_rebuild(); });
}

void TempGraphWidget::cancel_discovery_rebuild() {
    discovery_rebuild_guard_.invalidate();
    discovery_rebuild_pending_ = false;
}

void TempGraphWidget::run_discovery_rebuild() {
    discovery_rebuild_pending_ = false;

    if (!widget_obj_) {
        return; // Detached while queued
    }

    // Only rebuild when discovery actually added a curve we do not have. A bare
    // version bump (rediscovery of the same tools) must not tear the graph down
    // and throw away its trace — the controller re-resolves those subjects on
    // its own.
    if (!merge_discovered_extruders(config_, true)) {
        return;
    }

    save_widget_config(config_);
    spdlog::info("[TempGraphWidget] '{}' rebuilding: discovery added extruders", instance_id_);
    rebuild_in_place();
}

bool TempGraphWidget::on_edit_configure() {
    config_modal_ = std::make_unique<TempGraphConfigModal>(
        config_, [this](const nlohmann::json& new_config) { apply_config_save(new_config); });
    config_modal_->show(lv_screen_active());
    return true;
}

// ============================================================================
// Feature mapping
// ============================================================================

uint32_t TempGraphWidget::features_for_size(int width_px, int height_px) {
    return features_for_size(width_px, height_px, widget_size::current_breakpoint());
}

uint32_t TempGraphWidget::features_for_size(int width_px, int height_px, UiBreakpoint bp) {
    // Gradients always enabled — the draw callback auto-disables when >3 series visible
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_GRADIENTS;

    if (width_px >= widget_size::w_normal(bp) || height_px >= widget_size::h_tall(bp)) {
        // Medium: add target lines (with history trace — time-varying dashed line)
        features |= TEMP_GRAPH_FEATURE_TARGET_LINES | TEMP_GRAPH_FEATURE_TARGET_HISTORY;
    }

    if (height_px >= widget_size::h_tall(bp)) {
        // Tall: legend chips, Y-axis labels, and X-axis time labels — all
        // need vertical room (legend above/below curves, Y to the side,
        // X below). The 5-min window only renders 1–3 time labels so
        // width isn't the X-axis constraint.
        features |= TEMP_GRAPH_FEATURE_LEGEND;
        features |= TEMP_GRAPH_FEATURE_Y_AXIS;
        features |= TEMP_GRAPH_FEATURE_X_AXIS;
    }

    if (width_px >= widget_size::w_wide(bp) && height_px >= widget_size::h_tall(bp)) {
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
        if (!entry.contains("name") || !entry["name"].is_string())
            continue;

        const std::string klipper_name = entry["name"].get<std::string>();
        bool enabled;
        if (use_snapshot) {
            enabled =
                std::find(snapshot->begin(), snapshot->end(), klipper_name) != snapshot->end();
        } else {
            enabled = helix::json_util::safe_bool(entry, "enabled", true);
        }
        if (!enabled)
            continue;

        TempGraphSeriesSpec spec;
        spec.klipper_name = klipper_name;
        spec.display_name = TempGraphConfigModal::sensor_display_name(klipper_name);
        if (entry.contains("color") && entry["color"].is_number_integer()) {
            spec.color = lv_color_hex(entry["color"].get<uint32_t>());
        } else {
            spec.color = kTempGraphSeriesColors[color_idx % kTempGraphPaletteSize];
        }
        color_idx++;

        // Heaters have targets, sensors generally don't
        spec.show_target =
            (spec.klipper_name == "extruder" || spec.klipper_name.find("extruder") == 0 ||
             spec.klipper_name == "heater_bed" || spec.klipper_name == "chamber" ||
             sensors::klipper_object_has_target(spec.klipper_name));
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

    // Enumerate discovered extruders so multi-tool printers (Snapmaker U1,
    // toolchangers) get all nozzles enabled by default rather than just the
    // legacy single "extruder" entry.
    // One running index across nozzles, bed, chamber and aux sensors, so no two
    // series can land on the same palette slot. Bed and chamber used to carry
    // literal copies of slots 1 and 2, which collided with the second nozzle's
    // color on a multi-extruder machine.
    int color_idx = 0;

    const auto& exts = get_printer_state().temperature_state().extruders();
    if (exts.empty()) {
        // Pre-discovery / single-extruder fallback
        sensors.push_back({{"name", "extruder"},
                           {"enabled", true},
                           {"color", temp_graph_series_hex(color_idx++)}});
    } else {
        std::vector<std::string> extruder_names;
        extruder_names.reserve(exts.size());
        for (const auto& [name, _] : exts)
            extruder_names.push_back(name);
        std::sort(extruder_names.begin(), extruder_names.end());
        for (const auto& name : extruder_names) {
            sensors.push_back(
                {{"name", name}, {"enabled", true}, {"color", temp_graph_series_hex(color_idx++)}});
        }
    }
    sensors.push_back(
        {{"name", "heater_bed"}, {"enabled", true}, {"color", temp_graph_series_hex(color_idx++)}});

    // Check for chamber
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        sensors.push_back({{"name", "chamber"},
                           {"enabled", false},
                           {"color", temp_graph_series_hex(color_idx++)}});
    }

    // Add discovered auxiliary sensors (disabled by default)
    auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
    auto discovered = sensor_mgr.get_sensors_sorted();
    for (const auto& sensor : discovered) {
        // The chamber's own objects are the "chamber" row above.
        if (!sensor.enabled || sensor.role == sensors::TemperatureSensorRole::CHAMBER)
            continue;
        sensors.push_back({
            {"name", sensor.klipper_name},
            {"enabled", false},
            {"color", temp_graph_series_hex(color_idx++)},
        });
    }

    config_["sensors"] = sensors;
    spdlog::debug("[TempGraphWidget] Built default config with {} sensors for '{}'", sensors.size(),
                  instance_id_);
}

bool TempGraphWidget::merge_discovered_extruders(nlohmann::json& config, bool enabled) {
    if (!config.contains("sensors") || !config["sensors"].is_array())
        config["sensors"] = nlohmann::json::array();

    auto& arr = config["sensors"];
    std::set<std::string> existing;
    for (const auto& entry : arr) {
        if (entry.contains("name") && entry["name"].is_string())
            existing.insert(entry["name"].get<std::string>());
    }

    bool added = false;
    const auto& exts = get_printer_state().temperature_state().extruders();
    int color_idx = static_cast<int>(arr.size());
    for (const auto& [name, _] : exts) {
        if (existing.count(name))
            continue;
        lv_color_t c = kTempGraphSeriesColors[color_idx % kTempGraphPaletteSize];
        uint32_t color_hex = (static_cast<uint32_t>(c.red) << 16) |
                             (static_cast<uint32_t>(c.green) << 8) | static_cast<uint32_t>(c.blue);
        nlohmann::json row;
        row["name"] = name;
        row["enabled"] = enabled;
        row["color"] = color_hex;
        arr.push_back(std::move(row));
        ++color_idx;
        added = true;
    }
    return added;
}

// ============================================================================
// TempGraphConfigModal
// ============================================================================

namespace {

using helix::xml::IndexedSubjectPool;

// The settings dialog's rows, one slot per series: name, palette colour and
// on/off flag, bound by temp_graph_series_row.
IndexedSubjectPool s_cfg_name{"temp_graph_cfg", IndexedSubjectPool::Type::String, 96, "_name"};
IndexedSubjectPool s_cfg_color{"temp_graph_cfg", IndexedSubjectPool::Type::Color, 0, "_color"};
IndexedSubjectPool s_cfg_on{"temp_graph_cfg", IndexedSubjectPool::Type::Int, 0, "_on"};
HELIX_PSRAM_BSS lv_subject_t s_cfg_count_subject{};
HELIX_PSRAM_BSS lv_subject_t s_cfg_follow_subject{};
helix::WidgetSubjects s_cfg_subjects{"TempGraphConfigSubjects"};

void temp_graph_config_init_subjects() {
    s_cfg_subjects.ensure_init(
        [] {
            lv_subject_init_int(&s_cfg_count_subject, 0);
            s_cfg_subjects.publish("temp_graph_cfg_rows", &s_cfg_count_subject);
            lv_subject_init_int(&s_cfg_follow_subject, 0);
            s_cfg_subjects.publish("temp_graph_cfg_follow", &s_cfg_follow_subject);
        },
        [] {
            s_cfg_name.reclaim();
            s_cfg_color.reclaim();
            s_cfg_on.reclaim();
        });
}

uint32_t series_rgb(int color_idx) {
    return lv_color_to_u32(kTempGraphSeriesColors[color_idx % kTempGraphPaletteSize]) & 0xFFFFFF;
}

} // namespace

TempGraphWidget::TempGraphConfigModal* TempGraphWidget::TempGraphConfigModal::s_active_ = nullptr;

TempGraphWidget::TempGraphConfigModal::TempGraphConfigModal(const nlohmann::json& config,
                                                            SaveCallback on_save)
    : config_(config), on_save_(std::move(on_save)) {
    temp_graph_config_init_subjects();
    s_active_ = this;
    // The rows are published before the dialog is built, so the XML <repeat>
    // expands with them in place.
    populate_sensor_list();
    lv_subject_set_int(
        &s_cfg_follow_subject,
        config_.is_object() && helix::json_util::safe_bool(config_, "follow_overlay", false) ? 1
                                                                                             : 0);
}

TempGraphWidget::TempGraphConfigModal::~TempGraphConfigModal() {
    if (s_active_ == this)
        s_active_ = nullptr;
}

void TempGraphWidget::TempGraphConfigModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    spdlog::debug("[TempGraphConfigModal] Opened with {} sensor rows", rows_.size());
}

void TempGraphWidget::TempGraphConfigModal::on_ok() {
    // Collect current toggle/color states back into config
    nlohmann::json sensors = nlohmann::json::array();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& row = rows_[i];
        sensors.push_back({
            {"name", row.name},
            {"enabled", lv_subject_get_int(s_cfg_on.at(i)) != 0},
            {"color", series_rgb(row.color_idx)},
        });
    }

    nlohmann::json new_config = config_;
    new_config["sensors"] = sensors;
    new_config["follow_overlay"] = lv_subject_get_int(&s_cfg_follow_subject) != 0;

    if (on_save_) {
        on_save_(new_config);
    }

    spdlog::info("[TempGraphConfigModal] Saved config with {} sensors (follow={})", sensors.size(),
                 new_config["follow_overlay"].get<bool>());
    hide();
}

std::string
TempGraphWidget::TempGraphConfigModal::sensor_display_name(const std::string& klipper_name) {
    // Extruders: defer to PrinterTemperatureState which already produces
    // "Nozzle" (single) / "Nozzle 1", "Nozzle 2", ... (multi). Keeps the
    // chip row, graph legend, and config modal aligned on one source of truth.
    if (const auto tool_number = tool_number_for_extruder(klipper_name)) {
        const auto& exts = get_printer_state().temperature_state().extruders();
        auto it = exts.find(klipper_name);
        if (it != exts.end() && !it->second.display_name.empty()) {
            return it->second.display_name;
        }
        // Fallback when extruders haven't been discovered yet: a bare "extruder"
        // carries no signal about whether the printer has one nozzle or several,
        // so index 0 answers with the common single-extruder case rather than
        // guessing multi. "extruderN" (N >= 1) implies multi and gets a number.
        if (*tool_number == 0)
            return lv_tr("Nozzle");
        return std::string(lv_tr("Nozzle")) + " " + helix::ui::lane_number_text(*tool_number);
    }
    if (klipper_name == "heater_bed")
        return lv_tr("Bed");
    if (klipper_name == "chamber")
        return lv_tr("Chamber");

    // The object table strips the type prefix ("temperature_sensor ", "bme280 ", ...).
    const auto cls = helix::hw::classify_object(klipper_name);
    std::string display = cls.instance.empty() ? klipper_name : cls.instance;

    // Capitalize first letter
    if (!display.empty()) {
        display[0] = static_cast<char>(toupper(static_cast<unsigned char>(display[0])));
    }

    // Replace underscores with spaces
    for (auto& ch : display) {
        if (ch == '_')
            ch = ' ';
    }

    return helix::sensors::with_humidity_chip_suffix(std::move(display), klipper_name);
}

void TempGraphWidget::TempGraphConfigModal::populate_sensor_list() {
    rows_.clear();

    if (!config_.contains("sensors") || !config_["sensors"].is_array())
        config_["sensors"] = nlohmann::json::array();

    // Modal-time safety net: TempGraphWidget::attach already auto-upgrades on
    // first run after extruder discovery, but if a widget was created and
    // configured pre-discovery the user may still see only the legacy entry
    // when reopening this modal. Add the missing rows as enabled=false so the
    // currently visible curves don't change, then normalize ordering.
    TempGraphWidget::merge_discovered_extruders(config_, /*enabled=*/false);

    // Partition: extruders (sorted by name) first, then the rest in
    // original order. stable_partition preserves the trailing entries'
    // sequence so heater_bed / chamber / aux sensors stay where the user
    // left them.
    {
        auto& arr = config_["sensors"];
        auto entry_is_extruder = [](const nlohmann::json& entry) {
            return entry.contains("name") && entry["name"].is_string() &&
                   is_extruder_name(entry["name"].get<std::string>());
        };
        auto split = std::stable_partition(arr.begin(), arr.end(), entry_is_extruder);
        helix::sort_small(arr.begin(), split, [](const nlohmann::json& a, const nlohmann::json& b) {
            return a["name"].get<std::string>() < b["name"].get<std::string>();
        });
    }

    std::vector<bool> enabled;
    const auto& sensors = config_["sensors"];
    for (size_t i = 0; i < sensors.size(); ++i) {
        const auto& entry = sensors[i];
        if (!entry.contains("name") || !entry["name"].is_string())
            continue;

        SensorRow row;
        row.name = entry["name"].get<std::string>();
        row.display = sensor_display_name(row.name);

        // Find the matching color index from the palette
        if (entry.contains("color") && entry["color"].is_number_integer()) {
            uint32_t cfg_hex = entry["color"].get<uint32_t>();
            lv_color_t cfg_color = lv_color_hex(cfg_hex);
            row.color_idx = static_cast<int>(i) % kTempGraphPaletteSize; // default
            for (int ci = 0; ci < kTempGraphPaletteSize; ++ci) {
                if (kTempGraphSeriesColors[ci].red == cfg_color.red &&
                    kTempGraphSeriesColors[ci].green == cfg_color.green &&
                    kTempGraphSeriesColors[ci].blue == cfg_color.blue) {
                    row.color_idx = ci;
                    break;
                }
            }
        } else {
            row.color_idx = static_cast<int>(i) % kTempGraphPaletteSize;
        }

        enabled.push_back(helix::json_util::safe_bool(entry, "enabled", true));
        rows_.push_back(std::move(row));
    }

    s_cfg_name.ensure_size(rows_.size());
    s_cfg_color.ensure_size(rows_.size());
    s_cfg_on.ensure_size(rows_.size());
    for (size_t i = 0; i < rows_.size(); ++i) {
        s_cfg_name.set_string(i, rows_[i].display);
        s_cfg_color.set_color(i, series_rgb(rows_[i].color_idx));
        s_cfg_on.set_int(i, enabled[i] ? 1 : 0);
    }
    // The count goes last: it drives the XML <repeat>, whose rows read the
    // other subjects as they are created.
    lv_subject_set_int(&s_cfg_count_subject, static_cast<int>(rows_.size()));
}

void TempGraphWidget::TempGraphConfigModal::cycle_color(size_t idx) {
    if (idx >= rows_.size())
        return;
    auto& row = rows_[idx];
    row.color_idx = (row.color_idx + 1) % kTempGraphPaletteSize;
    s_cfg_color.set_color(idx, series_rgb(row.color_idx));

    spdlog::debug("[TempGraphConfigModal] Cycled '{}' to color index {}", row.name, row.color_idx);
}

void TempGraphWidget::TempGraphConfigModal::set_series_on(size_t idx, bool on) {
    if (idx < rows_.size())
        s_cfg_on.set_int(idx, on ? 1 : 0);
}

void TempGraphWidget::TempGraphConfigModal::set_follow(bool on) {
    lv_subject_set_int(&s_cfg_follow_subject, on ? 1 : 0);
}

void TempGraphWidget::config_swatch_cb(lv_event_t* e) {
    // XML event user_data is the row's index as a string.
    const char* index = static_cast<const char*>(lv_event_get_user_data(e));
    if (auto* modal = TempGraphConfigModal::active(); modal && index)
        modal->cycle_color(static_cast<size_t>(std::atoi(index)));
}

void TempGraphWidget::config_series_switch_cb(lv_event_t* e) {
    const char* index = static_cast<const char*>(lv_event_get_user_data(e));
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (auto* modal = TempGraphConfigModal::active(); modal && index && sw) {
        modal->set_series_on(static_cast<size_t>(std::atoi(index)),
                             lv_obj_has_state(sw, LV_STATE_CHECKED));
    }
}

void TempGraphWidget::config_follow_switch_cb(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (auto* modal = TempGraphConfigModal::active(); modal && sw)
        modal->set_follow(lv_obj_has_state(sw, LV_STATE_CHECKED));
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
