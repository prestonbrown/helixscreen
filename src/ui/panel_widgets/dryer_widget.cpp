// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dryer_widget.h"

#include "ui_ams_environment_overlay.h"
#include "ui_bed_drying_modal.h"
#include "ui_callback_helpers.h"
#include "ui_chamber_dryer_modal.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_subject_registry.h"
#include "ui_temperature_utils.h"
#include "ui_tile_rung.h"

#include "display_numbering.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix_psram_attr.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

using helix::ui::measure_text_px;

namespace {

constexpr int kMaxRows = 8;
constexpr int kMaxTimes = 4;

// The verdict, flags and texts the widget publishes, read by panel_widget_dryer.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
struct Flags {
    lv_subject_t rung{};
    lv_subject_t show_icon{};
    lv_subject_t show_drop{};
    lv_subject_t drop_rung{};
    lv_subject_t rh_x10{};
    lv_subject_t show_ring{};
    lv_subject_t ring_pct{};
    lv_subject_t show_info{};
    lv_subject_t show_time{};
    lv_subject_t show_name{};
    lv_subject_t show_temp{};
    lv_subject_t show_rh_detail{};
    lv_subject_t show_rh{};
    lv_subject_t show_status{};
    lv_subject_t show_end{};
    lv_subject_t show_count{};
    lv_subject_t show_times{};
    lv_subject_t row_count{};
};
HELIX_PSRAM_BSS Flags s_flags;

struct TextSubject {
    lv_subject_t subject{};
    char buf[64]{};
};
struct Texts {
    TextSubject time, name, temp, rh_detail, rh, status, end, count, times;
};
HELIX_PSRAM_BSS Texts s_texts;

struct RowSubjects {
    TextSubject name, time, detail;
    lv_subject_t pct{};
};
HELIX_PSRAM_BSS RowSubjects s_rows[kMaxRows];

helix::WidgetSubjects s_subjects{"DryerWidgetSubjects"};
bool s_rows_composed = false;

// The instance bound to the live tile is the only one that publishes.
helix::LiveTileOwner<helix::DryerWidget> s_tile;
std::function<void(const helix::DryerTap&)> s_open_hook;

void init_int(lv_subject_t& subject, int value, const char* name) {
    lv_subject_init_int(&subject, value);
    s_subjects.register_subject(&subject, name);
    helix::xml::register_subject_in_current_scope(name, &subject);
}

void init_text(TextSubject& text, const char* name) {
    lv_subject_init_string(&text.subject, text.buf, nullptr, sizeof(text.buf), "");
    s_subjects.register_subject(&text.subject, name);
    helix::xml::register_subject_in_current_scope(name, &text.subject);
}

void dryer_init_subjects() {
    // The gate and the summary the widget renders come first: the registry resolves the
    // gate subject before any widget exists. Every call, so a model that was deinited on
    // its own comes back with its sources attached.
    helix::dryer_model::init_subjects();
    s_subjects.ensure_init([] {
        init_int(s_flags.rung, static_cast<int>(helix::SizeRung::Cell), "home_dryer_rung");
        init_int(s_flags.show_icon, 0, "home_dryer_show_icon");
        init_int(s_flags.show_drop, 1, "home_dryer_show_drop");
        init_int(s_flags.drop_rung, 0, "home_dryer_drop_rung");
        init_int(s_flags.rh_x10, -1, "home_dryer_rh_x10");
        init_int(s_flags.show_ring, 0, "home_dryer_show_ring");
        init_int(s_flags.ring_pct, 0, "home_dryer_ring_pct");
        init_int(s_flags.show_info, 1, "home_dryer_show_info");
        init_int(s_flags.show_time, 0, "home_dryer_show_time");
        init_int(s_flags.show_name, 0, "home_dryer_show_name");
        init_int(s_flags.show_temp, 0, "home_dryer_show_temp");
        init_int(s_flags.show_rh_detail, 0, "home_dryer_show_rh_detail");
        init_int(s_flags.show_rh, 0, "home_dryer_show_rh");
        init_int(s_flags.show_status, 0, "home_dryer_show_status");
        init_int(s_flags.show_end, 0, "home_dryer_show_end");
        init_int(s_flags.show_count, 0, "home_dryer_show_count");
        init_int(s_flags.show_times, 0, "home_dryer_show_times");
        init_int(s_flags.row_count, 0, "home_dryer_row_count");
        init_text(s_texts.time, "home_dryer_time_text");
        init_text(s_texts.name, "home_dryer_name_text");
        init_text(s_texts.temp, "home_dryer_temp_text");
        init_text(s_texts.rh_detail, "home_dryer_rh_detail_text");
        init_text(s_texts.rh, "home_dryer_rh_text");
        init_text(s_texts.status, "home_dryer_status_text");
        init_text(s_texts.end, "home_dryer_end_text");
        init_text(s_texts.count, "home_dryer_count_text");
        init_text(s_texts.times, "home_dryer_times_text");
        for (int i = 0; i < kMaxRows; ++i) {
            // The names must outlive the registration, so they live in a static table.
            static HELIX_PSRAM_BSS char names[kMaxRows][4][32];
            snprintf(names[i][0], sizeof(names[i][0]), "home_dryer_row_%d_name", i);
            snprintf(names[i][1], sizeof(names[i][1]), "home_dryer_row_%d_time", i);
            snprintf(names[i][2], sizeof(names[i][2]), "home_dryer_row_%d_detail", i);
            snprintf(names[i][3], sizeof(names[i][3]), "home_dryer_row_%d_pct", i);
            init_text(s_rows[i].name, names[i][0]);
            init_text(s_rows[i].time, names[i][1]);
            init_text(s_rows[i].detail, names[i][2]);
            init_int(s_rows[i].pct, -1, names[i][3]);
        }
    });
}

// "2:30" while a cycle has hours left, "45m" under one, "9h" at ten or more, so the
// string always fits inside a ring half a cell wide.
std::string format_time_left(int minutes) {
    if (minutes < 0)
        return "";
    if (minutes >= 600)
        return fmt::format("{}h", (minutes + 30) / 60);
    if (minutes >= 60)
        return fmt::format("{}:{:02d}", minutes / 60, minutes % 60);
    return fmt::format("{}m", minutes);
}

std::string source_name(const helix::DryerSource& s) {
    switch (s.kind) {
    case helix::DryerSourceKind::Chamber:
        return lv_tr("Chamber");
    case helix::DryerSourceKind::Bed:
        return lv_tr("Bed");
    case helix::DryerSourceKind::Box:
        break;
    }
    if (!s.name.empty())
        return s.name;
    return s.unit_index >= 0
               ? fmt::format("{} {}", lv_tr("Box"), helix::ui::lane_number(s.unit_index))
               : std::string(lv_tr("Box"));
}

// "47 / 55°C", or the one temperature known. `current_only` drops the target for a rung too
// narrow to carry both.
std::string temp_text(const helix::DryerSource& s, bool current_only) {
    char buf[32];
    if (s.current_c >= 0 && s.target_c > 0 && !current_only)
        return helix::ui::temperature::format_temperature_pair(s.current_c, s.target_c, buf,
                                                               sizeof(buf));
    if (s.current_c >= 0)
        return fmt::format("{}°C", s.current_c);
    if (s.target_c > 0)
        return fmt::format("{}°C", s.target_c);
    return "";
}

std::string rh_text(int pct, bool with_unit) {
    if (pct < 0)
        return "";
    return with_unit ? fmt::format("{}% {}", pct, lv_tr("RH")) : fmt::format("{}%", pct);
}

struct TextSet {
    std::string time, name, temp, rh_detail, rh, status, end, count, times;
};

/// Everything the single-block rungs (Glance to Wide) show, composed once for the widget
/// to render. A text the rung does not draw stays empty and is never composed.
TextSet compose_single(const helix::DryerSummary& summary, helix::SizeRung rung, bool use_24h) {
    using helix::SizeRung;
    TextSet t;
    const auto* primary = helix::primary_source(summary);
    if (!primary) {
        const bool long_form =
            rung == SizeRung::Strip || rung == SizeRung::Cell || rung == SizeRung::Wide;
        t.rh = rh_text(summary.highest_rh_pct, long_form);
        if (rung == SizeRung::Cell || rung == SizeRung::Wide)
            t.status = lv_tr("Not drying");
        else if (rung == SizeRung::Column && !t.rh.empty())
            t.status = lv_tr("RH"); // the caption the value has no room for inline
        return t;
    }
    if (rung == SizeRung::Wide && summary.drying_count >= 2) {
        t.count = fmt::format("{} {}", summary.drying_count, lv_tr("drying"));
        int listed = 0;
        for (const auto& s : summary.sources) {
            if (!s.drying || listed == kMaxTimes)
                continue;
            if (!t.times.empty())
                t.times += "  ";
            t.times += s.remaining_min >= 0 ? format_time_left(s.remaining_min) : "--";
            ++listed;
        }
        return t;
    }
    t.time = format_time_left(primary->remaining_min);
    switch (rung) {
    case SizeRung::Strip:
    case SizeRung::Cell:
        t.temp = temp_text(*primary, false);
        break;
    case SizeRung::Column:
        t.temp = temp_text(*primary, true);
        break;
    case SizeRung::Wide:
        t.name = source_name(*primary);
        t.temp = temp_text(*primary, false);
        t.rh_detail = rh_text(primary->humidity_pct, true);
        if (primary->remaining_min > 0) {
            t.end = helix::format::eta_clock_time(primary->remaining_min * 60, 0, use_24h);
        }
        break;
    default:
        break;
    }
    return t;
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* heading;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"), theme_manager_get_font("font_heading"),
            theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

int widest(std::initializer_list<int> widths) {
    return widths.size() ? *std::max_element(widths.begin(), widths.end()) : 0;
}

enum class DState { Idle, Single, Multi };

struct RowMetrics {
    int row_h = 0;
    int row_gap = 0;
};

RowMetrics measure_rows(const Fonts& f) {
    const int line_gap = theme_manager_get_spacing("space_xxs");
    const int bar_h = theme_manager_get_spacing("space_xs");
    return {line_px(f.small) + line_px(f.xs) + bar_h + 2 * line_gap,
            theme_manager_get_spacing("space_xs")};
}

/// The box the humidity drop takes at each icon-ladder rung, in the faces the Humidity
/// tile draws it in.
struct DropGlyphs {
    int w[helix::kTileRungs]{};
    int h[helix::kTileRungs]{};
};

DropGlyphs measure_drop_glyphs() {
    DropGlyphs g;
    for (int r = 0; r < helix::kTileRungs; ++r) {
        const helix::ui::TileGlyphBox box =
            helix::ui::tile_glyph_box(helix::ui::tile_rung_face(helix::ui::TileLadder::Icon, r));
        g.w[r] = box.w;
        g.h[r] = box.h;
    }
    return g;
}

/// The widest idle text a single-block rung draws beside or under the drop, in that rung's
/// faces. Mirrors panel_widget_dryer.xml: a stack spaces its children by space_xxs, a row
/// by space_xs.
helix::DropTextBlock idle_text_block(helix::SizeRung rung, const Fonts& f) {
    using helix::SizeRung;
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    const std::string rh_short = "100%";
    const std::string rh_long = fmt::format("100% {}", lv_tr("RH"));
    const std::string not_drying = lv_tr("Not drying");
    const int xs_h = line_px(f.xs);

    switch (rung) {
    case SizeRung::Strip:
        return {measure_text_px(rh_long, f.xs), xs_h, true, col_gap, tight};
    case SizeRung::Column:
        return {widest({measure_text_px(rh_short, f.xs), measure_text_px(lv_tr("RH"), f.xs)}),
                2 * xs_h + row_gap, false, row_gap, tight};
    case SizeRung::Cell:
        return {widest({measure_text_px(rh_long, f.body), measure_text_px(not_drying, f.small)}),
                line_px(f.body) + row_gap + line_px(f.small), false, row_gap, comfort};
    case SizeRung::Wide:
        return {widest({measure_text_px(rh_long, f.heading), measure_text_px(not_drying, f.body)}),
                line_px(f.heading) + row_gap + line_px(f.body), true, col_gap, comfort};
    default:
        return {measure_text_px(rh_short, f.xs), xs_h, false, row_gap, tight};
    }
}

/// The minimum content box each rung draws in for the state being decided. Mirrors
/// panel_widget_dryer.xml: a stack spaces its children by space_xxs, a row by space_xs.
helix::RungNeeds measure_needs(DState state) {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    const int ring_sm = theme_manager_get_spacing("button_height_sm");
    const int ring_md = theme_manager_get_spacing("button_height");

    const std::string icon = helix::ui::icon::lookup_codepoint("heat_wave");
    const int icon_w = measure_text_px(icon, f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    const int body_h = line_px(f.body);
    const int heading_h = line_px(f.heading);

    // The widest each string can draw.
    const std::string rh_long = fmt::format("100% {}", lv_tr("RH"));
    const std::string time_w = "9:59";
    const std::string temps = "88/88°C";
    const std::string current = "88°C";
    const std::string name_w = lv_tr("Chamber");
    const std::string end_w = "(~12:00 PM)";
    const std::string count_w = fmt::format("8 {}", lv_tr("drying"));
    const std::string times_w = "9:59  9:59  9:59  9:59";

    helix::RungNeeds needs;
    switch (state) {
    case DState::Idle: {
        // At the smallest drop; decide() grows it into whatever room the rung leaves.
        const DropGlyphs drop = measure_drop_glyphs();
        const auto idle = [&](helix::SizeRung rung) {
            return helix::drop_block_box(drop.w[0], drop.h[0], idle_text_block(rung, f));
        };
        needs.glance = idle(helix::SizeRung::Glance);
        needs.strip = idle(helix::SizeRung::Strip);
        needs.column = idle(helix::SizeRung::Column);
        needs.cell = idle(helix::SizeRung::Cell);
        needs.wide = idle(helix::SizeRung::Wide);
        break;
    }
    case DState::Single:
    case DState::Multi: {
        needs.glance = rung_box(ring_sm + tight, ring_sm + 1);
        needs.strip =
            rung_box(measure_text_px(time_w, f.xs) + col_gap + measure_text_px(temps, f.xs) + tight,
                     xs_h + 1);
        // The icon on top keeps a half cell square a Glance ring and a tall one a column.
        needs.column = rung_box(
            widest({icon_w, measure_text_px(time_w, f.xs), measure_text_px(current, f.xs)}) + tight,
            icon_h + 2 * row_gap + 2 * xs_h + 1);
        needs.cell = rung_box(std::max(ring_md, measure_text_px(temps, f.small)) + comfort,
                              ring_md + row_gap + small_h + 1);
        if (state == DState::Single) {
            const int text_w =
                widest({measure_text_px(name_w, f.body), measure_text_px(temps, f.small),
                        measure_text_px(rh_long, f.small), measure_text_px(end_w, f.small)});
            needs.wide = rung_box(ring_md + col_gap + text_w + comfort,
                                  std::max(ring_md, body_h + 3 * small_h + 3 * row_gap) + 1);
        } else {
            needs.wide = rung_box(
                std::max(measure_text_px(count_w, f.heading), measure_text_px(times_w, f.small)) +
                    comfort,
                heading_h + small_h + row_gap + 1);
        }
        break;
    }
    }
    // Full lists at least three sources' worth of rows, so a 2x1 tile stays Wide.
    const RowMetrics rows = measure_rows(f);
    const std::string detail_w = fmt::format("88/88°C  100% {}", lv_tr("RH"));
    const int row_w =
        std::max(measure_text_px(name_w, f.small) + col_gap + measure_text_px("9:59", f.small),
                 measure_text_px(detail_w, f.xs));
    needs.full = rung_box(row_w + comfort, 3 * rows.row_h + 2 * rows.row_gap + 1);
    return needs;
}

void open_source(const helix::DryerTap& tap) {
    if (s_open_hook) {
        s_open_hook(tap);
        return;
    }
    switch (tap.target) {
    case helix::DryerTapTarget::Environment:
        helix::ui::open_environment_for_unit(tap.unit_index);
        break;
    case helix::DryerTapTarget::ChamberModal:
        helix::ui::ChamberDryerModal::show_owned();
        break;
    case helix::DryerTapTarget::BedModal:
        helix::ui::BedDryingModal::show_owned();
        break;
    case helix::DryerTapTarget::None:
        break;
    }
}

} // namespace

namespace helix {

struct DryerWidget::SummaryState {
    bool drying = false;
    bool multi = false;
    bool progress_known = false;
    int sources = 0;
};

DryerWidget::~DryerWidget() {
    detach();
}

DryerVerdict DryerWidget::decide(int width_px, int height_px, const SummaryState& state) const {
    // The tree's own padding where it exists; before it does (the load path asks fits_at
    // then) the root's padding on each side plus the body's gap.
    const ContentChrome chrome =
        content_chrome_or_estimate(root(), "space_xxs", "space_xxs", "space_xxs", "space_xxs");
    const DState dstate =
        !state.drying ? DState::Idle : (state.multi ? DState::Multi : DState::Single);
    const Fonts f = load_fonts();
    const RowMetrics rows = measure_rows(f);

    DryerInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.drying = state.drying;
    in.multi = state.multi;
    in.progress_known = state.progress_known;
    in.source_count = state.sources;
    in.needs = measure_needs(dstate);
    in.row_h = rows.row_h;
    in.row_gap = rows.row_gap;
    in.max_rows = kMaxRows;
    const auto& n = in.needs;
    spdlog::trace("[DryerWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} drying={} multi={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, state.drying, state.multi);
    DryerVerdict verdict = decide_dryer_layout(in);
    if (dstate == DState::Idle && verdict.rung != SizeRung::Full) {
        const DropGlyphs drop = measure_drop_glyphs();
        verdict.drop_rung = choose_drop_rung(in.content_w, in.content_h, drop.w, drop.h,
                                             idle_text_block(verdict.rung, f));
    }
    return verdict;
}

DryerWidget::SummaryState DryerWidget::read_state() {
    const DryerSummary& summary = dryer_model::summary();
    SummaryState state;
    state.drying = summary.drying_count > 0;
    state.multi = summary.drying_count >= 2;
    const DryerSource* primary = primary_source(summary);
    state.progress_known = primary && primary->progress_pct >= 0;
    state.sources = static_cast<int>(summary.sources.size());
    return state;
}

void DryerWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    install_delete_hook(widget_obj);
    s_tile.claim(this);
    // Sources that came up since the model last looked.
    dryer_model::refresh();

    const auto token = lifetime_.token();
    version_observer_ = ui::observe<int>(
        dryer_model::version_subject(), this,
        [token](DryerWidget* self, int) {
            if (token.expired())
                return;
            self->on_summary_changed();
        },
        dryer_model::lifetime());
    // The labels hold lv_tr() text from when it was composed, and the widths the rung was
    // chosen on were measured in that language.
    language_observer_ = ui::observe_language_change(this, [token](DryerWidget* self) {
        if (!token.expired())
            self->relayout_for_granted_size();
    });

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish_texts();
}

void DryerWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    language_observer_.reset();
    if (DryerWidget* displaced = s_tile.release(this)) {
        // The tile beneath a transient one is still on screen and publishes again.
        displaced->publish_verdict();
        displaced->publish_texts();
    }
}

void DryerWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px, int height_px) {
    const SummaryState state = read_state();
    verdict_ = decide(width_px, height_px, state);
    decided_drying_ = state.drying;
    decided_multi_ = state.multi;
    decided_progress_known_ = state.progress_known;
    decided_sources_ = state.sources;
    if (!s_tile.is_owner(this))
        return;
    publish_verdict();
    publish_texts();

    spdlog::debug("[DryerWidget] on_size_changed {}x{} rung={} ring={} rows={} fits={} drying={} "
                  "multi={} sources={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_ring,
                  verdict_.row_count, verdict_.fits, state.drying, state.multi, state.sources);
}

bool DryerWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content in either state.
    SummaryState idle;
    SummaryState drying;
    drying.drying = true;
    drying.progress_known = true;
    return decide(width_px, height_px, idle).fits && decide(width_px, height_px, drying).fits;
}

void DryerWidget::publish_verdict() {
    helix::ui::set_int_if_changed(&s_flags.rung, static_cast<int>(verdict_.rung));
    helix::ui::set_int_if_changed(&s_flags.show_ring, verdict_.show_ring ? 1 : 0);
    helix::ui::set_int_if_changed(&s_flags.row_count, verdict_.row_count);
    helix::ui::set_int_if_changed(&s_flags.drop_rung, verdict_.drop_rung);
}

void DryerWidget::publish_texts() {
    if (!s_tile.is_owner(this))
        return;
    const DryerSummary& summary = dryer_model::summary();
    const SizeRung rung = verdict_.rung;
    const bool use_24h =
        DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24;
    const bool drying = summary.drying_count > 0;

    // Full draws rows and nothing else; the other rungs draw one block and no rows.
    TextSet t;
    if (rung != SizeRung::Full)
        t = compose_single(summary, rung, use_24h);
    helix::ui::copy_string_if_changed(&s_texts.time.subject, t.time.c_str());
    helix::ui::copy_string_if_changed(&s_texts.name.subject, t.name.c_str());
    helix::ui::copy_string_if_changed(&s_texts.temp.subject, t.temp.c_str());
    helix::ui::copy_string_if_changed(&s_texts.rh_detail.subject, t.rh_detail.c_str());
    helix::ui::copy_string_if_changed(&s_texts.rh.subject, t.rh.c_str());
    helix::ui::copy_string_if_changed(&s_texts.status.subject, t.status.c_str());
    helix::ui::copy_string_if_changed(&s_texts.end.subject, t.end.c_str());
    helix::ui::copy_string_if_changed(&s_texts.count.subject, t.count.c_str());
    helix::ui::copy_string_if_changed(&s_texts.times.subject, t.times.c_str());

    const bool single = rung != SizeRung::Full;
    const auto flag = [](const std::string& s) { return s.empty() ? 0 : 1; };
    const bool ring = verdict_.show_ring;
    helix::ui::set_int_if_changed(&s_flags.show_drop, single && !drying ? 1 : 0);
    helix::ui::set_int_if_changed(&s_flags.show_icon,
                                  single && drying && rung == SizeRung::Column ? 1 : 0);
    helix::ui::set_int_if_changed(&s_flags.rh_x10,
                                  summary.highest_rh_pct >= 0 ? summary.highest_rh_pct * 10 : -1);
    helix::ui::set_int_if_changed(&s_flags.show_time, flag(t.time) && !ring ? 1 : 0);
    helix::ui::set_int_if_changed(&s_flags.show_name, flag(t.name));
    helix::ui::set_int_if_changed(&s_flags.show_temp, flag(t.temp));
    helix::ui::set_int_if_changed(&s_flags.show_rh_detail, flag(t.rh_detail));
    helix::ui::set_int_if_changed(&s_flags.show_rh, flag(t.rh));
    helix::ui::set_int_if_changed(&s_flags.show_status, flag(t.status));
    helix::ui::set_int_if_changed(&s_flags.show_end, flag(t.end));
    helix::ui::set_int_if_changed(&s_flags.show_count, flag(t.count));
    helix::ui::set_int_if_changed(&s_flags.show_times, flag(t.times));
    helix::ui::set_int_if_changed(&s_flags.show_info,
                                  (flag(t.time) && !ring) || flag(t.name) || flag(t.temp) ||
                                          flag(t.rh_detail) || flag(t.rh) || flag(t.status) ||
                                          flag(t.end) || flag(t.count) || flag(t.times)
                                      ? 1
                                      : 0);

    const DryerSource* primary = primary_source(summary);
    helix::ui::set_int_if_changed(&s_flags.ring_pct,
                                  ring && primary ? std::clamp(primary->progress_pct, 0, 100) : 0);

    // Rows only exist at Full; below it nothing is composed for them, and what Full left
    // behind is cleared once.
    if (rung != SizeRung::Full) {
        if (s_rows_composed) {
            for (auto& row : s_rows) {
                helix::ui::copy_string_if_changed(&row.name.subject, "");
                helix::ui::copy_string_if_changed(&row.time.subject, "");
                helix::ui::copy_string_if_changed(&row.detail.subject, "");
                helix::ui::set_int_if_changed(&row.pct, -1);
            }
            s_rows_composed = false;
        }
        return;
    }
    s_rows_composed = true;
    for (int i = 0; i < kMaxRows; ++i) {
        RowSubjects& row = s_rows[i];
        const bool shown = i < verdict_.row_count && i < static_cast<int>(summary.sources.size());
        if (!shown) {
            helix::ui::set_int_if_changed(&row.pct, -1);
            continue;
        }
        const DryerSource& s = summary.sources[static_cast<size_t>(i)];
        helix::ui::copy_string_if_changed(&row.name.subject, std::string(source_name(s)).c_str());
        helix::ui::copy_string_if_changed(
            &row.time.subject,
            std::string(s.drying ? format_time_left(s.remaining_min) : std::string(lv_tr("Idle")))
                .c_str());
        std::string detail = temp_text(s, !s.drying);
        const std::string rh = rh_text(s.humidity_pct, true);
        if (!rh.empty())
            detail += detail.empty() ? rh : "  " + rh;
        helix::ui::copy_string_if_changed(&row.detail.subject, detail.c_str());
        helix::ui::set_int_if_changed(&row.pct, s.drying ? s.progress_pct : -1);
    }
}

void DryerWidget::on_summary_changed() {
    const SummaryState state = read_state();
    if (state.drying != decided_drying_ || state.multi != decided_multi_ ||
        state.progress_known != decided_progress_known_ || state.sources != decided_sources_) {
        relayout_for_granted_size();
    }
    publish_texts();
}

void DryerWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DryerWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<DryerWidget>(e)) {
        self->record_interaction();
        open_source(tap_for_tile(dryer_model::summary()));
    }
    LVGL_SAFE_EVENT_CB_END();
}

void set_dryer_open_hook_for_test(std::function<void(const DryerTap&)> hook) {
    s_open_hook = std::move(hook);
}

void register_dryer_widget() {
    register_widget_factory("dryer",
                            [](const std::string&) { return std::make_unique<DryerWidget>(); });
    register_widget_subjects("dryer", dryer_init_subjects);

    lv_xml_register_event_cb(nullptr, "dryer_clicked_cb", DryerWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "dryer_row_clicked_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[DryerWidget] row_clicked_cb");
        if (auto index = ui::event_user_int(e)) {
            const auto& sources = dryer_model::summary().sources;
            if (*index >= 0 && *index < static_cast<int>(sources.size()))
                open_source(tap_for_source(sources[static_cast<size_t>(*index)]));
        }
        LVGL_SAFE_EVENT_CB_END();
    });
}

} // namespace helix
