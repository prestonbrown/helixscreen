// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clock_widget.h"

#include "core/subject.h"
#include "core/text_io.h"
#include "display_format.h"
#include "helix/ui/text_metrics.h"
#include "json_utils.h"
#include "locale_formats.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#if HELIX_ENABLE_SCREENSAVER
#include "platform_capabilities.h"
#include "screensaver.h"
#include "screensaver_clock.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

// Clock update interval (1 second)
static constexpr uint32_t kClockUpdateIntervalMs = 1000;

// Subjects owned by ClockWidget module — created before XML bindings resolve
static helix::Subject s_time_subject;
static char s_time_buffer[32];

static helix::Subject s_date_subject;
static char s_date_buffer[32];

static helix::Subject s_uptime_subject;
static char s_uptime_buffer[64];

static helix::WidgetSubjects s_subjects{"ClockWidgetSubjects"};

/// Read system uptime from /proc/uptime and format it for the widget
static void format_uptime(char* buf, size_t buf_size) {
    const std::optional<std::string> line = helix::text_io::read_first_line("/proc/uptime");
    const std::optional<double> parsed =
        line ? helix::text_io::parse_leading<double>(*line) : std::nullopt;
    const int uptime_secs = parsed ? static_cast<int>(*parsed) : -1;
    std::snprintf(buf, buf_size, "%s", helix::display_format::format_uptime(uptime_secs).c_str());
}

static void clock_widget_init_subjects() {
    s_subjects.ensure_init([] {
        // Time text (e.g. "2:30 PM" or "14:30")
        s_time_subject.init_string(s_time_buffer, nullptr, sizeof(s_time_buffer), "--:--");
        s_subjects.publish("clock_time_text", &s_time_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_time_subject, "clock_time_text", LV_SUBJECT_TYPE_STRING);

        // Date text (e.g. "Fri, Feb 28")
        s_date_subject.init_string(s_date_buffer, nullptr, sizeof(s_date_buffer), "");
        s_subjects.publish("clock_date_text", &s_date_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_date_subject, "clock_date_text", LV_SUBJECT_TYPE_STRING);

        // Uptime text (e.g. "Up: 3d 14h")
        s_uptime_subject.init_string(s_uptime_buffer, nullptr, sizeof(s_uptime_buffer), "");
        s_subjects.publish("clock_uptime_text", &s_uptime_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_uptime_subject, "clock_uptime_text",
                                     LV_SUBJECT_TYPE_STRING);

        spdlog::debug("[ClockWidget] Subjects initialized");
    });
}

namespace helix {
void register_clock_widget() {
    register_widget_factory(
        "clock", [](const std::string& id) { return std::make_unique<ClockWidget>(id); });
    register_widget_subjects("clock", clock_widget_init_subjects);
#if HELIX_ENABLE_SCREENSAVER
    register_clock_face_picker();
#endif
}
} // namespace helix

using namespace helix;

ClockWidget::ClockWidget(const std::string& instance_id)
    : instance_id_(instance_id), size_subject_name_(instance_id + "_clock_size"),
      face_subject_name_(instance_id + "_clock_face") {
    // Size mode: 0 compact (time only), 1 normal (time and date), 2 expanded, 3 large.
    UI_MANAGED_SUBJECT_INT(size_subject_, 1, size_subject_name_.c_str(), subjects_);
    // 1 while a painted face shows instead of the text.
    UI_MANAGED_SUBJECT_INT(face_subject_, 0, face_subject_name_.c_str(), subjects_);
    attr_storage_ = {"size_subject", size_subject_name_, "face_subject", face_subject_name_};
    for (const auto& a : attr_storage_) {
        attrs_.push_back(a.c_str());
    }
    attrs_.push_back(nullptr);
}

void ClockWidget::set_config(const nlohmann::json& config) {
    config_ = config.is_object() ? config : nlohmann::json::object();
    face_ = parse_clock_face(json_util::safe_string(config_, "face"));
}

ClockWidget::~ClockWidget() {
    detach();
}

void ClockWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Populate clock values immediately for initial display
    update_clock();
    // A recycled instance gets a new tree: its face goes back on at the size it last had.
    apply_face();

    spdlog::debug("[ClockWidget] Attached");
}

void ClockWidget::detach() {
#if HELIX_ENABLE_SCREENSAVER
    picker_.hide();
    face_view_.hide();
#endif
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
    active_ = true;
#if HELIX_ENABLE_SCREENSAVER
    // A face paints nothing while the display sleeps. Under a screensaver the active panel is
    // suspended, which deactivates this widget, so the saver's own measurement never counts
    // a tile's repaint.
    display_asleep_ = DisplaySleepListener::display_sleeping();
    sleep_listener_.listen([this](bool sleeping) { on_display_sleep(sleeping); });
#endif
    update_clock();
    apply_face();

    if (!clock_timer_) {
        clock_timer_ = lv_timer_create(clock_timer_cb, kClockUpdateIntervalMs, this);
        spdlog::debug("[ClockWidget] Started clock timer ({}ms interval)", kClockUpdateIntervalMs);
    }
}

void ClockWidget::on_deactivate() {
    active_ = false;
    // Off screen a face paints nothing; it keeps its canvas for the way back.
    apply_face();
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
                                      helix::display_format::format_time(&t).c_str(), font)));
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
                                          helix::locale::format_localized_date(&t).c_str(), font)));
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

    lv_subject_set_int(&size_subject_, mode);

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

    spdlog::trace("[ClockWidget] Size changed: {}x{} -> mode {}", colspan, rowspan, mode);
    apply_face();
}

bool ClockWidget::has_edit_configure() const {
#if HELIX_ENABLE_SCREENSAVER
    return true;
#else
    return false;
#endif
}

bool ClockWidget::on_edit_configure() {
#if HELIX_ENABLE_SCREENSAVER
    picker_.show_for(parent_screen_, widget_obj_, face_, [this](ClockFace f) { select_face(f); });
#endif
    return false; // no rebuild: the face is applied in place
}

void ClockWidget::select_face(ClockFace face) {
    face_ = face;
    config_["face"] = clock_face_id(face);
    save_widget_config(config_);
    apply_face();
    spdlog::info("[ClockWidget] {} face now {}", instance_id_, clock_face_id(face));
}

void ClockWidget::on_display_sleep(bool sleeping) {
#if HELIX_ENABLE_SCREENSAVER
    display_asleep_ = sleeping;
    apply_face();
#else
    (void)sleeping;
#endif
}

void ClockWidget::apply_face() {
#if HELIX_ENABLE_SCREENSAVER
    lv_obj_t* root = widget_obj_.get();
    if (!active_ || display_asleep_) {
        // Hidden, a face keeps its canvas and paints nothing; one never built waits until seen.
        face_view_.pause();
        return;
    }
    ClockFace shown = ClockFace::TEXT;
    ClockCanvasSize canvas{};
    if (root != nullptr && has_granted_size()) {
        // The face fills the tile inside its padding, within the board's memory budget.
        const int32_t w = granted_width_px() -
                          static_cast<int32_t>(lv_obj_get_style_pad_left(root, LV_PART_MAIN) +
                                               lv_obj_get_style_pad_right(root, LV_PART_MAIN));
        const int32_t h = granted_height_px() -
                          static_cast<int32_t>(lv_obj_get_style_pad_top(root, LV_PART_MAIN) +
                                               lv_obj_get_style_pad_bottom(root, LV_PART_MAIN));
        canvas = clock_face_canvas(face_, w, h,
                                   clock_face_area_budget_px(PlatformCapabilities::cached().tier));
        shown = clock_face_shown(face_, canvas.w, canvas.h);
    }
    if (shown == ClockFace::TEXT) {
        face_view_.hide();
        lv_subject_set_int(&face_subject_, 0);
        return;
    }
    // The face sits on the home card: its surround is the card's colour, in either theme.
    const lv_color_t card = theme_manager_get_color("card_bg");
    const ui::Rgb backdrop{card.red, card.green, card.blue};
    lv_obj_t* built = face_view_.canvas_obj();
    if (face_view_.face() == shown && built != nullptr && lv_obj_get_parent(built) == root &&
        face_view_.width() == canvas.w && face_view_.height() == canvas.h &&
        face_view_.backdrop() == backdrop) {
        face_view_.resume(ui::saver_clock_now());
        return;
    }
    const ClockFaceTempo tempo = clock_face_tempo(
        shown, ScreensaverManager::instance().standing_here(clock_face_saver(shown)),
        PlatformCapabilities::cached().cpu_cores);
    const bool shows =
        face_view_.show(root, shown, canvas.w, canvas.h, tempo, ui::saver_clock_now(), backdrop);
    lv_subject_set_int(&face_subject_, shows ? 1 : 0);
#endif
}

void ClockWidget::update_clock() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    struct tm* tm_info = localtime_r(&now, &tm_buf);

    if (tm_info) {
        // Time — use user's preferred format (12h/24h)
        std::string time_str = helix::display_format::format_time(tm_info);
        lv_subject_set_string(&s_time_subject, time_str.c_str());

        // Date — locale-aware formatting
        std::string date_str = helix::locale::format_localized_date(tm_info);
        lv_subject_set_string(&s_date_subject, date_str.c_str());
    }

    // Uptime from /proc/uptime
    char uptime_buf[64];
    format_uptime(uptime_buf, sizeof(uptime_buf));
    lv_subject_set_string(&s_uptime_subject, uptime_buf);
}

void ClockWidget::clock_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ClockWidget*>(lv_timer_get_user_data(timer));
    if (self) {
        self->update_clock();
    }
}
