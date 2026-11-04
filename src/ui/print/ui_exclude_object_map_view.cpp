// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_exclude_object_map_view.h"

#include "ui_open_instances.h"
#include "ui_utils.h"

#include "bed_dimensions.h"
#include "bed_grid.h"
#include "core/observer_factory.h"
#include "exclude_map_hatch.h"
#include "lv_draw_buf_guard.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace helix::ui {

namespace {
// Print status keeps its map alive while the details view opens its own.
OpenInstances<ExcludeObjectMapView>& open_map_views() {
    static OpenInstances<ExcludeObjectMapView> views;
    return views;
}
} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

ExcludeObjectMapView::ExcludeObjectMapView() {
    spdlog::debug("[ExcludeObjectMapView] Created");
}

ExcludeObjectMapView::~ExcludeObjectMapView() {
    open_map_views().remove(this);
    if (root_ || canvas_buf_) {
        destroy();
    }
}

// ============================================================================
// Create
// ============================================================================

void ExcludeObjectMapView::create(lv_obj_t* parent, helix::PrinterExcludedObjectsState* state,
                                  const helix::BedDimensions& bed, ObjectTapFn on_object_tapped,
                                  ExcludeTapMode tap_mode,
                                  const helix::gcode::ParsedGCodeFile* parsed_file,
                                  ExcludeMapMode mode) {
    if (root_) {
        spdlog::warn("[ExcludeObjectMapView] create() called but already active");
        return;
    }

    spdlog::debug("[ExcludeObjectMapView] create() bed={}x{} at ({},{}){}", bed.w_mm, bed.h_mm,
                  bed.origin_x, bed.origin_y, bed.circular ? " round" : "");

    state_ = state;
    on_object_tapped_ = std::move(on_object_tapped);
    tap_mode_ = tap_mode;
    mode_ = mode;
    copy_parsed_geometry(parsed_file);
    bed_ = helix::bed_dimensions_from_volume(bed.origin_x, bed.origin_x + bed.w_mm, bed.origin_y,
                                             bed.origin_y + bed.h_mm);
    bed_.circular = bed.circular;
    bed_w_mm_ = bed_.w_mm;
    bed_h_mm_ = bed_.h_mm;

    // Register XML event callback once (idempotent — registration only takes
    // effect the first time; subsequent calls are harmless no-ops).
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_map_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    open_map_views().add(this);

    // Instantiate the XML component
    root_ = static_cast<lv_obj_t*>(mode_ == ExcludeMapMode::Mini
                                       ? lv_xml_create(parent, "exclude_object_map_mini", nullptr)
                                       : lv_xml_create(parent, "exclude_object_map", nullptr));
    if (!root_) {
        spdlog::error("[ExcludeObjectMapView] lv_xml_create failed");
        open_map_views().remove(this);
        return;
    }

    // Disable scrolling on root view
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

    // Force layout so children have valid sizes
    lv_obj_update_layout(root_);

    // Find named children
    plate_area_ = helix::ui::find_required(root_, "plate_area", "ExcludeObjectMapView");

    // Disable scrolling on plate area
    if (plate_area_) {
        lv_obj_remove_flag(plate_area_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(plate_area_, LV_SCROLLBAR_MODE_OFF);
    }

    if (!plate_area_) {
        spdlog::error("[ExcludeObjectMapView] Could not find plate_area");
    }

    // Create transparent overlay container for object rects.
    // EVENT_BUBBLE ensures clicks on object rects reach the close button. The mini map
    // has nothing to tap, so it has no container.
    if (plate_area_ && mode_ == ExcludeMapMode::Interactive) {
        object_container_ = lv_obj_create(plate_area_);
        lv_obj_set_size(object_container_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(object_container_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(object_container_, 0, 0);
        lv_obj_set_style_pad_all(object_container_, 0, 0);
        lv_obj_remove_flag(object_container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(object_container_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(object_container_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_pos(object_container_, 0, 0);
    }

    build_surface();
    build_object_rects();

    // Set up observers to react to state changes
    if (state_) {
        auto rebuild_handler = [](ExcludeObjectMapView* self, int) {
            if (!self->root_)
                return;
            self->update_visual_states();
        };

        excluded_version_obs_ = observe<int>(state_->get_excluded_objects_version_subject(), this,
                                             rebuild_handler, state_->get_subjects_lifetime());

        defined_version_obs_ = observe<int>(
            state_->get_defined_objects_version_subject(), this,
            [](ExcludeObjectMapView* self, int) {
                if (!self->root_)
                    return;
                self->build_surface();
                self->build_object_rects();
            },
            state_->get_subjects_lifetime());
    }

    spdlog::info("[ExcludeObjectMapView] Created successfully");
}

// ============================================================================
// Build surface
// ============================================================================

void ExcludeObjectMapView::build_surface() {
    if (!plate_area_)
        return;

    // Compute actual coordinate extents from all object geometry.
    // Objects may have negative coordinates (e.g., bed centered at 0,0).
    float coord_min_x = bed_.origin_x, coord_min_y = bed_.origin_y;
    float coord_max_x = bed_.origin_x + bed_.w_mm, coord_max_y = bed_.origin_y + bed_.h_mm;
    bool have_extents = false;

    if (state_) {
        const auto& defined = state_->get_defined_objects();
        for (const auto& name : defined) {
            auto info = state_->get_object_geometry(name);
            if (!info || !info->has_bbox)
                continue;
            if (!have_extents) {
                coord_min_x = info->bbox_min.x;
                coord_min_y = info->bbox_min.y;
                coord_max_x = info->bbox_max.x;
                coord_max_y = info->bbox_max.y;
                have_extents = true;
            } else {
                coord_min_x = std::min(coord_min_x, info->bbox_min.x);
                coord_min_y = std::min(coord_min_y, info->bbox_min.y);
                coord_max_x = std::max(coord_max_x, info->bbox_max.x);
                coord_max_y = std::max(coord_max_y, info->bbox_max.y);
            }
        }
    }

    if (have_extents) {
        // Add 10% padding around the extents
        float range_x = coord_max_x - coord_min_x;
        float range_y = coord_max_y - coord_min_y;
        float pad_x = range_x * 0.1f, pad_y = range_y * 0.1f;
        coord_min_x -= pad_x;
        coord_min_y -= pad_y;
        coord_max_x += pad_x;
        coord_max_y += pad_y;
    }
    bed_w_mm_ = coord_max_x - coord_min_x;
    bed_h_mm_ = coord_max_y - coord_min_y;
    coord_min_x_ = coord_min_x;
    coord_min_y_ = coord_min_y;

    // Update plate dimensions label
    if (mode_ == ExcludeMapMode::Interactive) {
        lv_obj_t* dims_label =
            helix::ui::find_required(root_, "plate_dims_label", "ExcludeObjectMapView");
        if (dims_label) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f×%.0f mm", bed_w_mm_, bed_h_mm_);
            lv_label_set_text(dims_label, buf);
        }
    }

    // Build mapper after layout so plate_area has real dimensions
    lv_obj_update_layout(root_);
    const int vw = lv_obj_get_width(plate_area_);
    const int vh = lv_obj_get_height(plate_area_);
    built_w_ = vw;
    built_h_ = vh;
    mapper_ =
        std::make_unique<CoordMapper>(bed_w_mm_, bed_h_mm_, vw, vh, coord_min_x_, coord_min_y_);

    // A canvas for first-layer outline rendering. A resize swaps the buffer under the
    // canvas it already has.
    if (vw > 0 && vh > 0) {
        lv_draw_buf_t* buf = lv_draw_buf_create(vw, vh, LV_COLOR_FORMAT_ARGB8888, 0);
        if (buf) {
            if (!canvas_) {
                canvas_ = lv_canvas_create(plate_area_);
                lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_scrollbar_mode(canvas_, LV_SCROLLBAR_MODE_OFF);
                lv_obj_add_flag(canvas_, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_set_pos(canvas_, 0, 0);
            }
            lv_canvas_set_draw_buf(canvas_, buf);
            lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_TRANSP);
            helix::safe_draw_buf_destroy(canvas_buf_, "exclmap");
            canvas_buf_ = buf;
            // The canvas sizes itself to its buffer on the next layout pass.
            lv_obj_update_layout(root_);
        }
    }
}

void ExcludeObjectMapView::resize() {
    if (!root_ || !plate_area_)
        return;
    lv_obj_update_layout(root_);
    if (lv_obj_get_width(plate_area_) == built_w_ && lv_obj_get_height(plate_area_) == built_h_)
        return;
    build_surface();
    build_object_rects();
}

// ============================================================================
// Destroy
// ============================================================================

void ExcludeObjectMapView::destroy() {
    // A tree deleted under the view leaves root_ null but the buffer and the
    // observers still held.
    if (!root_ && !canvas_buf_ && !excluded_version_obs_ && !defined_version_obs_)
        return;

    spdlog::debug("[ExcludeObjectMapView] destroy()");

    // Release observers first to stop callbacks
    excluded_version_obs_.reset();
    defined_version_obs_.reset();

    // Leave the open set BEFORE deleting widgets, so a close event fired
    // during the delete cascade cannot reach this view.
    open_map_views().remove(this);

    // The observers released above were the only queued work that reaches
    // these widgets.
    {
        object_rects_.clear();
        mapper_.reset();

        // Sever the canvas widget's reference to the draw buffer BEFORE freeing
        // the buffer. The canvas (an lv_image subclass) renders only via its
        // image src, which points at canvas_buf_. Deletion of root_ — and thus
        // the canvas child — is deferred below (safe_delete_deferred), so the
        // canvas widget outlives this function by >=1 tick. Clearing the image
        // src (NULL resets the image attributes) leaves the still-live canvas
        // with nothing to draw, so freeing canvas_buf_ here cannot create a
        // use-after-free window if the hidden subtree is invalidated before the
        // async delete tick runs. canvas->draw_buf is only dereferenced by
        // explicit canvas API calls (set_px / fill_bg / init_layer), none of
        // which fire during a passive redraw.
        if (canvas_) {
            lv_image_set_src(canvas_, nullptr);
        }
        canvas_ = nullptr;

        // Free canvas draw buffer now that no live widget references it. A
        // blend of it from the last refresh may still be in flight.
        helix::safe_draw_buf_destroy(canvas_buf_, "exclmap");

        // Deferred delete: a bare lv_obj_delete(root_) here is a sync widget
        // deletion that can run inside a UpdateQueue process_pending batch
        // (memory-pressure reclaim chain: PrintStatusPanel::try_reclaim_cached_print_status
        // -> destroy_overlay_ui -> safe_delete_deferred(overlay_root_) ->
        // on_ui_destroyed() -> map_view_->destroy()). Two sync deletions in one
        // batch corrupt LVGL's global event linked list (#776/#190/#80).
        // safe_delete_deferred escapes the batch via lv_obj_delete_async, and
        // is equally correct on the standalone hide_exclude_map_view() path.
        lv_obj_t* root = root_;
        root_ = nullptr;
        if (root) {
            helix::ui::safe_delete_deferred(root);
        }
        plate_area_ = nullptr;
        object_container_ = nullptr;
    }

    state_ = nullptr;
    on_object_tapped_ = nullptr;
    parsed_objects_.reset();
    parsed_outlines_.clear();

    spdlog::debug("[ExcludeObjectMapView] Destroyed");
}

// ============================================================================
// Build object rects
// ============================================================================

void ExcludeObjectMapView::build_object_rects() {
    if (mode_ == ExcludeMapMode::Mini) {
        update_visual_states();
        return;
    }
    if (!object_container_ || !state_ || !mapper_)
        return;

    // build_object_rects runs from defined_version_obs_ (observe<int>,
    // deferred via UpdateQueue) — sync clean inside that batch corrupts
    // LVGL's event linked list (#878). Use the async-clean helper. [L081]
    lv_obj_update_layout(object_container_);
    helix::ui::safe_clean_children(object_container_);
    object_rects_.clear();

    const auto badges = compute_object_badges(*state_, parsed_objects_.get());
    int rects_created = 0;

    for (const auto& badge : badges) {
        const std::string& name = badge.name;
        glm::vec2 bbox_min{0.0f, 0.0f};
        glm::vec2 bbox_max{0.0f, 0.0f};
        bool have_bbox = false;

        // Priority 1: GCode parser bounding box (more accurate than Moonraker geometry)
        if (parsed_objects_) {
            auto it = parsed_objects_->objects.find(name);
            if (it != parsed_objects_->objects.end() && !it->second.bounding_box.is_empty()) {
                bbox_min = {it->second.bounding_box.min.x, it->second.bounding_box.min.y};
                bbox_max = {it->second.bounding_box.max.x, it->second.bounding_box.max.y};
                have_bbox = true;
                spdlog::trace("[ExcludeObjectMapView] Using parsed GCode bbox for '{}'", name);
            }
        }

        // Priority 2: Moonraker geometry from PrinterExcludedObjectsState
        if (!have_bbox) {
            auto info = state_->get_object_geometry(name);
            if (info && info->has_bbox) {
                bbox_min = info->bbox_min;
                bbox_max = info->bbox_max;
                have_bbox = true;
                spdlog::trace("[ExcludeObjectMapView] Using Moonraker bbox for '{}'", name);
            }
        }

        if (!have_bbox) {
            spdlog::trace("[ExcludeObjectMapView] No bbox for '{}', skipping", name);
            continue;
        }

        PixelRect pr = mapper_->bbox_to_rect(bbox_min, bbox_max);
        lv_obj_t* rect = create_object_rect(object_container_, badge, pr);
        if (rect) {
            object_rects_.push_back({name, badge.defined_index, rect});
            ++rects_created;
        }
    }

    spdlog::debug("[ExcludeObjectMapView] Built {} rects from {} defined objects", rects_created,
                  badges.size());

    // Show or hide the empty message imperatively. The XML component scope
    // persists across create/destroy cycles, so we cannot use lv_xml_register_subject
    // here — the scope would hold a dangling pointer after destroy() deinits it.
    lv_obj_t* empty_msg = helix::ui::find_required(root_, "empty_message", "ExcludeObjectMapView");
    if (empty_msg) {
        if (rects_created > 0) {
            lv_obj_add_flag(empty_msg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(empty_msg, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Draw polygon outlines on canvas (from parsed GCode or Moonraker data)
    if (canvas_) {
        draw_first_layer_outlines();

        // Make bounding box rects invisible — outlines replace them visually
        // but keep them as tap hit areas
        for (auto& orect : object_rects_) {
            if (!orect.rect)
                continue;
            lv_obj_set_style_border_opa(orect.rect, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(orect.rect, LV_OPA_TRANSP, 0);
        }
    }

    update_visual_states();
}

// ============================================================================
// draw_first_layer_outlines
// ============================================================================

// Convex hull using Andrew's monotone chain algorithm.
// Returns hull points in counter-clockwise order.
static std::vector<glm::vec2> convex_hull(std::vector<glm::vec2>& pts) {
    size_t n = pts.size();
    if (n < 3)
        return pts;

    std::sort(pts.begin(), pts.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });

    // Cross product of OA and OB vectors
    auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) -> float {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };

    std::vector<glm::vec2> hull(2 * n);
    int k = 0;

    // Lower hull
    for (size_t i = 0; i < n; ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0)
            k--;
        hull[k++] = pts[i];
    }

    // Upper hull
    for (int i = static_cast<int>(n) - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0)
            k--;
        hull[k++] = pts[i];
    }

    hull.resize(k - 1); // last point == first point, remove duplicate
    return hull;
}

void ExcludeObjectMapView::copy_parsed_geometry(const helix::gcode::ParsedGCodeFile* parsed) {
    parsed_objects_.reset();
    parsed_outlines_.clear();
    if (!parsed) {
        return;
    }
    parsed_objects_ = std::make_unique<helix::gcode::ParsedGCodeFile>();
    parsed_objects_->objects = parsed->objects;

    const auto* first_layer = parsed->get_layer(0);
    if (!first_layer) {
        return;
    }
    std::unordered_map<std::string, std::vector<glm::vec2>> object_points;
    for (const auto& seg : first_layer->segments) {
        if (!seg.is_extrusion) {
            continue;
        }
        const auto& obj_name = parsed->get_object_name(seg.object_name_index);
        if (obj_name.empty()) {
            continue;
        }
        auto& pts = object_points[obj_name];
        pts.push_back({seg.start.x, seg.start.y});
        pts.push_back({seg.end.x, seg.end.y});
    }
    for (auto& [name, pts] : object_points) {
        auto hull = convex_hull(pts);
        if (hull.size() >= 3) {
            parsed_outlines_[name] = std::move(hull);
        }
    }
}

void ExcludeObjectMapView::draw_first_layer_outlines() {
    if (!canvas_ || !mapper_ || !state_)
        return;

    // Clear canvas to transparent
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    draw_bed_reference(&layer);

    // Outline per object: the parsed first-layer hull, else Klipper's polygon.
    std::unordered_map<std::string, std::vector<glm::vec2>> object_polygons = parsed_outlines_;

    // Klipper's polygon for objects the parse did not outline.
    const auto& defined = state_->get_defined_objects();
    for (const auto& name : defined) {
        if (object_polygons.count(name) > 0)
            continue; // already have outline
        auto geom = state_->get_object_geometry(name);
        if (geom && !geom->polygon.empty()) {
            object_polygons[name] = geom->polygon;
        } else if (geom && geom->has_bbox) {
            const glm::vec2 lo = geom->bbox_min;
            const glm::vec2 hi = geom->bbox_max;
            object_polygons[name] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
        }
    }

    if (object_polygons.empty()) {
        lv_canvas_finish_layer(canvas_, &layer);
        return;
    }

    for (const auto& [name, poly] : object_polygons) {
        spdlog::trace("[ExcludeObjectMapView] Object '{}' polygon has {} points", name,
                      poly.size());
    }

    // Build name -> color index mapping
    std::unordered_map<std::string, int> name_to_index;
    for (int i = 0; i < static_cast<int>(defined.size()); ++i) {
        name_to_index[defined[i]] = i;
    }

    const auto& excluded = state_->get_excluded_objects();
    const bool mini = mode_ == ExcludeMapMode::Mini;
    const std::string& current = state_->get_current_object();
    const lv_color_t current_color = theme_manager_get_color("primary");
    const int hatch_px = std::max<int>(4, theme_manager_get_spacing("space_xs"));

    // Draw polygon outlines on canvas
    const auto draw_line = [&](float x1, float y1, float x2, float y2, lv_color_t color,
                               int32_t width, lv_opa_t opa) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = color;
        dsc.width = width;
        dsc.p1.x = static_cast<lv_value_precise_t>(x1);
        dsc.p1.y = static_cast<lv_value_precise_t>(y1);
        dsc.p2.x = static_cast<lv_value_precise_t>(x2);
        dsc.p2.y = static_cast<lv_value_precise_t>(y2);
        dsc.opa = opa;
        dsc.round_start = 1;
        dsc.round_end = 1;
        lv_draw_line(&layer, &dsc);
    };

    for (const auto& [obj_name, polygon] : object_polygons) {
        auto it = name_to_index.find(obj_name);
        if (it == name_to_index.end())
            continue;
        if (polygon.size() < 3)
            continue;

        const bool is_excluded = excluded.count(obj_name) > 0;
        const bool is_current = mini && !is_excluded && obj_name == current;
        const lv_color_t color = is_current ? current_color : object_badge_color(it->second);
        // The mini map reads at a glance: the object printing now is full strength and
        // heavier, the rest sit back, and an excluded one is faded and hatched.
        const lv_opa_t opa =
            is_excluded ? object_badge_opa(true) : (mini && !is_current ? LV_OPA_70 : LV_OPA_COVER);
        const int32_t width = is_current ? 3 : 2;

        std::vector<glm::vec2> px(polygon.size());
        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto [x, y] = mapper_->mm_to_px(polygon[i].x, polygon[i].y);
            px[i] = {x, y};
        }

        // Draw closed polygon edges
        for (size_t i = 0; i < px.size(); ++i) {
            const glm::vec2& a = px[i];
            const glm::vec2& b = px[(i + 1) % px.size()];
            draw_line(a.x, a.y, b.x, b.y, color, width, opa);
        }

        if (mini && is_excluded) {
            for (const auto& seg : hatch_polygon(px, static_cast<float>(hatch_px)))
                draw_line(seg.a.x, seg.a.y, seg.b.x, seg.b.y, color, 1, LV_OPA_50);
        }
    }

    lv_canvas_finish_layer(canvas_, &layer);

    spdlog::debug("[ExcludeObjectMapView] Drew polygon outlines for {} objects",
                  object_polygons.size());
}

void ExcludeObjectMapView::draw_bed_reference(lv_layer_t* layer) const {
    if (!mapper_ || !plate_area_) {
        return;
    }
    const double x_min = bed_.origin_x;
    const double x_max = bed_.origin_x + bed_.w_mm;
    const double y_min = bed_.origin_y;
    const double y_max = bed_.origin_y + bed_.h_mm;

    // The plate's own border color: the grid faint, the bed edge solid. The
    // canvas clips whatever lies outside the zoomed view.
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_obj_get_style_border_color(plate_area_, LV_PART_MAIN);
    auto draw = [&](const helix::BedGridSegment& seg) {
        const auto [x1, y1] =
            mapper_->mm_to_px(static_cast<float>(seg.x1), static_cast<float>(seg.y1));
        const auto [x2, y2] =
            mapper_->mm_to_px(static_cast<float>(seg.x2), static_cast<float>(seg.y2));
        dsc.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
        dsc.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
        lv_draw_line(layer, &dsc);
    };

    dsc.width = 1;
    dsc.opa = LV_OPA_40;
    for (const auto& seg : helix::bed_grid_lines(x_min, x_max, y_min, y_max,
                                                 helix::kBedGridSpacingMm, bed_.circular)) {
        draw(seg);
    }
    dsc.width = 2;
    dsc.opa = LV_OPA_COVER;
    for (const auto& seg : helix::bed_outline_segments(x_min, x_max, y_min, y_max, bed_.circular)) {
        draw(seg);
    }
}

// ============================================================================
// create_object_rect
// ============================================================================

lv_obj_t* ExcludeObjectMapView::create_object_rect(lv_obj_t* parent, const ObjectBadge& badge,
                                                   const PixelRect& rect) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y));
    lv_obj_set_size(obj, static_cast<int32_t>(rect.w), static_cast<int32_t>(rect.h));

    char obj_name[32];
    snprintf(obj_name, sizeof(obj_name), "obj_rect_%d", badge.defined_index);
    lv_obj_set_name(obj, obj_name);

    lv_color_t color = object_badge_color(badge.defined_index);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 3, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Number badge — the same disc the 2D/3D render draws (ui_exclude_object_badges.h)
    const int32_t diameter = object_badge_diameter();
    lv_obj_t* disc = lv_obj_create(obj);
    lv_obj_set_size(disc, diameter, diameter);
    lv_obj_align(disc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(disc, color, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(disc, 0, 0);
    lv_obj_set_style_pad_all(disc, 0, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(disc, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* num_label = lv_label_create(disc);
    lv_label_set_text(num_label, badge.number.c_str());
    lv_obj_set_style_text_font(num_label, object_badge_font(), 0);
    lv_obj_set_style_text_color(num_label, object_badge_text_color(color), 0);
    lv_obj_align(num_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(num_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(num_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Clicked callback — uses `this` captured in user_data
    lv_obj_add_event_cb(obj, on_object_clicked, LV_EVENT_CLICKED, this);

    return obj;
}

// ============================================================================
// update_visual_states
// ============================================================================

void ExcludeObjectMapView::update_visual_states() {
    if (!state_)
        return;

    const auto& excluded = state_->get_excluded_objects();
    const auto& current_obj = state_->get_current_object();
    bool have_canvas_outlines = (canvas_ != nullptr);

    lv_color_t primary_color = theme_manager_get_color("primary");
    lv_color_t danger_color = theme_manager_get_color("danger");

    for (int i = 0; i < static_cast<int>(object_rects_.size()); ++i) {
        const auto& entry = object_rects_[i];
        lv_obj_t* rect = entry.rect;
        if (!rect)
            continue;

        bool is_excluded = excluded.count(entry.name) > 0;
        bool is_current = (entry.name == current_obj);

        if (have_canvas_outlines) {
            // Canvas handles visuals — rects are invisible tap targets only
            lv_obj_set_style_border_width(rect, 0, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(rect, object_badge_opa(is_excluded), 0);
            if (is_excluded && tap_mode_ == ExcludeTapMode::ExcludeOnly) {
                lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            }
        } else if (is_excluded) {
            lv_obj_set_style_border_color(rect, danger_color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(rect, object_badge_opa(true), 0);
            if (tap_mode_ == ExcludeTapMode::ExcludeOnly) {
                lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            }
        } else if (is_current) {
            lv_obj_set_style_border_color(rect, primary_color, 0);
            lv_obj_set_style_bg_color(rect, primary_color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_20, 0);
            lv_obj_set_style_opa(rect, LV_OPA_COVER, 0);
            lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_color_t color = object_badge_color(entry.defined_index);
            lv_obj_set_style_border_color(rect, color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(rect, LV_OPA_COVER, 0);
            lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // Redraw canvas outlines to reflect excluded/current state
    if (have_canvas_outlines) {
        draw_first_layer_outlines();
    }
}

// ============================================================================
// Static event callbacks
// ============================================================================

void ExcludeObjectMapView::on_close_clicked(lv_event_t* e) {
    ExcludeObjectMapView* view = open_map_views().owner_of(lv_event_get_current_target_obj(e));
    if (!view) {
        return;
    }
    spdlog::debug("[ExcludeObjectMapView] Close button clicked");
    // A copy: the callback usually destroys this view, and its own
    // std::function with it.
    auto close = view->close_cb_;
    if (close) {
        close();
    }
}

void ExcludeObjectMapView::on_object_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectMapView*>(lv_event_get_user_data(e));
    if (!self || !self->on_object_tapped_)
        return;
    lv_obj_t* target = lv_event_get_target_obj(e);
    for (const auto& entry : self->object_rects_) {
        if (entry.rect == target) {
            spdlog::info("[ExcludeObjectMapView] Object rect clicked: '{}'", entry.name);
            self->on_object_tapped_(entry.name);
            return;
        }
    }

    spdlog::debug("[ExcludeObjectMapView] on_object_clicked: no matching rect found");
}

} // namespace helix::ui
