// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_layout_fitter.h"

#include "ui_ams_mini_status.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

namespace {

/// The strip's progress block (components/print_status_strip.xml) before it
/// is laid out, within a few pixels: the percent line in font_body_bold, the
/// bar, the filament line and, with `up_next`, the up-next line, #space_xs
/// apart. Rows holding icons can stand taller than their text.
int32_t estimated_text_height(bool up_next) {
    const int32_t heading = lv_font_get_line_height(theme_manager_get_font("font_body_bold"));
    const int32_t small = lv_font_get_line_height(theme_manager_get_font("font_small"));
    const int32_t rows = up_next ? 4 : 3;
    const int32_t gap = theme_manager_get_spacing("space_xs");
    return heading + theme_manager_get_spacing("progress_bar_height") + (rows - 2) * small +
           (rows - 1) * gap;
}

/// The up-next line shows with jobs queued, except in a micro strip beside
/// its buttons (`row`).
bool strip_shows_up_next(bool row) {
    lv_subject_t* queued = lv_xml_get_subject(nullptr, "job_queue_count");
    if (!queued || lv_subject_get_int(queued) == 0)
        return false;
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    return !row || !bp || lv_subject_get_int(bp) > 0;
}

/// The laid-out progress block's rows and the gaps between them; 0 before
/// its first layout.
int32_t measured_text_height(lv_obj_t* progress) {
    if (!progress) {
        return 0;
    }
    int32_t h = 0;
    int32_t rows = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(progress); ++i) {
        lv_obj_t* row = lv_obj_get_child(progress, static_cast<int32_t>(i));
        if (lv_obj_is_hidden(row) || lv_obj_is_ignore_layout(row)) {
            continue;
        }
        h += lv_obj_get_height(row);
        ++rows;
    }
    if (rows == 0 || h == 0) {
        return 0;
    }
    return h + (rows - 1) * lv_obj_get_style_pad_row(progress, LV_PART_MAIN);
}

int32_t strip_text_height(lv_obj_t* progress, bool up_next) {
    const int32_t measured = measured_text_height(progress);
    return measured > 0 ? measured : estimated_text_height(up_next);
}

/// The ultrawide class, as the XML's `<if cond="ui_is_ultrawide eq 1">` sees it.
bool is_ultrawide() {
    lv_subject_t* uw = lv_xml_get_subject(nullptr, "ui_is_ultrawide");
    return uw && lv_subject_get_int(uw) == 1;
}

/// The job tile's pre-print checklist in full (components/print_status_job_checklist.xml):
/// the file line and a row per step, #space_xxs apart, inside the tile's #space_sm
/// padding; 0 outside pre-print. Rows not laid out yet are font_small lines.
int32_t checklist_height(lv_obj_t* root) {
    lv_subject_t* preparing = lv_xml_get_subject(nullptr, "preparing_visible");
    if (!preparing || lv_subject_get_int(preparing) == 0) {
        return 0;
    }
    lv_subject_t* steps = lv_xml_get_subject(nullptr, "ps_step_count");
    const int32_t rows = 1 + (steps ? lv_subject_get_int(steps) : 0);
    int32_t row_h = lv_font_get_line_height(theme_manager_get_font("font_small"));
    if (lv_obj_t* row = lv_obj_find_by_name(root, "ps_step_row_0")) {
        row_h = std::max(row_h, lv_obj_get_height(row));
    }
    return rows * row_h + (rows - 1) * theme_manager_get_spacing("space_xxs") +
           2 * theme_manager_get_spacing("space_sm");
}

/// The strip is a ui_card: its padding and border on both sides.
int32_t strip_chrome() {
    return 2 * (theme_manager_get_spacing("space_sm") + theme_manager_get_spacing("border_width"));
}

} // namespace

PrintStatusLayoutFitter::PrintStatusLayoutFitter(std::string log_tag, Subjects subjects, Host host)
    : log_tag_(std::move(log_tag)), subjects_(subjects), host_(std::move(host)) {}

int32_t PrintStatusLayoutFitter::strip_row_height(lv_obj_t* progress) {
    return std::max(theme_manager_get_spacing("ps_strip_button"),
                    strip_text_height(progress, strip_shows_up_next(true))) +
           strip_chrome();
}

int32_t PrintStatusLayoutFitter::strip_stacked_height(lv_obj_t* progress) {
    return strip_text_height(progress, strip_shows_up_next(false)) +
           theme_manager_get_spacing("space_md") + theme_manager_get_spacing("button_height") +
           strip_chrome();
}

const PrintStatusTileLayout& PrintStatusLayoutFitter::recompute() {
    lv_obj_t* root = host_.root ? host_.root() : nullptr;
    lv_obj_t* content = root ? lv_obj_find_by_name(root, "overlay_content") : nullptr;
    if (!content || !host_.subjects_ready()) {
        return last_;
    }
    const int32_t w = lv_obj_get_content_width(content);
    const int32_t h = lv_obj_get_content_height(content);
    if (w <= 0 || h <= 0) {
        return last_;
    }
    const int32_t gap = theme_manager_get_spacing("space_md");
    const bool lane_tray = has_lane_tray();
    if (is_ultrawide()) {
        last_ = decide_print_status_ultrawide_layout(w, h, gap, lane_tray);
    } else {
        lv_obj_t* progress = lv_obj_find_by_name(root, "ps_strip_progress");
        last_ = decide_print_status_tile_layout(w, h, gap, strip_row_height(progress),
                                                strip_stacked_height(progress), lane_tray,
                                                checklist_height(root));
    }
    const auto set = [](lv_subject_t* s, int v) {
        if (s && lv_subject_get_int(s) != v) {
            lv_subject_set_int(s, v);
        }
    };
    set(subjects_.arrangement, static_cast<int>(last_.arrangement));
    set(subjects_.strip_stacked, last_.strip_stacked ? 1 : 0);
    set(subjects_.checklist_compact, last_.checklist_compact ? 1 : 0);
    set(subjects_.preview_w, last_.columns.preview_w);
    set(subjects_.printer_w, last_.columns.printer_w);
    set(subjects_.readouts_w, last_.columns.readouts_w);
    set(subjects_.actions_w, last_.columns.actions_w);
    set(subjects_.lane_tray, static_cast<int>(last_.lane_tray));
    set(subjects_.lane_tray_h, last_.lane_tray_h);
    set(subjects_.printer_tile_h,
        last_.lane_tray == PrintStatusLaneTray::Strip ? last_.printer_h : lv_pct(100));
    // The tray picks its spool or bar view from the width it is given.
    if (lv_obj_t* tray = lv_obj_find_by_name(root, "ps_lane_tray_status");
        tray && ui_ams_mini_status_is_valid(tray)) {
        const int32_t tray_w =
            last_.lane_tray == PrintStatusLaneTray::Column ? last_.columns.readouts_w : w;
        ui_ams_mini_status_set_width(tray, tray_w - 2 * theme_manager_get_spacing("space_sm"));
    }
    spdlog::debug("[{}] Content {}x{}: {} strip {} ({}px), job {}x{}, printer {}x{}, checklist {}, "
                  "lane tray {} ({}px)",
                  log_tag_, w, h,
                  last_.arrangement == PrintStatusArrangement::Stacked ? "stacked" : "side by side",
                  last_.strip_stacked ? "stacked" : "row", last_.strip_h, last_.job_w, last_.job_h,
                  last_.printer_w, last_.printer_h, last_.checklist_compact ? "compact" : "list",
                  static_cast<int>(last_.lane_tray), last_.lane_tray_h);
    return last_;
}

} // namespace helix::ui
