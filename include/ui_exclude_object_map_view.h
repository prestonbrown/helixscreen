#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"
#include "ui_widget_ref.h"

#include "bed_coord_mapper.h"
#include "bed_dimensions.h"
#include "core/observer_guard.h"
#include "gcode_parser.h"
#include "instance_subjects.h"

#include <cmath>
#include <functional>
#include <glm/vec2.hpp>
#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ExcludeObjectMapViewTestAccess; // NAMESPACE_OK: test seam befriended below, defined in the
                                      // unit test

namespace helix {
class PrinterExcludedObjectsState;
}

namespace helix::ui {

/// Interactive covers a preview card with numbered, tappable objects and a close button.
/// Mini takes a normal slot in a tile and draws outlines alone: the object being printed
/// bright, excluded objects faded and hatched, nothing to tap.
enum class ExcludeMapMode { Interactive, Mini };

class ExcludeObjectMapView {
  public:
    // The value lives on helix::BedCoordMapper; aliased here for code that
    // names ExcludeObjectMapView::kMinTouchTargetPx.
    static constexpr float kMinTouchTargetPx = helix::BedCoordMapper::kMinTouchTargetPx;

    using PixelRect = helix::PixelRect;
    using CoordMapper = helix::BedCoordMapper;

    ExcludeObjectMapView();
    ~ExcludeObjectMapView();

    ExcludeObjectMapView(const ExcludeObjectMapView&) = delete;
    ExcludeObjectMapView& operator=(const ExcludeObjectMapView&) = delete;

    void create(lv_obj_t* parent, helix::PrinterExcludedObjectsState* state,
                const helix::BedDimensions& bed, ObjectTapFn on_object_tapped,
                ExcludeTapMode tap_mode, const helix::gcode::ParsedGCodeFile* parsed_file = nullptr,
                ExcludeMapMode mode = ExcludeMapMode::Interactive);
    void destroy();

    /// Rebuild the drawing surface when the plate area's size has changed since it was
    /// last built, and leave it alone otherwise. A host whose slot is resized calls this.
    void resize();

    [[nodiscard]] lv_obj_t* root() const {
        return root_;
    }
    [[nodiscard]] bool is_active() const {
        return root_ != nullptr;
    }

    void set_close_callback(std::function<void()> cb) {
        close_cb_ = std::move(cb);
    }

  private:
    friend class ::ExcludeObjectMapViewTestAccess;

    void build_surface();
    /// Publish this view's subjects under fresh names, for the interactive map.
    void publish_view_subjects();
    void build_object_rects();
    void update_visual_states();
    void draw_first_layer_outlines();
    /// The bed's 50mm grid and edge, whatever of them falls in view.
    void draw_bed_reference(lv_layer_t* layer) const;
    void copy_parsed_geometry(const helix::gcode::ParsedGCodeFile* parsed);
    lv_obj_t* create_object_rect(lv_obj_t* parent, const ObjectBadge& badge, const PixelRect& rect);

    static void on_close_clicked(lv_event_t* e);
    static void on_object_clicked(lv_event_t* e);

    // Null once LVGL deletes the widget, so a tree deleted under the view is
    // never touched again.
    WidgetRef root_;
    WidgetRef plate_area_;
    WidgetRef object_container_;
    WidgetRef canvas_;
    lv_draw_buf_t* canvas_buf_{nullptr};

    helix::PrinterExcludedObjectsState* state_{nullptr};
    ObjectTapFn on_object_tapped_;
    ExcludeTapMode tap_mode_{ExcludeTapMode::ExcludeOnly};
    // Copied from the parse at create(): its owner can free it while the map is
    // open. parsed_objects_ holds only the objects, and is null with no parse;
    // parsed_outlines_ holds each object's first-layer hull.
    std::unique_ptr<helix::gcode::ParsedGCodeFile> parsed_objects_;
    std::unordered_map<std::string, std::vector<glm::vec2>> parsed_outlines_;

    ExcludeMapMode mode_{ExcludeMapMode::Interactive};

    // The interactive map's plate size line and empty-state flag. Published per
    // view under unique names: the print status overlay and a home tile can each
    // hold a map at once.
    lv_subject_t dims_text_{};
    char dims_buf_[32] = {};
    lv_subject_t has_objects_{};
    helix::InstanceSubjects view_subjects_{"exclude_map"}; ///< After the subjects it publishes

    // The printer's bed, and the extent the map shows (the objects plus a
    // margin, else the bed) with its lower-left corner.
    helix::BedDimensions bed_{helix::kDefaultBedSizeMm, helix::kDefaultBedSizeMm, 0.0f, 0.0f};
    float bed_w_mm_{235.0f};
    float bed_h_mm_{235.0f};
    float coord_min_x_{0.0f};
    float coord_min_y_{0.0f};
    // The plate size the surface was built for.
    int built_w_{0};
    int built_h_{0};

    std::unique_ptr<CoordMapper> mapper_;
    ObserverGuard excluded_version_obs_;
    ObserverGuard defined_version_obs_;

    std::function<void()> close_cb_;

    struct ObjectRect {
        std::string name;
        int defined_index{-1}; ///< Keys number and colour; rects skip bbox-less objects
        lv_obj_t* rect{nullptr};
    };
    std::vector<ObjectRect> object_rects_;
};

} // namespace helix::ui
