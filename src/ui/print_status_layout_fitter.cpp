// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_layout_fitter.h"

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
        if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_has_flag(row, LV_OBJ_FLAG_IGNORE_LAYOUT)) {
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
    lv_obj_t* progress = lv_obj_find_by_name(root, "ps_strip_progress");
    last_ =
        decide_print_status_tile_layout(w, h, theme_manager_get_spacing("space_md"),
                                        strip_row_height(progress), strip_stacked_height(progress));
    const auto set = [](lv_subject_t* s, int v) {
        if (s && lv_subject_get_int(s) != v) {
            lv_subject_set_int(s, v);
        }
    };
    set(subjects_.arrangement, static_cast<int>(last_.arrangement));
    set(subjects_.strip_stacked, last_.strip_stacked ? 1 : 0);
    set(subjects_.checklist_compact, last_.checklist_compact ? 1 : 0);
    spdlog::debug("[{}] Content {}x{}: {} strip {} ({}px), job {}x{}, printer {}x{}, checklist {}",
                  log_tag_, w, h,
                  last_.arrangement == PrintStatusArrangement::Stacked ? "stacked" : "side by side",
                  last_.strip_stacked ? "stacked" : "row", last_.strip_h, last_.job_w, last_.job_h,
                  last_.printer_w, last_.printer_h, last_.checklist_compact ? "compact" : "list");
    return last_;
}

} // namespace helix::ui
