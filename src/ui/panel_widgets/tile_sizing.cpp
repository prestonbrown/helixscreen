// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/tile_sizing.h"

#include "ui_tile_rung.h"

#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "panel_widget_size.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix {

namespace {

/// The rung `#icon_size` names at the current tier, which is the size every
/// other icon on screen draws at.
///
/// Resolved from the token rather than assumed: the tiers are free to re-point
/// it, and a tile that capped to a hardcoded rung would quietly disagree with
/// its neighbours the day one did.
int authored_rung() {
    const char* size = lv_xml_get_const_silent(nullptr, "icon_size");
    if (size) {
        for (int r = 0; r < kTileRungs; ++r) {
            // The icon token is "icon_font_<name>"; compare past the prefix.
            const char* token = ui::tile_rung_font_token(ui::TileLadder::Icon, r);
            if (std::strcmp(token + std::strlen("icon_font_"), size) == 0) {
                return r;
            }
        }
        spdlog::warn("[TileSizing] icon_size '{}' names no rung; capping at md", size);
    }
    return 2;
}

} // namespace

TileSizing::TileSizing(const std::string& instance_id)
    : icon_name_(instance_id + "_tile_icon"), label_name_(instance_id + "_tile_label"),
      dir_name_(instance_id + "_tile_dir"), target_name_(instance_id + "_tile_target"),
      drawn_name_(instance_id + "_tile_drawn") {
    // Registered here, in the constructor, because the manager parses this
    // tile's XML before attach() runs and the parser drops a binding whose
    // subject is missing at parse time.
    UI_MANAGED_SUBJECT_INT(icon_rung_subject_, 2, icon_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(label_subject_, 1, label_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(direction_subject_, 0, dir_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(show_target_subject_, 1, target_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(drawn_rung_subject_, 2, drawn_name_.c_str(), subjects_);

    attr_storage_ = {"tile_icon_subject", icon_name_, "tile_label_subject",  label_name_,
                     "tile_dir_subject",  dir_name_,  "tile_target_subject", target_name_};
    rebuild_attrs();
}

void TileSizing::add_subject_attr(const char* prop, const std::string& subject_name) {
    attr_storage_.emplace_back(prop);
    attr_storage_.push_back(subject_name);
    rebuild_attrs();
}

void TileSizing::rebuild_attrs() {
    attrs_.clear();
    attrs_.reserve(attr_storage_.size() + 1);
    for (const auto& s : attr_storage_) {
        attrs_.push_back(s.c_str());
    }
    attrs_.push_back(nullptr);
}

TileSizing::~TileSizing() {
    set_content_root(nullptr);
    // Withdraws each name from the XML scope before the subjects are freed, so
    // a later parse cannot bind to storage that is gone.
    subjects_.deinit_all();
}

void TileSizing::set_content_root(lv_obj_t* root) {
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    if (content_root_ && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(content_root_, on_content_root_deleted, this);
    }
    content_root_ = root;
    if (content_root_) {
        lv_obj_add_event_cb(content_root_, on_content_root_deleted, LV_EVENT_DELETE, this);
    }
}

void TileSizing::on_content_root_deleted(lv_event_t* e) {
    static_cast<TileSizing*>(lv_event_get_user_data(e))->content_root_ = nullptr;
}

TileVerdict TileSizing::decide(int width_px, int height_px) const {
    int badge_w = 0;
    int badge_h = 0;
    if (!content_.icon_badge.empty()) {
        const lv_font_t* badge_face = theme_manager_get_font("font_body_bold");
        badge_w = ui::text_width(content_.icon_badge.c_str(), badge_face) +
                  theme_manager_get_spacing("space_xxs");
        badge_h = line_px(badge_face);
    }

    // The component draws the caption translated, so that is what is measured.
    // A label with no translation (a device's own name) comes back unchanged.
    const char* label = content_.label.empty() ? "" : lv_tr(content_.label.c_str());

    const int detail_h =
        content_.detail_font ? line_px(theme_manager_get_font(content_.detail_font)) : 0;

    TileRungMetrics rungs[kTileRungs];
    for (int r = 0; r < kTileRungs; ++r) {
        const ui::TileFace icon_face = ui::tile_rung_face(
            ui::TileLadder::Icon, r,
            content_.icon_animates ? ui::kTileAnimatedMaxScale : ui::kTileMaxScale);
        const lv_font_t* value_face = ui::tile_rung_face(ui::TileLadder::Value, r).font;
        const lv_font_t* label_face = ui::tile_rung_face(ui::TileLadder::Label, r).font;

        // The glyph occupies its face's box at the scale it is drawn at.
        const ui::TileGlyphBox glyph = ui::tile_glyph_box(icon_face);
        int glyph_w = glyph.w;
        int glyph_h = glyph.h;
        if (content_.icon_box == IconBox::Square) {
            glyph_w = glyph_h;
        } else if (content_.icon_box == IconBox::Disc) {
            glyph_w = glyph_h = ui::tile_disc_edge(icon_face);
        }
        rungs[r].icon_w = glyph_w + badge_w;
        rungs[r].icon_h = std::max(glyph_h, badge_h);
        rungs[r].value_full_w = ui::text_width(content_.widest_value.c_str(), value_face);
        rungs[r].value_current_w = ui::text_width(content_.widest_current.c_str(), value_face);
        rungs[r].value_h = line_px(value_face);
        rungs[r].label_w = ui::text_width(label, label_face);
        rungs[r].label_h = line_px(label_face);
        rungs[r].detail_h = detail_h;
    }

    const int gap = theme_manager_get_spacing("space_xs");

    // The chrome between the tile's outer box and the box its content draws in
    // (measure_content_chrome): padding alone. decide_tile_layout prices a gap
    // of `gap` between every pair of parts each candidate draws, which covers
    // the flex gap the tile draws between them, so the verdict reads nothing the
    // last verdict hid or showed. Before the tree exists (the load path asks
    // fits_at then) a gap on each side is the conservative stand-in.
    // A gap on each side on top of whatever is measured. A measured-exact fit
    // renders as an overlap by a pixel or two, and the walk cannot see a
    // container that is not on the single-child chain, so the reservation is
    // deliberately generous. It errs toward a smaller rung, never toward one
    // that overflows.
    // styles.tile_row pads each side by #space_sm.
    const int row_inset = 2 * theme_manager_get_spacing("space_sm");
    int chrome_w = 2 * gap;
    int chrome_h = 2 * gap;
    if (content_root_) {
        const ContentChrome chrome =
            measure_content_chrome(content_root_, gap, row_inset, BranchGap::None);
        chrome_w = chrome.w;
        chrome_h = chrome.h;
    }

    const int avail_w = std::max(width_px - chrome_w, 1);
    const int avail_h = std::max(height_px - chrome_h, 1);

    TileVerdict verdict =
        decide_tile_layout(avail_w, avail_h, gap, rungs, content_.has_value, label_drawn(),
                           authored_rung(), row_inset, content_.label_is_identity);

    // At micro and tiny a whole cell is barely wider than the glyph itself, so
    // a tile of one cell or less keeps the authored rung those screens were
    // designed with. A tile given more than a cell on both axes has room the
    // authored rung would leave empty, and grows like it does on every tier.
    if (widget_size::current_breakpoint() <= UiBreakpoint::Tiny &&
        std::min(width_px, height_px) <= whole_cell_px()) {
        verdict.icon_rung = std::min(verdict.icon_rung, authored_rung());
    }
    return verdict;
}

int TileSizing::whole_cell_px() const {
    const UiBreakpoint bp = widget_size::current_breakpoint();
    const float cell = has_cell_metrics_ ? std::min(cell_metrics_.cell_w, cell_metrics_.cell_h)
                                         : static_cast<float>(GridLayout::GRID_CELL[to_int(bp)]);
    const int gutter = has_cell_metrics_ ? cell_metrics_.gutter : GridLayout::gutter_px();
    return static_cast<int>(grid_track_extent(cell, gutter, GridLayout::TRACKS_PER_CELL));
}

bool TileSizing::fits(int width_px, int height_px) const {
    // Where half a cell is offered at all. A track is 34px at micro and 40px at
    // tiny. Neither can carry a glyph and a reading together at any rung, so a
    // tile with a reading floors at a whole cell on both. An icon-only tile is
    // left to decide(), which drops the label and measures its own glyph against
    // the box: a micro track is within a pixel of the widest glyph, so that
    // verdict differs from tile to tile. Declining the size here rather than
    // lowering the registry minimum keeps one floor for every screen and lets
    // the resize clamp and the load path grow past it, which they both already
    // do through grow_span_to_fit.
    const UiBreakpoint bp = widget_size::current_breakpoint();
    const bool small_tier_floor = content_.has_value && bp <= UiBreakpoint::Tiny;
    if (small_tier_floor) {
        const int whole_cell = whole_cell_px();
        if (width_px < whole_cell || height_px < whole_cell) {
            return false;
        }
    }
    return decide(width_px, height_px).fits;
}

bool TileSizing::label_drawn() const {
    if (content_.label.empty()) {
        return false;
    }
    if (content_.label_always_drawn) {
        return true;
    }
    // Absent only where no settings exist (a bare test fixture); a label that
    // may be drawn is measured.
    lv_subject_t* shown = lv_xml_get_subject(nullptr, "show_widget_labels");
    return !shown || lv_subject_get_int(shown) != 0;
}

void TileSizing::follow_label_setting() {
    if (label_setting_observer_ || content_.label.empty() || content_.label_always_drawn) {
        return;
    }
    auto& settings = SettingsManager::instance();
    lv_subject_t* shown = settings.subject_show_widget_labels();
    // Observed only once SettingsManager has registered it, so the guard's
    // lifetime is the owner's rather than a subject that never initialised.
    if (lv_xml_get_subject(nullptr, "show_widget_labels") != shown) {
        return;
    }
    label_setting_observer_ = ui::observe<int>(
        shown, this, [](TileSizing* self, int /*shown*/) { self->remeasure(); },
        settings.get_subjects_lifetime());
}

void TileSizing::remeasure() {
    if (last_width_px_ >= 0) {
        measure_and_publish(last_width_px_, last_height_px_);
    }
}

void TileSizing::measure_and_publish(int width_px, int height_px) {
    last_width_px_ = width_px;
    last_height_px_ = height_px;
    follow_label_setting();
    const TileVerdict v = decide(width_px, height_px);
    lv_subject_set_int(&icon_rung_subject_, v.icon_rung);
    lv_subject_set_int(&drawn_rung_subject_,
                       ui::tile_drawn_rung(v.icon_rung, content_.icon_animates
                                                            ? ui::kTileAnimatedMaxScale
                                                            : ui::kTileMaxScale));
    lv_subject_set_int(&label_subject_, static_cast<int>(v.label));
    lv_subject_set_int(&direction_subject_, static_cast<int>(v.direction));
    lv_subject_set_int(&show_target_subject_, v.show_target ? 1 : 0);
    spdlog::trace("[TileSizing] {} at {}x{}: rung={} dir={} label={} target={} fits={}", icon_name_,
                  width_px, height_px, v.icon_rung, static_cast<int>(v.direction),
                  static_cast<int>(v.label), v.show_target, v.fits);
}

} // namespace helix
