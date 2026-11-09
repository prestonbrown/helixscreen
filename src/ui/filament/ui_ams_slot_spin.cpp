// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_lane_spool.h"
#include "ui_ams_slot_internal.h"
#include "ui_spool_canvas.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "display_settings_manager.h"
#include "printer_state.h"
#include "spool_spin.h"
#include "ui/ams_drawing_utils.h"

using namespace helix;
using helix::ui::observe;

void apply_spool_spin(AmsSlotData* data) {
    if (!data || !data->lane_spool)
        return;
    AmsState& ams = AmsState::instance();
    const auto int_of = [](lv_subject_t* s, int fallback) {
        return s ? lv_subject_get_int(s) : fallback;
    };
    helix::ui::SpoolSpinInput in;
    in.lane = data->backend_index == ams.active_backend_index() ? data->slot_index : -1;
    in.current_slot = int_of(ams.get_current_slot_subject(), -1);
    in.target_slot = int_of(ams.get_pending_target_slot_subject(), -1);
    in.action = static_cast<AmsAction>(
        int_of(ams.get_ams_action_subject(), static_cast<int>(AmsAction::IDLE)));
    in.lane_active_loaded = int_of(ams.get_slot_active_loaded_subject(data->slot_index), 0) != 0;
    PrinterState& printer = get_printer_state();
    // RAW_PRINT_STATE_OK: a spool turns only while filament moves, so a paused job stands still.
    in.printing = printer.print_state().get_print_job_state() == PrintJobState::PRINTING;
    in.extruder_velocity_centimm =
        lv_subject_get_int(printer.motion_state().get_live_extruder_velocity_subject());
    in.animations_enabled = DisplaySettingsManager::instance().get_animations_enabled();
    in.reduced_effects = helix::ui::reduced_effects();
    helix::ui::ams_lane_spool_set_spin(data->lane_spool, helix::ui::decide_spool_spin(in));
}

// What else turns the spool: the print, its flow and the animations setting.
void observe_slot_spin(AmsSlotData* data) {
    lv_obj_t* obj = data->container;
    const auto respin = [](lv_obj_t* o, int /*value*/) { apply_spool_spin(get_slot_data(o)); };
    PrinterState& printer = get_printer_state();
    const SubjectLifetime printer_subjects = printer.get_subjects_lifetime();
    // RAW_PRINT_STATE_OK: wakes the same decision, which reads the job state above
    data->print_state_observer = observe<int>(printer.print_state().get_print_state_enum_subject(),
                                              obj, respin, printer_subjects);
    data->extruder_velocity_observer = observe<int>(
        printer.motion_state().get_live_extruder_velocity_subject(), obj, respin, printer_subjects);
    DisplaySettingsManager& display = DisplaySettingsManager::instance();
    data->animations_enabled_observer = observe<int>(display.subject_animations_enabled(), obj,
                                                     respin, display.get_subjects_lifetime());
}
