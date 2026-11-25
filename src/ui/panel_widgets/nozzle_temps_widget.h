// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"

#include <cstddef>
#include <string>
#include <vector>

namespace helix {

class PrinterState;
class NozzleTempsTestAccess;
struct ToolInfo;

/// Collapse a tool list to the distinct physical extruders feeding it, in
/// physical_heads() order: first seen, or by extruder with @p tools_are_lanes,
/// so a row sits where the same head sits on every other surface. Multiplexing backends (AFC
/// BoxTurtle, Happy Hare, ERCF) expose one logical tool per spool lane that all share a single
/// extruder; a true toolchanger maps each tool to its own extruder. The nozzle-temps widget shows
/// one row per physical nozzle, so it iterates this rather than tools() directly.
/// Tools with no extruder_name are dropped.
[[nodiscard]] std::vector<std::string> distinct_extruder_names(const std::vector<ToolInfo>& tools,
                                                               bool tools_are_lanes = false);

/// Panel widget showing per-extruder temperature rows.
/// Gated on show_tool_badge (multi-tool printers only). Displays each
/// extruder's current/target temperature, plus a bed temperature row
/// at the bottom.
///
/// Each row's value and color are bound in XML to subjects this widget
/// publishes; the widget itself never writes a label. The reading observers
/// that feed them are registered on the row object, which makes them widget
/// observers: the display-sleep hold (lv_subject_hold_obj_observers()) skips
/// them like every XML binding, and LVGL deletes them with the row.
class NozzleTempsWidget : public PanelWidget {
  public:
    explicit NozzleTempsWidget(PrinterState& printer_state);
    ~NozzleTempsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "nozzle_temps";
    }

    /// A nozzle_temp_row tap; the row's index into extruder_rows_ is event user_data.
    static void extruder_row_clicked_cb(lv_event_t* e);
    /// A nozzle_temp_bed_row tap: opens the bed graph.
    static void bed_row_clicked_cb(lv_event_t* e);

  private:
    friend class NozzleTempsTestAccess;

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    struct ExtruderRow {
        std::string name;
        std::string short_name; ///< Compact label (e.g. "T0") shown in narrow layouts
        std::string long_name;  ///< Verbose label (e.g. "Nozzle 1") shown when colspan >= 2
        std::string
            number_name; ///< Bare 1-based display number (e.g. "2"), the rung below both spellings
        lv_obj_t* row_obj = nullptr;
        lv_obj_t* label_long = nullptr;
        lv_obj_t* label_short = nullptr;
        lv_obj_t* label_number = nullptr;
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* target_label = nullptr;
        // The last readings. The ladder measures the value it will render from
        // them, and a reading arrives through its own observer, so both halves
        // of the value are published from these rather than from one subject.
        int cached_temp = 0;
        int cached_target = 0;
    };

    std::vector<ExtruderRow> extruder_rows_;

    lv_obj_t* bed_row_ = nullptr;
    lv_obj_t* bed_icon_ = nullptr;
    lv_obj_t* bed_temp_label_ = nullptr;
    lv_obj_t* bed_target_label_ = nullptr;
    int cached_bed_temp_ = 0;
    int cached_bed_target_ = 0;

    ObserverGuard version_observer_;
    ObserverGuard language_observer_;
    ObserverGuard temp_unit_observer_;
    ObserverGuard tool_number_style_observer_;
    /// Rebuilds the row names when the mounted tool changes on nozzles that
    /// share one heater: the one row is named after the mounted nozzle.
    ObserverGuard mounted_tool_observer_;
    ObserverGuard tools_version_observer_;
    int named_mounted_tool_ = -1;
    int rebuild_gen_ = 0;     // Generation counter to break infinite rebuild cycles (L074)
    bool rebuilding_ = false; // Re-entrancy guard: drain() inside clear_rows() can fire
                              // version_observer_ which calls rebuild_rows() again (#723)

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer (per-row in extruder_rows_, bed observers, version_observer_)
    // destructs. Without this, queued observer callbacks captured via
    // tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void rebuild_rows();
    void clear_rows();

    /// Null every cached widget pointer (row objects/labels, bed row/labels)
    /// without touching observers or the widget tree. Shared by clear_rows()
    /// and the raw-delete hook.
    void forget_row_widgets();

    void on_hooked_root_deleted() override;

    /// Fill a row's three spellings from its extruder's name. Runs before the
    /// row joins extruder_rows_: the number rung counts the rows built so far.
    void name_row(ExtruderRow& row) const;
    /// Build the row's XML for @p row, the @p index-th of extruder_rows_.
    void create_extruder_row(lv_obj_t* container, ExtruderRow& row, size_t index);
    /// Open the nozzle graph, on extruder row @p index's tool when there are several.
    void handle_extruder_row_clicked(int index);
    void create_bed_row(lv_obj_t* container);

    /// Observe the extruder's two readings on the row's object. No-op without a
    /// row object: an observer needs one to be deleted with, and to be held.
    void observe_row_sources(ExtruderRow& row);
    /// Observe the bed's two readings on the bed row's object.
    void observe_bed_sources();
    /// Register @p cb on @p subject with the row object @p row as its target.
    void observe_reading(lv_subject_t* subject, lv_observer_cb_t cb, lv_obj_t* row);
    /// One observed reading changed on the row object @p owner.
    void on_reading(lv_obj_t* owner, bool is_target, int value);
    static void current_reading_cb(lv_observer_t* observer, lv_subject_t* subject);
    static void target_reading_cb(lv_observer_t* observer, lv_subject_t* subject);

    /// Publish a row's value and heat state onto nozzle_row_<index>_*, and the
    /// bed's onto nozzle_bed_*: the subjects the row XML binds.
    static void publish_row(const ExtruderRow& row, size_t index);
    static void publish_bed(int temp_deci, int target_deci);

    /// Re-format every row from its cached readings (language or unit switch).
    void reformat_values();
};

void register_nozzle_temps_widget();

} // namespace helix
