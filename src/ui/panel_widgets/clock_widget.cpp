// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clock_widget.h"

#include "ui_format_utils.h"

#include "locale_formats.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <ctime>

// Clock update interval (1 second)
static constexpr uint32_t CLOCK_UPDATE_INTERVAL_MS = 1000;

// Subjects owned by ClockWidget module — created before XML bindings resolve
static lv_subject_t s_time_subject;
static char s_time_buffer[32];

static lv_subject_t s_date_subject;
static char s_date_buffer[32];

static lv_subject_t s_uptime_subject;
static char s_uptime_buffer[32];

static lv_subject_t s_size_mode_subject;

static bool s_subjects_initialized = false;
static SubjectManager s_subjects;

/// Read system uptime from /proc/uptime and format as human-readable string
static void format_uptime(char* buf, size_t buf_size) {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) {
        std::snprintf(buf, buf_size, "Up: --");
        return;
    }

    double uptime_secs = 0;
    if (fscanf(f, "%lf", &uptime_secs) != 1) {
        fclose(f);
        std::snprintf(buf, buf_size, "Up: --");
        return;
    }
    fclose(f);

    auto total_minutes = static_cast<int>(uptime_secs / 60.0);
    int days = total_minutes / (60 * 24);
    int hours = (total_minutes / 60) % 24;
    int minutes = total_minutes % 60;

    if (days > 0) {
        std::snprintf(buf, buf_size, "Up: %dd %dh", days, hours);
    } else if (hours > 0) {
        std::snprintf(buf, buf_size, "Up: %dh %dm", hours, minutes);
    } else {
        std::snprintf(buf, buf_size, "Up: %dm", minutes);
    }
}

static void clock_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    // Time text (e.g. "2:30 PM" or "14:30")
    lv_subject_init_string(&s_time_subject, s_time_buffer, nullptr, sizeof(s_time_buffer), "--:--");
    s_subjects.publish("clock_time_text", &s_time_subject);
    SubjectDebugRegistry::instance().register_subject(&s_time_subject, "clock_time_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    // Date text (e.g. "Fri, Feb 28")
    lv_subject_init_string(&s_date_subject, s_date_buffer, nullptr, sizeof(s_date_buffer), "");
    s_subjects.publish("clock_date_text", &s_date_subject);
    SubjectDebugRegistry::instance().register_subject(&s_date_subject, "clock_date_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    // Uptime text (e.g. "Up: 3d 14h")
    lv_subject_init_string(&s_uptime_subject, s_uptime_buffer, nullptr, sizeof(s_uptime_buffer),
                           "");
    s_subjects.publish("clock_uptime_text", &s_uptime_subject);
    SubjectDebugRegistry::instance().register_subject(&s_uptime_subject, "clock_uptime_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    // Size mode (0=compact/1x1, 1=normal/2x1, 2=expanded/2x2+)
    lv_subject_init_int(&s_size_mode_subject, 1);
    s_subjects.publish("clock_size_mode", &s_size_mode_subject);
    SubjectDebugRegistry::instance().register_subject(&s_size_mode_subject, "clock_size_mode",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    s_subjects_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init)
    StaticSubjectRegistry::instance().register_deinit("ClockWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_subjects_initialized = false;
            spdlog::trace("[ClockWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[ClockWidget] Subjects initialized");
}

namespace helix {
void register_clock_widget() {
    register_widget_factory("clock",
                            [](const std::string&) { return std::make_unique<ClockWidget>(); });
    register_widget_subjects("clock", clock_widget_init_subjects);
}
} // namespace helix

using namespace helix;

ClockWidget::ClockWidget() = default;

ClockWidget::~ClockWidget() {
    detach();
}

void ClockWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Populate clock values immediately for initial display
    update_clock();

    spdlog::debug("[ClockWidget] Attached");
}

void ClockWidget::detach() {
    if (lv_is_initialized()) {
        if (clock_timer_) {
            lv_timer_delete(clock_timer_);
            clock_timer_ = nullptr;
        }
    }

    if (widget_obj_) {
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[ClockWidget] Detached");
}

void ClockWidget::on_activate() {
    update_clock();

    if (!clock_timer_) {
        clock_timer_ = lv_timer_create(clock_timer_cb, CLOCK_UPDATE_INTERVAL_MS, this);
        spdlog::debug("[ClockWidget] Started clock timer ({}ms interval)",
                      CLOCK_UPDATE_INTERVAL_MS);
    }
}

void ClockWidget::on_deactivate() {
    if (clock_timer_) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
        spdlog::debug("[ClockWidget] Stopped clock timer");
    }
}

void ClockWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    // Determine size mode from physical pixels: 0=compact, 1=normal,
    // 2=expanded, 3=large
    int mode;
    if (width_px < widget_size::w_normal() && height_px < widget_size::h_tall()) {
        mode = 0; // compact: time only
    } else if (height_px < widget_size::h_tall()) {
        mode = 1; // normal: time + date
    } else if (width_px >= widget_size::w_wide()) {
        mode = 3; // large: big time + date + uptime
    } else {
        mode = 2; // expanded: time + date + uptime
    }

    lv_subject_set_int(&s_size_mode_subject, mode);

    // Apply fonts scaled to widget size
    if (!widget_obj_)
        return;

    const lv_font_t* time_font = nullptr;
    const lv_font_t* date_font = nullptr;

    if (mode == 3) {
        // Large mode: use responsive XL font for time display
        time_font = theme_manager_get_font("font_xl");
        date_font = theme_manager_get_font("font_heading");
    } else if (mode == 0) {
        time_font = theme_manager_get_font("font_body");
        date_font = theme_manager_get_font("font_body");
    } else {
        time_font = theme_manager_get_font("font_heading");
        date_font = theme_manager_get_font("font_body");
    }

    auto* time_label = lv_obj_find_by_name(widget_obj_, "clock_time");
    if (time_label && time_font) {
        lv_obj_set_style_text_font(time_label, time_font, 0);
    }

    auto* date_label = lv_obj_find_by_name(widget_obj_, "clock_date");
    if (date_label && date_font) {
        lv_obj_set_style_text_font(date_label, date_font, 0);
    }

    // Uptime: visible only in expanded (2) and large (3) modes
    auto* uptime_label = lv_obj_find_by_name(widget_obj_, "clock_uptime");
    if (uptime_label) {
        if (mode >= 2) {
            lv_obj_remove_flag(uptime_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(uptime_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::trace("[ClockWidget] Size changed: {}x{} -> mode {}", colspan, rowspan, mode);
}

void ClockWidget::update_clock() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    struct tm* tm_info = localtime_r(&now, &tm_buf);

    if (tm_info) {
        // Time — use user's preferred format (12h/24h)
        std::string time_str = helix::ui::format_time(tm_info);
        lv_subject_copy_string(&s_time_subject, time_str.c_str());

        // Date — locale-aware formatting
        std::string date_str = helix::ui::format_localized_date(tm_info);
        lv_subject_copy_string(&s_date_subject, date_str.c_str());
    }

    // Uptime from /proc/uptime
    char uptime_buf[32];
    format_uptime(uptime_buf, sizeof(uptime_buf));
    lv_subject_copy_string(&s_uptime_subject, uptime_buf);
}

void ClockWidget::clock_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ClockWidget*>(lv_timer_get_user_data(timer));
    if (self) {
        self->update_clock();
    }
}
