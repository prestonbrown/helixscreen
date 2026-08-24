// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_environment_overlay.cpp
 * @brief Implementation of AmsEnvironmentOverlay (environment detail + dryer controls)
 */

#include "ui_ams_environment_overlay.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_keyboard_manager.h"
#include "ui_nav_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
#include "data_root_resolver.h"
#include "filament_database.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "preset_materials.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>

namespace helix::ui {

namespace {
// Clamp a preset's nominal dry temperature to a box's reported settable range so
// the displayed value matches what start_drying() will actually send.
float clamp_preset_temp(float temp_c, const DryerInfo& dryer) {
    if (dryer.max_temp_c > 0.0f && temp_c > dryer.max_temp_c) {
        temp_c = dryer.max_temp_c;
    }
    if (dryer.min_temp_c > 0.0f && temp_c < dryer.min_temp_c) {
        temp_c = dryer.min_temp_c;
    }
    return temp_c;
}
} // namespace

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AmsEnvironmentOverlay> g_ams_environment_overlay;

AmsEnvironmentOverlay& get_ams_environment_overlay() {
    if (!g_ams_environment_overlay) {
        g_ams_environment_overlay = std::make_unique<AmsEnvironmentOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "AmsEnvironmentOverlay", []() { g_ams_environment_overlay.reset(); });
    }
    return *g_ams_environment_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AmsEnvironmentOverlay::AmsEnvironmentOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AmsEnvironmentOverlay::~AmsEnvironmentOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

std::vector<std::string> AmsEnvironmentOverlay::fallback_comfort_materials() {
    std::vector<std::string> out;
    for (const auto& m : helix::presets::all()) {
        if (!m.empty()) {
            out.push_back(m);
        }
    }
    return out;
}

void AmsEnvironmentOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(temp_text_subject_, temp_text_buf_, "--",
                                  "ams_env_overlay_temp_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(target_temp_text_subject_, target_temp_text_buf_, "",
                                  "ams_env_overlay_target_temp_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(humidity_text_subject_, humidity_text_buf_, "--",
                                  "ams_env_overlay_humidity_text", subjects_);
        UI_MANAGED_SUBJECT_INT(humidity_visible_subject_, 0, "ams_env_overlay_humidity_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_STRING(title_text_subject_, title_text_buf_, "",
                                  "ams_env_overlay_title_text", subjects_);
        UI_MANAGED_SUBJECT_INT(dryer_visible_subject_, 0, "ams_env_overlay_dryer_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(no_dryer_visible_subject_, 0, "ams_env_overlay_no_dryer_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(drying_active_subject_, 0, "ams_env_overlay_drying_active",
                               subjects_);
        UI_MANAGED_SUBJECT_STRING(drying_text_subject_, drying_text_buf_, "",
                                  "ams_env_overlay_drying_text", subjects_);
        UI_MANAGED_SUBJECT_INT(drying_progress_subject_, 0, "ams_env_overlay_drying_progress",
                               subjects_);
        // Per-material comfort row subjects (4 rows max)
        for (int i = 0; i < MAX_COMFORT_ROWS; ++i) {
            char name[48];
            snprintf(name, sizeof(name), "ams_env_comfort_%d_visible", i);
            UI_MANAGED_SUBJECT_INT(comfort_visible_[i], 0, name, subjects_);

            snprintf(name, sizeof(name), "ams_env_comfort_%d_status", i);
            UI_MANAGED_SUBJECT_INT(comfort_status_[i], 0, name, subjects_);

            snprintf(name, sizeof(name), "ams_env_comfort_%d_text", i);
            UI_MANAGED_SUBJECT_STRING(comfort_text_[i], comfort_text_buf_[i], "", name, subjects_);
        }
        UI_MANAGED_SUBJECT_STRING(start_stop_text_subject_, start_stop_text_buf_,
                                  lv_tr("Start Drying"), "ams_env_overlay_start_stop_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(preset_text_subject_, preset_text_buf_, "",
                                  "ams_env_overlay_preset_text", subjects_);
    });
}

void AmsEnvironmentOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_ams_env_start_stop_clicked", on_start_stop_clicked);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AmsEnvironmentOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Self-register our XML component if a host panel hasn't already. Makes the
    // overlay safe to open directly (e.g. CLI --ams-environment) without first
    // visiting the AMS panel, which is where lazy registration otherwise happens.
    if (!lv_xml_component_get_scope("ams_environment_overlay")) {
        lv_xml_register_component_from_file(
            helix::asset_component_uri("ui_xml/ams_environment_overlay.xml").c_str());
    }

    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_environment_overlay", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find widget pointers for dryer controls
    preset_dropdown_ = lv_obj_find_by_name(overlay_, "preset_dropdown");
    temp_input_ = lv_obj_find_by_name(overlay_, "temp_input");
    duration_input_ = lv_obj_find_by_name(overlay_, "duration_input");
    temp_range_label_ = lv_obj_find_by_name(overlay_, "temp_range_label");

    // Register textareas with keyboard manager for on-screen numeric input
    if (temp_input_) {
        KeyboardManager::instance().register_textarea(temp_input_);
    }
    if (duration_input_) {
        KeyboardManager::instance().register_textarea(duration_input_);
    }

    // Register preset dropdown change callback imperatively (dropdown not in XML event_cb)
    if (preset_dropdown_) {
        lv_obj_add_event_cb(preset_dropdown_, on_preset_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

// ============================================================================
// SHOW / REFRESH
// ============================================================================

void AmsEnvironmentOverlay::show(lv_obj_t* parent_screen, int unit_index) {
    spdlog::debug("[{}] show() called for unit {}", get_name(), unit_index);

    parent_screen_ = parent_screen;
    unit_index_ = unit_index;
    AmsState::instance().set_dryer_mirror_unit(unit_index);

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    refresh();

    // Restore the temp/duration the user last started a dry with (per-printer),
    // so the inputs default to remembered values rather than the XML constants.
    Config* config = Config::get_instance();
    if (temp_input_) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d",
                 config->get<int>(config->df() + "ams/dryer_last_temp", 55));
        lv_textarea_set_text(temp_input_, buf);
    }
    if (duration_input_) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d",
                 config->get<int>(config->df() + "ams/dryer_last_duration", 240));
        lv_textarea_set_text(duration_input_, buf);
    }

    NavigationManager::instance().register_overlay_instance(overlay_, this);
    NavigationManager::instance().push_overlay(overlay_);
}

void AmsEnvironmentOverlay::refresh() {
    if (!overlay_) {
        return;
    }

    spdlog::debug("[{}] Refreshing from backend", get_name());
    update_from_backend();
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void AmsEnvironmentOverlay::on_activate() {
    OverlayBase::on_activate();

    // Keep the readouts live while the overlay is open. Environment temperature/
    // humidity and dryer state do NOT bump AmsState's slots_version (that tracks
    // slot data only), so observe the per-unit environment indicator text subjects
    // and the system dryer subjects directly; each re-pulls via refresh() on change.
    // update_from_backend() only writes subject strings/ints — no synchronous widget
    // deletion — so running it inside the deferred observer callbacks is safe.
    using helix::ui::observe_int_sync;
    using helix::ui::observe_string;
    auto& ams = AmsState::instance();

    auto refresh_if_visible = [](AmsEnvironmentOverlay* self) {
        if (self->is_visible()) {
            self->refresh();
        }
    };

    if (auto* s = ams.get_env_ind_temp_text_subject(unit_index_)) {
        env_temp_observer_ = observe_string<AmsEnvironmentOverlay>(
            s, this, [refresh_if_visible](AmsEnvironmentOverlay* self, const char*) {
                refresh_if_visible(self);
            });
    }
    if (auto* s = ams.get_env_ind_humidity_text_subject(unit_index_)) {
        env_humidity_observer_ = observe_string<AmsEnvironmentOverlay>(
            s, this, [refresh_if_visible](AmsEnvironmentOverlay* self, const char*) {
                refresh_if_visible(self);
            });
    }
    if (auto* s = ams.get_dryer_active_subject()) {
        dryer_active_observer_ = observe_int_sync<AmsEnvironmentOverlay>(
            s, this,
            [refresh_if_visible](AmsEnvironmentOverlay* self, int) { refresh_if_visible(self); });
    }
    if (auto* s = ams.get_dryer_current_temp_subject()) {
        dryer_temp_observer_ = observe_int_sync<AmsEnvironmentOverlay>(
            s, this,
            [refresh_if_visible](AmsEnvironmentOverlay* self, int) { refresh_if_visible(self); });
    }

    // Pull current state immediately on activation.
    refresh();

    spdlog::trace("[{}] Activated", get_name());
}

void AmsEnvironmentOverlay::on_deactivate() {
    OverlayBase::on_deactivate();

    env_temp_observer_.reset();
    env_humidity_observer_.reset();
    dryer_active_observer_.reset();
    dryer_temp_observer_.reset();

    spdlog::debug("[{}] Deactivated", get_name());
}

// ============================================================================
// BACKEND QUERIES
// ============================================================================

void AmsEnvironmentOverlay::update_from_backend() {
    AmsBackend* backend = AmsState::instance().get_backend();

    if (!backend) {
        spdlog::warn("[{}] No backend available", get_name());
        snprintf(title_text_buf_, sizeof(title_text_buf_), "%s",
                 lv_tr("No Multi-Filament System connected"));
        lv_subject_copy_string(&title_text_subject_, title_text_buf_);
        lv_subject_set_int(&dryer_visible_subject_, 0);
        lv_subject_set_int(&no_dryer_visible_subject_, 1);
        lv_subject_set_int(&drying_active_subject_, 0);
        lv_subject_set_int(&humidity_visible_subject_, 0);
        return;
    }

    // Get system info for unit data
    auto info = backend->get_system_info();

    // Build title text (e.g., "CFS Unit 1" or "AMS Unit 1")
    if (unit_index_ < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[unit_index_];
        if (!unit.display_name.empty()) {
            snprintf(title_text_buf_, sizeof(title_text_buf_), "%s", unit.display_name.c_str());
        } else {
            snprintf(title_text_buf_, sizeof(title_text_buf_), "%s %s %d", info.type_name.c_str(),
                     lv_tr("Unit"), unit_index_ + 1);
        }
    } else {
        snprintf(title_text_buf_, sizeof(title_text_buf_), "%s %s %d", info.type_name.c_str(),
                 lv_tr("Unit"), unit_index_ + 1);
    }
    lv_subject_copy_string(&title_text_subject_, title_text_buf_);

    // Get environment data from unit
    float temp_c = 0.0f;
    float humidity_pct = 0.0f;
    bool has_env = false;
    bool has_humidity = false;

    if (unit_index_ < static_cast<int>(info.units.size())) {
        const auto& unit = info.units[unit_index_];
        if (unit.environment.has_value()) {
            temp_c = unit.environment->temperature_c;
            humidity_pct = unit.environment->humidity_pct;
            has_humidity = unit.environment->has_humidity;
            has_env = true;
        }
    }

    // Get dryer info
    DryerInfo dryer = backend->get_dryer_info(unit_index_);

    // Update temperature display (current temp always in temp_text, target in target_temp_text)
    if (has_env) {
        float display_temp =
            (dryer.active && dryer.current_temp_c > 0) ? dryer.current_temp_c : temp_c;
        snprintf(temp_text_buf_, sizeof(temp_text_buf_),
                 "%.0f\xC2\xB0"
                 "C",
                 display_temp);
    } else {
        snprintf(temp_text_buf_, sizeof(temp_text_buf_), "--");
    }
    lv_subject_copy_string(&temp_text_subject_, temp_text_buf_);

    // Target temp (shown via XML visibility binding when drying_active=1)
    if (dryer.active && dryer.target_temp_c > 0) {
        snprintf(target_temp_text_buf_, sizeof(target_temp_text_buf_),
                 "%.0f\xC2\xB0"
                 "C",
                 dryer.target_temp_c);
    } else {
        target_temp_text_buf_[0] = '\0';
    }
    lv_subject_copy_string(&target_temp_text_subject_, target_temp_text_buf_);

    // Update humidity display (show "--" if no sensor, i.e. 0%)
    if (has_env && humidity_pct > 0) {
        snprintf(humidity_text_buf_, sizeof(humidity_text_buf_), "%.0f%%", humidity_pct);
    } else {
        snprintf(humidity_text_buf_, sizeof(humidity_text_buf_), "--");
    }
    lv_subject_copy_string(&humidity_text_subject_, humidity_text_buf_);

    // Humidity readout + Material Comfort strip, for THIS unit. Matches the
    // badge's own rule (AmsState env_ind_humidity_visible): a reading exists
    // only when the unit reports an environment with a humidity sensor.
    lv_subject_set_int(&humidity_visible_subject_, (has_env && has_humidity) ? 1 : 0);

    // Dryer visibility
    lv_subject_set_int(&dryer_visible_subject_, dryer.supported ? 1 : 0);
    lv_subject_set_int(&no_dryer_visible_subject_, dryer.supported ? 0 : 1);

    // Drying active state
    lv_subject_set_int(&drying_active_subject_, dryer.active ? 1 : 0);

    if (dryer.active) {
        int hours = dryer.remaining_min / 60;
        int mins = dryer.remaining_min % 60;
        snprintf(drying_text_buf_, sizeof(drying_text_buf_), "%s: %d:%02d %s", lv_tr("Drying"),
                 hours, mins, lv_tr("left"));
        lv_subject_copy_string(&drying_text_subject_, drying_text_buf_);

        int progress = dryer.get_progress_pct();
        lv_subject_set_int(&drying_progress_subject_, progress >= 0 ? progress : 0);

        // Button text when drying
        snprintf(start_stop_text_buf_, sizeof(start_stop_text_buf_), "%s", lv_tr("Stop Drying"));
    } else {
        snprintf(start_stop_text_buf_, sizeof(start_stop_text_buf_), "%s", lv_tr("Start Drying"));
    }
    lv_subject_copy_string(&start_stop_text_subject_, start_stop_text_buf_);

    // Material comfort ranges
    update_comfort_text(humidity_pct);

    // Update temp range label with backend limits (e.g., "Temp °C (35-55)")
    if (temp_range_label_ && dryer.supported) {
        char range_buf[32];
        snprintf(range_buf, sizeof(range_buf), lv_tr("Temp °C (%d–%d)"),
                 static_cast<int>(dryer.min_temp_c), static_cast<int>(dryer.max_temp_c));
        lv_label_set_text(temp_range_label_, range_buf);
    }

    // Populate dryer presets and auto-select based on loaded materials
    if (dryer.supported) {
        populate_presets();
        auto_select_preset();
    }
}

// ============================================================================
// MATERIAL COMFORT RANGES
// ============================================================================

void AmsEnvironmentOverlay::update_comfort_text(float humidity_pct) {
    // Without a live humidity reading the comfort verdict is meaningless: a 0%
    // reading would compare <= every material's "good" ceiling and render a
    // fabricated all-"OK" panel (e.g. boxes with a heater but no humidity
    // sensor — common on the QIDI box). Hide the rows entirely so the overlay
    // shows "--" for humidity and no comfort claim, rather than a false all-clear.
    if (humidity_pct <= 0.0f) {
        for (int i = 0; i < MAX_COMFORT_ROWS; ++i) {
            lv_subject_set_int(&comfort_visible_[i], 0);
        }
        return;
    }

    // Collect unique materials loaded in slots
    std::vector<std::string> loaded_materials;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        if (unit_index_ < static_cast<int>(info.units.size())) {
            const auto& unit = info.units[unit_index_];
            for (int i = 0; i < unit.slot_count; ++i) {
                int gi = unit.first_slot_global_index + i;
                SlotInfo slot = backend->get_slot_info(gi);
                if (!slot.material.empty()) {
                    bool already_listed = false;
                    for (const auto& m : loaded_materials) {
                        if (m == slot.material) {
                            already_listed = true;
                            break;
                        }
                    }
                    if (!already_listed) {
                        loaded_materials.push_back(slot.material);
                    }
                }
            }
        }
    }

    // Fall back to the user's configured quick-preset materials if no slots are
    // loaded. Previously a fourth hardcoded list ({"PLA","PETG","ABS","PA"})
    // that disagreed with every other preset copy in the codebase.
    if (loaded_materials.empty()) {
        loaded_materials = fallback_comfort_materials();
    }

    // Update subjects for each comfort row (up to MAX_COMFORT_ROWS)
    int row_idx = 0;
    for (const auto& mat_name : loaded_materials) {
        if (row_idx >= MAX_COMFORT_ROWS)
            break;

        const auto range = filament::get_comfort_range(mat_name.c_str());
        if (!range)
            continue;

        const char* status_text;
        int status_val; // 0=OK, 1=Marginal, 2=Too humid
        if (humidity_pct <= range->max_humidity_good) {
            status_text = lv_tr("OK");
            status_val = 0;
        } else if (humidity_pct <= range->max_humidity_warn) {
            status_text = lv_tr("Marginal");
            status_val = 1;
        } else {
            status_text = lv_tr("Too humid");
            status_val = 2;
        }

        // Set status subject (controls which icon variant is visible in XML)
        lv_subject_set_int(&comfort_status_[row_idx], status_val);

        // Set text subject
        snprintf(comfort_text_buf_[row_idx], sizeof(comfort_text_buf_[row_idx]),
                 "%s: %s (< %.0f%%)", mat_name.c_str(), status_text, range->max_humidity_good);
        lv_subject_copy_string(&comfort_text_[row_idx], comfort_text_buf_[row_idx]);

        // Show row
        lv_subject_set_int(&comfort_visible_[row_idx], 1);

        row_idx++;
    }

    // Hide unused rows
    for (int i = row_idx; i < MAX_COMFORT_ROWS; ++i) {
        lv_subject_set_int(&comfort_visible_[i], 0);
    }
}

// ============================================================================
// DRYER PRESETS
// ============================================================================

void AmsEnvironmentOverlay::populate_presets() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || !preset_dropdown_) {
        return;
    }

    cached_presets_ = backend->get_drying_presets();

    // The box's max settable temp clamps both the dropdown label and the applied
    // value, so a preset whose nominal dry temp exceeds this box's ceiling (e.g.
    // PA at 70°C on a 65°C box) displays and sends the same clamped value.
    DryerInfo dryer = backend->get_dryer_info(unit_index_);

    // Build dropdown options string (newline-separated)
    std::string options;
    for (const auto& preset : cached_presets_) {
        if (!options.empty()) {
            options += '\n';
        }
        char buf[64];
        int hours = preset.duration_min / 60;
        snprintf(buf, sizeof(buf), "%s %g°C/%dh", preset.name.c_str(),
                 clamp_preset_temp(preset.temp_c, dryer), hours);
        options += buf;
    }

    // Add Custom option
    if (!options.empty()) {
        options += '\n';
    }
    options += lv_tr("Custom...");

    lv_dropdown_set_options(preset_dropdown_, options.c_str());

    spdlog::debug("[{}] Populated {} presets", get_name(), cached_presets_.size());
}

void AmsEnvironmentOverlay::apply_preset(int index) {
    if (index < 0 || index >= static_cast<int>(cached_presets_.size())) {
        // "Custom..." selected — leave spinboxes as-is
        return;
    }

    const auto& preset = cached_presets_[index];

    // Clamp to the box's reported limits so the value shown in the field matches
    // what Start Drying will actually send (see populate_presets()).
    float applied_temp = preset.temp_c;
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        applied_temp = clamp_preset_temp(preset.temp_c, backend->get_dryer_info(unit_index_));
    }

    if (temp_input_) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", static_cast<int>(applied_temp));
        lv_textarea_set_text(temp_input_, buf);
    }
    if (duration_input_) {
        // Duration field is in minutes (HH's MMU_HEATER TIMER takes minutes).
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", preset.duration_min);
        lv_textarea_set_text(duration_input_, buf);
    }

    spdlog::debug("[{}] Applied preset: {} ({}°C, {}min)", get_name(), preset.name, preset.temp_c,
                  preset.duration_min);
}

void AmsEnvironmentOverlay::auto_select_preset() {
    if (cached_presets_.empty() || !preset_dropdown_) {
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        return;
    }

    // Collect loaded materials and find the most conservative drying preset
    auto info = backend->get_system_info();
    if (unit_index_ >= static_cast<int>(info.units.size())) {
        return;
    }

    const auto& unit = info.units[unit_index_];
    float lowest_dry_temp = 999.0f;
    std::string best_match_name;

    for (int i = 0; i < unit.slot_count; ++i) {
        int gi = unit.first_slot_global_index + i;
        SlotInfo slot = backend->get_slot_info(gi);
        if (slot.material.empty())
            continue;

        const auto range = filament::get_comfort_range(slot.material);
        if (range && range->dry_temp_c > 0 && range->dry_temp_c < lowest_dry_temp) {
            lowest_dry_temp = static_cast<float>(range->dry_temp_c);
            best_match_name = slot.material;
        }
    }

    if (best_match_name.empty()) {
        return;
    }

    // Find matching preset by temperature (most conservative)
    int best_idx = -1;
    float best_diff = 999.0f;
    for (int i = 0; i < static_cast<int>(cached_presets_.size()); ++i) {
        float diff = std::abs(cached_presets_[i].temp_c - lowest_dry_temp);
        if (diff < best_diff) {
            best_diff = diff;
            best_idx = i;
        }
    }

    if (best_idx >= 0) {
        lv_dropdown_set_selected(preset_dropdown_, static_cast<uint32_t>(best_idx));
        apply_preset(best_idx);
        spdlog::debug("[{}] Auto-selected preset '{}' for loaded material '{}' (dry temp {}°C)",
                      get_name(), cached_presets_[best_idx].name, best_match_name, lowest_dry_temp);
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AmsEnvironmentOverlay::on_start_stop_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsEnvironmentOverlay] on_start_stop_clicked");
    LV_UNUSED(e);

    auto& overlay = get_ams_environment_overlay();
    AmsBackend* backend = AmsState::instance().get_backend();

    if (!backend) {
        NOTIFY_WARNING("{}", lv_tr("No Multi-Filament System connected"));
    } else {
        DryerInfo dryer = backend->get_dryer_info(overlay.unit_index_);

        if (dryer.active) {
            // Stop drying
            spdlog::info("[AmsEnvironmentOverlay] Stopping drying");
            AmsError result = backend->stop_drying(overlay.unit_index_);
            if (result.success()) {
                NOTIFY_INFO("{}", lv_tr("Drying stopped"));
            } else {
                helix::ui::notify_ams_error(result, lv_tr("Stop failed"));
            }
        } else {
            // Start drying with textarea values, clamped to backend limits.
            // Duration field is in minutes (HH's MMU_HEATER TIMER takes minutes,
            // minval=0, so sub-hour cycles are valid).
            // Defaults remember the last values the user started a dry with
            // (per-printer); fall back to 55°C / 240 min on first use.
            Config* config = Config::get_instance();
            float temp_c =
                static_cast<float>(config->get<int>(config->df() + "ams/dryer_last_temp", 55));
            int duration_min = config->get<int>(config->df() + "ams/dryer_last_duration", 240);

            if (overlay.temp_input_) {
                const char* text = lv_textarea_get_text(overlay.temp_input_);
                if (text && text[0])
                    temp_c = static_cast<float>(atoi(text));
            }
            if (overlay.duration_input_) {
                const char* text = lv_textarea_get_text(overlay.duration_input_);
                if (text && text[0])
                    duration_min = atoi(text);
            }

            // Clamp temperature to backend-reported limits
            if (temp_c < dryer.min_temp_c)
                temp_c = dryer.min_temp_c;
            if (temp_c > dryer.max_temp_c)
                temp_c = dryer.max_temp_c;

            // Clamp duration to backend limit
            if (duration_min > dryer.max_duration_min)
                duration_min = dryer.max_duration_min;
            if (duration_min <= 0)
                duration_min = 240;

            spdlog::info("[AmsEnvironmentOverlay] Starting drying: {}°C for {}min", temp_c,
                         duration_min);

            AmsError result = backend->start_drying(temp_c, duration_min, -1, overlay.unit_index_);
            if (result.success()) {
                // Remember the chosen values so the next open restores them.
                config->set<int>(config->df() + "ams/dryer_last_temp", static_cast<int>(temp_c));
                config->set<int>(config->df() + "ams/dryer_last_duration", duration_min);
                config->save();
                NOTIFY_INFO("{}", lv_tr("Drying started"));
            } else {
                helix::ui::notify_ams_error(result, lv_tr("Start failed"));
            }
        }

        overlay.refresh();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void AmsEnvironmentOverlay::on_preset_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AmsEnvironmentOverlay] on_preset_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown || !lv_obj_is_valid(dropdown)) {
        spdlog::warn("[AmsEnvironmentOverlay] on_preset_changed: invalid target");
    } else {
        int selected = static_cast<int>(lv_dropdown_get_selected(dropdown));
        spdlog::debug("[AmsEnvironmentOverlay] Preset selected: {}", selected);
        get_ams_environment_overlay().apply_preset(selected);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void ensure_ams_env_indicator_registered() {
    static bool s_registered = false;
    if (s_registered) {
        return;
    }

    // The badge's click callback and component file are shared by the single-unit
    // AmsPanel and the multi-unit overview; whichever runs first registers them.
    // The event_cb registration is last-wins-safe, but a second component
    // registration would orphan the first scope node (lv_xml_register_component_from_data
    // does no dedup), so the guard makes this happen exactly once per process.
    lv_xml_register_event_cb(nullptr, "on_env_indicator_clicked", [](lv_event_t* e) {
        auto* ind = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        int unit =
            ind ? static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(ind))) : 0;
        spdlog::info("[AMS Environment] Indicator clicked — opening overlay for unit {}", unit);
        auto& overlay = get_ams_environment_overlay();
        overlay.show(lv_screen_active(), unit);
    });

    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/components/ams_environment_indicator.xml").c_str());

    s_registered = true;
}

} // namespace helix::ui
