// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_service.h"

#include "ui_breakpoint.h"
#include "ui_callback_helpers.h"
#include "ui_chamber_dryer_modal.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "app_constants.h"
#include "app_globals.h"
#include "chamber_stall_prompt.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "temp_graph_controller.h"
#include "temperature_controller.h"
#include "temperature_history_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

using namespace helix;
using helix::ui::observe;
using helix::ui::temperature::deci_to_degrees_f;

// ============================================================================
// Helper: heater type index
// ============================================================================
static int idx(HeaterType type) {
    return static_cast<int>(type);
}

static const char* heater_label(HeaterType type) {
    switch (type) {
    case HeaterType::Nozzle:
        return "Nozzle";
    case HeaterType::Bed:
        return "Bed";
    case HeaterType::Chamber:
        return "Chamber";
    }
    return "Unknown";
}

// ============================================================================
// Constructor
// ============================================================================

TemperatureService::TemperatureService(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
    // Preset temperatures are derived per user preset slot, not per hardcoded
    // material. TemperatureController owns the derivation (nozzle/bed from the
    // filament database, chamber from its documented enclosure ladder) so the
    // service and the controller can never drift apart — they used to maintain
    // two independent copies of the same three lookups.
    const HeaterPresets nozzle_presets = compute_heater_presets(HeaterType::Nozzle);
    const HeaterPresets bed_presets = compute_heater_presets(HeaterType::Bed);
    const HeaterPresets chamber_presets = compute_heater_presets(HeaterType::Chamber);

    // ── Nozzle config ───────────────────────────────────────────────────
    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];
    nozzle.config = {.type = HeaterType::Nozzle,
                     .name = "Nozzle",
                     .color = helix::kTempGraphSeriesColors[0], // nozzle
                     .temp_range_max = 320.0f,
                     .y_axis_increment = 80,
                     .presets = nozzle_presets,
                     .keypad_range = {0.0f, 350.0f}};
    nozzle.cooling_threshold_deci = 400; // 40°C
    nozzle.klipper_name = "extruder";    // Updated dynamically for multi-extruder
    nozzle.min_temp = app_constants::temperature::kDefaultMinTemp;
    nozzle.max_temp = app_constants::temperature::kDefaultNozzleMax;

    // ── Bed config ──────────────────────────────────────────────────────
    auto& bed = heaters_[idx(HeaterType::Bed)];
    bed.config = {.type = HeaterType::Bed,
                  .name = "Bed",
                  .color = helix::kTempGraphSeriesColors[1], // bed
                  .temp_range_max = 140.0f,
                  .y_axis_increment = 35,
                  .presets = bed_presets,
                  .keypad_range = {0.0f, 150.0f}};
    bed.cooling_threshold_deci = 350; // 35°C
    bed.klipper_name = "heater_bed";
    bed.min_temp = app_constants::temperature::kDefaultMinTemp;
    bed.max_temp = app_constants::temperature::kDefaultBedMax;

    // ── Chamber config ──────────────────────────────────────────────────
    auto& chamber = heaters_[idx(HeaterType::Chamber)];
    chamber.config = {.type = HeaterType::Chamber,
                      .name = "Chamber",
                      .color = helix::kTempGraphSeriesColors[2], // chamber
                      .temp_range_max = 80.0f,
                      .y_axis_increment = 20,
                      .presets = chamber_presets,
                      .keypad_range = {0.0f, 80.0f}};
    chamber.cooling_threshold_deci = 300;            // 30°C
    chamber.klipper_name = "heater_generic chamber"; // Updated from discovery
    chamber.read_only = true; // Default sensor-only; updated at runtime from capability subject
    chamber.min_temp = 0;
    chamber.max_temp = 80;

    // Zero all string buffers
    for (auto& h : heaters_) {
        h.display_buf.fill('\0');
        h.status_buf.fill('\0');
    }

    // Subscribe to temperature subjects with individual ObserverGuards.
    // Nozzle observers are separate so they can be rebound when switching
    // extruders in multi-extruder setups (bed/chamber observers stay constant).
    nozzle.temp_observer = observe<int>(
        printer_state_.temperature_state().get_active_extruder_temp_subject(), this,
        [](TemperatureService* self, int temp) { self->on_temp_changed(HeaterType::Nozzle, temp); },
        printer_state_.get_subjects_lifetime());
    nozzle.target_observer = observe<int>(
        printer_state_.temperature_state().get_active_extruder_target_subject(), this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Nozzle, target);
        },
        printer_state_.get_subjects_lifetime());
    bed.temp_observer = observe<int>(
        printer_state_.temperature_state().get_bed_temp_subject(bed.temp_lifetime), this,
        [](TemperatureService* self, int temp) { self->on_temp_changed(HeaterType::Bed, temp); },
        bed.temp_lifetime);
    bed.target_observer = observe<int>(
        printer_state_.temperature_state().get_bed_target_subject(bed.target_lifetime), this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Bed, target);
        },
        bed.target_lifetime);
    chamber.temp_observer = observe<int>(
        printer_state_.temperature_state().get_chamber_temp_subject(chamber.temp_lifetime), this,
        [](TemperatureService* self, int temp) {
            self->on_temp_changed(HeaterType::Chamber, temp);
        },
        chamber.temp_lifetime);
    chamber.target_observer = observe<int>(
        printer_state_.temperature_state().get_chamber_target_subject(chamber.target_lifetime),
        this,
        [](TemperatureService* self, int target) {
            self->on_target_changed(HeaterType::Chamber, target);
        },
        chamber.target_lifetime);
    // M141 cooling mode parks the ≤40°C setpoint on the cooling-fan target while
    // the heater target stays 0. Observe it too so the effective chamber setpoint
    // reflects "Maintaining" sets. recompute_chamber_target() reads BOTH subjects.
    chamber.fan_target_observer = observe<int>(
        printer_state_.temperature_state().get_chamber_fan_target_subject(
            chamber.fan_target_lifetime),
        this,
        [](TemperatureService* self, int /*fan_target*/) { self->recompute_chamber_target(); },
        chamber.fan_target_lifetime);

    // Register XML event callbacks (BEFORE any lv_xml_create calls)
    register_xml_callbacks({
        {"on_chamber_fault_reset_clicked", on_chamber_fault_reset_clicked},
        {"on_chamber_retry_target_clicked", on_chamber_retry_target_clicked},
        {"on_chamber_filter_fan_clicked", on_chamber_filter_fan_clicked},
        {"on_chamber_dryer_start_clicked", on_chamber_dryer_start_clicked},
        {"on_chamber_dryer_stop_clicked", on_chamber_dryer_stop_clicked},
    });

    spdlog::debug("[TempPanel] Constructed - subscribed to PrinterState temperature subjects");
}

TemperatureService::~TemperatureService() {
    deinit_subjects();
}

// ============================================================================
// Generic temperature/target change handlers
// ============================================================================

void TemperatureService::on_temp_changed(HeaterType type, int temp_deci) {
    auto& h = heaters_[idx(type)];

    // Filter garbage data at the source
    int max_valid = (type == HeaterType::Nozzle) ? 4000 : (type == HeaterType::Bed) ? 2000 : 1500;
    if (temp_deci <= 0 || temp_deci > max_valid) {
        return;
    }

    h.current = temp_deci;
    update_display(type);
    update_status(type);

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    if (!subjects_initialized_) {
        return;
    }

    // Throttle live graph updates to 1 Hz
    if (now_ms - h.last_graph_update_ms < kGraphSampleIntervalMs) {
        return;
    }
    h.last_graph_update_ms = now_ms;

    float temp_deg = deci_to_degrees_f(temp_deci);
    update_graphs(type, temp_deg, now_ms);
}

void TemperatureService::on_target_changed(HeaterType type, int target_deci) {
    // Chamber's effective setpoint is heater-OR-fan (M141 splits the setpoint
    // across two Klipper objects). Route through the combine path rather than
    // taking the heater readback as gospel. Nozzle/Bed keep the direct write.
    if (type == HeaterType::Chamber) {
        recompute_chamber_target();
        return;
    }

    auto& h = heaters_[idx(type)];
    h.target = target_deci;
    update_display(type);
    update_status(type);
}

void TemperatureService::recompute_chamber_target() {
    auto& chamber = heaters_[idx(HeaterType::Chamber)];

    // Derive both the effective target and the control mode from the data-layer
    // subjects, which fold in the cooling-fan resting target so M141 S0 (and a
    // deliberate set equal to it) reads as Resting (Auto) rather than a
    // deliberate "Maintaining" set.
    chamber.target = lv_subject_get_int(
        printer_state_.temperature_state().get_chamber_effective_target_subject());
    chamber.chamber_mode =
        helix::ui::temperature::chamber_mode_word(static_cast<helix::ChamberMode>(
            lv_subject_get_int(printer_state_.temperature_state().get_chamber_mode_subject())));

    update_display(HeaterType::Chamber);
    update_status(HeaterType::Chamber);
}

// ============================================================================
// Display + Status updates (generic)
// ============================================================================

void TemperatureService::update_display(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];
    char text[sizeof(h.display_buf)];
    format_display(h, text, sizeof(text));
    lv_subject_copy_string(&h.display_subject, text);
}

void TemperatureService::format_display(const HeaterState& h, char* buf, size_t size) {
    helix::ui::temperature::format_temperature_pair(static_cast<int>(deci_to_degrees_f(h.current)),
                                                    static_cast<int>(deci_to_degrees_f(h.target)),
                                                    buf, size, /*with_unit=*/false);
}

void TemperatureService::update_status(HeaterType type) {
    if (!subjects_initialized_) {
        return;
    }

    auto& h = heaters_[idx(type)];

    // Re-check read_only for chamber from live capability subject
    if (type == HeaterType::Chamber) {
        auto* cap_subj = printer_state_.capabilities_state().subject(Capability::HasChamberHeater);
        h.read_only = (lv_subject_get_int(cap_subj) == 0);
    }

    const int power_pct = lv_subject_get_int(printer_state_.get_heater_power_subject(type));

    if (h.read_only) {
        // Nothing of ours drives this chamber, so there is no duty to report;
        // the word is all the status area can say.
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", lv_tr("Monitoring"));
        lv_subject_set_int(&h.status_state_subject,
                           static_cast<int>(helix::ui::temperature::HeaterStatusState::None));
    } else {
        // One shared classifier feeds this overlay and the controls panel, so
        // they can never disagree. The chamber passes its mode: Maintaining
        // treats the target as a cooling ceiling, not a heat goal.
        auto mode = (type == HeaterType::Chamber)
                        ? static_cast<helix::ChamberMode>(lv_subject_get_int(
                              printer_state_.temperature_state().get_chamber_mode_subject()))
                        : helix::ChamberMode::Heating;
        auto status =
            helix::ui::temperature::classify_heater_status(h.current, h.target, power_pct, mode);
        lv_subject_set_int(&h.status_state_subject, static_cast<int>(status.state));
        snprintf(h.status_buf.data(), h.status_buf.size(), "%s", status.duty.c_str());
    }

    lv_subject_copy_string(&h.status_subject, h.status_buf.data());

    int heating_state = (h.target > 0) ? 1 : 0;
    lv_subject_set_int(&h.heating_subject, heating_state);

    spdlog::trace("[TempPanel] {} status: '{}' (heating={})", heater_label(type),
                  h.status_buf.data(), heating_state);
}

// ============================================================================
// Graph updates (generic)
// ============================================================================

void TemperatureService::update_graphs(HeaterType type, float temp_deg, int64_t now_ms) {
    auto& h = heaters_[idx(type)];

    for (const auto& reg : h.temp_graphs) {
        if (ui_temp_graph_is_valid(reg.graph) && reg.series_id != SeriesId::None) {
            ui_temp_graph_update_series_with_time(reg.graph, reg.series_id, temp_deg, now_ms);
        }
    }
}

// ============================================================================
// Subject init/deinit
// ============================================================================

void TemperatureService::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[TempPanel] init_subjects() called twice - ignoring");
        return;
    }

    // Initialize display + status + heating subjects for each heater
    const char* display_names[] = {"nozzle_temp_display", "bed_temp_display",
                                   "chamber_temp_display"};
    const char* status_names[] = {"nozzle_status", "bed_status", "chamber_status"};
    const char* status_state_names[] = {"nozzle_status_state", "bed_status_state",
                                        "chamber_status_state"};
    const char* heating_names[] = {"nozzle_heating", "bed_heating", "chamber_heating"};

    for (int i = 0; i < helix::kHeaterTypeCount; ++i) {
        auto& h = heaters_[i];

        // The macro copies the initial value INTO display_buf, so it is
        // formatted elsewhere: a copy of a buffer onto itself is undefined.
        char initial[sizeof(h.display_buf)];
        format_display(h, initial, sizeof(initial));

        UI_MANAGED_SUBJECT_STRING_N(h.display_subject, h.display_buf.data(), h.display_buf.size(),
                                    initial, display_names[i], subjects_);
        UI_MANAGED_SUBJECT_STRING_N(h.status_subject, h.status_buf.data(), h.status_buf.size(), "",
                                    status_names[i], subjects_);
        UI_MANAGED_SUBJECT_INT(h.status_state_subject, 0, status_state_names[i], subjects_);
        UI_MANAGED_SUBJECT_INT(h.heating_subject, 0, heating_names[i], subjects_);
    }

    subjects_initialized_ = true;
    spdlog::debug("[TempPanel] Subjects initialized for {} heater types", helix::kHeaterTypeCount);
}

void TemperatureService::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    // Set flag BEFORE deinit to prevent deferred callbacks from accessing
    // torn-down subjects during cleanup
    subjects_initialized_ = false;
    subjects_.deinit_all();
    spdlog::debug("[TempPanel] Subjects deinitialized");
}

// ============================================================================
// Setters (backward-compat)
// ============================================================================

void TemperatureService::set_heater(HeaterType type, int current, int target) {
    auto& h = heaters_[idx(type)];
    helix::ui::temperature::validate_and_clamp_pair(
        current, target, helix::ui::temperature::degrees_to_deci(h.min_temp),
        helix::ui::temperature::degrees_to_deci(h.max_temp), heater_label(type));
    h.current = current;
    h.target = target;
    update_display(type);
}

void TemperatureService::set_heater_limits(HeaterType type, int min_temp, int max_temp) {
    auto& h = heaters_[idx(type)];
    h.min_temp = min_temp;
    h.max_temp = max_temp;
    spdlog::debug("[TempPanel] {} limits updated: {}-{}°C", heater_label(type), min_temp, max_temp);
}

// ============================================================================
// XML event callbacks - chamber-heater diagnostics (issue #1290)
// ============================================================================
// Both fire from the chamber card's banner and filter-fan switch inside
// temp_graph_overlay with no instance user data, so they reach the
// controller through the same app_globals registration production wires in
// SubjectInitializer.

void TemperatureService::on_chamber_fault_reset_clicked(lv_event_t* /*e*/) {
    if (auto* tc = get_temperature_controller()) {
        tc->reset_chamber_fault();
    } else {
        spdlog::warn("[TempPanel] chamber fault reset clicked with no controller registered");
    }
}

// The route around the gcode queue when there is one, since a refused
// target often matters most while a wait holds that queue. Gcode only with no
// job holding the machine: during one it would queue behind the wait.
void TemperatureService::on_chamber_retry_target_clicked(lv_event_t* /*e*/) {
    if (helix::ui::ChamberStallPrompt::retry()) {
        return;
    }
    auto* tc = get_temperature_controller();
    const double target = helix::ui::ChamberStallPrompt::retry_target_c();
    lv_subject_t* job = get_printer_state().print_state().get_job_holds_machine_subject();
    if (job && lv_subject_get_int(job) != 0) {
        spdlog::warn("[TempPanel] chamber retry during a job with no route around the queue");
        NOTIFY_WARNING("{}", lv_tr("The heater can't be changed while the printer is waiting."));
    } else if (tc && target > 0) {
        tc->set_target(helix::HeaterType::Chamber, target);
    } else {
        spdlog::warn("[TempPanel] chamber retry clicked with no target to resend");
    }
}

void TemperatureService::on_chamber_filter_fan_clicked(lv_event_t* /*e*/) {
    auto* tc = get_temperature_controller();
    if (!tc) {
        spdlog::warn("[TempPanel] chamber filter-fan clicked with no controller registered");
    } else {
        // Toggle: invert OUR pin request, not the running state: the device also
        // runs this fan on its own, and a click must not read that as "already
        // on". A missing subject (state torn down mid-click) or an unknown value
        // fails safe to "turn on".
        lv_subject_t* req_subj = lv_xml_get_subject(nullptr, "chamber_filter_fan_requested");
        tc->set_chamber_filter_fan(!req_subj || lv_subject_get_int(req_subj) != 1);
    }
    // The switch flips itself on tap, but chamber_filter_fan_on only changes
    // when a status frame confirms the new state. Re-notify it with the
    // current value so a request that changed nothing (rejected SET_PIN,
    // Klippy not ready) snaps the switch back to the truth.
    if (auto* s = lv_xml_get_subject(nullptr, "chamber_filter_fan_on")) {
        lv_subject_notify(s);
    }
}

// Chamber filament dryer (#1299): Start opens the preset modal, Stop ends the
// cycle, both through the globally registered TemperatureController.
void TemperatureService::on_chamber_dryer_start_clicked(lv_event_t* /*e*/) {
    helix::ui::ChamberDryerModal::show_owned();
}

void TemperatureService::on_chamber_dryer_stop_clicked(lv_event_t* /*e*/) {
    if (auto* tc = get_temperature_controller()) {
        tc->stop_chamber_drying();
    } else {
        spdlog::warn("[TempPanel] chamber dryer stop clicked with no controller registered");
    }
}

// ============================================================================
// MULTI-EXTRUDER SUPPORT
// ============================================================================

void TemperatureService::select_extruder(const std::string& name) {
    if (name == active_extruder_name_) {
        return;
    }

    if (!subjects_initialized_) {
        return;
    }

    spdlog::info("[TempPanel] Switching extruder: {} -> {}", active_extruder_name_, name);
    active_extruder_name_ = name;

    // Sync the global active extruder subjects (extruder_temp/extruder_target)
    // so XML-bound elements (temp_display, nozzle_icon) update to the selected tool
    printer_state_.temperature_state().set_active_extruder(name);

    auto& nozzle = heaters_[idx(HeaterType::Nozzle)];

    // Rebind nozzle observers to the selected extruder's subjects
    SubjectLifetime temp_lt, target_lt;
    auto* temp_subj = printer_state_.temperature_state().get_extruder_temp_subject(name, temp_lt);
    auto* target_subj =
        printer_state_.temperature_state().get_extruder_target_subject(name, target_lt);

    if (temp_subj) {
        nozzle.temp_observer = observe<int>(
            temp_subj, this,
            [](TemperatureService* self, int temp) {
                self->on_temp_changed(HeaterType::Nozzle, temp);
            },
            temp_lt);
        nozzle.current = lv_subject_get_int(temp_subj);
    }
    if (target_subj) {
        nozzle.target_observer = observe<int>(
            target_subj, this,
            [](TemperatureService* self, int target) {
                self->on_target_changed(HeaterType::Nozzle, target);
            },
            target_lt);
        nozzle.target = lv_subject_get_int(target_subj);
    }

    update_display(HeaterType::Nozzle);
    update_status(HeaterType::Nozzle);

    // Rebuild the filament panel's mini graph against the new extruder
    // (#9). Without this the controller stays bound to whichever extruder
    // was active at setup_mini_combined_graph() time — typically T0, since
    // the setup runs at panel construction — and the chart shows T0's cold
    // baseline while the user is actively heating T1.
    if (mini_graph_controller_ && mini_graph_container_ && lv_obj_is_valid(mini_graph_container_)) {
        mini_graph_controller_.reset();
        setup_mini_combined_graph(mini_graph_container_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mini Combined Graph (for FilamentPanel)
// ─────────────────────────────────────────────────────────────────────────────

void TemperatureService::setup_mini_combined_graph(lv_obj_t* container) {
    if (!container) {
        spdlog::warn("[TempPanel] setup_mini_combined_graph: null container");
        return;
    }
    // Remember the container so select_extruder() can rebuild the mini
    // graph against the new active extruder. Without this the graph is
    // pinned to whichever extruder was active at first setup.
    mini_graph_container_ = container;

    static constexpr int kMiniGraphPoints = 300;

    helix::TempGraphControllerConfig config;
    config.point_count = kMiniGraphPoints;
    config.axis_size = "xs";
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES |
                        TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS |
                        TEMP_GRAPH_FEATURE_GRADIENTS | TEMP_GRAPH_FEATURE_TARGET_HISTORY;
    // At MICRO/TINY the graph is pinned to ~100px — the X-axis time labels eat
    // roughly a fifth of that for minimal value, so drop them to give the data
    // lines more room to breathe.
    if (auto* bp_subj = theme_manager_get_breakpoint_subject()) {
        UiBreakpoint bp = as_breakpoint(lv_subject_get_int(bp_subj));
        if (bp == UiBreakpoint::Micro || bp == UiBreakpoint::Tiny) {
            features &= ~TEMP_GRAPH_FEATURE_X_AXIS;
        }
    }
    config.initial_features = features;
    {
        helix::TempGraphSeriesSpec nozzle_spec;
        nozzle_spec.klipper_name = active_extruder_name_;
        nozzle_spec.color = heaters_[idx(HeaterType::Nozzle)].config.color;
        nozzle_spec.show_target = true;
        // Source the localized "Nozzle [N]" label from PrinterTemperatureState
        // so the mini-graph legend matches the rest of the temp UI instead of
        // showing the raw Klipper "extruder" / "extruderN" identifier.
        const auto& exts = printer_state_.temperature_state().extruders();
        auto it = exts.find(active_extruder_name_);
        if (it != exts.end() && !it->second.display_name.empty())
            nozzle_spec.display_name = it->second.display_name;
        helix::TempGraphSeriesSpec bed_spec;
        bed_spec.klipper_name = "heater_bed";
        bed_spec.display_name = lv_tr("Bed");
        bed_spec.color = heaters_[idx(HeaterType::Bed)].config.color;
        bed_spec.show_target = true;
        config.series = {std::move(nozzle_spec), std::move(bed_spec)};
    }

    // Add chamber series if printer has a chamber heater or sensor
    {
        const auto& chamber = heaters_[idx(HeaterType::Chamber)];
        auto* heater_subj =
            printer_state_.capabilities_state().subject(Capability::HasChamberHeater);
        bool has_heater = heater_subj && lv_subject_get_int(heater_subj) != 0;
        // One source for the reading, so this series cannot disagree with the
        // chamber readout about which probe it means. A target line needs a
        // heater behind it: a sensor-only chamber has a temperature and
        // nothing to set.
        const auto& temp_state = printer_state_.temperature_state();
        const std::string& klipper = temp_state.chamber_temperature_source();
        if (!klipper.empty()) {
            helix::TempGraphSeriesSpec spec;
            spec.klipper_name = klipper;
            spec.display_name = lv_tr("Chamber");
            spec.color = chamber.config.color;
            spec.show_target = has_heater && !temp_state.chamber_heater_name().empty();
            config.series.push_back(std::move(spec));
        }
    }

    mini_graph_controller_ =
        std::make_unique<helix::TempGraphController>(container, std::move(config));

    spdlog::debug("[TempPanel] Mini combined graph created with {} point capacity",
                  kMiniGraphPoints);
}

void TemperatureService::register_heater_graph(ui_temp_graph_t* graph, SeriesId series_id,
                                               const std::string& heater) {
    if (heater.rfind("extruder", 0) == 0) {
        heaters_[idx(HeaterType::Nozzle)].temp_graphs.push_back({graph, series_id});
    } else if (heater == "heater_bed") {
        heaters_[idx(HeaterType::Bed)].temp_graphs.push_back({graph, series_id});
    } else if (heater.find("chamber") != std::string::npos) {
        heaters_[idx(HeaterType::Chamber)].temp_graphs.push_back({graph, series_id});
    }
    spdlog::debug("[TempPanel] Registered external graph for {}", heater);
}

void TemperatureService::unregister_heater_graph(ui_temp_graph_t* graph) {
    auto remove_from = [graph](std::vector<HeaterState::RegisteredGraph>& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [graph](const HeaterState::RegisteredGraph& rg) {
                                     return rg.graph == graph;
                                 }),
                  vec.end());
    };
    for (auto& h : heaters_) {
        remove_from(h.temp_graphs);
    }
    spdlog::debug("[TempPanel] Unregistered external graph");
}
