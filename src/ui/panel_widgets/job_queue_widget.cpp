// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_widget.h"

#include "ui_next_tick.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "job_queue_state.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>

// Module-level subject for size mode — static like all panel widget subjects
static lv_subject_t s_size_mode_subject;
static bool s_subjects_initialized = false;
static SubjectManager s_subjects;

static void job_queue_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    // Size mode (0=compact, 1=normal/2x2, 2=expanded/3x2+)
    lv_subject_init_int(&s_size_mode_subject, 1);
    s_subjects.publish("jq_size_mode", &s_size_mode_subject);
    SubjectDebugRegistry::instance().register_subject(&s_size_mode_subject, "jq_size_mode",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    s_subjects_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init)
    StaticSubjectRegistry::instance().register_deinit("JobQueueWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_subjects_initialized = false;
            spdlog::trace("[JobQueueWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[JobQueueWidget] Subjects initialized");
}

namespace helix {
void register_job_queue_widget() {
    register_widget_factory("job_queue",
                            [](const std::string&) { return std::make_unique<JobQueueWidget>(); });
    register_widget_subjects("job_queue", job_queue_widget_init_subjects);

    // Register click callback for opening the modal (L039: unique name)
    lv_xml_register_event_cb(nullptr, "on_job_queue_widget_clicked", [](lv_event_t* e) {
        auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        // Walk up to find the widget root with user_data
        while (obj && !lv_obj_get_user_data(obj)) {
            obj = lv_obj_get_parent(obj);
        }
        if (!obj)
            return;
        auto* widget = static_cast<JobQueueWidget*>(lv_obj_get_user_data(obj));
        if (widget) {
            widget->open_modal();
        }
    });
}
} // namespace helix

using namespace helix;

JobQueueWidget::JobQueueWidget() = default;

JobQueueWidget::~JobQueueWidget() {
    detach();
}

void JobQueueWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Find the scrollable job list container by name
    job_list_container_ = lv_obj_find_by_name(widget_obj_, "job_list_container");
    if (!job_list_container_) {
        spdlog::warn("[JobQueueWidget] Could not find job_list_container");
    }

    // Observe job_queue_count subject to rebuild the list when count changes
    auto* count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    auto* jqs = get_job_queue_state();
    if (count_subj) {
        count_observer_ = helix::ui::observe<int>(
            count_subj, this,
            [](JobQueueWidget* self, int /*count*/) {
                // Defer rebuild (#80) AND use safe_clean_children inside
                // rebuild_job_list (#776): run_next_tick moves the rebuild off
                // the observer callback's stack, and safe_clean_children schedules
                // child deletion via lv_obj_delete_async so sync lv_obj_clean()
                // can't corrupt LVGL's event linked list.
                if (!self->list_rebuild_pending_) {
                    self->list_rebuild_pending_ = true;
                    helix::ui::run_next_tick(self->lifetime_.token(), [self]() {
                        self->list_rebuild_pending_ = false;
                        if (self->job_list_container_)
                            self->rebuild_job_list();
                    });
                }
            },
            jqs ? jqs->get_subjects_lifetime() : SubjectLifetime{});
    }

    spdlog::debug("[JobQueueWidget] Attached");
}

void JobQueueWidget::detach() {
    // Invalidate lifetime guard so pending next-tick callbacks become no-ops.
    // That drops a queued rebuild, so the coalescing flag goes with it, or a
    // recycled instance would never queue another.
    lifetime_.invalidate();
    list_rebuild_pending_ = false;

    if (lv_is_initialized()) {
        count_observer_ = {};
    }

    job_list_container_ = nullptr;

    if (widget_obj_) {
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[JobQueueWidget] Detached");
}

void JobQueueWidget::on_activate() {
    // Trigger a fetch from JobQueueState when panel becomes visible
    auto* jqs = get_job_queue_state();
    if (jqs) {
        jqs->fetch();
    }
}

void JobQueueWidget::on_deactivate() {
    // Nothing needed — no timer to stop
}

void JobQueueWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                     int height_px) {
    // Determine size mode from the widget's real granted pixels, not its
    // unitless grid span — see panel_widget_size.h for why.
    int mode;
    if (width_px < widget_size::w_normal() || height_px < widget_size::h_tall()) {
        mode = 0; // compact: header + summary only
    } else if (width_px < widget_size::w_wide() && height_px < widget_size::h_taller()) {
        mode = 1; // normal: header + summary + compact job list
    } else {
        mode = 2; // expanded: full details with timestamps
    }

    current_size_mode_ = mode;
    lv_subject_set_int(&s_size_mode_subject, mode);

    // Rebuild list since mode affects what is shown
    rebuild_job_list();

    spdlog::trace("[JobQueueWidget] Size changed: {}x{}px -> mode {}", width_px, height_px, mode);
}

void JobQueueWidget::rebuild_job_list() {
    if (!job_list_container_)
        return;

    // Flush pending layout before cleaning — deferred observer callbacks can run
    // between layout passes, causing use-after-free in layout_update_core (#711).
    // safe_clean_children schedules child deletion via lv_obj_delete_async, so it
    // runs outside any UpdateQueue::process_pending() batch — prevents event-list
    // corruption (#776).
    lv_obj_update_layout(job_list_container_);
    helix::ui::safe_clean_children(job_list_container_);

    auto* jqs = get_job_queue_state();

    // Update empty state visibility
    auto* empty_label = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "jq_empty_state") : nullptr;

    bool has_jobs = jqs && jqs->is_loaded() && !jqs->get_jobs().empty();
    bool show_list = (current_size_mode_ > 0);

    if (empty_label) {
        // Show empty state only when: no jobs, not compact mode, and data is loaded
        bool show_empty = !has_jobs && show_list && jqs && jqs->is_loaded();
        if (show_empty) {
            lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!has_jobs || !show_list)
        return;

    const auto& jobs = jqs->get_jobs();
    const lv_font_t* item_font = theme_manager_get_font("font_small");
    lv_color_t text_color = theme_manager_get_color("text");

    for (const auto& job : jobs) {
        // Extract just the filename (strip path)
        std::string display_name = job.filename;
        auto slash = display_name.rfind('/');
        if (slash != std::string::npos) {
            display_name = display_name.substr(slash + 1);
        }

        // Create a row container for each job entry
        lv_obj_t* row = lv_obj_create(job_list_container_);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Filename label
        lv_obj_t* name_label = lv_label_create(row);
        lv_label_set_text(name_label, display_name.c_str());
        if (item_font) {
            lv_obj_set_style_text_font(name_label, item_font, 0);
        }
        lv_obj_set_style_text_color(name_label, text_color, 0);
        lv_obj_set_flex_grow(name_label, 1);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

        // Time in queue (only in expanded mode)
        if (current_size_mode_ >= 2 && job.time_in_queue > 0) {
            lv_obj_t* time_label = lv_label_create(row);
            int mins = static_cast<int>(job.time_in_queue / 60);
            int hours = mins / 60;
            mins = mins % 60;
            char time_buf[32];
            if (hours > 0) {
                std::snprintf(time_buf, sizeof(time_buf), "%dh %dm", hours, mins);
            } else {
                std::snprintf(time_buf, sizeof(time_buf), "%dm", mins);
            }
            lv_label_set_text(time_label, time_buf);
            if (item_font) {
                lv_obj_set_style_text_font(time_label, item_font, 0);
            }
            lv_obj_set_style_text_color(time_label, theme_manager_get_color("text_muted"), 0);
        }
    }
}

void JobQueueWidget::open_modal() {
    if (!parent_screen_)
        return;
    job_queue_modal_.show(parent_screen_);
}
