// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clock_widget.h"

#include "ui_format_utils.h"

#include "helix/ui/text_metrics.h"
#include "locale_formats.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "text_io.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

// Clock update interval (1 second)
static constexpr uint32_t kClockUpdateIntervalMs = 1000;

// Subjects owned by ClockWidget module — created before XML bindings resolve
static lv_subject_t s_time_subject;
static char s_time_buffer[32];

static lv_subject_t s_date_subject;
static char s_date_buffer[32];

static lv_subject_t s_uptime_subject;
static char s_uptime_buffer[64];

static lv_subject_t s_size_mode_subject;

static helix::WidgetSubjects s_subjects{"ClockWidgetSubjects"};

/// Read system uptime from /proc/uptime and format it for the widget
static void format_uptime(char* buf, size_t buf_size) {
    const std::optional<std::string> line = helix::text_io::read_first_line("/proc/uptime");
    const std::optional<double> parsed =
        line ? helix::text_io::parse_leading<double>(*line) : std::nullopt;
    const int uptime_secs = parsed ? static_cast<int>(*parsed) : -1;
    std::snprintf(buf, buf_size, "%s", helix::ui::format_uptime(uptime_secs).c_str());
}

static void clock_widget_init_subjects() {
    s_subjects.ensure_init([] {
        // Time text (e.g. "2:30 PM" or "14:30")
        lv_subject_init_string(&s_time_subject, s_time_buffer, nullptr, sizeof(s_time_buffer),
                               "--:--");
        s_subjects.publish("clock_time_text", &s_time_subject);
        SubjectDebugRegistry::instance().register_subject(
            &s_time_subject, "clock_time_text", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

        // Date text (e.g. "Fri, Feb 28")
        lv_subject_init_string(&s_date_subject, s_date_buffer, nullptr, sizeof(s_date_buffer), "");
        s_subjects.publish("clock_date_text", &s_date_subject);
        SubjectDebugRegistry::instance().register_subject(
            &s_date_subject, "clock_date_text", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

        // Uptime text (e.g. "Up: 3d 14h")
        lv_subject_init_string(&s_uptime_subject, s_uptime_buffer, nullptr, sizeof(s_uptime_buffer),
                               "");
        s_subjects.publish("clock_uptime_text", &s_uptime_subject);
        SubjectDebugRegistry::instance().register_subject(
            &s_uptime_subject, "clock_uptime_text", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

        // Size mode (0=compact/1x1, 1=normal/2x1, 2=expanded/2x2+)
        lv_subject_init_int(&s_size_mode_subject, 1);
        s_subjects.publish("clock_size_mode", &s_size_mode_subject);
        SubjectDebugRegistry::instance().register_subject(&s_size_mode_subject, "clock_size_mode",
                                                          LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

        spdlog::debug("[ClockWidget] Subjects initialized");
    });
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
        clock_timer_ = lv_timer_create(clock_timer_cb, kClockUpdateIntervalMs, this);
        spdlog::debug("[ClockWidget] Started clock timer ({}ms interval)", kClockUpdateIntervalMs);
    }
}

void ClockWidget::on_deactivate() {
    if (clock_timer_) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
        spdlog::debug("[ClockWidget] Stopped clock timer");
    }
}

namespace {

/// The widest the time ever renders in @p font, over every hour, so whether the
/// date fits does not change as the clock ticks.
int widest_time_px(const lv_font_t* font) {
    struct tm t {};
    t.tm_min = 58;
    int widest = 0;
    for (int hour = 0; hour < 24; ++hour) {
        t.tm_hour = hour;
        widest = std::max(widest, static_cast<int>(helix::ui::text_width(
                                      helix::ui::format_time(&t).c_str(), font)));
    }
    return widest;
}

/// The widest the date ever renders in @p font: every weekday against every
/// month, on a two-digit day.
int widest_date_px(const lv_font_t* font) {
    struct tm t {};
    t.tm_mday = 28;
    int widest = 0;
    for (int mon = 0; mon < 12; ++mon) {
        for (int wday = 0; wday < 7; ++wday) {
            t.tm_mon = mon;
            t.tm_wday = wday;
            widest = std::max(widest, static_cast<int>(helix::ui::text_width(
                                          helix::ui::format_localized_date(&t).c_str(), font)));
        }
    }
    return widest;
}

int line_height_px(const lv_font_t* font) {
    return font ? static_cast<int>(lv_font_get_line_height(font)) : 0;
}

} // namespace

bool ClockWidget::time_and_date_fit(int width_px, int height_px) const {
    const lv_font_t* time_font = theme_manager_get_font("font_heading");
    const lv_font_t* date_font = theme_manager_get_font("font_body");
    int chrome_w = 0;
    int chrome_h = 0;
    if (widget_obj_) {
        chrome_w = static_cast<int>(lv_obj_get_style_pad_left(widget_obj_, LV_PART_MAIN) +
                                    lv_obj_get_style_pad_right(widget_obj_, LV_PART_MAIN));
        chrome_h = static_cast<int>(lv_obj_get_style_pad_top(widget_obj_, LV_PART_MAIN) +
                                    lv_obj_get_style_pad_bottom(widget_obj_, LV_PART_MAIN) +
                                    lv_obj_get_style_pad_row(widget_obj_, LV_PART_MAIN));
    }
    const int need_w = std::max(widest_time_px(time_font), widest_date_px(date_font));
    const int need_h = line_height_px(time_font) + line_height_px(date_font);
    return width_px - chrome_w >= need_w && height_px - chrome_h >= need_h;
}

void ClockWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    // Determine size mode from physical pixels: 0=compact, 1=normal,
    // 2=expanded, 3=large
    int mode;
    if (height_px < widget_size::h_tall()) {
        // A short box shows the date wherever the time and date measurably fit.
        mode = time_and_date_fit(width_px, height_px) ? 1 : 0;
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
    char uptime_buf[64];
    format_uptime(uptime_buf, sizeof(uptime_buf));
    lv_subject_copy_string(&s_uptime_subject, uptime_buf);
}

void ClockWidget::clock_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ClockWidget*>(lv_timer_get_user_data(timer));
    if (self) {
        self->update_clock();
    }
}
