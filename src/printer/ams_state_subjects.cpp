// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_subjects.cpp
 * @brief AmsState: subject storage: init, XML publication, teardown and the subject getters
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_state_internal.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace helix {
using ams_state_detail::assert_main_thread;
using ams_state_detail::slot_error_state;

namespace {

/// @p subject with @p lifetime set to @p owner_token, or nullptr with an
/// emptied token when there is no subject to observe.
lv_subject_t* with_lifetime(lv_subject_t* subject, SubjectLifetime owner_token,
                            SubjectLifetime& lifetime) {
    if (subject) {
        lifetime = std::move(owner_token);
    } else {
        lifetime.reset();
    }
    return subject;
}

} // namespace

void AmsState::init_subjects(bool register_xml) {
    assert_main_thread();

    if (initialized_) {
        // Re-entry on the shared singleton: always rebind the print-state
        // observer to PrinterState's *current* print_state_enum subject.
        //
        // A `if (!print_state_observer_)` guard is NOT sufficient: PrinterState
        // can deinit+reinit its subjects between cases (LVGLUITestFixture does
        // this; production soft-restart can too). lv_subject_deinit() frees our
        // observer node, but ObserverGuard::operator bool() only checks for a
        // non-null observer pointer — it stays truthy with a dangling pointer to
        // the freed/recreated subject, so the guard would skip re-install and
        // the label would never recompute on print-state changes.
        //
        // install_print_state_observer() is idempotent (reset()s the prior
        // guard, then rebinds to the current subject with the current lifetime
        // token), so calling it unconditionally is safe and self-healing.
        //
        // register_xml is honored on re-entry too: the first init may have run
        // with false and published no names, and skipping them here would keep
        // lv_xml_get_subject() null for the rest of the process.
        // register_xml_subject_names() must mirror the first-init registration
        // list below — a name added there must be added here too.
        install_print_state_observer();
        if (register_xml) {
            register_xml_subject_names();
        }
        return;
    }

    spdlog::trace("[AMS State] Initializing subjects");

    // Backend selector subjects
    INIT_SUBJECT_INT(backend_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_data_revision, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(active_backend, 0, subjects_, register_xml);

    // System-level subjects
    INIT_SUBJECT_INT(ams_type, static_cast<int>(AmsType::NONE), subjects_, register_xml);
    INIT_SUBJECT_INT(ams_is_tool_changer, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_is_filament_system, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_action, static_cast<int>(AmsAction::IDLE), subjects_, register_xml);
    action_mirror_.store(AmsAction::IDLE, std::memory_order_relaxed);
    // Granular load/unload sub-phase (Snapmaker U1). -1 = no active step.
    INIT_SUBJECT_INT(ams_operation_phase, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_operation_indeterminate, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(toolchange_step, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(current_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(pending_target_slot, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_current_tool, -1, subjects_, register_xml);
    // These subjects need ams_ prefix for XML but member vars don't have it
    lv_subject_init_int(&filament_loaded_, 0);
    subjects_.register_subject(&filament_loaded_, register_xml ? "ams_filament_loaded" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_filament_loaded", &filament_loaded_);

    lv_subject_init_int(&filament_runout_, 0);
    subjects_.register_subject(&filament_runout_, register_xml ? "ams_filament_runout" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_filament_runout", &filament_runout_);

    lv_subject_init_int(&bypass_active_, 0);
    subjects_.register_subject(&bypass_active_, register_xml ? "ams_bypass_active" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_bypass_active", &bypass_active_);

    // External spool color subject (loaded from persistent settings)
    {
        auto ext_spool = helix::SettingsManager::instance().get_external_spool_info();
        int initial_color = ext_spool.has_value() ? static_cast<int>(ext_spool->color_rgb) : 0;
        lv_subject_init_int(&external_spool_color_, initial_color);
        subjects_.register_subject(&external_spool_color_,
                                   register_xml ? "ams_external_spool_color" : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope("ams_external_spool_color",
                                                          &external_spool_color_);

        // Material string flavor — same source, string subject idiom as
        // ams_system_name_ (own buffer, nullptr prev_buf).
        lv_subject_init_string(&external_spool_material_, external_spool_material_buf_, nullptr,
                               sizeof(external_spool_material_buf_),
                               ext_spool.has_value() ? ext_spool->material.c_str() : "");
        subjects_.register_subject(&external_spool_material_,
                                   register_xml ? "ams_external_spool_material" : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope("ams_external_spool_material",
                                                          &external_spool_material_);
    }

    lv_subject_init_int(&supports_bypass_, 0);
    subjects_.register_subject(&supports_bypass_, register_xml ? "ams_supports_bypass" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_supports_bypass", &supports_bypass_);
    INIT_SUBJECT_INT(ams_slot_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_cards_compact, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(slots_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(tool_map_version, 0, subjects_, register_xml);
    // Default 1 (present) so non-auto-feed / unknown backends never gate Resume (#991).
    INIT_SUBJECT_INT(active_tool_port_present, 1, subjects_, register_xml);

    // String subjects (buffer names don't match macro convention)
    lv_subject_init_string(&ams_action_detail_, action_detail_buf_, nullptr,
                           sizeof(action_detail_buf_), "");
    subjects_.register_subject(&ams_action_detail_, register_xml ? "ams_action_detail" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_action_detail", &ams_action_detail_);

    lv_subject_init_string(&ams_system_name_, system_name_buf_, nullptr, sizeof(system_name_buf_),
                           "");
    subjects_.register_subject(&ams_system_name_, register_xml ? "ams_system_name" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_system_name", &ams_system_name_);

    // Logo uses pointer subject — bind_src expects a pointer to the path string buffer.
    // Init to nullptr so XML bind_src doesn't fire lv_image_set_src("") warnings before
    // sync_from_backend populates the real logo path.
    lv_subject_init_pointer(&ams_system_logo_, nullptr);
    subjects_.register_subject(&ams_system_logo_, register_xml ? "ams_system_logo" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_system_logo", &ams_system_logo_);

    INIT_SUBJECT_STRING(ams_current_tool_text, "---", subjects_, register_xml);

    // Endless-spool status. Starts Hidden with no text so a printer whose
    // backend never reports the feature renders nothing rather than flashing a
    // default sentence before the first sync.
    INIT_SUBJECT_INT(ams_endless_state,
                     static_cast<int>(helix::printer::EndlessSpoolStatusKind::Hidden), subjects_,
                     register_xml);
    lv_subject_init_string(&ams_endless_text_, ams_endless_text_buf_, nullptr,
                           sizeof(ams_endless_text_buf_), "");
    subjects_.register_subject(&ams_endless_text_, register_xml ? "ams_endless_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_endless_text", &ams_endless_text_);

    // Tool change progress subjects
    INIT_SUBJECT_INT(toolchange_visible, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_current_toolchange, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_number_of_toolchanges, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(toolchange_text, "", subjects_, register_xml);

    // Filament path visualization subjects
    INIT_SUBJECT_INT(path_topology, static_cast<int>(PathTopology::HUB), subjects_, register_xml);
    INIT_SUBJECT_INT(path_filament_segment, static_cast<int>(PathSegment::NONE), subjects_,
                     register_xml);

    // Dryer subjects (for AMS systems with integrated drying)
    INIT_SUBJECT_INT(dryer_supported, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_current_temp, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_target_temp, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_remaining_min, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(dryer_progress_pct, -1, subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_current_temp_text, "---", subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_target_temp_text, "---", subjects_, register_xml);
    INIT_SUBJECT_STRING(dryer_time_text, "", subjects_, register_xml);

    // Dryer modal editing subjects (raw int + formatted text)
    INIT_SUBJECT_INT(modal_target_temp, DEFAULT_DRYER_TEMP_C, subjects_, register_xml);
    INIT_SUBJECT_INT(modal_duration_min, DEFAULT_DRYER_DURATION_MIN, subjects_, register_xml);

    // Currently Loaded display subjects (for reactive UI binding)
    // These subjects need ams_ prefix for XML but member vars don't have it
    lv_subject_init_string(&current_material_text_, current_material_text_buf_, nullptr,
                           sizeof(current_material_text_buf_), "---");
    subjects_.register_subject(&current_material_text_,
                               register_xml ? "ams_current_material_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_current_material_text",
                                                      &current_material_text_);

    lv_subject_init_string(&current_slot_text_, current_slot_text_buf_, nullptr,
                           sizeof(current_slot_text_buf_), "None");
    subjects_.register_subject(&current_slot_text_,
                               register_xml ? "ams_current_slot_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_current_slot_text", &current_slot_text_);

    lv_subject_init_string(&current_weight_text_, current_weight_text_buf_, nullptr,
                           sizeof(current_weight_text_buf_), "");
    subjects_.register_subject(&current_weight_text_,
                               register_xml ? "ams_current_weight_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_current_weight_text",
                                                      &current_weight_text_);

    lv_subject_init_int(&current_has_weight_, 0);
    subjects_.register_subject(&current_has_weight_,
                               register_xml ? "ams_current_has_weight" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_current_has_weight",
                                                      &current_has_weight_);

    INIT_SUBJECT_INT(current_color, 0x505050, subjects_, register_xml);

    // Clog detection meter subjects
    INIT_SUBJECT_INT(clog_meter_mode, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_value, 0, subjects_,
                     register_xml); // SUBJECT_OK: ClogMeterModel observes it via a lambda
    INIT_SUBJECT_INT(
        clog_meter_warning, 0, subjects_,
        register_xml); // SUBJECT_OK: ClogMeterModel observes it via clog_meter_subjects()
    INIT_SUBJECT_INT(clog_meter_status, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_mode_text, "", subjects_, register_xml);
    INIT_SUBJECT_INT(
        clog_meter_danger_pct, 0, subjects_,
        register_xml); // SUBJECT_OK: ClogMeterModel observes it via clog_meter_subjects()
    INIT_SUBJECT_INT(
        clog_meter_peak_pct, 0, subjects_,
        register_xml); // SUBJECT_OK: ClogMeterModel observes it via clog_meter_subjects()
    INIT_SUBJECT_STRING(clog_meter_center_text, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_label_left, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_label_right, "", subjects_, register_xml);
    INIT_SUBJECT_INT(clog_meter_note_kind, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(clog_meter_note_text, "", subjects_, register_xml);

    // Filament buffer reading, system level
    INIT_SUBJECT_INT(buffer_present, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(buffer_slider, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(buffer_bias_pct, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_INT(buffer_status, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_INT(buffer_gauge, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_INT(buffer_value_pct, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_INT(buffer_target_pct, -1, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_STRING(buffer_label, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(buffer_value_text, "", subjects_,
                        register_xml); // SUBJECT_OK: the 2x1 widget binds it
    INIT_SUBJECT_STRING(buffer_short_text, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(buffer_lean_text, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(buffer_target_text, "", subjects_, register_xml);

    // Per-slot subjects (dynamic names require manual init)
    char name_buf[32];
    for (int i = 0; i < MAX_SLOTS; ++i) {
        lv_subject_init_int(&slot_colors_[i], static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_color", i);
        subjects_.register_subject(&slot_colors_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_colors_[i]);

        lv_subject_init_int(&slot_statuses_[i], static_cast<int>(SlotStatus::UNKNOWN));
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_status", i);
        subjects_.register_subject(&slot_statuses_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_statuses_[i]);

        lv_subject_init_string(&slot_remaining_[i], slot_remaining_buf_[i], nullptr,
                               sizeof(slot_remaining_buf_[i]), "");
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_remaining", i);
        subjects_.register_subject(&slot_remaining_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_remaining_[i]);

        lv_subject_init_string(&slot_materials_[i], slot_materials_buf_[i], nullptr,
                               sizeof(slot_materials_buf_[i]), "");
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_material", i);
        subjects_.register_subject(&slot_materials_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_materials_[i]);

        // Per-slot fill percent (SlotInfo::display_fill_pct encoding: 0-100, -1
        // = unknown). Observed by the ams_slot widget so spool fill renders from
        // state on every panel. -1 initial → "no data yet, leave render as-is".
        lv_subject_init_int(&slot_fills_[i], -1);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_fill", i);
        subjects_.register_subject(&slot_fills_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_fills_[i]);

        // Per-slot LIVE state subjects (path segment, toolhead-present, active-loaded)
        lv_subject_init_int(&slot_segments_[i], static_cast<int>(PathSegment::NONE));
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_segment", i);
        subjects_.register_subject(&slot_segments_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_segments_[i]);

        lv_subject_init_int(&slot_toolhead_present_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_toolhead_present", i);
        subjects_.register_subject(&slot_toolhead_present_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_toolhead_present_[i]);

        lv_subject_init_int(&slot_active_loaded_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_active_loaded", i);
        subjects_.register_subject(&slot_active_loaded_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_active_loaded_[i]);

        lv_subject_init_int(&slot_lane_states_[i], static_cast<int>(helix::ui::LaneState::Empty));
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_lane_state", i);
        subjects_.register_subject(&slot_lane_states_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_lane_states_[i]);

        // Per-slot error state, published so a lane bar can draw its own
        // status line from subjects. has_error = BLOCKED or a carried
        // SlotError; severity defaults to INFO when no error is carried.
        lv_subject_init_int(&slot_has_error_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_has_error", i);
        subjects_.register_subject(&slot_has_error_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_has_error_[i]);

        lv_subject_init_int(&slot_error_severity_[i], static_cast<int>(SlotError::Severity::INFO));
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_error_severity", i);
        subjects_.register_subject(&slot_error_severity_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &slot_error_severity_[i]);
    }

    // Per-unit environment subjects (CFS temperature/humidity)
    for (int i = 0; i < MAX_UNITS; ++i) {
        lv_subject_init_int(&unit_temp_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_temp", i);
        subjects_.register_subject(&unit_temp_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &unit_temp_[i]);

        lv_subject_init_int(&unit_humidity_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_humidity", i);
        subjects_.register_subject(&unit_humidity_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &unit_humidity_[i]);

        lv_subject_init_int(&unit_absent_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_absent", i);
        subjects_.register_subject(&unit_absent_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &unit_absent_[i]);

        lv_subject_init_int(&unit_disconnected_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_disconnected", i);
        subjects_.register_subject(&unit_disconnected_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &unit_disconnected_[i]);
    }

    // Per-unit environment indicator display subjects (formatted text for XML binding)
    for (int i = 0; i < MAX_UNITS; ++i) {
        char name_buf[48];

        lv_subject_init_string(&env_ind_temp_text_[i], env_ind_temp_text_buf_[i], nullptr,
                               ENV_IND_TEXT_BUF_SIZE, "---");
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_temp_text", i);
        subjects_.register_subject(&env_ind_temp_text_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_temp_text_[i]);

        lv_subject_init_string(&env_ind_humidity_text_[i], env_ind_humidity_text_buf_[i], nullptr,
                               ENV_IND_TEXT_BUF_SIZE, "---");
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_text", i);
        subjects_.register_subject(&env_ind_humidity_text_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_text_[i]);

        lv_subject_init_int(&env_ind_humidity_status_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_status", i);
        subjects_.register_subject(&env_ind_humidity_status_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_status_[i]);

        lv_subject_init_int(&env_ind_humidity_visible_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_visible", i);
        subjects_.register_subject(&env_ind_humidity_visible_[i],
                                   register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_visible_[i]);

        lv_subject_init_int(&env_ind_visible_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_visible", i);
        subjects_.register_subject(&env_ind_visible_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_visible_[i]);

        lv_subject_init_int(&env_ind_drying_active_[i], 0);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_active", i);
        subjects_.register_subject(&env_ind_drying_active_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_drying_active_[i]);

        lv_subject_init_string(&env_ind_drying_text_[i], env_ind_drying_text_buf_[i], nullptr,
                               ENV_IND_DRYING_BUF_SIZE, "");
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_text", i);
        subjects_.register_subject(&env_ind_drying_text_[i], register_xml ? name_buf : nullptr);
        if (register_xml)
            helix::xml::register_subject_in_current_scope(name_buf, &env_ind_drying_text_[i]);
    }

    INIT_SUBJECT_INT(ams_unit_view_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_page_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_page_current, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_page_has_prev, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(ams_page_has_next, 0, subjects_, register_xml);
    lv_subject_init_string(&ams_page_unit_name_, page_unit_name_buf_, nullptr,
                           sizeof(page_unit_name_buf_), "");
    subjects_.register_subject(&ams_page_unit_name_, register_xml ? "ams_page_unit_name" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_page_unit_name", &ams_page_unit_name_);
    // Pointer subject like ams_system_logo: null until a unit with a logo is shown.
    lv_subject_init_pointer(&ams_page_unit_logo_, nullptr);
    subjects_.register_subject(&ams_page_unit_logo_, register_xml ? "ams_page_unit_logo" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_page_unit_logo", &ams_page_unit_logo_);
    INIT_SUBJECT_INT(ams_units_dryer_version, 0, subjects_, register_xml);

    lv_subject_init_int(&viewed_unit_disconnected_, 0);
    subjects_.register_subject(&viewed_unit_disconnected_,
                               register_xml ? "ams_viewed_unit_disconnected" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_viewed_unit_disconnected",
                                                      &viewed_unit_disconnected_);
    lv_subject_init_int(&all_units_disconnected_, 0);
    subjects_.register_subject(&all_units_disconnected_,
                               register_xml ? "ams_all_units_disconnected" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_all_units_disconnected",
                                                      &all_units_disconnected_);

    // Always-off placeholders for units past MAX_UNITS. A rig with more units
    // than we allocate subjects for still gets a card per unit; its environment
    // indicator binds these, so the badge stays hidden instead of the parser
    // warning once per binding about names nothing registered.
    lv_subject_init_int(&env_ind_off_flag_, 0);
    subjects_.register_subject(&env_ind_off_flag_,
                               register_xml ? ENV_IND_OFF_FLAG_SUBJECT : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope(ENV_IND_OFF_FLAG_SUBJECT, &env_ind_off_flag_);

    lv_subject_init_string(&env_ind_off_text_, env_ind_off_text_buf_, nullptr,
                           ENV_IND_TEXT_BUF_SIZE, "");
    subjects_.register_subject(&env_ind_off_text_,
                               register_xml ? ENV_IND_OFF_TEXT_SUBJECT : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope(ENV_IND_OFF_TEXT_SUBJECT, &env_ind_off_text_);

    // Detail-view env indicator mirror subjects.
    lv_subject_init_string(&env_ind_detail_temp_text_, env_ind_detail_temp_text_buf_, nullptr,
                           ENV_IND_TEXT_BUF_SIZE, "---");
    subjects_.register_subject(&env_ind_detail_temp_text_,
                               register_xml ? "ams_env_ind_detail_temp_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_temp_text",
                                                      &env_ind_detail_temp_text_);

    lv_subject_init_string(&env_ind_detail_humidity_text_, env_ind_detail_humidity_text_buf_,
                           nullptr, ENV_IND_TEXT_BUF_SIZE, "---");
    subjects_.register_subject(&env_ind_detail_humidity_text_,
                               register_xml ? "ams_env_ind_detail_humidity_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_text",
                                                      &env_ind_detail_humidity_text_);

    lv_subject_init_int(&env_ind_detail_humidity_status_, 0);
    subjects_.register_subject(&env_ind_detail_humidity_status_,
                               register_xml ? "ams_env_ind_detail_humidity_status" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_status",
                                                      &env_ind_detail_humidity_status_);

    lv_subject_init_int(&env_ind_detail_humidity_visible_, 0);
    subjects_.register_subject(&env_ind_detail_humidity_visible_,
                               register_xml ? "ams_env_ind_detail_humidity_visible" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_visible",
                                                      &env_ind_detail_humidity_visible_);

    lv_subject_init_int(&env_ind_detail_visible_, 0);
    subjects_.register_subject(&env_ind_detail_visible_,
                               register_xml ? "ams_env_ind_detail_visible" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_visible",
                                                      &env_ind_detail_visible_);

    lv_subject_init_int(&env_ind_detail_drying_active_, 0);
    subjects_.register_subject(&env_ind_detail_drying_active_,
                               register_xml ? "ams_env_ind_detail_drying_active" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_drying_active",
                                                      &env_ind_detail_drying_active_);

    lv_subject_init_string(&env_ind_detail_drying_text_, env_ind_detail_drying_text_buf_, nullptr,
                           ENV_IND_DRYING_BUF_SIZE, "");
    subjects_.register_subject(&env_ind_detail_drying_text_,
                               register_xml ? "ams_env_ind_detail_drying_text" : nullptr);
    if (register_xml)
        helix::xml::register_subject_in_current_scope("ams_env_ind_detail_drying_text",
                                                      &env_ind_detail_drying_text_);

    // Ask the factory for a backend. In mock mode, it returns a mock backend.
    // In real mode with no printer connected, it returns nullptr.
    // This keeps mock/real decision entirely in the factory.
    if (registry_.count() == 0) {
        auto backend = AmsBackend::create(AmsType::NONE, nullptr, nullptr);
        if (backend) {
            // Register first, start second, as init_backends_from_hardware()
            // does. A backend answers INVALID_LANE_ID for every slot until
            // add_backend() stamps its index, so anything start() files before
            // then lands on no lane and is dropped.
            set_backend(std::move(backend));
            if (auto* b = get_backend(0)) {
                b->start();
            }
            sync_from_backend();
            spdlog::debug("[AMS State] Backend initialized via factory ({} slots)",
                          lv_subject_get_int(&ams_slot_count_));
        }
    }

    initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "AmsState", []() { AmsState::instance().deinit_subjects(); });

    // Observe PrinterState's print state so the ams_action_detail label can
    // flip between "Idle" / "Printing" / "Paused" when the AMS itself is IDLE
    // but a print is in progress. print_state_enum is a *static* PrinterState
    // subject — no SubjectLifetime token required.
    //
    // Wire the observer here rather than inside the `if (initialized_)` guard
    // so tests that re-enter init_subjects() on the shared singleton (after
    // a prior test left it initialized) still get the observer attached.
    // PrinterState::init_subjects() must run before this point; tests/main
    // do so during fixture setup / app boot.
    install_print_state_observer();
}

void AmsState::install_print_state_observer() {
    // Idempotent: reset any prior guard before installing a fresh one. The
    // reset path uses the alive token from the *previous* install so it can
    // safely skip lv_observer_remove() if PrinterState already deinit'd its
    // subjects (e.g. between tests).
    print_state_observer_.reset();
    auto lifetime = get_printer_state().print_state().get_static_subjects_lifetime();
    // RAW_PRINT_STATE_OK: subscribes to the WIRE deliberately - recompute_action_detail()
    // labels what the printer reports, and reads the same subject.
    print_state_observer_ = helix::ui::observe<int>(
        get_printer_state().print_state().get_print_state_enum_subject(), this,
        [](AmsState* self, int /*print_state*/) { self->recompute_action_detail(); }, lifetime);
}

void AmsState::register_xml_subject_names() {
    // Publishes the ALREADY-INITIALIZED subjects under their XML names —
    // registration only, no lv_subject_init_* (init memzeros the subject and
    // would wipe the observers bound since the first init). Re-registering an
    // existing name replaces the record's subject pointer, so names the first
    // init already published are harmlessly re-published.
    //
    // MUST mirror the registration list in init_subjects(): same names, same
    // order, same loops. A name registered there but not here stays
    // unpublished after a register_xml=false first init
    // (prestonbrown/helixscreen#1374). scripts/check_ams_xml_mirror.py fails
    // the commit when the two name sets differ.

    // Backend selector subjects
    helix::xml::register_subject_in_current_scope("backend_count", &backend_count_);
    helix::xml::register_subject_in_current_scope("ams_data_revision", &ams_data_revision_);
    helix::xml::register_subject_in_current_scope("active_backend", &active_backend_);

    // System-level subjects
    helix::xml::register_subject_in_current_scope("ams_type", &ams_type_);
    helix::xml::register_subject_in_current_scope("ams_is_tool_changer", &ams_is_tool_changer_);
    helix::xml::register_subject_in_current_scope("ams_is_filament_system",
                                                  &ams_is_filament_system_);
    helix::xml::register_subject_in_current_scope("ams_action", &ams_action_);
    helix::xml::register_subject_in_current_scope("ams_operation_phase", &ams_operation_phase_);
    helix::xml::register_subject_in_current_scope("ams_operation_indeterminate",
                                                  &ams_operation_indeterminate_);
    helix::xml::register_subject_in_current_scope("toolchange_step", &toolchange_step_);
    helix::xml::register_subject_in_current_scope("current_slot", &current_slot_);
    helix::xml::register_subject_in_current_scope("pending_target_slot", &pending_target_slot_);
    helix::xml::register_subject_in_current_scope("ams_current_tool", &ams_current_tool_);
    // Members without the ams_ prefix; the XML names carry it
    helix::xml::register_subject_in_current_scope("ams_filament_loaded", &filament_loaded_);
    helix::xml::register_subject_in_current_scope("ams_filament_runout", &filament_runout_);
    helix::xml::register_subject_in_current_scope("ams_bypass_active", &bypass_active_);
    helix::xml::register_subject_in_current_scope("ams_external_spool_color",
                                                  &external_spool_color_);
    helix::xml::register_subject_in_current_scope("ams_external_spool_material",
                                                  &external_spool_material_);
    helix::xml::register_subject_in_current_scope("ams_supports_bypass", &supports_bypass_);
    helix::xml::register_subject_in_current_scope("ams_slot_count", &ams_slot_count_);
    helix::xml::register_subject_in_current_scope("ams_cards_compact", &ams_cards_compact_);
    helix::xml::register_subject_in_current_scope("slots_version", &slots_version_);
    helix::xml::register_subject_in_current_scope("tool_map_version", &tool_map_version_);
    helix::xml::register_subject_in_current_scope("active_tool_port_present",
                                                  &active_tool_port_present_);

    // String subjects (buffer names don't match macro convention)
    helix::xml::register_subject_in_current_scope("ams_action_detail", &ams_action_detail_);
    helix::xml::register_subject_in_current_scope("ams_system_name", &ams_system_name_);
    helix::xml::register_subject_in_current_scope("ams_system_logo", &ams_system_logo_);
    helix::xml::register_subject_in_current_scope("ams_current_tool_text", &ams_current_tool_text_);
    helix::xml::register_subject_in_current_scope("ams_endless_state", &ams_endless_state_);
    helix::xml::register_subject_in_current_scope("ams_endless_text", &ams_endless_text_);

    // Tool change progress subjects
    helix::xml::register_subject_in_current_scope("toolchange_visible", &toolchange_visible_);
    helix::xml::register_subject_in_current_scope("ams_current_toolchange",
                                                  &ams_current_toolchange_);
    helix::xml::register_subject_in_current_scope("ams_number_of_toolchanges",
                                                  &ams_number_of_toolchanges_);
    helix::xml::register_subject_in_current_scope("toolchange_text", &toolchange_text_);

    // Filament path visualization subjects
    helix::xml::register_subject_in_current_scope("path_topology", &path_topology_);
    helix::xml::register_subject_in_current_scope("path_filament_segment", &path_filament_segment_);

    // Dryer subjects
    helix::xml::register_subject_in_current_scope("dryer_supported", &dryer_supported_);
    helix::xml::register_subject_in_current_scope("dryer_active", &dryer_active_);
    helix::xml::register_subject_in_current_scope("dryer_current_temp", &dryer_current_temp_);
    helix::xml::register_subject_in_current_scope("dryer_target_temp", &dryer_target_temp_);
    helix::xml::register_subject_in_current_scope("dryer_remaining_min", &dryer_remaining_min_);
    helix::xml::register_subject_in_current_scope("dryer_progress_pct", &dryer_progress_pct_);
    helix::xml::register_subject_in_current_scope("dryer_current_temp_text",
                                                  &dryer_current_temp_text_);
    helix::xml::register_subject_in_current_scope("dryer_target_temp_text",
                                                  &dryer_target_temp_text_);
    helix::xml::register_subject_in_current_scope("dryer_time_text", &dryer_time_text_);

    // Dryer modal editing subjects
    helix::xml::register_subject_in_current_scope("modal_target_temp", &modal_target_temp_);
    helix::xml::register_subject_in_current_scope("modal_duration_min", &modal_duration_min_);

    // Currently Loaded display subjects
    helix::xml::register_subject_in_current_scope("ams_current_material_text",
                                                  &current_material_text_);
    helix::xml::register_subject_in_current_scope("ams_current_slot_text", &current_slot_text_);
    helix::xml::register_subject_in_current_scope("ams_current_weight_text", &current_weight_text_);
    helix::xml::register_subject_in_current_scope("ams_current_has_weight", &current_has_weight_);
    helix::xml::register_subject_in_current_scope("current_color", &current_color_);

    // Clog detection meter subjects
    helix::xml::register_subject_in_current_scope("clog_meter_mode", &clog_meter_mode_);
    helix::xml::register_subject_in_current_scope(
        "clog_meter_value",
        &clog_meter_value_); // SUBJECT_OK: ClogMeterModel observes it via a lambda
    helix::xml::register_subject_in_current_scope(
        "clog_meter_warning",
        &clog_meter_warning_); // SUBJECT_OK: ClogMeterModel observes it via clog_meter_subjects()
    helix::xml::register_subject_in_current_scope("clog_meter_status", &clog_meter_status_);
    helix::xml::register_subject_in_current_scope("clog_meter_mode_text", &clog_meter_mode_text_);
    helix::xml::register_subject_in_current_scope(
        "clog_meter_danger_pct",
        &clog_meter_danger_pct_); // SUBJECT_OK: ClogMeterModel observes it via
                                  // clog_meter_subjects()
    helix::xml::register_subject_in_current_scope(
        "clog_meter_peak_pct",
        &clog_meter_peak_pct_); // SUBJECT_OK: ClogMeterModel observes it via clog_meter_subjects()
    helix::xml::register_subject_in_current_scope("clog_meter_center_text",
                                                  &clog_meter_center_text_);
    helix::xml::register_subject_in_current_scope("clog_meter_label_left", &clog_meter_label_left_);
    helix::xml::register_subject_in_current_scope("clog_meter_label_right",
                                                  &clog_meter_label_right_);
    helix::xml::register_subject_in_current_scope("clog_meter_note_kind", &clog_meter_note_kind_);
    helix::xml::register_subject_in_current_scope("clog_meter_note_text", &clog_meter_note_text_);

    // Filament buffer reading
    helix::xml::register_subject_in_current_scope("buffer_present", &buffer_present_);
    helix::xml::register_subject_in_current_scope("buffer_slider", &buffer_slider_);
    helix::xml::register_subject_in_current_scope(
        "buffer_bias_pct",
        &buffer_bias_pct_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope(
        "buffer_status",
        &buffer_status_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope(
        "buffer_gauge",
        &buffer_gauge_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope(
        "buffer_value_pct",
        &buffer_value_pct_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope(
        "buffer_target_pct",
        &buffer_target_pct_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope("buffer_label", &buffer_label_);
    helix::xml::register_subject_in_current_scope(
        "buffer_value_text",
        &buffer_value_text_); // SUBJECT_OK: the 2x1 widget binds it
    helix::xml::register_subject_in_current_scope("buffer_short_text", &buffer_short_text_);
    helix::xml::register_subject_in_current_scope("buffer_lean_text", &buffer_lean_text_);
    helix::xml::register_subject_in_current_scope("buffer_target_text", &buffer_target_text_);

    // Per-slot subjects (snprintf'd names)
    char name_buf[48];
    for (int i = 0; i < MAX_SLOTS; ++i) {
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_color", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_colors_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_status", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_statuses_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_remaining", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_remaining_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_material", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_materials_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_fill", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_fills_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_segment", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_segments_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_toolhead_present", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_toolhead_present_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_active_loaded", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_active_loaded_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_lane_state", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_lane_states_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_has_error", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_has_error_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_slot_%d_error_severity", i);
        helix::xml::register_subject_in_current_scope(name_buf, &slot_error_severity_[i]);
    }

    // Per-unit environment subjects (CFS temperature/humidity)
    for (int i = 0; i < MAX_UNITS; ++i) {
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_temp", i);
        helix::xml::register_subject_in_current_scope(name_buf, &unit_temp_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_humidity", i);
        helix::xml::register_subject_in_current_scope(name_buf, &unit_humidity_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_absent", i);
        helix::xml::register_subject_in_current_scope(name_buf, &unit_absent_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_unit_%d_disconnected", i);
        helix::xml::register_subject_in_current_scope(name_buf, &unit_disconnected_[i]);
    }
    helix::xml::register_subject_in_current_scope("ams_viewed_unit_disconnected",
                                                  &viewed_unit_disconnected_);
    helix::xml::register_subject_in_current_scope("ams_unit_view_active", &ams_unit_view_active_);
    helix::xml::register_subject_in_current_scope("ams_page_count", &ams_page_count_);
    helix::xml::register_subject_in_current_scope("ams_page_current", &ams_page_current_);
    helix::xml::register_subject_in_current_scope("ams_page_has_prev", &ams_page_has_prev_);
    helix::xml::register_subject_in_current_scope("ams_page_has_next", &ams_page_has_next_);
    helix::xml::register_subject_in_current_scope("ams_page_unit_name", &ams_page_unit_name_);
    helix::xml::register_subject_in_current_scope("ams_page_unit_logo", &ams_page_unit_logo_);
    helix::xml::register_subject_in_current_scope("ams_units_dryer_version",
                                                  &ams_units_dryer_version_);
    helix::xml::register_subject_in_current_scope("ams_all_units_disconnected",
                                                  &all_units_disconnected_);

    // Per-unit environment indicator display subjects
    for (int i = 0; i < MAX_UNITS; ++i) {
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_temp_text", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_temp_text_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_text", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_text_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_status", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_status_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_humidity_visible", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_humidity_visible_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_visible", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_visible_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_active", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_drying_active_[i]);
        snprintf(name_buf, sizeof(name_buf), "ams_env_ind_%d_drying_text", i);
        helix::xml::register_subject_in_current_scope(name_buf, &env_ind_drying_text_[i]);
    }

    // Off-flag placeholders and detail-view env indicator mirrors
    helix::xml::register_subject_in_current_scope(ENV_IND_OFF_FLAG_SUBJECT, &env_ind_off_flag_);
    helix::xml::register_subject_in_current_scope(ENV_IND_OFF_TEXT_SUBJECT, &env_ind_off_text_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_temp_text",
                                                  &env_ind_detail_temp_text_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_text",
                                                  &env_ind_detail_humidity_text_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_status",
                                                  &env_ind_detail_humidity_status_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_humidity_visible",
                                                  &env_ind_detail_humidity_visible_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_visible",
                                                  &env_ind_detail_visible_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_drying_active",
                                                  &env_ind_detail_drying_active_);
    helix::xml::register_subject_in_current_scope("ams_env_ind_detail_drying_text",
                                                  &env_ind_detail_drying_text_);
}

void AmsState::deinit_subjects() {
    assert_main_thread();

    if (!initialized_) {
        return;
    }

    spdlog::trace("[AMS State] Deinitializing subjects");

    // Expire the deferred setters still queued on the UpdateQueue. They capture
    // `this` and write the subjects torn down below, so without this the next
    // drain notifies a freed observer list (#1165, #1146).
    async_lifetime_.invalidate();

    // Clear dangling API pointer — the IMoonrakerAPI is destroyed during teardown
    // before AmsState re-initializes. Without this, sync_from_backend() would
    // dereference a freed pointer on the next init_subjects() cycle.
    api_ = nullptr;

    // Tear down the print-state observer BEFORE deiniting subjects so the
    // LVGL observer is removed cleanly (reset(), not release() — see project
    // CLAUDE.md § "ObserverGuard::reset() is the default").
    print_state_observer_.reset();

    // IMPORTANT: clear_backends() MUST precede subjects_.deinit_all() because
    // BackendSlotSubjects are managed outside SubjectManager for lifetime reasons
    clear_backends();

    // Use SubjectManager for automatic cleanup of all registered subjects
    subjects_.deinit_all();

    initialized_ = false;
    spdlog::trace("[AMS State] Subjects deinitialized");
}

lv_subject_t* AmsState::get_slot_color_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_colors_[slot_index];
}

lv_subject_t* AmsState::get_slot_status_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_statuses_[slot_index];
}

lv_subject_t* AmsState::get_slot_lane_state_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_lane_states_[slot_index];
}

lv_subject_t* AmsState::get_slot_has_error_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_has_error_[slot_index];
}

lv_subject_t* AmsState::get_slot_error_severity_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_error_severity_[slot_index];
}

lv_subject_t* AmsState::get_slot_remaining_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_remaining_[slot_index];
}

lv_subject_t* AmsState::get_slot_material_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_materials_[slot_index];
}

lv_subject_t* AmsState::get_slot_fill_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_fills_[slot_index];
}

// Per-slot LIVE state subjects. The arrays live in the singleton but are
// registered with subjects_, so deinit_subjects() frees their observers; the
// (slot, SubjectLifetime&) overloads hand out get_subjects_lifetime() (#1700).
lv_subject_t* AmsState::get_slot_segment_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_segments_[slot_index];
}

lv_subject_t* AmsState::get_slot_segment_subject(int slot_index, SubjectLifetime& lifetime) {
    return with_lifetime(get_slot_segment_subject(slot_index), get_subjects_lifetime(), lifetime);
}

lv_subject_t* AmsState::get_slot_toolhead_present_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_toolhead_present_[slot_index];
}

lv_subject_t* AmsState::get_slot_toolhead_present_subject(int slot_index,
                                                          SubjectLifetime& lifetime) {
    return with_lifetime(get_slot_toolhead_present_subject(slot_index), get_subjects_lifetime(),
                         lifetime);
}

lv_subject_t* AmsState::get_slot_active_loaded_subject(int slot_index) {
    if (slot_index < 0 || slot_index >= MAX_SLOTS) {
        return nullptr;
    }
    return &slot_active_loaded_[slot_index];
}

lv_subject_t* AmsState::get_slot_active_loaded_subject(int slot_index, SubjectLifetime& lifetime) {
    return with_lifetime(get_slot_active_loaded_subject(slot_index), get_subjects_lifetime(),
                         lifetime);
}

lv_subject_t* AmsState::get_unit_temp_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &unit_temp_[unit_index];
}

lv_subject_t* AmsState::get_unit_humidity_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &unit_humidity_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_temp_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_temp_text_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_text_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_visible_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_visible_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_status_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_status_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_humidity_visible_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_humidity_visible_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_drying_active_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_drying_active_[unit_index];
}

lv_subject_t* AmsState::get_env_ind_drying_text_subject(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return nullptr;
    }
    return &env_ind_drying_text_[unit_index];
}

AmsState::EnvIndicatorSubjectNames AmsState::env_indicator_subject_names(int unit_index) {
    EnvIndicatorSubjectNames names;

    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        names.temp_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.humidity_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.drying_text = ENV_IND_OFF_TEXT_SUBJECT;
        names.humidity_status = ENV_IND_OFF_FLAG_SUBJECT;
        names.humidity_visible = ENV_IND_OFF_FLAG_SUBJECT;
        names.visible = ENV_IND_OFF_FLAG_SUBJECT;
        names.drying_active = ENV_IND_OFF_FLAG_SUBJECT;
        return names;
    }

    auto expand = [unit_index](const char* suffix) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ams_env_ind_%d_%s", unit_index, suffix);
        return std::string(buf);
    };
    names.temp_text = expand("temp_text");
    names.humidity_text = expand("humidity_text");
    names.humidity_status = expand("humidity_status");
    names.humidity_visible = expand("humidity_visible");
    names.visible = expand("visible");
    names.drying_active = expand("drying_active");
    names.drying_text = expand("drying_text");
    return names;
}

std::string AmsState::unit_disconnected_subject_name(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return ENV_IND_OFF_FLAG_SUBJECT;
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "ams_unit_%d_disconnected", unit_index);
    return buf;
}

std::string AmsState::unit_absent_subject_name(int unit_index) {
    if (unit_index < 0 || unit_index >= MAX_UNITS) {
        return ENV_IND_OFF_FLAG_SUBJECT;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "ams_unit_%d_absent", unit_index);
    return buf;
}

lv_subject_t* AmsState::backend_slot_subject(int backend_index, int slot_index,
                                             SubjectLifetime& lifetime,
                                             std::vector<lv_subject_t> BackendSlotSubjects::*member,
                                             lv_subject_t (&primary)[MAX_SLOTS]) {
    assert_main_thread();
    lifetime.reset();
    if (backend_index == 0) {
        if (slot_index < 0 || slot_index >= MAX_SLOTS) {
            return nullptr;
        }
        lifetime = get_subjects_lifetime();
        return &primary[slot_index];
    }
    int sec_idx = backend_index - 1;
    if (sec_idx < 0 || sec_idx >= static_cast<int>(secondary_slot_subjects_.size())) {
        return nullptr;
    }
    auto& subs = secondary_slot_subjects_[sec_idx];
    if (slot_index < 0 || slot_index >= subs.slot_count) {
        return nullptr;
    }
    lifetime = subs.lifetime;
    return &(subs.*member)[slot_index];
}

lv_subject_t* AmsState::get_slot_color_subject(int backend_index, int slot_index) {
    SubjectLifetime unused;
    return get_slot_color_subject(backend_index, slot_index, unused);
}

lv_subject_t* AmsState::get_slot_status_subject(int backend_index, int slot_index) {
    SubjectLifetime unused;
    return get_slot_status_subject(backend_index, slot_index, unused);
}

lv_subject_t* AmsState::get_slot_material_subject(int backend_index, int slot_index) {
    SubjectLifetime unused;
    return get_slot_material_subject(backend_index, slot_index, unused);
}

lv_subject_t* AmsState::get_slot_color_subject(int backend_index, int slot_index,
                                               SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime, &BackendSlotSubjects::colors,
                                slot_colors_);
}

lv_subject_t* AmsState::get_slot_status_subject(int backend_index, int slot_index,
                                                SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime, &BackendSlotSubjects::statuses,
                                slot_statuses_);
}

lv_subject_t* AmsState::get_slot_fill_subject(int backend_index, int slot_index,
                                              SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime, &BackendSlotSubjects::fills,
                                slot_fills_);
}

lv_subject_t* AmsState::get_slot_material_subject(int backend_index, int slot_index,
                                                  SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime,
                                &BackendSlotSubjects::materials, slot_materials_);
}

lv_subject_t* AmsState::get_slot_lane_state_subject(int backend_index, int slot_index,
                                                    SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime,
                                &BackendSlotSubjects::lane_states, slot_lane_states_);
}

lv_subject_t* AmsState::get_slot_has_error_subject(int backend_index, int slot_index,
                                                   SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime,
                                &BackendSlotSubjects::has_errors, slot_has_error_);
}

lv_subject_t* AmsState::get_slot_error_severity_subject(int backend_index, int slot_index,
                                                        SubjectLifetime& lifetime) {
    return backend_slot_subject(backend_index, slot_index, lifetime,
                                &BackendSlotSubjects::severities, slot_error_severity_);
}

void AmsState::BackendSlotSubjects::init(int count) {
    slot_count = count;
    colors.resize(count);
    statuses.resize(count);
    fills.resize(count);
    lane_states.resize(count);
    has_errors.resize(count);
    severities.resize(count);
    material_bufs.resize(count);
    materials.resize(count);
    for (int i = 0; i < count; ++i) {
        lv_subject_init_int(&colors[i], static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        lv_subject_init_int(&statuses[i], static_cast<int>(SlotStatus::UNKNOWN));
        lv_subject_init_int(&fills[i], -1);
        lv_subject_init_int(&lane_states[i], static_cast<int>(helix::ui::LaneState::Empty));
        lv_subject_init_int(&has_errors[i], 0);
        lv_subject_init_int(&severities[i], static_cast<int>(SlotError::Severity::INFO));
        lv_subject_init_string(&materials[i], material_bufs[i].data(), nullptr,
                               BackendSlotSubjects::MATERIAL_BUF_SIZE, "");
    }
    // Fresh lifetime token: observers bound via the token'd accessors expire
    // when deinit() invalidates it on backend rediscovery.
    lifetime = std::make_shared<bool>(true);
}

void AmsState::BackendSlotSubjects::deinit() {
    // Invalidate the lifetime token FIRST so any live observer's weak_ptr is
    // dead before the subjects it points at are freed (#705 ordering).
    if (lifetime) {
        *lifetime = false;
    }
    lifetime.reset();
    for (auto& c : colors)
        lv_subject_deinit(&c);
    for (auto& s : statuses)
        lv_subject_deinit(&s);
    for (auto* group : {&fills, &lane_states, &has_errors, &severities, &materials})
        for (auto& subj : *group)
            lv_subject_deinit(&subj);
    colors.clear();
    statuses.clear();
    fills.clear();
    lane_states.clear();
    has_errors.clear();
    severities.clear();
    materials.clear();
    material_bufs.clear();
    slot_count = 0;
}

void AmsState::BackendSlotSubjects::write(int i, const SlotInfo& slot) {
    lv_subject_set_int(&colors[i], static_cast<int>(slot.color_rgb));
    lv_subject_set_int(&statuses[i], static_cast<int>(slot.status));
    lv_subject_set_int(&fills[i], slot.display_fill_pct());
    lv_subject_set_int(&lane_states[i], static_cast<int>(helix::ui::classify_lane(slot)));
    // No prev buffer, so LVGL notifies on every copy; compare here instead.
    if (strcmp(lv_subject_get_string(&materials[i]), slot.material.c_str()) != 0)
        lv_subject_copy_string(&materials[i], slot.material.c_str());
    bool has_error = false;
    int severity = 0;
    slot_error_state(slot, has_error, severity);
    lv_subject_set_int(&has_errors[i], has_error ? 1 : 0);
    lv_subject_set_int(&severities[i], severity);
}
} // namespace helix
