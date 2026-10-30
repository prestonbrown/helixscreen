// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "exclude_map_widget.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_exclude_object_badges.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_panel_print_status.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "bed_dimensions.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_excluded_objects_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <string>

using helix::ui::measure_text_px;

namespace {

// The verdict and texts the widget publishes, read by panel_widget_exclude_map.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_placeholder_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_map_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_skip_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_name_subject{};
HELIX_PSRAM_BSS lv_subject_t s_has_current_subject{};
HELIX_PSRAM_BSS lv_subject_t s_count_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_objects_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_skipped_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_printing_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_current_name_subject{};
HELIX_PSRAM_BSS lv_subject_t s_placeholder_text_subject{};
HELIX_PSRAM_BSS char s_count_text_buf[16];
HELIX_PSRAM_BSS char s_objects_text_buf[32];
HELIX_PSRAM_BSS char s_skipped_text_buf[32];
HELIX_PSRAM_BSS char s_printing_text_buf[48];
HELIX_PSRAM_BSS char s_current_name_buf[96];
HELIX_PSRAM_BSS char s_placeholder_text_buf[32];
helix::WidgetSubjects s_subjects{"ExcludeMapWidgetSubjects"};
// The instance bound to the live tile. Only it publishes and acts: a transient instance
// (a fits_at probe, a rebuild's outgoing widget) must not overwrite the tile's verdict.
helix::LiveTileOwner<helix::ExcludeMapWidget> s_tile;

void exclude_map_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "exclude_map_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_placeholder_subject, 1, "exclude_map_placeholder", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_map_subject, 0, "exclude_map_show_map", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_skip_subject, 0, "exclude_map_show_skip", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_name_subject, 0, "exclude_map_show_name", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_has_current_subject, 0, "exclude_map_has_current", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_count_text_subject, s_count_text_buf, "",
                                  "exclude_map_count_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_objects_text_subject, s_objects_text_buf, "",
                                  "exclude_map_objects_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_skipped_text_subject, s_skipped_text_buf, "",
                                  "exclude_map_skipped_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_printing_text_subject, s_printing_text_buf, "",
                                  "exclude_map_printing_text", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_current_name_subject, s_current_name_buf, "",
                                  "exclude_map_current_name", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_placeholder_text_subject, s_placeholder_text_buf, "",
                                  "exclude_map_placeholder_text", s_subjects);
    });
}

// The texts, composed once for the labels and the measurement alike.
std::string count_text(const helix::ExcludeCounts& c) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d/%d", c.remaining(), c.total);
    return buf;
}
std::string objects_text(int total) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), lv_tr("%d objects"), total);
    return buf;
}
std::string skipped_text(int skipped) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), lv_tr("%d skipped"), skipped);
    return buf;
}
std::string printing_text(const helix::ExcludeCounts& c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), lv_tr("%d of %d printing"), c.remaining(), c.total);
    return buf;
}
const char* placeholder_text(helix::ExcludeMapState state) {
    return state == helix::ExcludeMapState::SingleObject ? lv_tr("Single object")
                                                         : lv_tr("No objects");
}

/// Digits in the largest object count a tile is accepted for.
constexpr int kWidestCountDigits = 3;

/// The widest counts a print with `digits` digits in its object count can show.
helix::ExcludeCounts widest_counts(int digits) {
    int nines = 0;
    for (int i = 0; i < std::max(1, digits); ++i)
        nines = nines * 10 + 9;
    return {nines, nines};
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

/// The minimum content box each rung draws in. Mirrors panel_widget_exclude_map.xml:
/// spacing is the XML's pad_row (space_xxs) and pad_column (space_xs), and the text
/// beside the Wide rung's map takes half the width.
helix::RungNeeds measure_needs(helix::ExcludeMapState state, int digits) {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);

    const int icon_w =
        measure_text_px(helix::ui::icon::lookup_codepoint("debug_step_over"), f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    // The smallest map worth drawing: a button's height square.
    const int map_min = theme_manager_get_spacing("button_height_sm");

    helix::RungNeeds needs;
    if (state != helix::ExcludeMapState::Objects) {
        const int caption_w = std::max(measure_text_px(lv_tr("No objects"), f.xs),
                                       measure_text_px(lv_tr("Single object"), f.xs));
        needs.glance = rung_box(icon_w + tight, icon_h + 1);
        needs.strip = rung_box(std::max(icon_w, caption_w) + tight, icon_h + row_gap + xs_h + 1);
        return needs;
    }

    const helix::ExcludeCounts widest = widest_counts(digits);
    const int count_w = measure_text_px(count_text(widest), f.xs);
    const int objects_w = measure_text_px(objects_text(widest.total), f.xs);
    const int skipped_w = measure_text_px(skipped_text(widest.skipped), f.xs);
    const int printing_w = measure_text_px(printing_text(widest), f.xs);
    const int now_w = measure_text_px(lv_tr("Now printing"), f.xs);

    needs.glance = rung_box(std::max(icon_w, count_w) + tight, icon_h + row_gap + xs_h + 1);
    needs.strip = rung_box(std::max(objects_w, skipped_w) + tight, 2 * xs_h + row_gap + 1);
    needs.column = rung_box(std::max(map_min, count_w) + tight, map_min + row_gap + xs_h + 1);
    needs.cell = rung_box(std::max(map_min, printing_w) + comfort, map_min + row_gap + xs_h + 1);
    {
        const int text_w = std::max({now_w, printing_w, skipped_w});
        // Caption, name, counts and the skipped line beside the map.
        const int text_h = 3 * xs_h + small_h + 3 * row_gap;
        needs.wide =
            rung_box(map_min + col_gap + 2 * text_w + comfort, std::max(map_min, text_h) + 1);
    }
    {
        const int button_w = measure_text_px(lv_tr("Skip Objects..."), f.small) + icon_w +
                             2 * theme_manager_get_spacing("space_md");
        const int text_w = std::max({now_w, printing_w, button_w});
        // The map over caption, name, counts, the skipped line and the button.
        const int text_h =
            3 * xs_h + small_h + theme_manager_get_spacing("button_height_sm") + 4 * row_gap;
        needs.full = rung_box(text_w + comfort, 2 * map_min + row_gap + text_h + 1);
    }
    return needs;
}

} // namespace

namespace helix {

struct ExcludeMapWidget::Live {
    ExcludeMapState state = ExcludeMapState::NoObjects;
    ExcludeCounts counts;
    std::string current;
};

ExcludeMapWidget::ExcludeMapWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

ExcludeMapWidget::~ExcludeMapWidget() {
    detach();
}

ExcludeMapWidget::Live ExcludeMapWidget::read_live() const {
    const auto& objects = printer_state_.excluded_objects_state();
    Live live;
    live.counts = count_objects(objects.get_defined_objects(), objects.get_excluded_objects());
    const bool printing =
        lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
    live.state = classify_exclude_map(printing, live.counts.total);
    live.current = objects.get_current_object();
    return live;
}

ExcludeMapVerdict ExcludeMapWidget::decide(int width_px, int height_px, ExcludeMapState state,
                                           int digits) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side.
    const ContentChrome chrome = content_chrome_or_estimate(root(), "space_xxs", "space_xxs");
    ExcludeMapLayoutInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.state = state;
    in.needs = measure_needs(state, digits);
    in.full_min_h = widget_size::h_tall() - chrome.h;
    const auto& n = in.needs;
    spdlog::trace("[ExcludeMapWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} state={}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, static_cast<int>(state));
    return decide_exclude_map_layout(in);
}

void ExcludeMapWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    const auto token = lifetime_.token();
    auto handler = [token](ExcludeMapWidget* self, int) {
        if (token.expired())
            return;
        self->on_objects_changed();
    };
    auto& objects = printer_state_.excluded_objects_state();
    // The excluded version also moves when the object printing now changes.
    defined_observer_ = ui::observe<int>(objects.get_defined_objects_version_subject(), this,
                                         handler, objects.get_subjects_lifetime());
    excluded_observer_ = ui::observe<int>(objects.get_excluded_objects_version_subject(), this,
                                          handler, objects.get_subjects_lifetime());
    print_active_observer_ =
        ui::observe<int>(printer_state_.print_state().get_print_active_subject(), this, handler,
                         printer_state_.get_subjects_lifetime());

    // The texts hold lv_tr() output from when they were composed, and the widths the rung
    // was chosen on were measured in that language.
    language_observer_ = ui::observe_language_change(this, [token](ExcludeMapWidget* self) {
        if (!token.expired())
            self->relayout_for_granted_size();
    });

    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
}

void ExcludeMapWidget::detach() {
    lifetime_.invalidate();
    language_observer_.reset();
    defined_observer_.reset();
    excluded_observer_.reset();
    print_active_observer_.reset();
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
    active_ = false;
    if (ExcludeMapWidget* displaced = s_tile.release(this))
        displaced->relayout_for_granted_size();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ExcludeMapWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event: expire the observer tokens and drop the tree
    // pointers only. A handler already queued would otherwise publish by searching the
    // freed tree. The map view sees its own host die and keeps nothing of it.
    lifetime_.invalidate();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ExcludeMapWidget::on_activate() {
    active_ = true;
    if (!widget_obj_)
        return;
    if (has_granted_size())
        relayout_for_granted_size();
    else
        sync_map();
}

void ExcludeMapWidget::on_deactivate() {
    active_ = false;
    // A hidden tile draws nothing, so its map is not kept redrawing.
    if (map_view_) {
        map_view_->destroy();
        map_view_.reset();
    }
}

void ExcludeMapWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                       int height_px) {
    const Live live = read_live();
    const int digits = static_cast<int>(std::to_string(live.counts.total).size());
    verdict_ = decide(width_px, height_px, live.state, digits);
    decided_state_ = live.state;
    decided_digits_ = digits;
    if (!s_tile.is_owner(this))
        return;

    publish(live);
    sync_map();

    spdlog::debug("[ExcludeMapWidget] on_size_changed {}x{} rung={} placeholder={} map={} "
                  "skip={} fits={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.placeholder,
                  verdict_.show_map, verdict_.show_skip_button, verdict_.fits);
}

bool ExcludeMapWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content in either state, with the widest
    // object count the counts are budgeted for: a tile accepted for 5 objects must still
    // hold a print of a hundred.
    return decide(width_px, height_px, ExcludeMapState::Objects, kWidestCountDigits).fits &&
           decide(width_px, height_px, ExcludeMapState::NoObjects, 1).fits;
}

std::string ExcludeMapWidget::current_display_name(const std::string& current) const {
    if (current.empty()) {
        return {};
    }
    lv_obj_t* label =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "exclude_map_now_name") : nullptr;
    const lv_font_t* font = label ? lv_obj_get_style_text_font(label, LV_PART_MAIN)
                                  : theme_manager_get_font("font_small");
    return helix::ui::object_display_name(
        printer_state_.excluded_objects_state().get_defined_objects(), current, font);
}

void ExcludeMapWidget::publish(const Live& live) {
    helix::ui::set_int_if_changed(&s_placeholder_subject, verdict_.placeholder ? 1 : 0);
    helix::ui::set_int_if_changed(&s_show_map_subject, verdict_.show_map ? 1 : 0);
    helix::ui::set_int_if_changed(&s_show_skip_subject, verdict_.show_skip_button ? 1 : 0);
    helix::ui::set_int_if_changed(&s_show_name_subject, verdict_.show_name ? 1 : 0);
    helix::ui::set_int_if_changed(&s_has_current_subject, live.current.empty() ? 0 : 1);
    helix::ui::set_int_if_changed(&s_rung_subject, static_cast<int>(verdict_.rung));

    if (verdict_.placeholder) {
        helix::ui::copy_string_if_changed(&s_placeholder_text_subject,
                                          std::string(placeholder_text(live.state)).c_str());
        return;
    }
    // Only the texts the rung shows are composed.
    switch (verdict_.rung) {
    case SizeRung::Glance:
    case SizeRung::Column:
        helix::ui::copy_string_if_changed(&s_count_text_subject,
                                          std::string(count_text(live.counts)).c_str());
        break;
    case SizeRung::Strip:
        helix::ui::copy_string_if_changed(&s_objects_text_subject,
                                          std::string(objects_text(live.counts.total)).c_str());
        helix::ui::copy_string_if_changed(&s_skipped_text_subject,
                                          std::string(skipped_text(live.counts.skipped)).c_str());
        break;
    case SizeRung::Cell:
        helix::ui::copy_string_if_changed(&s_printing_text_subject,
                                          std::string(printing_text(live.counts)).c_str());
        break;
    case SizeRung::Wide:
    case SizeRung::Full:
    case SizeRung::Large:
        helix::ui::copy_string_if_changed(&s_printing_text_subject,
                                          std::string(printing_text(live.counts)).c_str());
        helix::ui::copy_string_if_changed(&s_skipped_text_subject,
                                          std::string(skipped_text(live.counts.skipped)).c_str());
        helix::ui::copy_string_if_changed(&s_current_name_subject,
                                          current_display_name(live.current).c_str());
        break;
    }
}

void ExcludeMapWidget::sync_map() {
    const bool wanted = s_tile.is_owner(this) && active_ && verdict_.show_map && widget_obj_;
    if (!wanted) {
        if (map_view_) {
            map_view_->destroy();
            map_view_.reset();
        }
        return;
    }
    lv_obj_t* host = lv_obj_find_by_name(widget_obj_, "exclude_map_host");
    if (!host)
        return;
    // The rung's flags have just moved the host; its size is what the map is built for.
    lv_obj_update_layout(widget_obj_);
    if (map_view_ && map_view_->is_active()) {
        map_view_->resize();
        return;
    }
    const auto bed = bed_dimensions(get_moonraker_api(), &printer_state_);
    map_view_ = std::make_unique<ui::ExcludeObjectMapView>();
    map_view_->create(host, &printer_state_.excluded_objects_state(), bed, {},
                      ui::ExcludeTapMode::ExcludeOnly, nullptr, ui::ExcludeMapMode::Mini);
}

void ExcludeMapWidget::on_objects_changed() {
    if (!s_tile.is_owner(this))
        return;
    const Live live = read_live();
    const int digits = static_cast<int>(std::to_string(live.counts.total).size());
    // The content being measured changed: a placeholder swaps for the map, and a count
    // gaining a digit widens its text.
    if ((live.state != decided_state_ || digits != decided_digits_) && has_granted_size()) {
        relayout_for_granted_size();
        return;
    }
    publish(live);
}

void ExcludeMapWidget::open_full_map() {
    if (verdict_.placeholder || !parent_screen_)
        return;
    if (!PrintStatusPanel::push_overlay_with_exclude_map(parent_screen_))
        spdlog::error("[ExcludeMapWidget] Failed to open the exclude map in print status");
}

void ExcludeMapWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ExcludeMapWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<ExcludeMapWidget>(e)) {
        self->record_interaction();
        self->open_full_map();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ExcludeMapWidget::skip_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ExcludeMapWidget] skip_cb");
    // The button opens the map itself; the tile's own tap must not open it a second time.
    lv_event_stop_bubbling(e);
    if (auto* owner = s_tile.owner()) {
        owner->record_interaction();
        owner->open_full_map();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void register_exclude_map_widget() {
    register_widget_factory("exclude_map", [](const std::string&) {
        return std::make_unique<ExcludeMapWidget>(get_printer_state());
    });
    register_widget_subjects("exclude_map", exclude_map_init_subjects);

    lv_xml_register_event_cb(nullptr, "exclude_map_clicked_cb", ExcludeMapWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "exclude_map_skip_cb", ExcludeMapWidget::skip_cb);
}

} // namespace helix
