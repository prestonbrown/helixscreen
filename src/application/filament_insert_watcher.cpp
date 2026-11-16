// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_insert_watcher.h"

#include "ui_ams_context_menu.h"
#include "ui_ams_edit_overlay.h"
#include "ui_external_spool_menu.h"
#include "ui_insert_notice.h"
#include "ui_nav.h"

#include "ams_backend.h"
#include "ams_insert_signals.h"
#include "ams_state.h"
#include "app_globals.h"
#include "connection_state.h"
#include "filament_insert_policy.h"
#include "filament_sensor_manager.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

std::optional<bool> sensed_presence() {
    auto& mgr = FilamentSensorManager::instance();
    return manual_load_presence(lv_subject_get_int(mgr.get_entry_detected_subject()),
                                lv_subject_get_int(mgr.get_toolhead_detected_subject()));
}

/// What an insert is judged on for a backend without its own reading: the slot status,
/// on lanes with a prep sensor.
std::optional<bool> presence_from_slot_status(const AmsBackend& backend, int slot_index) {
    if (!backend.slot_has_prep_sensor(slot_index)) {
        return std::nullopt;
    }
    return slot_status_reports_filament(backend.get_slot_info(slot_index).status);
}

/// The editor is still open while something (QR scanner, keyboard) covers it, and
/// reopening it then would replace the edit in progress.
bool editor_open() {
    lv_obj_t* root = ui::get_ams_edit_overlay().get_root();
    return root && (nav::is_in_stack(root) || nav::is_push_pending(root));
}

} // namespace

FilamentInsertWatcher::FilamentInsertWatcher(IMoonrakerAPI* api) : api_(api) {}

void FilamentInsertWatcher::start() {
    auto& ams = AmsState::instance();
    ams_observer_ = ui::observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](FilamentInsertWatcher* self, int) { self->on_ams_changed(); },
        ams.get_subjects_lifetime());

    auto& sensors = FilamentSensorManager::instance();
    auto on_sensor = [](FilamentInsertWatcher* self, int) { self->on_sensor_changed(); };
    entry_observer_ = ui::observe<int>(sensors.get_entry_detected_subject(), this, on_sensor,
                                       sensors.subjects_lifetime());
    toolhead_observer_ = ui::observe<int>(sensors.get_toolhead_detected_subject(), this, on_sensor,
                                          sensors.subjects_lifetime());

    auto on_reseed = [](FilamentInsertWatcher* self, int) { self->reseed(); };
    auto& net = get_printer_state().network_state();
    connection_observer_ = ui::observe<int>(net.get_printer_connection_state_subject(), this,
                                            on_reseed, get_printer_state().get_subjects_lifetime());
    klippy_observer_ = ui::observe<int>(net.get_klippy_state_subject(), this, on_reseed,
                                        get_printer_state().get_subjects_lifetime());
    config_observer_ = ui::observe<int>(sensors.get_config_revision_subject(), this, on_reseed,
                                        sensors.subjects_lifetime());
}

bool FilamentInsertWatcher::operation_busy() const {
    const auto action =
        static_cast<AmsAction>(lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
    return ams_action_is_busy(action) || get_printer_state().app_macro_activity().recently_active();
}

void FilamentInsertWatcher::reseed() {
    // A reconnect, a Klipper restart or a sensor reconfiguration moves readings
    // without anyone inserting filament; whatever arrives next is a snapshot.
    lane_prev_.assign(lane_prev_.size(), std::nullopt);
    lane_waiting_.assign(lane_waiting_.size(), false);
    sensor_prev_.reset();
    ui::release_all_insert_prompts();
}

void FilamentInsertWatcher::on_ams_changed() {
    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        lane_prev_.clear();
        return;
    }
    FilamentInsertContext ctx;
    ctx.print_active =
        lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0;
    ctx.operation_busy = operation_busy();
    ctx.setting_enabled = SettingsManager::instance().get_filament_auto_open_editor();
    ctx.editor_open = editor_open();

    const int open_slot = judge_lanes(*backend, ctx);
    if (open_slot >= 0) {
        open_lane(open_slot);
    }
}

int FilamentInsertWatcher::judge_lanes(const AmsBackend& backend, FilamentInsertContext ctx) {
    const IInsertSignals* const signals = backend.as<IInsertSignals>();
    const int count = std::max(0, backend.get_system_info().total_slots);
    lane_prev_.resize(count);
    lane_waiting_.resize(count, false);
    int open_slot = -1;
    for (int i = 0; i < count; ++i) {
        ctx.is_present =
            signals ? signals->slot_insert_presence(i) : presence_from_slot_status(backend, i);
        // An insert held for the lane's tag reader is judged as the edge it was.
        const bool waiting = lane_waiting_[i];
        ctx.was_present = waiting ? std::optional<bool>(false) : lane_prev_[i];
        lane_prev_[i] = ctx.is_present;
        lane_waiting_[i] = false;
        if (!should_open_editor_on_insert(ctx)) {
            // A held insert a guard now blocks gets no prompt from the editor,
            // so it gives the claim back.
            if (waiting || ctx.is_present != true) {
                ui::release_insert_prompt(i);
            }
            continue;
        }
        switch (lane_insert_action(
            signals ? signals->slot_insert_identity(i) : InsertIdentity::NotRead, waiting)) {
        case InsertEditorAction::Wait:
            lane_waiting_[i] = true;
            ui::claim_insert_prompt(i);
            break;
        case InsertEditorAction::Open:
            // The editor is this insert's one prompt. Only the first lane to
            // fill opens it; later ones are not queued and claim nothing.
            if (open_slot < 0) {
                open_slot = i;
                ui::claim_insert_prompt(i);
            } else {
                ui::release_insert_prompt(i);
            }
            break;
        case InsertEditorAction::Ignore:
            ui::release_insert_prompt(i);
            break;
        }
    }
    return open_slot;
}

void FilamentInsertWatcher::on_sensor_changed() {
    FilamentInsertContext ctx;
    ctx.was_present = sensor_prev_;
    ctx.is_present = sensed_presence();
    sensor_prev_ = ctx.is_present;

    // With an AMS in charge its lanes (or an operation) own the filament; only a
    // printer without one, or with bypass engaged, is hand-fed.
    auto& ams = AmsState::instance();
    if (ams.get_backend() && !ams.any_bypass_active()) {
        return;
    }
    ctx.print_active =
        lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0;
    ctx.operation_busy = operation_busy();
    ctx.setting_enabled = SettingsManager::instance().get_filament_auto_open_editor();
    ctx.editor_open = editor_open();
    if (should_open_editor_on_insert(ctx)) {
        open_external();
    }
}

void FilamentInsertWatcher::open_lane(int slot_index) {
    spdlog::info("[FilamentInsertWatcher] Filament inserted in slot {}, opening editor",
                 slot_index);
    ui::open_slot_editor(lv_screen_active(), api_, slot_index, /*open_on_picker=*/false);
}

void FilamentInsertWatcher::open_external() {
    spdlog::info(
        "[FilamentInsertWatcher] Filament inserted by hand, opening external spool editor");
    ui::open_external_spool_editor(lv_screen_active());
}

} // namespace helix
