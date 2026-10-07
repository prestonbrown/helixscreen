#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"
#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "bed_coord_mapper.h"
#include "gcode_parser.h"

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

// Forward declarations
namespace helix {
class PrinterExcludedObjectsState;
}

namespace helix::ui {

class ExcludeObjectMapView {
  public:
    // Canonical value lives on helix::BedCoordMapper; aliased here so existing
    // ExcludeObjectMapView::MIN_TOUCH_TARGET_PX references keep compiling.
    static constexpr float MIN_TOUCH_TARGET_PX = helix::BedCoordMapper::MIN_TOUCH_TARGET_PX;

    using PixelRect = helix::PixelRect;
    using CoordMapper = helix::BedCoordMapper;

    ExcludeObjectMapView();
    ~ExcludeObjectMapView();

    // Non-copyable
    ExcludeObjectMapView(const ExcludeObjectMapView&) = delete;
    ExcludeObjectMapView& operator=(const ExcludeObjectMapView&) = delete;

    void create(lv_obj_t* parent, helix::PrinterExcludedObjectsState* state, float bed_w_mm,
                float bed_h_mm, ObjectTapFn on_object_tapped, ExcludeTapMode tap_mode,
                const helix::gcode::ParsedGCodeFile* parsed_file = nullptr);
    void destroy();

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

    void build_object_rects();
    void update_visual_states();
    void draw_first_layer_outlines();
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

    float bed_w_mm_{235.0f};
    float bed_h_mm_{235.0f};

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
