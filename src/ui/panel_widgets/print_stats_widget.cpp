// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_stats_widget.h"

#include "ui_event_safety.h"
#include "ui_next_tick.h"
#include "ui_panel_history_dashboard.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "locale_formats.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "print_activity.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <string>

using helix::ui::measure_text_px;

namespace {

// The verdict and texts the widget publishes, read by panel_widget_print_stats.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
struct Subjects {
    lv_subject_t rung{};
    lv_subject_t content{};
    lv_subject_t support{};
    lv_subject_t show_bars{};
    lv_subject_t show_day_labels{};
    lv_subject_t side_by_side{};
    lv_subject_t strip_text{};
    lv_subject_t hero{};
    lv_subject_t hero_label{};
    lv_subject_t support_text{};
    lv_subject_t day[helix::kActivityDays]{};
    lv_subject_t day_label[helix::kActivityDays]{};
    char hero_buf[24]{};
    char hero_label_buf[96]{};
    char support_text_buf[160]{};
    char day_label_buf[helix::kActivityDays][16]{};
};
HELIX_PSRAM_BSS Subjects s_subj;
helix::WidgetSubjects s_subjects{"PrintStatsWidgetSubjects"};
// The instance bound to the live tile. Only it publishes: a transient instance (a
// rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::PrintStatsWidget> s_tile;

void print_stats_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_subj.rung, static_cast<int>(helix::SizeRung::Cell),
                               "print_stats_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.content, static_cast<int>(helix::PrintStatsContent::Loading),
                               "print_stats_content", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.support, static_cast<int>(helix::PrintStatsSupport::None),
                               "print_stats_support", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_bars, 0, "print_stats_show_bars", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_day_labels, 0, "print_stats_show_day_labels",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.side_by_side, 0, "print_stats_side_by_side", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.strip_text,
                               static_cast<int>(helix::PrintStatsStripText::HeadingWeek),
                               "print_stats_strip_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.hero, s_subj.hero_buf, "--", "print_stats_hero",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.hero_label, s_subj.hero_label_buf, "",
                                  "print_stats_hero_label", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.support_text, s_subj.support_text_buf, "",
                                  "print_stats_support_text", s_subjects);
        // Subject names are held by pointer, so they must outlive this function.
        static const char* const kDayNames[helix::kActivityDays] = {
            "print_stats_day_0", "print_stats_day_1", "print_stats_day_2", "print_stats_day_3",
            "print_stats_day_4", "print_stats_day_5", "print_stats_day_6"};
        static const char* const kDayLabelNames[helix::kActivityDays] = {
            "print_stats_day_label_0", "print_stats_day_label_1", "print_stats_day_label_2",
            "print_stats_day_label_3", "print_stats_day_label_4", "print_stats_day_label_5",
            "print_stats_day_label_6"};
        for (int i = 0; i < helix::kActivityDays; ++i) {
            UI_MANAGED_SUBJECT_INT(s_subj.day[i], 0, kDayNames[i], s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_subj.day_label[i], s_subj.day_label_buf[i], "",
                                      kDayLabelNames[i], s_subjects);
        }

        spdlog::debug("[PrintStatsWidget] Subjects initialized");
    });
}

using helix::line_px;

/// The Strip's short label: "/wk" for this week's figure, the label itself otherwise.
std::string strip_short_label(helix::PrintStatsContent content, const std::string& label) {
    return content == helix::PrintStatsContent::Activity ? std::string(lv_tr("/wk")) : label;
}

/// The minimum content box each rung draws in. Mirrors panel_widget_print_stats.xml:
/// the root's gap is space_xxs between rows, space_xs between the hero and the bars
/// when they sit side by side, and space_xs between bars. The hero is budgeted at a
/// fixed worst-case sample so the rung holds while the hours change. The support line
/// is not budgeted here: the decision measures it as it would draw and drops it to a
/// shorter level when it does not fit.
helix::RungNeeds measure_needs(const helix::ContentChrome& chrome) {
    using helix::rung_box;
    const lv_font_t* heading = theme_manager_get_font("font_heading");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const lv_font_t* xs = theme_manager_get_font("font_xs");
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    const int heading_h = line_px(heading);
    const int small_h = line_px(small);

    const int hero_w = measure_text_px(helix::format_print_hours(888.0).c_str(), heading);
    const int label_w = measure_text_px(lv_tr("this week"), small);
    const int text_w = std::max(hero_w, label_w);
    const int hero_block_h = heading_h + small_h;
    const int bar_w = col_gap;
    const int bars_w = helix::kActivityDays * bar_w + (helix::kActivityDays - 1) * col_gap;
    // Bars shorter than a heading line stop reading as a chart; the baseline is 1px.
    const int bars_h = heading_h + 1;

    helix::RungNeeds needs;
    // The hero and its label share one line; the label ellipsizes, so only its first
    // letters are budgeted.
    const int label_min_w = measure_text_px("th…", small);
    needs.strip = rung_box(hero_w + col_gap + label_min_w, heading_h);
    needs.cell = rung_box(text_w, hero_block_h);
    needs.wide =
        rung_box(text_w + col_gap + bars_w, std::max(hero_block_h, bars_h) + row_gap + small_h);
    // Full is a size class as well as a content fit: a 2x1 tile can hold the content,
    // but the rung is for 2x2 and up.
    needs.full =
        rung_box(std::max({text_w, bars_w, helix::widget_size::w_normal() - chrome.w}),
                 std::max(hero_block_h + row_gap + bars_h + line_px(xs) + row_gap + small_h,
                          helix::widget_size::h_tall() - chrome.h));
    return needs;
}

} // namespace

namespace helix {

void register_print_stats_widget() {
    register_widget_factory(
        "print_stats", [](const std::string&) { return std::make_unique<PrintStatsWidget>(); });
    register_widget_subjects("print_stats", print_stats_init_subjects);
    lv_xml_register_event_cb(nullptr, "print_stats_clicked_cb",
                             PrintStatsWidget::print_stats_clicked_cb);
}

PrintStatsWidget::PrintStatsWidget() = default;

PrintStatsWidget::~PrintStatsWidget() {
    detach();
}

PrintStatsTileVerdict PrintStatsWidget::decide(int width_px, int height_px,
                                               PrintStatsContent content, bool lifetime_known,
                                               const std::string& hero,
                                               const std::string& label) const {
    // The root's padding, as panel_widget_print_stats.xml sets it. The needs and the
    // support line's height count the gap above the support line.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_sm", "space_xxs");
    const lv_font_t* small = theme_manager_get_font("font_small");
    PrintStatsTileLayoutInput li;
    li.content_w = width_px - chrome.w;
    li.content_h = height_px - chrome.h;
    li.content = content;
    li.lifetime_known = lifetime_known;
    li.needs = measure_needs(chrome);
    if (lifetime_known) {
        li.count_line_w = measure_text_px(count_line().c_str(), small);
        li.full_line_w = measure_text_px(full_line().c_str(), small);
    }
    li.support_line_h = line_px(small) + theme_manager_get_spacing("space_xxs");
    // The Strip line in each font pair, the full label then the short one. The hero is
    // hidden when there is no history, and so is its gap.
    const lv_font_t* heading = theme_manager_get_font("font_heading");
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* xs = theme_manager_get_font("font_xs");
    const lv_font_t* hero_fonts[] = {heading, body, body, small};
    const lv_font_t* label_fonts[] = {small, small, xs, xs};
    const bool hero_shown = content != PrintStatsContent::NoHistory;
    const std::string short_label = strip_short_label(content, label);
    const int col_gap = theme_manager_get_spacing("space_xs");
    for (int i = 0; i < kPrintStatsStripTexts; ++i) {
        const int pair = i % 4;
        const std::string& text = i < 4 ? label : short_label;
        const int hero_w =
            hero_shown ? measure_text_px(hero.c_str(), hero_fonts[pair]) + col_gap : 0;
        li.strip_line_w[i] = hero_w + measure_text_px(text.c_str(), label_fonts[pair]);
    }
    const auto& n = li.needs;
    spdlog::trace("[PrintStatsWidget] box {}x{} chrome {}x{} needs s{}x{} c{}x{} w{}x{} f{}x{} "
                  "content={}",
                  width_px, height_px, chrome.w, chrome.h, n.strip.w, n.strip.h, n.cell.w, n.cell.h,
                  n.wide.w, n.wide.h, n.full.w, n.full.h, static_cast<int>(content));
    return decide_print_stats_tile_layout(li);
}

std::string PrintStatsWidget::count_line() const {
    return format_print_count(lifetime_totals_.total_jobs);
}

std::string PrintStatsWidget::full_line() const {
    return format_lifetime_line(lifetime_totals_, true);
}

void PrintStatsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    s_tile.claim(this);
    install_delete_hook(widget_obj_);

    // attach() runs inside a ScopedFreeze (rebuild_widget_grid), so queue_update()
    // callbacks are silently dropped: nothing is fetched here. Data loading is
    // deferred to on_activate().
    if (auto* hm = get_print_history_manager()) {
        auto token = lifetime_.token();
        history_observer_ = [this, token]() {
            if (token.expired())
                return;
            // A finished print changes the server totals too, so re-ask.
            fetch_lifetime_totals();
            refresh();
        };
        hm->add_observer(&history_observer_);
    }

    language_observer_ =
        helix::ui::observe_language_change(this, [](PrintStatsWidget* self) { self->refresh(); });

    // Instances are recycled across rebuilds and the size callback may not follow.
    refresh();
    spdlog::debug("[PrintStatsWidget] Attached");
}

void PrintStatsWidget::on_activate() {
    // Instances are recycled across grid rebuilds, so this runs again on every
    // activation: the totals request is the only thing that re-seeds a fresh
    // instance's lifetime figures (attach() cannot, see above).
    fetch_lifetime_totals();

    auto* hm = get_print_history_manager();
    if (!hm)
        return;

    if (hm->is_loaded(helix::HistoryScope::RECENT)) {
        refresh();
    }

    // The seven bars reach back to the oldest local midnight, whether or not the
    // cache is already populated: a startup slice that stops short of that
    // undercounts on a busy printer. Deferred to next tick so it runs outside any
    // ScopedFreeze. ensure_covers_since(), not fetch(): fetch() means "the cached
    // list is wrong", so asking it for a populate while a request is already out
    // queues a second identical one.
    helix::ui::run_next_tick(lifetime_.token(), []() {
        if (auto* history = get_print_history_manager()) {
            history->ensure_covers_since(local_day_starts(std::time(nullptr))[0]);
        }
    });
}

void PrintStatsWidget::detach() {
    lifetime_.invalidate();
    // invalidate() drops any totals callback still in flight, so the guard has
    // to be cleared by hand: this instance may be recycled into the next grid
    // rebuild, and a stale flag would block its refetch forever.
    lifetime_totals_in_flight_ = false;
    if (auto* hm = get_print_history_manager()) {
        hm->remove_observer(&history_observer_);
    }
    history_observer_ = nullptr;
    language_observer_.reset();

    uninstall_delete_hook();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    if (PrintStatsWidget* displaced = s_tile.release(this))
        displaced->refresh();

    spdlog::debug("[PrintStatsWidget] Detached");
}

void PrintStatsWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: drop pointers and expire the guard only.
    //
    // refresh() publishes through static lv_subject_t values rather than into the
    // tile tree, so a drained totals callback is harmless on its own. What is not
    // harmless is widget_obj_ outliving the object it names: a later detach() would
    // call lv_obj_set_user_data() on freed memory.
    lifetime_.invalidate();
    lifetime_totals_in_flight_ = false;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void PrintStatsWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                       int height_px) {
    granted_w_ = width_px;
    granted_h_ = height_px;
    refresh();
}

bool PrintStatsWidget::fits_at(int width_px, int height_px) const {
    return decide(width_px, height_px, PrintStatsContent::Activity, false).fits;
}

void PrintStatsWidget::fetch_lifetime_totals() {
    if (lifetime_totals_in_flight_)
        return;

    IMoonrakerAPI* api = get_moonraker_api();
    if (!api)
        return;

    lifetime_totals_in_flight_ = true;

    // Both callbacks fire on the HTTP/WebSocket thread and touch members, so
    // both go through bg_cb: it marshals the whole body to the main thread and
    // drops it if the widget was torn down while the request was in flight.
    api->history().get_history_totals(
        lifetime_.bg_cb("PrintStatsWidget::get_history_totals",
                        [this](const PrintHistoryTotals& totals) {
                            lifetime_totals_in_flight_ = false;
                            lifetime_totals_ = totals;
                            lifetime_totals_valid_ = true;
                            refresh();
                        }),
        lifetime_.bg_cb("PrintStatsWidget::get_history_totals_error",
                        [this](const MoonrakerError& error) {
                            lifetime_totals_in_flight_ = false;
                            spdlog::warn("[PrintStatsWidget] Totals fetch failed, the lifetime "
                                         "line stays hidden: {}",
                                         error.message);
                        }));
}

void PrintStatsWidget::refresh() {
    if (granted_w_ <= 0 || granted_h_ <= 0 || !s_tile.is_owner(this))
        return;

    auto* hm = get_print_history_manager();
    const bool loaded = hm && hm->is_loaded(helix::HistoryScope::RECENT);
    const int cached_jobs = hm ? static_cast<int>(hm->get_jobs().size()) : 0;
    const int64_t lifetime_prints =
        lifetime_totals_valid_ ? static_cast<int64_t>(lifetime_totals_.total_jobs) : -1;
    const PrintStatsContent content = print_stats_content(loaded, cached_jobs, lifetime_prints);
    const ActivityDayStarts starts = local_day_starts(std::time(nullptr));
    const WeekActivity week = hm ? summarize_week(hm->get_jobs(), starts) : WeekActivity{};
    const auto bars = scale_day_bars(week.hours_by_day);

    std::string hero = "--";
    std::string hero_label = lv_tr("this week");
    if (content == PrintStatsContent::NoHistory) {
        hero = format_print_hours(0.0);
        hero_label = lv_tr("No prints yet");
    } else if (content == PrintStatsContent::Activity) {
        hero = format_print_hours(week.week_hours);
        if (week.week_prints == 0)
            hero_label = lv_tr("No prints this week");
    }

    const PrintStatsTileVerdict verdict =
        decide(granted_w_, granted_h_, content, lifetime_totals_valid_, hero, hero_label);
    if (verdict.strip_text >= PrintStatsStripText::HeadingShort)
        hero_label = strip_short_label(content, hero_label);

    std::string support_text;
    if (verdict.support == PrintStatsSupport::PrintsOnly) {
        support_text = count_line();
    } else if (verdict.support == PrintStatsSupport::Full) {
        support_text = full_line();
    }

    helix::ui::set_int_if_changed(&s_subj.rung, static_cast<int>(verdict.rung));
    helix::ui::set_int_if_changed(&s_subj.content, static_cast<int>(content));
    helix::ui::set_int_if_changed(&s_subj.support, static_cast<int>(verdict.support));
    helix::ui::set_int_if_changed(&s_subj.show_bars, verdict.show_bars ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_day_labels, verdict.show_day_labels ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.side_by_side, verdict.side_by_side ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.strip_text, static_cast<int>(verdict.strip_text));
    helix::ui::copy_string_if_changed(&s_subj.hero, hero.c_str());
    helix::ui::copy_string_if_changed(&s_subj.hero_label, hero_label.c_str());
    helix::ui::copy_string_if_changed(&s_subj.support_text, support_text.c_str());
    for (int i = 0; i < kActivityDays; ++i) {
        const std::time_t day_start = static_cast<std::time_t>(starts[static_cast<size_t>(i)]);
        std::tm local{};
        localtime_r(&day_start, &local);
        helix::ui::set_int_if_changed(&s_subj.day[i], bars[static_cast<size_t>(i)]);
        helix::ui::copy_string_if_changed(
            &s_subj.day_label[i],
            weekday_initial(ui::format_localized_weekday_abbr(&local)).c_str());
    }

    spdlog::debug("[PrintStatsWidget] refresh {}x{} content={} rung={} support={} week={} "
                  "prints, {} cached",
                  granted_w_, granted_h_, static_cast<int>(content), static_cast<int>(verdict.rung),
                  static_cast<int>(verdict.support), week.week_prints, cached_jobs);
}

void PrintStatsWidget::handle_clicked() {
    if (parent_screen_)
        get_global_history_dashboard_panel().show(parent_screen_);
}

void PrintStatsWidget::print_stats_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatsWidget] print_stats_clicked_cb");
    if (auto* self = panel_widget_from_event<PrintStatsWidget>(e)) {
        self->record_interaction();
        self->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
