#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"
#include "ui_exclude_object_map_view.h"
#include "ui_exclude_object_side_list.h"
#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include <lvgl.h>
#include <memory>

namespace helix {
class PrinterExcludedObjectsState;
}

namespace helix::ui {

/// Where a host lets exclude mode draw.
struct ExcludeModeTargets {
    lv_obj_t* card = nullptr;            ///< Preview card: the map covers it in thumbnail mode; the
                                         ///< portrait list stops below it
    lv_obj_t* columns = nullptr;         ///< Row the object list floats over
    const char* controls_name = nullptr; ///< Child of columns the list covers in portrait
    lv_obj_t* gcode_viewer = nullptr;    ///< May be null
    lv_subject_t* map_active = nullptr;  ///< 1 while the map covers the card; may be null
    bool thumbnail_mode = true;          ///< Map + list; otherwise render badges + list
    float bed_w_mm = 0.0f;
    float bed_h_mm = 0.0f;
};

/// Exclude mode over a preview: the object list, plus the top-down map in
/// thumbnail mode or numbered badges on the 2D/3D render. Print status and
/// print details each own one. Every tap on an object, from the list, the map
/// or the render, reaches the host's callback.
class ExcludeModeController {
  public:
    ExcludeModeController() = default;
    ~ExcludeModeController();
    ExcludeModeController(const ExcludeModeController&) = delete;
    ExcludeModeController& operator=(const ExcludeModeController&) = delete;

    void show(const ExcludeModeTargets& targets, PrinterExcludedObjectsState* state,
              ExcludeTapMode mode, ObjectTapFn on_tap);
    void hide();
    [[nodiscard]] bool is_open() const {
        return side_list_ && side_list_->is_active();
    }
    /// Publish the render badges again, e.g. after the viewer parsed a new file.
    void refresh_render_badges();

  private:
    static void on_viewer_tap(lv_obj_t* viewer, const char* name, void* user_data);

    std::unique_ptr<ExcludeObjectMapView> map_view_;
    std::unique_ptr<ExcludeObjectSideList> side_list_;
    PrinterExcludedObjectsState* state_ = nullptr;
    WidgetRef viewer_; ///< Null once LVGL deletes the viewer
    lv_subject_t* map_active_ = nullptr;
    ObjectTapFn on_tap_;
    ObserverGuard excluded_obs_;
    ObserverGuard defined_obs_;
};

} // namespace helix::ui
