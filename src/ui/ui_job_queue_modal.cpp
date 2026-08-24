// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_job_queue_modal.h"

#include "ui_button.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "job_queue_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "theme_manager.h"

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
    if (count_subj) {
        count_observer_ = helix::ui::observe_int_sync<JobQueueModal>(
            count_subj, this, [](JobQueueModal* self, int /*count*/) {
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
            });
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

    const auto& state = jqs->get_queue_state();
    bool is_paused = (state == "paused");

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
    const lv_font_t* name_font = theme_manager_get_font("font_body");
    const lv_font_t* small_font = theme_manager_get_font("font_small");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t muted_color = theme_manager_get_color("text_muted");
    lv_color_t danger_color = theme_manager_get_color("danger");

    for (const auto& job : jobs) {
        // Extract just the filename
        std::string display_name = job.filename;
        auto slash = display_name.rfind('/');
        if (slash != std::string::npos) {
            display_name = display_name.substr(slash + 1);
        }

        // Row container — clickable to start print
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, theme_manager_get_color("card_bg"), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, theme_get_accent_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        int32_t row_pad = theme_manager_get_spacing("space_sm");
        lv_obj_set_style_pad_all(row, row_pad, 0);
        lv_obj_set_style_pad_gap(row, row_pad, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Store job data for row click and delete callbacks
        // Widget pool recycling exception: dynamic list with per-item callbacks
        auto* row_data = make_row_data(job.job_id, job.filename);
        lv_obj_set_user_data(row, row_data);

        // Clean up row data when row is deleted
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* rd = static_cast<RowData*>(lv_event_get_user_data(e));
                free_row_data(rd);
            },
            LV_EVENT_DELETE, row_data);

        // Row click → start job
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* rd = static_cast<RowData*>(lv_event_get_user_data(e));
                if (rd && s_active_instance_) {
                    s_active_instance_->start_job(rd->job_id, rd->filename);
                }
            },
            LV_EVENT_CLICKED, row_data);

        // Left side: filename + time info (L071: pass clicks to parent row)
        lv_obj_t* info_col = lv_obj_create(row);
        lv_obj_set_height(info_col, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(info_col, 0, 0);
        lv_obj_set_style_border_width(info_col, 0, 0);
        lv_obj_set_style_pad_all(info_col, 0, 0);
        lv_obj_set_style_pad_gap(info_col, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_flex_flow(info_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(info_col, 1);
        lv_obj_remove_flag(info_col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(info_col, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);

        // Filename
        lv_obj_t* name_label = lv_label_create(info_col);
        lv_label_set_text(name_label, display_name.c_str());
        if (name_font)
            lv_obj_set_style_text_font(name_label, name_font, 0);
        lv_obj_set_style_text_color(name_label, text_color, 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_label, lv_pct(100));

        // Time in queue
        if (job.time_in_queue > 0) {
            lv_obj_t* time_label = lv_label_create(info_col);
            int mins = static_cast<int>(job.time_in_queue / 60);
            int hours = mins / 60;
            mins = mins % 60;
            std::string queued;
            if (hours > 0) {
                queued = fmt::format(lv_tr("Queued {}h {}m ago"), hours, mins);
            } else if (mins > 0) {
                queued = fmt::format(lv_tr("Queued {}m ago"), mins);
            } else {
                queued = lv_tr("Just queued");
            }
            lv_label_set_text(time_label, queued.c_str());
            if (small_font)
                lv_obj_set_style_text_font(time_label, small_font, 0);
            lv_obj_set_style_text_color(time_label, muted_color, 0);
        }

        // Delete icon (right side) — plain clickable label, no button chrome
        const char* trash_glyph = ui_icon::lookup_codepoint("trash_can_outline");
        lv_obj_t* del_icon = lv_label_create(row);
        lv_label_set_text(del_icon, trash_glyph ? trash_glyph : "X");
        lv_obj_set_style_text_color(del_icon, danger_color, 0);
        lv_obj_set_style_text_color(del_icon, theme_manager_get_color("text"), LV_STATE_PRESSED);
        lv_obj_add_flag(del_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(del_icon, theme_manager_get_spacing("space_xs"), 0);
        const char* icon_font_name = lv_xml_get_const(nullptr, "icon_font_sm");
        const lv_font_t* icon_font =
            icon_font_name ? lv_xml_get_font(nullptr, icon_font_name) : nullptr;
        if (icon_font) {
            lv_obj_set_style_text_font(del_icon, icon_font, 0);
        }

        // Delete click — uses row_data from parent row (freed when row is deleted)
        lv_obj_add_event_cb(
            del_icon,
            [](lv_event_t* e) {
                auto* rd = static_cast<RowData*>(lv_event_get_user_data(e));
                if (rd && s_active_instance_) {
                    s_active_instance_->remove_job(rd->job_id);
                }
            },
            LV_EVENT_CLICKED, row_data);
    }
}

void JobQueueModal::toggle_queue() {
    auto* api = get_moonraker_api();
    if (!api)
        return;

    auto* jqs = get_job_queue_state();
    if (!jqs)
        return;

    bool is_paused = (jqs->get_queue_state() == "paused");
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

    // The queue entry is removed BEFORE the start is attempted, so an
    // incomplete refusal loses the job: it is deleted and the print then fails.
    // can_start_new_print() covers both axes — what the printer reports AND a
    // start this app has already committed to but the printer has not confirmed,
    // which is the whole of a host-side pre-print block.
    auto& ps = get_printer_state();
    if (!ps.can_start_new_print()) {
        spdlog::info("[JobQueueModal] Printer busy, cannot start '{}' now", filename);
        // TODO: Moonraker doesn't have a reorder API — could delete+re-add at position 0
        return;
    }

    spdlog::info("[JobQueueModal] Starting print: {}", filename);
    auto token = lifetime_.token();

    // Remove from queue first, then start the print
    api->queue().remove_jobs(
        {job_id},
        [token, this, filename, api]() {
            if (token.expired())
                return;
            api->job().start_print(
                filename,
                [token, this]() {
                    if (token.expired())
                        return;
                    token.defer("JobQueueModal::start_job", [this]() {
                        spdlog::info("[JobQueueModal] Print started, closing modal");
                        hide();
                    });
                },
                [](const MoonrakerError& err) {
                    spdlog::warn("[JobQueueModal] Start print failed: {}", err.message);
                });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[JobQueueModal] Remove job before start failed: {}", err.message);
        });
}

} // namespace helix
