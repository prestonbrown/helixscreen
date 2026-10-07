// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_exclude_object_map_view.h"

#include "ui_open_instances.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "bed_dimensions.h"
#include "lv_draw_buf_guard.h"
#include "observer_factory.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"

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
                                  float bed_w_mm, float bed_h_mm, ObjectTapFn on_object_tapped,
                                  ExcludeTapMode tap_mode,
                                  const helix::gcode::ParsedGCodeFile* parsed_file) {
    if (root_) {
        spdlog::warn("[ExcludeObjectMapView] create() called but already active");
        return;
    }

    spdlog::debug("[ExcludeObjectMapView] create() bed={}x{}", bed_w_mm, bed_h_mm);

    state_ = state;
    on_object_tapped_ = std::move(on_object_tapped);
    tap_mode_ = tap_mode;
    copy_parsed_geometry(parsed_file);
    const auto bed = helix::bed_dimensions_from_volume(0.0f, bed_w_mm, 0.0f, bed_h_mm);
    bed_w_mm_ = bed.w_mm;
    bed_h_mm_ = bed.h_mm;

    // Register XML event callback once (idempotent — registration only takes
    // effect the first time; subsequent calls are harmless no-ops).
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_map_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    open_map_views().add(this);

    // Instantiate the XML component
    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_map", nullptr));
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
    plate_area_ = lv_obj_find_by_name(root_, "plate_area");

    // Disable scrolling on plate area
    if (plate_area_) {
        lv_obj_remove_flag(plate_area_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(plate_area_, LV_SCROLLBAR_MODE_OFF);
    }

    if (!plate_area_) {
        spdlog::error("[ExcludeObjectMapView] Could not find plate_area");
    }

    // Create transparent overlay container for object rects.
    // EVENT_BUBBLE ensures clicks on object rects reach the close button.
    if (plate_area_) {
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

    // Compute actual coordinate extents from all object geometry.
    // Objects may have negative coordinates (e.g., bed centered at 0,0).
    float coord_min_x = 0.0f, coord_min_y = 0.0f;
    float coord_max_x = bed_w_mm_, coord_max_y = bed_h_mm_;
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
        bed_w_mm_ = coord_max_x - coord_min_x;
        bed_h_mm_ = coord_max_y - coord_min_y;
    }

    // Update plate dimensions label
    lv_obj_t* dims_label = lv_obj_find_by_name(root_, "plate_dims_label");
    if (dims_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f×%.0f mm", bed_w_mm_, bed_h_mm_);
        lv_label_set_text(dims_label, buf);
    }

    // Build mapper after layout so plate_area has real dimensions
    if (plate_area_) {
        lv_obj_update_layout(root_);
        int vw = lv_obj_get_width(plate_area_);
        int vh = lv_obj_get_height(plate_area_);
        mapper_ =
            std::make_unique<CoordMapper>(bed_w_mm_, bed_h_mm_, vw, vh, coord_min_x, coord_min_y);

        // Create canvas for first-layer outline rendering
        if (vw > 0 && vh > 0) {
            canvas_buf_ = lv_draw_buf_create(vw, vh, LV_COLOR_FORMAT_ARGB8888, 0);
            if (canvas_buf_) {
                canvas_ = lv_canvas_create(plate_area_);
                lv_canvas_set_draw_buf(canvas_, canvas_buf_);
                lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_TRANSP);
                lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_scrollbar_mode(canvas_, LV_SCROLLBAR_MODE_OFF);
                lv_obj_add_flag(canvas_, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_set_pos(canvas_, 0, 0);
            }
        }
    }

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
                self->build_object_rects();
            },
            state_->get_subjects_lifetime());
    }

    spdlog::info("[ExcludeObjectMapView] Created successfully");
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

    // Freeze queue, drain pending callbacks, then delete widgets
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();

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
    lv_obj_t* empty_msg = lv_obj_find_by_name(root_, "empty_message");
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
        }
    }

    if (object_polygons.empty())
        return;

    for (const auto& [name, poly] : object_polygons) {
        spdlog::info("[ExcludeObjectMapView] Object '{}' polygon has {} points", name, poly.size());
    }

    // Build name -> color index mapping
    std::unordered_map<std::string, int> name_to_index;
    for (int i = 0; i < static_cast<int>(defined.size()); ++i) {
        name_to_index[defined[i]] = i;
    }

    const auto& excluded = state_->get_excluded_objects();

    // Draw polygon outlines on canvas
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    for (const auto& [obj_name, polygon] : object_polygons) {
        auto it = name_to_index.find(obj_name);
        if (it == name_to_index.end())
            continue;
        if (polygon.size() < 3)
            continue;

        lv_color_t color = object_badge_color(it->second);

        // Draw closed polygon edges
        for (size_t i = 0; i < polygon.size(); ++i) {
            size_t j = (i + 1) % polygon.size();
            auto [px1, py1] = mapper_->mm_to_px(polygon[i].x, polygon[i].y);
            auto [px2, py2] = mapper_->mm_to_px(polygon[j].x, polygon[j].y);

            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.color = color;
            dsc.width = 2;
            dsc.p1.x = static_cast<lv_value_precise_t>(px1);
            dsc.p1.y = static_cast<lv_value_precise_t>(py1);
            dsc.p2.x = static_cast<lv_value_precise_t>(px2);
            dsc.p2.y = static_cast<lv_value_precise_t>(py2);
            dsc.opa = object_badge_opa(excluded.count(obj_name) > 0);
            dsc.round_start = 1;
            dsc.round_end = 1;

            lv_draw_line(&layer, &dsc);
        }
    }

    lv_canvas_finish_layer(canvas_, &layer);

    spdlog::debug("[ExcludeObjectMapView] Drew polygon outlines for {} objects",
                  object_polygons.size());
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
