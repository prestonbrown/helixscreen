#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "print_status_layout_decision.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class PrinterState;
class PrinterExcludedObjectsState;
} // namespace helix

namespace helix::ui {

class PrintExcludeObjectManager;

/// Side-panel companion to ExcludeObjectMapView. Slides in from the right edge
/// of the print-status thumbnail card; the map shrinks to share horizontal
/// space. Rows mirror the map's numbered badges + colors so the spatial layout
/// and tappable list stay visually linked.
class ExcludeObjectSideList {
  public:
    ExcludeObjectSideList();
    ~ExcludeObjectSideList();

    ExcludeObjectSideList(const ExcludeObjectSideList&) = delete;
    ExcludeObjectSideList& operator=(const ExcludeObjectSideList&) = delete;

    /// Create the panel as a floating child of `parent` (overlay_content).
    /// `geom` says which edge to cover and how much of it — the controls are the
    /// right-hand column in landscape and the bottom of the stack in portrait,
    /// so the list anchors and slides in along the matching axis.
    void create(lv_obj_t* parent, PrinterState* printer_state, PrintExcludeObjectManager* manager,
                SideListGeometry geom);

    /// Animate out and destroy.
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

    /// Link a gcode viewer so row taps highlight the matching object inside
    /// it (spatial feedback for which object you're about to exclude). May be
    /// nullptr (thumbnail mode — viewer is hidden anyway).
    void set_gcode_viewer(lv_obj_t* viewer) {
        gcode_viewer_ = viewer;
    }

  private:
    enum class RowState { Unset, Idle, Printing, Excluded };

    /// The parts of a row that change with its state.
    struct Row {
        lv_obj_t* row;
        lv_obj_t* name_label;
        lv_obj_t* status_label;
        RowState state;
    };

    /// Recreate the rows when the defined object names differ from the ones shown.
    void rebuild_rows();
    /// Restyle the existing rows in place, so the list keeps its scroll position.
    void update_row_states();
    Row create_row(lv_obj_t* parent, int index, const std::string& name);
    static void apply_row_state(const Row& row);
    static lv_color_t color_for_index(int index);
    static void on_row_clicked(lv_event_t* e);
    static void on_close_clicked(lv_event_t* e);

    lv_obj_t* root_{nullptr};
    lv_obj_t* rows_container_{nullptr};
    lv_obj_t* empty_state_{nullptr};
    lv_obj_t* gcode_viewer_{nullptr};

    PrinterState* printer_state_{nullptr};
    PrintExcludeObjectManager* manager_{nullptr};

    /// Names the rows were built from, in row order; rows_[i] shows row_names_[i].
    std::vector<std::string> row_names_;
    std::vector<Row> rows_;

    ObserverGuard excluded_version_obs_;
    ObserverGuard defined_version_obs_;

    std::function<void()> close_cb_;

    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
