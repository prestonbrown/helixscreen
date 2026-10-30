// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_widget.h"

#include "ui_event_safety.h"
#include "ui_filename_utils.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "helix_psram_attr.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "queued_job_start.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_io.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

using helix::ui::measure_text_px;
using helix::ui::widest_word_px;

namespace {

// The verdict and texts the widget publishes, read by panel_widget_job_queue.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
struct Subjects {
    lv_subject_t state{};
    lv_subject_t action{};
    lv_subject_t rung{};
    lv_subject_t show_thumb{};
    lv_subject_t show_detail{};
    lv_subject_t show_auto{};
    lv_subject_t show_more{};
    lv_subject_t list_rows{};
    lv_subject_t hero{};
    lv_subject_t count{};
    lv_subject_t cell_name{};
    lv_subject_t detail{};
    lv_subject_t more{};
    lv_subject_t action_text{};
    lv_subject_t after[helix::kJobQueueTileListRows]{};
    char hero_buf[320]{};
    char count_buf[16]{};
    char cell_name_buf[320]{};
    char detail_buf[320]{};
    char more_buf[64]{};
    char action_text_buf[96]{};
    char after_buf[helix::kJobQueueTileListRows][320]{};
};
HELIX_PSRAM_BSS Subjects s_subj;
helix::WidgetSubjects s_subjects{"JobQueueWidgetSubjects"};
// The instance bound to the live tile. Only it publishes: a transient instance (a
// rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::JobQueueWidget> s_tile;

void job_queue_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_subj.state, static_cast<int>(helix::JobQueueTileState::NotLoaded),
                               "jq_tile_state", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.action, static_cast<int>(helix::JobQueueTileAction::None),
                               "jq_tile_action", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.rung, static_cast<int>(helix::SizeRung::Cell), "jq_tile_rung",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_thumb, 0, "jq_tile_show_thumb", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_detail, 0, "jq_tile_show_detail", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_auto, 0, "jq_tile_show_auto", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.show_more, 0, "jq_tile_show_more", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_subj.list_rows, 0, "jq_tile_list_rows", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.hero, s_subj.hero_buf, "", "jq_tile_hero", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.count, s_subj.count_buf, "", "jq_tile_count", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.cell_name, s_subj.cell_name_buf, "", "jq_tile_cell_name",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.detail, s_subj.detail_buf, "", "jq_tile_detail",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.more, s_subj.more_buf, "", "jq_tile_more", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_subj.action_text, s_subj.action_text_buf, "",
                                  "jq_tile_action_text", s_subjects);
        // Subject names are held by pointer, so they must outlive this function.
        static const char* const kAfterNames[helix::kJobQueueTileListRows] = {
            "jq_tile_after_0", "jq_tile_after_1", "jq_tile_after_2"};
        for (int i = 0; i < helix::kJobQueueTileListRows; ++i) {
            UI_MANAGED_SUBJECT_STRING(s_subj.after[i], s_subj.after_buf[i], "", kAfterNames[i],
                                      s_subjects);
        }
    });
}

using helix::line_px;

/// @p text as it fits @p width in @p font on at most @p max_lines wrapped lines, cut at
/// the end with an ellipsis when it does not. @p lines_out gets the lines it takes.
std::string fit_text_to_lines(const std::string& text, const lv_font_t* font, int width,
                              int max_lines, int* lines_out) {
    const int line_h = line_px(font);
    const auto lines_of = [&](const std::string& t) {
        lv_point_t size{};
        lv_text_get_size(&size, t.c_str(), font, 0, 0, width, LV_TEXT_FLAG_NONE);
        return line_h > 0 ? std::max(1, static_cast<int>(size.y) / line_h) : 1;
    };
    std::string fitted = text;
    int lines = lines_of(fitted);
    std::string base = text;
    while (lines > max_lines && !base.empty()) {
        // Drop one UTF-8 character.
        base = std::string(helix::text_io::utf8_prefix(base, base.size() - 1));
        fitted = base + "…";
        lines = lines_of(fitted);
    }
    if (lines_out)
        *lines_out = std::min(lines, max_lines);
    return fitted;
}

/// The fonts, spacing and line heights panel_widget_job_queue.xml draws with.
struct TileMetrics {
    const lv_font_t* heading = theme_manager_get_font("font_heading");
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    int row_gap = theme_manager_get_spacing("space_xxs");
    int col_gap = theme_manager_get_spacing("space_xs");
    int btn = theme_manager_get_spacing("button_height_sm");
    int icon = line_px(theme_manager_get_font("icon_font_xs"));
    int heading_h = line_px(heading);
    int body_h = line_px(body);
    int small_h = line_px(small);
    /// One read-only job_queue_row: a body line padded space_sm above and below, and the
    /// list's gap before it.
    int list_row_h = line_px(body) + 2 * theme_manager_get_spacing("space_sm") + row_gap;
};

/// The minimum content box each rung draws in for @p state. Mirrors
/// panel_widget_job_queue.xml: the root's gap is space_xxs, the rows' space_xs, and the
/// text column stacks with no gap. Names ellipsize, so they are budgeted at the least
/// they can show; the queued count, "Starts automatically" and the button labels do not,
/// so they are budgeted at their full width in the current language. @p hero is the
/// composed hero text, which for the count states is what the Cell and Wide rungs draw.
helix::RungNeeds measure_needs(const helix::ContentChrome& chrome, helix::JobQueueTileState state,
                               const std::string& hero) {
    using helix::JobQueueTileState;
    using helix::rung_box;
    const TileMetrics m;
    const bool waiting = state == JobQueueTileState::WaitingForStart;
    const bool counting =
        state == JobQueueTileState::AutoNext || state == JobQueueTileState::Printing;
    const bool automatic = state == JobQueueTileState::AutoNext;
    const bool quiet = !waiting && !counting && state != JobQueueTileState::Starting;

    helix::RungNeeds needs;
    if (quiet) {
        // The caption alone on one line at Strip, under the muted icon from Cell, where
        // it wraps between words, so its widest word is the width budgeted there.
        const std::string caption =
            state == JobQueueTileState::NotLoaded ? std::string(lv_tr("Job Queue")) : hero;
        const int caption_w = widest_word_px(caption, m.small);
        needs.strip = rung_box(caption_w, m.small_h);
        needs.cell = rung_box(std::max(m.icon, caption_w), m.icon + m.row_gap + m.small_h);
        needs.wide = needs.cell;
        needs.full =
            rung_box(needs.wide.w, std::max(needs.wide.h, helix::widget_size::h_tall() - chrome.h));
        return needs;
    }

    // Strip: the queue icon and the count, with the icon-only start button while a job
    // waits. A two-digit count is budgeted so the rung holds while jobs come and go.
    const int count_w = m.icon + m.col_gap + measure_text_px("88", m.heading);
    needs.strip = waiting ? rung_box(count_w + m.col_gap + m.btn, std::max(m.heading_h, m.btn))
                          : rung_box(count_w, m.heading_h);

    const int name_body_w = measure_text_px("88…", m.body);
    const int name_heading_w = measure_text_px("88…", m.heading);
    if (!counting) {
        // Waiting, and Starting, which draws the same name and thumbnail for the seconds
        // the job loads and keeps the button's room so the tile does not change rung.
        const int short_w =
            measure_text_px(lv_tr("Start next"), m.body) + helix::button_label_chrome_w("space_xs");
        const int action_w = measure_text_px(lv_tr("Bed clear · Start next"), m.body) +
                             helix::button_label_chrome_w("space_xs");
        // Cell: the name over the short button. Wide: the thumbnail beside the name and
        // its estimate line, over the long button. The root's gap sits between.
        needs.cell = rung_box(std::max(name_body_w, short_w), m.body_h + m.row_gap + m.btn);
        needs.wide = rung_box(std::max(m.btn + m.col_gap + name_heading_w, action_w),
                              std::max(m.btn, m.heading_h + m.small_h) + m.row_gap + m.btn);
    } else {
        // "N queued" over "Next: name", and while the queue advances by itself
        // "Starts automatically", which wraps at Cell where the height allows it
        // (auto_line_h) and stays one line from Wide.
        const std::string auto_text = lv_tr("Starts automatically");
        const int detail_w = measure_text_px("Ne…", m.small);
        const int cell_auto_w = automatic ? widest_word_px(auto_text, m.small) : 0;
        const int wide_auto_w = automatic ? measure_text_px(auto_text.c_str(), m.small) : 0;
        needs.cell =
            rung_box(std::max({measure_text_px(hero.c_str(), m.body), detail_w, cell_auto_w}),
                     m.body_h + m.small_h);
        needs.wide =
            rung_box(std::max({measure_text_px(hero.c_str(), m.heading), detail_w, wide_auto_w}),
                     m.heading_h + m.small_h + (automatic ? m.small_h : 0));
    }

    // Full lists at least one following job. It is a size class as well as a content
    // fit: a 2x1 tile can hold the content, but the rung is for 2x2 and up.
    needs.full = rung_box(needs.wide.w, std::max(needs.wide.h + m.list_row_h,
                                                 helix::widget_size::h_tall() - chrome.h));
    return needs;
}

std::string next_display_name(const JobQueueState* jqs, size_t index) {
    if (!jqs || index >= jqs->get_jobs().size())
        return {};
    return helix::gcode::get_display_filename(jqs->get_jobs()[index].filename);
}

} // namespace

namespace helix {

JobQueueWidget::JobQueueWidget() = default;

JobQueueWidget::~JobQueueWidget() {
    detach();
}

JobQueueTileInputs JobQueueWidget::read_inputs() const {
    auto* jqs = get_job_queue_state();
    JobQueueTileInputs in;
    in.component_available = get_printer_state().is_job_queue_available();
    in.loaded = jqs && jqs->has_ever_loaded();
    in.job_count = jqs ? static_cast<int>(jqs->get_jobs().size()) : 0;
    in.queue_state = jqs ? jqs->run_state() : QueueRunState::Paused;
    in.printer_busy =
        lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0;
    in.automatic_transition = jqs && jqs->automatic_transition();
    in.start_in_flight = jqs && jqs->start_in_flight();
    return in;
}

JobQueueTileVerdict JobQueueWidget::decide(int width_px, int height_px,
                                           const JobQueueTileInputs& in,
                                           std::string* cell_name) const {
    if (cell_name)
        cell_name->clear();
    // The root's padding, as panel_widget_job_queue.xml sets it. measure_needs() counts
    // the gaps between the root's parts, which differ by rung.
    const ContentChrome chrome = content_padding_or_estimate(root(), "space_sm", "space_xxs");
    const TileMetrics m;
    JobQueueTileLayoutInput li;
    li.content_w = width_px - chrome.w;
    li.content_h = height_px - chrome.h;
    li.state = job_queue_tile_state(in);
    li.job_count = in.job_count;
    const std::string hero =
        compose_job_queue_tile_text(li.state, next_display_name(get_job_queue_state(), 0),
                                    in.job_count, 0, 0)
            .hero;
    li.needs = measure_needs(chrome, li.state, hero);
    li.more_line_h = m.small_h;
    li.list_row_h = m.list_row_h;
    li.auto_line_h = 2 * m.small_h;
    const auto& n = li.needs;
    spdlog::trace("[JobQueueWidget] box {}x{} chrome {}x{} needs s{}x{} c{}x{} w{}x{} f{}x{} "
                  "state={}",
                  width_px, height_px, chrome.w, chrome.h, n.strip.w, n.strip.h, n.cell.w, n.cell.h,
                  n.wide.w, n.wide.h, n.full.w, n.full.h, static_cast<int>(li.state));
    JobQueueTileVerdict verdict = decide_job_queue_tile_layout(li);
    if (verdict.rung != SizeRung::Cell)
        return verdict;

    // A Cell names the next job on up to two lines where the height holds the second,
    // in the body font while it waits and in the small font under "N queued".
    const bool counting =
        li.state == JobQueueTileState::AutoNext || li.state == JobQueueTileState::Printing;
    const lv_font_t* font = counting ? m.small : m.body;
    const int line_h = counting ? m.small_h : m.body_h;
    const int max_lines = li.content_h >= li.needs.cell.h + line_h ? 2 : 1;
    int lines = 1;
    std::string name = fit_text_to_lines(next_display_name(get_job_queue_state(), 0), font,
                                         li.content_w, max_lines, &lines);
    if (cell_name)
        *cell_name = std::move(name);
    li.cell_name_extra_h = (lines - 1) * line_h;
    return decide_job_queue_tile_layout(li);
}

void JobQueueWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    const auto token = lifetime_.token();
    const auto on_change = [token](JobQueueWidget* self, int) {
        if (!token.expired())
            self->schedule_refresh();
    };
    auto* jqs = get_job_queue_state();
    const SubjectLifetime queue_lifetime = jqs ? jqs->get_subjects_lifetime() : SubjectLifetime{};
    const auto observe_queue_subject = [&](const char* name) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        return subject ? ui::observe<int>(subject, this, on_change, queue_lifetime)
                       : ObserverGuard{};
    };
    count_observer_ = observe_queue_subject("job_queue_count");
    revision_observer_ = observe_queue_subject("job_queue_revision");
    start_observer_ = observe_queue_subject("job_queue_start_in_flight");
    run_state_observer_ = observe_queue_subject("job_queue_run_state");
    automatic_observer_ = observe_queue_subject("job_queue_automatic_transition");
    estimate_observer_ = observe_queue_subject("job_queue_next_estimate");
    print_active_observer_ =
        ui::observe<int>(get_printer_state().print_state().get_print_active_subject(), this,
                         on_change, get_printer_state().get_subjects_lifetime());
    // The texts hold lv_tr() output from when they were composed, and the widths the
    // rung was chosen on were measured in that language.
    language_observer_ = ui::observe_language_change(this, [token](JobQueueWidget* self) {
        if (!token.expired())
            self->refresh();
    });

    // Instances are recycled across rebuilds and the size callback may not follow.
    refresh();
    spdlog::debug("[JobQueueWidget] Attached");
}

void JobQueueWidget::detach() {
    lifetime_.invalidate();
    refresh_timer_.cancel();
    count_observer_.reset();
    revision_observer_.reset();
    start_observer_.reset();
    run_state_observer_.reset();
    automatic_observer_.reset();
    estimate_observer_.reset();
    print_active_observer_.reset();
    language_observer_.reset();
    if (JobQueueWidget* displaced = s_tile.release(this))
        displaced->refresh();
    spdlog::debug("[JobQueueWidget] Detached");
}

void JobQueueWidget::on_hooked_root_deleted() {
    lifetime_.invalidate();
    refresh_timer_.cancel();
}

void JobQueueWidget::on_activate() {
    if (auto* jqs = get_job_queue_state())
        jqs->fetch();
}

void JobQueueWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                     int height_px) {
    granted_w_ = width_px;
    granted_h_ = height_px;
    refresh();
}

bool JobQueueWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Strip in either the empty-looking state or
    // the one with the widest strip content.
    JobQueueTileInputs not_loaded;
    not_loaded.component_available = true;
    JobQueueTileInputs waiting = not_loaded;
    waiting.loaded = true;
    waiting.job_count = 1;
    return decide(width_px, height_px, not_loaded).fits &&
           decide(width_px, height_px, waiting).fits;
}

void JobQueueWidget::schedule_refresh() {
    refresh_timer_.schedule_once([this]() { refresh(); });
}

void JobQueueWidget::refresh() {
    if (granted_w_ <= 0 || granted_h_ <= 0)
        return;
    const JobQueueTileInputs in = read_inputs();
    verdict_ = decide(granted_w_, granted_h_, in, &cell_name_);
    if (!s_tile.is_owner(this))
        return;
    publish(in);
    spdlog::debug("[JobQueueWidget] refresh {}x{} state={} rung={} fits={} rows={}", granted_w_,
                  granted_h_, static_cast<int>(job_queue_tile_state(in)),
                  static_cast<int>(verdict_.rung), verdict_.fits, verdict_.list_rows);
}

void JobQueueWidget::publish(const JobQueueTileInputs& in) {
    auto* jqs = get_job_queue_state();
    const JobQueueTileState state = job_queue_tile_state(in);
    const JobQueueTileText text =
        compose_job_queue_tile_text(state, next_display_name(jqs, 0), in.job_count,
                                    jqs ? jqs->next_estimated_seconds() : 0, verdict_.more_count);

    helix::ui::set_int_if_changed(&s_subj.state, static_cast<int>(state));
    helix::ui::set_int_if_changed(&s_subj.action, static_cast<int>(job_queue_tile_action(state)));
    helix::ui::set_int_if_changed(&s_subj.rung, static_cast<int>(verdict_.rung));
    // A part with no text would still take a line of the column.
    helix::ui::set_int_if_changed(&s_subj.show_thumb, verdict_.show_thumbnail ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_detail,
                                  verdict_.show_detail && !text.detail.empty() ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_auto, verdict_.show_auto_line ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.show_more,
                                  verdict_.show_more && !text.more.empty() ? 1 : 0);
    helix::ui::set_int_if_changed(&s_subj.list_rows, verdict_.list_rows);
    helix::ui::copy_string_if_changed(&s_subj.hero, text.hero.c_str());
    helix::ui::copy_string_if_changed(&s_subj.count, text.count.c_str());
    helix::ui::copy_string_if_changed(&s_subj.detail, text.detail.c_str());
    helix::ui::copy_string_if_changed(&s_subj.cell_name, cell_name_.c_str());
    helix::ui::copy_string_if_changed(&s_subj.more, text.more.c_str());
    helix::ui::copy_string_if_changed(
        &s_subj.action_text,
        std::string(verdict_.rung == SizeRung::Cell ? text.action_short : text.action).c_str());
    for (int i = 0; i < kJobQueueTileListRows; ++i)
        helix::ui::copy_string_if_changed(
            &s_subj.after[i],
            std::string(next_display_name(jqs, static_cast<size_t>(i) + 1)).c_str());
}

void JobQueueWidget::on_tile_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[JobQueueWidget] on_tile_clicked");
    if (auto* self = panel_widget_from_event<JobQueueWidget>(e))
        self->record_interaction();
    open_job_queue_modal();
    LVGL_SAFE_EVENT_CB_END();
}

void JobQueueWidget::on_action_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[JobQueueWidget] on_action_clicked");
    // A start tap is not a tap on the tile, which opens the queue modal. The button
    // does not bubble today; this guards against it ever gaining event_bubble.
    lv_event_stop_bubbling(e);
    if (lv_subject_get_int(&s_subj.action) != static_cast<int>(JobQueueTileAction::StartNext))
        return;
    auto* jqs = get_job_queue_state();
    if (!jqs || jqs->get_jobs().empty())
        return;
    const JobQueueEntry job = jqs->get_jobs().front();
    queue::queued_job_starter().start(job);
    LVGL_SAFE_EVENT_CB_END();
}

void register_job_queue_widget() {
    register_widget_factory("job_queue",
                            [](const std::string&) { return std::make_unique<JobQueueWidget>(); });
    register_widget_subjects("job_queue", job_queue_init_subjects);

    lv_xml_register_event_cb(nullptr, "on_job_queue_widget_clicked",
                             JobQueueWidget::on_tile_clicked);
    lv_xml_register_event_cb(nullptr, "on_job_queue_tile_action",
                             JobQueueWidget::on_action_clicked);
}

} // namespace helix
