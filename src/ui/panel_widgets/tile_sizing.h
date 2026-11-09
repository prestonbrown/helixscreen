// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_guard.h"
#include "grid_layout.h"
#include "helix/ui/text_metrics.h"
#include "src/ui/panel_widgets/tile_layout.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix {

/**
 * @brief Per-instance sizing for a centred-icon home tile
 *
 * Measures the tile's content in the faces it actually renders, decides with
 * decide_tile_layout(), and publishes the verdict as subjects XML binds
 * appearance to. One of these per widget INSTANCE, not per widget type: fan,
 * thermistor and power_device are multi-instance, and a type-global subject
 * would make the second instance overwrite the first's verdict so both tiles
 * agreed on whichever measured last.
 *
 * Construct it in the widget's CONSTRUCTOR. The manager creates a widget, then
 * calls lv_xml_create() on its component, and only then attach(). The XML
 * parser permanently skips a binding whose subject does not exist when the
 * component is parsed, silently and with nothing in the log, so subjects
 * registered from attach() would never bind and the tile would render at its
 * default appearance forever.
 */
class TileSizing {
  public:
    enum class IconBox { Glyph, Square, Disc };

    /// What this tile draws. The two value strings are WORST CASES, never a
    /// live reading: a size accepted while the tile reads 95 must still draw
    /// 888.
    struct Content {
        std::string widest_value;   ///< widest "current / target" ever shown
        std::string widest_current; ///< widest current half alone
        std::string label;
        bool has_value = false;
        /// Text drawn beside the glyph in #font_body_bold at every rung, such
        /// as a tool digit. It widens the glyph's box rather than scaling.
        std::string icon_badge;
        /// True for a label that names what the tile reads (a fan, a sensor)
        /// and is drawn whatever show_widget_labels says. Every other label
        /// follows the setting, and is measured only while it is on.
        bool label_always_drawn = false;
        /// What the icon occupies at a rung: the glyph itself, a square one
        /// line of the face tall (a canvas sized from the face, such as the
        /// spool), or the disc a badged glyph sits in (tile_badge.xml).
        IconBox icon_box = IconBox::Glyph;
        /// The glyph animates, so it is measured and drawn unscaled
        /// (kTileAnimatedMaxScale).
        bool icon_animates = false;
        /// The label is the only thing telling this tile from its siblings
        /// (which fan, which sensor), so the glyph shrinks before it goes.
        bool label_is_identity = false;
        /// Font token of a line drawn under the label in a column and never in
        /// a row, such as bypass's spool row; null for none. Only while it is
        /// drawn: a tile that shows it on a state change sets it then and
        /// calls remeasure().
        const char* detail_font = nullptr;
    };

    explicit TileSizing(const std::string& instance_id);

    /// Most tiles know their content at construction; this is that spelling.
    TileSizing(const std::string& instance_id, Content content) : TileSizing(instance_id) {
        content_ = std::move(content);
    }
    ~TileSizing();

    TileSizing(const TileSizing&) = delete;
    TileSizing& operator=(const TileSizing&) = delete;

    /// The live track geometry, so the half-cell floor is measured against the
    /// cell this grid actually built rather than the nominal edge for the tier.
    /// A grid whose content box forces a smaller track would otherwise never
    /// satisfy the floor, and the span would grow until something clipped.
    void set_cell_metrics(const CellMetrics& metrics) {
        cell_metrics_ = metrics;
        has_cell_metrics_ = true;
    }

    void set_content(Content content) {
        content_ = std::move(content);
    }

    /// The tile's root, once it exists. The verdict is computed against the box
    /// the content actually draws in, which is the outer box less whatever
    /// padding the containers between them consume; estimating that instead of
    /// measuring it picks a rung that spills out of the tile's own container.
    /// Forgotten when the root is deleted, since a deferred re-measure can run
    /// after the tree is gone.
    void set_content_root(lv_obj_t* root);

    const Content& content() const {
        return content_;
    }

    lv_obj_t* content_root() const {
        return content_root_;
    }

    /// Measure at this pixel box and publish the verdict.
    void measure_and_publish(int width_px, int height_px);

    /// Measure again at the last box, after something the verdict reads has
    /// changed. Does nothing before the first measurement.
    void remeasure();

    /// Whether the tile can draw here. Monotonic, as PanelWidget::fits_at
    /// requires, because decide_tile_layout is.
    bool fits(int width_px, int height_px) const;

    /// Flat key/value pairs naming this instance's subjects, for
    /// lv_xml_create(). The vector owns the strings; it stays alive as long as
    /// this object does.
    /// Shaped for lv_xml_create(), which takes a mutable char** even though it
    /// only reads: a flat key/value list terminated by one nullptr.
    const char** subject_attrs() const {
        return const_cast<const char**>(attrs_.data());
    }

    /// One whole cell on this grid, in px: the live track geometry when the
    /// grid has reported it, the tier's nominal cell until then.
    int whole_cell_px() const;

    /// Pass one more subject name to this tile's component, as prop @p prop.
    /// Call from the widget's constructor, before the component is parsed.
    void add_subject_attr(const char* prop, const std::string& subject_name);

    const std::string& icon_subject_name() const {
        return icon_name_;
    }

    /// The rung the glyph draws at (ui::tile_drawn_rung()), for a part sized
    /// off the glyph. Not passed by default; add_subject_attr() it.
    const std::string& drawn_subject_name() const {
        return drawn_name_;
    }

    /// The icon rung last published.
    int icon_rung() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&icon_rung_subject_));
    }

  private:
    TileVerdict decide(int width_px, int height_px) const;
    void rebuild_attrs();
    static void on_content_root_deleted(lv_event_t* e);
    /// Whether the label, if this tile has one, is drawn at all.
    bool label_drawn() const;
    /// Re-measure at the last box whenever show_widget_labels moves, since the
    /// label's presence is part of the measurement.
    void follow_label_setting();

    std::string icon_name_;
    std::string label_name_;
    std::string dir_name_;
    std::string target_name_;
    std::string drawn_name_;
    std::vector<std::string> attr_storage_;
    std::vector<const char*> attrs_;

    Content content_;
    lv_obj_t* content_root_ = nullptr;
    CellMetrics cell_metrics_{};
    bool has_cell_metrics_ = false;
    int last_width_px_ = -1;
    int last_height_px_ = -1;

    lv_subject_t icon_rung_subject_{};
    lv_subject_t label_subject_{};
    lv_subject_t direction_subject_{};
    lv_subject_t show_target_subject_{};
    lv_subject_t drawn_rung_subject_{};
    SubjectManager subjects_;
    ObserverGuard label_setting_observer_;
};

} // namespace helix
