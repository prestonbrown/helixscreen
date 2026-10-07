// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_start.h"

#include "ui_callback_helpers.h"
#include "ui_job_queue_modal.h"
#include "ui_modal.h"
#include "ui_panel_print_select.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {
namespace {

// The queue modal is opened from the completion dialog, the print-status
// panel and the home widget — no one caller outlives the others — so it lives
// for the process and is reshown, exactly like the member instances
// PrintStatusWidget and JobQueueWidget own. Never destructed: teardown runs
// ModalStack::clear() to retire the dialog, and a destructor firing after
// lv_deinit() would touch dead LVGL state.
JobQueueModal& queue_modal() {
    static JobQueueModal* instance = new JobQueueModal();
    return *instance;
}

void open_job_queue_modal() {
    queue_modal().show(lv_screen_active());
}

void on_completion_start_next_cb(lv_event_t*) {
    // Leave the result dialog the same way OK does, then start the next job.
    lv_obj_t* top = Modal::get_top();
    if (top) {
        Modal::hide(top);
    }
    start_next_queued_job();
}

} // namespace

UpNextTapAction decide_up_next_tap(bool can_start_new_print) {
    return can_start_new_print ? UpNextTapAction::StartNextJob : UpNextTapAction::OpenQueueModal;
}

void start_next_queued_job() {
    auto* jqs = get_job_queue_state();
    if (!jqs || jqs->get_jobs().empty()) {
        // The Start next surfaces are count-driven and hide on an empty
        // queue, so this is only the race where the last job was removed
        // between the render and the tap.
        spdlog::debug("[JobQueueStart] Start next found no queued job");
        return;
    }
    // The same entry the queue modal's row tap uses: the busy guard, the
    // saved option states and the entry's removal on confirmed start all
    // live in the detail-view pipeline every manual Print tap already uses.
    get_print_select_panel(get_printer_state(), get_moonraker_api())
        ->start_queued_job(jqs->get_jobs().front());
}

void handle_up_next_tap() {
    if (decide_up_next_tap(get_printer_state().print_state().can_start_new_print()) ==
        UpNextTapAction::StartNextJob) {
        start_next_queued_job();
    } else {
        open_job_queue_modal();
    }
}

void register_job_queue_start_callbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_xml_callbacks({
        {"on_up_next_tap",
         [](lv_event_t* e) {
             // Claim the tap so it does not fall through to the enclosing card's or
             // panel's own click handler (status navigation / file browser).
             lv_event_stop_bubbling(e);
             handle_up_next_tap();
         }},
        {"on_print_complete_start_next", on_completion_start_next_cb},
    });
    spdlog::debug("[JobQueueStart] XML callbacks registered");
}

} // namespace helix
