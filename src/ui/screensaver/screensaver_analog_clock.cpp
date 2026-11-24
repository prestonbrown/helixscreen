// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_analog_clock.h"

#include "display_settings_manager.h"
#include "locale_formats.h"
#include "screensaver_clock.h"
#include "screensaver_motion.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <fmt/format.h>

namespace helix::ui {

namespace {

/// struct tm for `date`, as the locale formatters read it.
struct tm tm_for(const ClockDate& date) {
    struct tm t {};
    t.tm_year = date.year - 1900;
    t.tm_mon = date.month - 1;
    t.tm_mday = date.day;
    t.tm_wday = date.weekday;
    t.tm_hour = 12;
    return t;
}

/// The first character of `s`, a whole UTF-8 sequence.
std::string first_char(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const auto lead = static_cast<unsigned char>(s[0]);
    const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    return s.substr(0, std::min(len, s.size()));
}

/// "2:31" for a countdown of 2 h 30 min 1 s to 2 h 31 min.
std::string hours_minutes(int32_t seconds) {
    const int32_t minutes = analog_countdown_minutes(seconds);
    return fmt::format("{}:{:02}", minutes / 60, minutes % 60);
}

} // namespace

AnalogStyle analog_style_for_run(int setting, bool random_saver, int last, uint32_t draw) {
    const int count = static_cast<int>(kAnalogStyleCount);
    if (!random_saver && setting >= 0 && setting < count) {
        return static_cast<AnalogStyle>(setting);
    }
    if (last < 0 || last >= count) {
        return static_cast<AnalogStyle>(draw % static_cast<uint32_t>(count));
    }
    // One of the other faces, each as likely.
    const int step = 1 + static_cast<int>(draw % static_cast<uint32_t>(count - 1));
    return static_cast<AnalogStyle>((last + step) % count);
}

bool AnalogClockScreensaver::on_start() {
    auto& settings = DisplaySettingsManager::instance();
    const bool random_saver =
        settings.get_screensaver_type() == static_cast<int>(ScreensaverType::RANDOM);
    style_ = analog_style_for_run(settings.get_screensaver_clock_face(), random_saver, last_style_,
                                  screensaver::random_int(rng(), 0, 1 << 20));
    last_style_ = static_cast<int>(style_);

    clock_.reset();
    clock_.step(0, read_input());
    start_sim(face());

    dial_canvas_.reset(lv_canvas_create(overlay().obj()));
    // DECLARATIVE_OK: an offscreen canvas over the sim's dial, which LVGL draws text into.
    lv_obj_set_hidden(dial_canvas_.get(), true);
    write_texts();
    sim_.repaint();
    std::vector<DirtyRect> dirty;
    FrameTarget frame = canvas().frame();
    sim_.compose(frame, dirty);
    spdlog::debug("[Screensaver] analog clock face {}", static_cast<int>(style_));
    return true;
}

void AnalogClockScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    clock_.step(dt_ms, read_input());
    FrameTarget frame = canvas().frame();
    if (sim_.prepare(dt_ms, frame, face())) {
        write_texts();
    }
    sim_.compose(frame, dirty);
}

void AnalogClockScreensaver::on_stop() {
    SimSaver<AnalogClockSim>::on_stop();
    // The overlay deletes the canvas with itself.
    dial_canvas_.reset();
}

void AnalogClockScreensaver::write_texts() {
    lv_obj_t* canvas_obj = dial_canvas_.get();
    if (canvas_obj == nullptr || sim_.texts().empty()) {
        return;
    }
    const FrameTarget dial = sim_.dial();
    if (dial.data == nullptr) {
        return;
    }
    lv_draw_buf_init(&dial_buf_, dial.w, dial.h, kLvDrawCanvasFormat, dial.stride, dial.data,
                     dial.stride * dial.h);
    lv_canvas_set_draw_buf(canvas_obj, &dial_buf_);

    // The strings outlive the layer: LVGL draws them when the layer finishes.
    std::vector<std::string> words;
    words.reserve(sim_.texts().size());
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_obj, &layer);
    for (const AnalogText& t : sim_.texts()) {
        words.push_back(text_for(t));
        const std::string& word = words.back();
        if (word.empty() || t.box.empty()) {
            continue;
        }
        const int32_t bw = t.box.x2 - t.box.x1 + 1;
        const int32_t bh = t.box.y2 - t.box.y1 + 1;
        const lv_font_t* font = clock_font_fitting(bh, bw, word.c_str(), t.display);
        if (font == nullptr) {
            continue;
        }
        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.font = font;
        label.color = lv_color_make(t.color.r, t.color.g, t.color.b);
        label.opa = LV_OPA_COVER;
        label.align = t.align == AnalogAlign::LEFT ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
        label.text = word.c_str();
        label.flag = LV_TEXT_FLAG_EXPAND;
        const int32_t font_h = theme_manager_get_font_height(font);
        const int32_t top = t.box.y1 + std::max(0, (bh - font_h) / 2);
        const lv_area_t area{t.box.x1, top, t.box.x2, t.box.y2};
        lv_draw_label(&layer, &label, &area);
    }
    lv_canvas_finish_layer(canvas_obj, &layer);
}

std::string AnalogClockScreensaver::text_for(const AnalogText& text) const {
    // Time left in whole minutes, rounded up, as the sim keys its redraws on.
    ClockReading r = clock_.reading();
    r.countdown_s = analog_countdown_minutes(r.countdown_s) * 60;
    switch (text.role) {
    case AnalogTextRole::DATE_WINDOW: {
        const struct tm t = tm_for(r.date);
        return fmt::format("{} {}", helix::locale::format_localized_weekday_abbr(&t), r.date.day);
    }
    case AnalogTextRole::MAKER:
        return "HelixScreen";
    case AnalogTextRole::CAPTION_24H:
        return lv_tr("24H");
    case AnalogTextRole::CAPTION_PRINT:
        return lv_tr("PRINT");
    case AnalogTextRole::VALUE_PRINT:
        return fmt::format("{}%", r.mode == ClockMode::DONE ? 100 : r.progress_pct);
    case AnalogTextRole::CAPTION_LEFT:
        return lv_tr("LEFT");
    case AnalogTextRole::VALUE_LEFT:
        return hours_minutes(r.countdown_s);
    case AnalogTextRole::CAPTION_ENDS:
        return lv_tr("ENDS");
    case AnalogTextRole::VALUE_ENDS:
        return clock_minute_text(r.eta_minute_of_day);
    case AnalogTextRole::STATE_WORD:
        return clock_print_text(r).eta;
    case AnalogTextRole::WEEKDAY_LETTER: {
        ClockDate day = r.date;
        day.weekday = text.index;
        const struct tm t = tm_for(day);
        return first_char(helix::locale::format_localized_weekday_abbr(&t));
    }
    case AnalogTextRole::COUNTDOWN: {
        const ClockPrintText p = clock_print_text(r);
        return r.mode == ClockMode::COUNTDOWN ? p.left : p.eta;
    }
    case AnalogTextRole::COLUMN_WEEKDAY:
        return clock_date_text(r.date).weekday;
    case AnalogTextRole::COLUMN_DATE:
        return clock_date_text(r.date).date;
    case AnalogTextRole::COLUMN_STATUS:
        return clock_print_text(r).eta;
    case AnalogTextRole::COLUMN_DETAIL: {
        const int32_t pct = r.mode == ClockMode::DONE ? 100 : r.progress_pct;
        const ClockPrintText p = clock_print_text(r);
        return p.left.empty() ? fmt::format("{}%", pct) : fmt::format("{}% · {}", pct, p.left);
    }
    }
    return {};
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
