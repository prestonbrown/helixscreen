// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_exclude_object_map_view.h"

#include "ui_print_exclude_object_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace helix::ui {

// File-scope pointer so static callbacks can reach the active view.
static ExcludeObjectMapView* g_active_map_view = nullptr;

// ============================================================================
// CoordMapper
// ============================================================================

ExcludeObjectMapView::CoordMapper::CoordMapper(float bed_w_mm, float bed_h_mm, int viewport_w_px,
                                               int viewport_h_px, float origin_x, float origin_y)
    : origin_x_(origin_x), origin_y_(origin_y), viewport_h_(viewport_h_px) {
    float scale_x = static_cast<float>(viewport_w_px) / bed_w_mm;
    float scale_y = static_cast<float>(viewport_h_px) / bed_h_mm;
    scale_ = std::min(scale_x, scale_y);
    float rendered_w = bed_w_mm * scale_;
    float rendered_h = bed_h_mm * scale_;
    offset_x_ = (static_cast<float>(viewport_w_px) - rendered_w) / 2.0f;
    offset_y_ = (static_cast<float>(viewport_h_px) - rendered_h) / 2.0f;
}

std::pair<float, float> ExcludeObjectMapView::CoordMapper::mm_to_px(float x_mm, float y_mm) const {
    // Subtract origin to handle non-zero-based coordinate systems
    float px = offset_x_ + (x_mm - origin_x_) * scale_;
    float bed_h_px = static_cast<float>(viewport_h_) - 2.0f * offset_y_;
    float py = offset_y_ + bed_h_px - (y_mm - origin_y_) * scale_;
    return {px, py};
}

ExcludeObjectMapView::PixelRect
ExcludeObjectMapView::CoordMapper::bbox_to_rect(glm::vec2 bbox_min, glm::vec2 bbox_max) const {
    auto [x1, y1] = mm_to_px(bbox_min.x, bbox_max.y);
    auto [x2, y2] = mm_to_px(bbox_max.x, bbox_min.y);
    float raw_w = x2 - x1, raw_h = y2 - y1;
    float w = std::max(raw_w, MIN_TOUCH_TARGET_PX);
    float h = std::max(raw_h, MIN_TOUCH_TARGET_PX);
    if (w > raw_w)
        x1 -= (w - raw_w) / 2.0f;
    if (h > raw_h)
        y1 -= (h - raw_h) / 2.0f;
    return {x1, y1, w, h};
}

// ============================================================================
// KeyBarMode
// ============================================================================

ExcludeObjectMapView::KeyBarMode ExcludeObjectMapView::key_bar_mode(int object_count) {
    if (object_count <= 4)
        return KeyBarMode::FullNames;
    if (object_count <= 7)
        return KeyBarMode::Abbreviated;
    return KeyBarMode::Summary;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

ExcludeObjectMapView::ExcludeObjectMapView() {
    spdlog::debug("[ExcludeObjectMapView] Created");
}

ExcludeObjectMapView::~ExcludeObjectMapView() {
    if (root_) {
        destroy();
    }
}

// ============================================================================
// Create
// ============================================================================

void ExcludeObjectMapView::create(lv_obj_t* parent, helix::PrinterExcludedObjectsState* state,
                                  float bed_w_mm, float bed_h_mm,
                                  PrintExcludeObjectManager* exclude_manager,
                                  std::shared_ptr<helix::gcode::ParsedGCodeFile> parsed_file) {
    if (root_) {
        spdlog::warn("[ExcludeObjectMapView] create() called but already active");
        return;
    }

    spdlog::debug("[ExcludeObjectMapView] create() bed={}x{}", bed_w_mm, bed_h_mm);

    state_ = state;
    exclude_manager_ = exclude_manager;
    parsed_file_ = std::move(parsed_file);
    bed_w_mm_ = (bed_w_mm > 0.0f) ? bed_w_mm : 235.0f;
    bed_h_mm_ = (bed_h_mm > 0.0f) ? bed_h_mm : 235.0f;

    // Register XML event callback once (idempotent — registration only takes
    // effect the first time; subsequent calls are harmless no-ops).
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_map_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    // Expose this instance to static callbacks
    g_active_map_view = this;

    // Instantiate the XML component
    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_map", nullptr));
    if (!root_) {
        spdlog::error("[ExcludeObjectMapView] lv_xml_create failed");
        g_active_map_view = nullptr;
        return;
    }

    // Disable scrolling on root view
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);

    // Force layout so children have valid sizes
    lv_obj_update_layout(root_);

    // Find named children
    plate_area_ = lv_obj_find_by_name(root_, "plate_area");
    key_bar_ = lv_obj_find_by_name(root_, "key_bar");

    // Disable scrolling on plate area and key bar
    if (plate_area_) {
        lv_obj_remove_flag(plate_area_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(plate_area_, LV_SCROLLBAR_MODE_OFF);
    }
    if (key_bar_) {
        lv_obj_remove_flag(key_bar_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(key_bar_, LV_SCROLLBAR_MODE_OFF);
    }

    if (!plate_area_) {
        spdlog::error("[ExcludeObjectMapView] Could not find plate_area");
    }
    if (!key_bar_) {
        spdlog::error("[ExcludeObjectMapView] Could not find key_bar");
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

    // Build object rects and key bar
    build_object_rects();
    build_key_bar();

    // Set up observers to react to state changes
    if (state_) {
        auto rebuild_handler = [](ExcludeObjectMapView* self, int) {
            if (!self->root_)
                return;
            self->update_visual_states();
        };

        excluded_version_obs_ = observe_int_sync<ExcludeObjectMapView>(
            state_->get_excluded_objects_version_subject(), this, rebuild_handler);

        defined_version_obs_ =
            observe_int_sync<ExcludeObjectMapView>(state_->get_defined_objects_version_subject(),
                                                   this, [](ExcludeObjectMapView* self, int) {
                                                       if (!self->root_)
                                                           return;
                                                       self->build_object_rects();
                                                       self->build_key_bar();
                                                   });
    }

    spdlog::info("[ExcludeObjectMapView] Created successfully");
}

// ============================================================================
// Destroy
// ============================================================================

void ExcludeObjectMapView::destroy() {
    if (!root_)
        return;

    spdlog::debug("[ExcludeObjectMapView] destroy()");

    // Release observers first to stop callbacks
    excluded_version_obs_.reset();
    defined_version_obs_.reset();

    // Null the global pointer BEFORE deleting widgets, so any queued close
    // events that fire during the delete cascade cannot reach a stale pointer.
    if (g_active_map_view == this) {
        g_active_map_view = nullptr;
    }

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
        if (canvas_ && lv_obj_is_valid(canvas_)) {
            lv_image_set_src(canvas_, nullptr);
        }
        canvas_ = nullptr;

        // Free canvas draw buffer now that no live widget references it.
        if (canvas_buf_) {
            lv_draw_buf_destroy(canvas_buf_);
            canvas_buf_ = nullptr;
        }

        // Deferred delete: a bare lv_obj_delete(root_) here is a sync widget
        // deletion that can run inside a UpdateQueue process_pending batch
        // (memory-pressure reclaim chain: PrintStatusPanel::try_reclaim_cached_print_status
        // -> destroy_overlay_ui -> safe_delete_deferred(overlay_root_) ->
        // on_ui_destroyed() -> map_view_->destroy()). Two sync deletions in one
        // batch corrupt LVGL's global event linked list (#776/#190/#80).
        // safe_delete_deferred escapes the batch via lv_obj_delete_async, and
        // is equally correct on the standalone hide_exclude_map_view() path.
        helix::ui::safe_delete_deferred(root_);
        root_ = nullptr;
        plate_area_ = nullptr;
        key_bar_ = nullptr;
        object_container_ = nullptr;
    }

    state_ = nullptr;
    exclude_manager_ = nullptr;
    parsed_file_.reset();

    spdlog::debug("[ExcludeObjectMapView] Destroyed");
}

// ============================================================================
// Build object rects
// ============================================================================

void ExcludeObjectMapView::build_object_rects() {
    if (!object_container_ || !state_ || !mapper_)
        return;

    // build_object_rects runs from defined_version_obs_ (observe_int_sync,
    // deferred via UpdateQueue) — sync clean inside that batch corrupts
    // LVGL's event linked list (#878). Use the async-clean helper. [L081]
    lv_obj_update_layout(object_container_);
    helix::ui::safe_clean_children(object_container_);
    object_rects_.clear();

    const auto& defined = state_->get_defined_objects();
    int index = 0;
    int rects_created = 0;

    for (const auto& name : defined) {
        glm::vec2 bbox_min{0.0f, 0.0f};
        glm::vec2 bbox_max{0.0f, 0.0f};
        bool have_bbox = false;

        // Priority 1: GCode parser bounding box (more accurate than Moonraker geometry)
        if (parsed_file_) {
            auto it = parsed_file_->objects.find(name);
            if (it != parsed_file_->objects.end() && !it->second.bounding_box.is_empty()) {
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
            ++index;
            continue;
        }

        PixelRect pr = mapper_->bbox_to_rect(bbox_min, bbox_max);
        lv_obj_t* rect = create_object_rect(object_container_, index, name, pr);
        if (rect) {
            object_rects_.push_back({name, rect});
            ++rects_created;
        }
        ++index;
    }

    spdlog::debug("[ExcludeObjectMapView] Built {} rects from {} defined objects", rects_created,
                  defined.size());

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

void ExcludeObjectMapView::draw_first_layer_outlines() {
    if (!canvas_ || !mapper_ || !state_)
        return;

    // Clear canvas to transparent
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_TRANSP);

    // Collect polygon data per object. Priority:
    // 1. Convex hull from ParsedGCodeFile layer 0 segments
    // 2. Polygon from Moonraker ObjectInfo (slicer-provided outline)
    std::unordered_map<std::string, std::vector<glm::vec2>> object_polygons;

    if (parsed_file_) {
        // Source 1: compute convex hulls from first layer extrusion segments
        const auto* first_layer = parsed_file_->get_layer(0);
        if (first_layer && !first_layer->segments.empty()) {
            std::unordered_map<std::string, std::vector<glm::vec2>> object_points;
            for (const auto& seg : first_layer->segments) {
                if (!seg.is_extrusion)
                    continue;
                const auto& obj_name = parsed_file_->get_object_name(seg.object_name_index);
                if (obj_name.empty())
                    continue;
                auto& pts = object_points[obj_name];
                pts.push_back({seg.start.x, seg.start.y});
                pts.push_back({seg.end.x, seg.end.y});
            }
            for (auto& [name, pts] : object_points) {
                auto hull = convex_hull(pts);
                if (hull.size() >= 3) {
                    object_polygons[name] = std::move(hull);
                }
            }
        }
    }

    // Source 2: use Moonraker polygon data for any objects not covered by source 1
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

    // Draw polygon outlines on canvas
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    for (const auto& [obj_name, polygon] : object_polygons) {
        auto it = name_to_index.find(obj_name);
        if (it == name_to_index.end())
            continue;
        if (polygon.size() < 3)
            continue;

        lv_color_t color = get_object_color(it->second);

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
            dsc.opa = LV_OPA_COVER;
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

lv_obj_t* ExcludeObjectMapView::create_object_rect(lv_obj_t* parent, int index,
                                                   const std::string& name, const PixelRect& rect) {
    // Main rect
    (void)name; // name is tracked in object_rects_ by the caller

    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y));
    lv_obj_set_size(obj, static_cast<int32_t>(rect.w), static_cast<int32_t>(rect.h));

    char obj_name[32];
    snprintf(obj_name, sizeof(obj_name), "obj_rect_%d", index);
    lv_obj_set_name(obj, obj_name);

    lv_color_t color = get_object_color(index);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 3, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Number badge — 22x22 circle centered in the rect
    lv_obj_t* badge = lv_obj_create(obj);
    lv_obj_set_size(badge, 22, 22);
    lv_obj_align(badge, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(badge, color, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 11, 0); // circle
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Number label inside badge
    lv_obj_t* num_label = lv_label_create(badge);
    char num_buf[16];
    snprintf(num_buf, sizeof(num_buf), "%d", index + 1);
    lv_label_set_text(num_label, num_buf);
    lv_obj_set_style_text_font(num_label, theme_manager_get_font("font_small"), 0);
    lv_obj_set_style_text_color(num_label, lv_color_black(), 0);
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
            lv_obj_set_style_opa(rect, is_excluded ? LV_OPA_30 : LV_OPA_COVER, 0);
            if (is_excluded) {
                lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
            }
        } else if (is_excluded) {
            lv_obj_set_style_border_color(rect, danger_color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(rect, LV_OPA_30, 0);
            lv_obj_remove_flag(rect, LV_OBJ_FLAG_CLICKABLE);
        } else if (is_current) {
            lv_obj_set_style_border_color(rect, primary_color, 0);
            lv_obj_set_style_bg_color(rect, primary_color, 0);
            lv_obj_set_style_bg_opa(rect, LV_OPA_20, 0);
            lv_obj_set_style_opa(rect, LV_OPA_COVER, 0);
            lv_obj_add_flag(rect, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_color_t color = get_object_color(i);
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
// build_key_bar (stub — full implementation in Task 6)
// ============================================================================

void ExcludeObjectMapView::build_key_bar() {
    if (!key_bar_)
        return;

    lv_obj_update_layout(key_bar_);
    helix::ui::safe_clean_children(key_bar_); // [L081] same observer path as build_object_rects

    if (!state_)
        return;

    const auto& defined = state_->get_defined_objects();
    int count = static_cast<int>(defined.size());
    if (count == 0)
        return;

    KeyBarMode mode = key_bar_mode(count);

    if (mode == KeyBarMode::Summary) {
        // Summary label
        const auto& excluded = state_->get_excluded_objects();
        int excluded_count = static_cast<int>(excluded.size());
        const std::string summary = fmt::format(
            lv_tr("Tap an object to exclude it | {} objects ({} excluded)"), count, excluded_count);
        lv_obj_t* label = lv_label_create(key_bar_);
        lv_label_set_text(label, summary.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text_muted"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
        return;
    }

    // FullNames or Abbreviated: colored dot + number + name per object
    const auto& excluded = state_->get_excluded_objects();

    for (int i = 0; i < count && i < static_cast<int>(object_rects_.size()); ++i) {
        const auto& entry = object_rects_[i];
        bool is_excluded = excluded.count(entry.name) > 0;

        // Key entry container — dim excluded objects to signal they are skipped
        lv_obj_t* entry_row = lv_obj_create(key_bar_);
        lv_obj_set_size(entry_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(entry_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(entry_row, 0, 0);
        lv_obj_set_style_pad_all(entry_row, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_flex_flow(entry_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(entry_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(entry_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(entry_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(entry_row, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (is_excluded) {
            lv_obj_set_style_opa(entry_row, LV_OPA_40, 0);
        }

        // Colored dot
        lv_color_t color = get_object_color(i);
        lv_obj_t* dot = lv_obj_create(entry_row);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Number + name label; strikethrough on excluded entries
        lv_obj_t* name_label = lv_label_create(entry_row);
        lv_obj_set_style_text_font(name_label, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_pad_left(name_label, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_remove_flag(name_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(name_label, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (is_excluded) {
            lv_obj_set_style_text_decor(name_label, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        }

        if (mode == KeyBarMode::FullNames) {
            // Show number + name, auto-truncate with LVGL dot mode
            char buf[64];
            snprintf(buf, sizeof(buf), "%d %s", i + 1, entry.name.c_str());
            lv_label_set_text(name_label, buf);
            lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
            // Limit width to share space among entries
            int max_label_w = lv_obj_get_width(key_bar_) / std::max(count, 1) - 20;
            if (max_label_w > 30) {
                lv_obj_set_width(name_label, max_label_w);
            }
        } else {
            // Abbreviated: just the number
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", i + 1);
            lv_label_set_text(name_label, buf);
        }
    }
}

// ============================================================================
// get_object_color
// ============================================================================

lv_color_t ExcludeObjectMapView::get_object_color(int index) const {
    return theme_manager_get_object_palette_color(index);
}

// ============================================================================
// Static event callbacks
// ============================================================================

void ExcludeObjectMapView::on_close_clicked(lv_event_t* /*e*/) {
    spdlog::debug("[ExcludeObjectMapView] Close button clicked");
    if (g_active_map_view && g_active_map_view->close_cb_) {
        g_active_map_view->close_cb_();
    }
}

void ExcludeObjectMapView::on_object_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectMapView*>(lv_event_get_user_data(e));
    if (!self || !self->exclude_manager_)
        return;

    lv_obj_t* target = lv_event_get_target_obj(e);

    // Find the name by matching the pointer against our recorded rects
    for (const auto& entry : self->object_rects_) {
        if (entry.rect == target) {
            spdlog::info("[ExcludeObjectMapView] Object rect clicked: '{}'", entry.name);
            self->exclude_manager_->request_exclude(entry.name);
            return;
        }
    }

    spdlog::debug("[ExcludeObjectMapView] on_object_clicked: no matching rect found");
}

} // namespace helix::ui
