// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_job_queue_modal.h"

#include "ui_button.h"
#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_panel_print_select.h"
#include "ui_row_text.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "job_queue_state.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <lvgl/lvgl.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>

namespace {
// Per-row data for click and delete callbacks
struct RowData {
    char* job_id;
    char* filename;
};

// NOTE: this site owns TWO strings behind one pointer, so it cannot use
// helix::ui::set_owned_user_string() (single char* slot). It keeps its own
// allocator, but with the same contract: every lv_malloc is checked and a
// partial allocation is rolled back rather than left half-built.
RowData* make_row_data(const std::string& job_id, const std::string& filename) {
    auto* rd = static_cast<RowData*>(lv_malloc(sizeof(RowData)));
    if (!rd) {
        spdlog::error("[JobQueueModal] Failed to allocate row data for '{}'", filename);
        return nullptr;
    }
    rd->job_id = static_cast<char*>(lv_malloc(job_id.size() + 1));
    rd->filename = static_cast<char*>(lv_malloc(filename.size() + 1));
    if (!rd->job_id || !rd->filename) {
        spdlog::error("[JobQueueModal] Failed to allocate row strings for '{}'", filename);
        lv_free(rd->job_id);
        lv_free(rd->filename);
        lv_free(rd);
        return nullptr;
    }
    std::memcpy(rd->job_id, job_id.c_str(), job_id.size() + 1);
    std::memcpy(rd->filename, filename.c_str(), filename.size() + 1);
    return rd;
}

void free_row_data(RowData* rd) {
    if (!rd)
        return;
    lv_free(rd->job_id);
    lv_free(rd->filename);
    lv_free(rd);
}
} // namespace

namespace helix {

bool JobQueueModal::callbacks_registered_ = false;
JobQueueModal* JobQueueModal::s_active_instance_ = nullptr;

JobQueueModal::JobQueueModal() = default;

JobQueueModal::~JobQueueModal() {
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }
}

void JobQueueModal::register_callbacks() {
    if (callbacks_registered_)
        return;

    lv_xml_register_event_cb(nullptr, "on_jq_modal_close", [](lv_event_t*) {
        if (s_active_instance_) {
            s_active_instance_->hide();
        }
    });

    lv_xml_register_event_cb(nullptr, "on_jq_modal_toggle_queue", [](lv_event_t*) {
        if (s_active_instance_) {
            s_active_instance_->toggle_queue();
        }
    });

    // job_queue_row: the row starts its job, its trash icon removes it. Both
    // read the RowData the row carries in user_data.
    lv_xml_register_event_cb(nullptr, "on_jq_row_start", [](lv_event_t* e) {
        auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        auto* rd = static_cast<RowData*>(lv_obj_get_user_data(row));
        if (rd && s_active_instance_) {
            s_active_instance_->start_job(rd->job_id, rd->filename);
        }
    });

    lv_xml_register_event_cb(nullptr, "on_jq_row_delete", [](lv_event_t* e) {
        auto* icon = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        auto* rd = static_cast<RowData*>(lv_obj_get_user_data(lv_obj_get_parent(icon)));
        if (rd && s_active_instance_) {
            s_active_instance_->remove_job(rd->job_id);
        }
    });

    callbacks_registered_ = true;
}

bool JobQueueModal::show(lv_obj_t* parent) {
    register_callbacks();
    s_active_instance_ = this;

    // Refresh data before showing
    auto* jqs = get_job_queue_state();
    if (jqs) {
        jqs->fetch();
    }

    return Modal::show(parent);
}

void JobQueueModal::on_show() {
    wire_cancel_button("btn_close");

    // Observe job_queue_count to auto-refresh list when data changes (e.g., after delete)
    auto* count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    auto* jqs = get_job_queue_state();
    if (count_subj) {
        count_observer_ = helix::ui::observe<int>(
            count_subj, this,
            [](JobQueueModal* self, int /*count*/) {
                // Defer rebuild (#80) AND use safe_clean_children inside
                // populate_job_list (#776): lifetime_.defer moves the rebuild
                // off the observer callback's stack, and safe_clean_children
                // escapes UpdateQueue::process_pending() so sync lv_obj_clean()
                // can't corrupt LVGL's event linked list.
                if (!self->list_rebuild_pending_) {
                    self->list_rebuild_pending_ = true;
                    self->lifetime_.defer("JobQueueModal::rebuild", [self]() {
                        self->list_rebuild_pending_ = false;
                        if (s_active_instance_ == self) {
                            self->populate_job_list();
                            self->update_queue_state_ui();
                        }
                    });
                }
            },
            jqs ? jqs->get_subjects_lifetime() : SubjectLifetime{});
    }

    populate_job_list();
    update_queue_state_ui();
}

void JobQueueModal::on_hide() {
    count_observer_ = {};
    list_rebuild_pending_ = false;
    // lifetime_.invalidate() already called by Modal::hide() before on_hide()
    s_active_instance_ = nullptr;
}

void JobQueueModal::on_ok() {
    hide();
}

void JobQueueModal::update_queue_state_ui() {
    auto* state_label = find_widget("queue_state_label");
    auto* toggle_btn = find_widget("btn_toggle_queue");
    if (!state_label)
        return;

    auto* jqs = get_job_queue_state();
    if (!jqs)
        return;

    const bool is_paused = jqs->run_state() == helix::QueueRunState::Paused;

    // Whole strings, not "Queue: %s" over an untranslated state word.
    lv_label_set_text(state_label, is_paused ? lv_tr("Queue: Paused") : lv_tr("Queue: Ready"));
    if (toggle_btn) {
        ui_button_set_text(toggle_btn, is_paused ? lv_tr("Start") : lv_tr("Pause"));
    }
}

void JobQueueModal::populate_job_list() {
    auto* list = find_widget("modal_job_list");
    auto* empty_state = find_widget("modal_empty_state");
    if (!list)
        return;

    lv_obj_update_layout(list);
    helix::ui::safe_clean_children(list);

    auto* jqs = get_job_queue_state();
    if (!jqs || !jqs->is_loaded() || jqs->get_jobs().empty()) {
        if (empty_state) {
            lv_obj_remove_flag(empty_state, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (empty_state) {
        lv_obj_add_flag(empty_state, LV_OBJ_FLAG_HIDDEN);
    }

    const auto& jobs = jqs->get_jobs();

    for (const auto& job : jobs) {
        // Extract just the filename
        std::string display_name = helix::gcode::get_filename_basename(job.filename);

        std::string queued;
        if (job.time_in_queue > 0) {
            int mins = static_cast<int>(job.time_in_queue / 60);
            int hours = mins / 60;
            mins = mins % 60;
            if (hours > 0) {
                queued = fmt::format(lv_tr("Queued {}h {}m ago"), hours, mins);
            } else if (mins > 0) {
                queued = fmt::format(lv_tr("Queued {}m ago"), mins);
            } else {
                queued = lv_tr("Just queued");
            }
        }

        const char* attrs[] = {
            "queued_text", queued.c_str(), "hide_queued", queued.empty() ? "true" : "false",
            nullptr,
        };
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "job_queue_row", attrs));
        if (!row) {
            continue;
        }
        helix::ui::set_row_label_text(row, "job_filename", display_name.c_str());

        attach_row_data(row, job.job_id, job.filename);
    }
}

void JobQueueModal::attach_row_data(lv_obj_t* row, const std::string& job_id,
                                    const std::string& filename) {
    // Job data for the row's start and delete callbacks, freed with the row.
    auto* row_data = make_row_data(job_id, filename);
    lv_obj_set_user_data(row, row_data);
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* rd = static_cast<RowData*>(lv_event_get_user_data(e));
            free_row_data(rd);
        },
        LV_EVENT_DELETE, row_data);
}

void JobQueueModal::toggle_queue() {
    auto* api = get_moonraker_api();
    if (!api)
        return;

    auto* jqs = get_job_queue_state();
    if (!jqs)
        return;

    const bool is_paused = jqs->run_state() == helix::QueueRunState::Paused;
    auto token = lifetime_.token();

    auto on_success = [token, this]() {
        if (token.expired())
            return;
        token.defer("JobQueueModal::toggle_queue", [this]() {
            auto* jqs2 = get_job_queue_state();
            if (jqs2)
                jqs2->fetch();
            update_queue_state_ui();
        });
    };

    auto on_error = [](const MoonrakerError& err) {
        spdlog::warn("[JobQueueModal] Queue toggle failed: {}", err.message);
    };

    if (is_paused) {
        api->queue().start_queue(on_success, on_error);
    } else {
        api->queue().pause_queue(on_success, on_error);
    }
}

void JobQueueModal::remove_job(const std::string& job_id) {
    auto* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[JobQueueModal] Removing job: {}", job_id);

    auto token = lifetime_.token();
    api->queue().remove_jobs(
        {job_id},
        [token, this]() {
            if (token.expired())
                return;
            token.defer("JobQueueModal::remove_job", [this]() {
                // Fetch refreshed data — count observer will auto-rebuild the list
                auto* jqs = get_job_queue_state();
                if (jqs)
                    jqs->fetch();
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[JobQueueModal] Remove job failed: {}", err.message);
        });
}

void JobQueueModal::start_job(const std::string& job_id, const std::string& filename) {
    auto* api = get_moonraker_api();
    if (!api)
        return;

    spdlog::info("[JobQueueModal] Starting queued job {}: {}", job_id, filename);

    // The start walks the print select panel's entry point: the busy guard,
    // the saved option states and the printer-stopping command check all
    // live in the detail-view pipeline every manual Print tap already uses,
    // and the queue entry is deleted only once the printer confirms the
    // start. Hiding first returns the user to the panel the job opens on.
    hide();
    get_print_select_panel(get_printer_state(), api)
        ->start_queued_job(JobQueueEntry{job_id, filename, 0.0, 0.0});
}

} // namespace helix
