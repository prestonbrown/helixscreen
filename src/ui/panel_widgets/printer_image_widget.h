// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_icon_binder.h"

#include "callout_layout.h"
#include "callout_presenter.h"
#include "core/observer_guard.h"
#include "exact_size_image.h"
#include "panel_widget.h"

#include <array>
#include <string>
#include <vector>

namespace helix {

namespace ui {

/// What the printer picture was last drawn from: the source, its file's
/// mtime and size, and the content size of the area it is laid out in.
struct PrinterImageStamp {
    std::string source;      ///< resolved LVGL path
    std::string fingerprint; ///< printer_image_source_fingerprint() of the source
    int area_w = 0;
    int area_h = 0;
};

/// How a refresh differs from what is on screen. Only NewPixels makes the
/// decoded source stale; any change moves the callouts.
enum class PrinterImageChange {
    None,     ///< same picture, same area: nothing to redo
    Resized,  ///< same picture, another area: relayout
    NewPixels ///< another source, or the same path rewritten: reload and relayout
};

/// Pure: compares what is shown with what a refresh would show. Nothing shown
/// yet (an empty source) is NewPixels.
PrinterImageChange classify_printer_image_change(const PrinterImageStamp& shown,
                                                 const PrinterImageStamp& next);

} // namespace ui

class PrinterImageWidget : public PanelWidget {
  public:
    PrinterImageWidget();
    ~PrinterImageWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    /// Records the granted cell span (a single cell draws no callouts) and
    /// schedules a relayout of the chips.
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    /// Factory-registration key. Exposed so callers scanning a heterogeneous
    /// widget list can match on id() and static_cast, instead of dynamic_cast —
    /// the firmware builds -fno-rtti.
    static constexpr const char* kWidgetId = "printer_image";

    const char* id() const override {
        return kWidgetId;
    }

    /// Called when panel activates — re-check if printer image changed in settings
    void on_activate();

    /// Reload printer image and printer info subjects from config
    void reload_from_config();

    /// The picture may have changed on disk: re-resolve it and reload it even
    /// under the same path and size.
    void refresh_printer_image();

    /// XML event callback — opens printer manager overlay
    static void printer_manager_clicked_cb(lv_event_t* e);

    /// XML event callbacks for the live callout chips — one per heater/fan/light.
    /// The toolhead chip (merged nozzle+fan) reuses printer_callout_nozzle_cb.
    static void printer_callout_nozzle_cb(lv_event_t* e);
    static void printer_callout_bed_cb(lv_event_t* e);
    static void printer_callout_chamber_cb(lv_event_t* e);
    static void printer_callout_fan_cb(lv_event_t* e);
    static void printer_callout_light_cb(lv_event_t* e);
    /// Long-press on the light chip: opens the LEDs overlay on the chamber light.
    static void printer_callout_light_hold_cb(lv_event_t* e);

  protected:
    void on_hooked_root_deleted() override;

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Persistent disk cache for exact-size printer image
    lv_timer_t* cache_timer_ = nullptr;
    // Defers the image refresh out of the synchronous attach()/on_activate()
    // path: lv_image_set_inner_align forces lv_obj_update_layout, which cascades
    // into the parent grid's grid_update. Running that during a panel rebuild
    // (mid-rebuild grid) walked the freed descriptor off the heap end (#983/#1025).
    lv_timer_t* refresh_timer_ = nullptr;
    std::string current_source_path_; // Resolved source image (LVGL path)
    /// What the last apply drew from. Kept across a rebuild, so a recycled
    /// instance knows whether its new tree shows the same picture.
    helix::ui::PrinterImageStamp shown_stamp_;
    /// Set by attach(): the new tree's chips are unplaced until the next apply.
    bool callouts_unplaced_ = false;
    /// Natural size of the source, read once per path: every relayout needs it
    /// (an untagged image's aspect, a user tag's size guard), and a decoder
    /// info call opens the file.
    std::string natural_size_path_;
    int natural_w_ = 0;
    int natural_h_ = 0;

    /// The 1:1 cached copy of the picture for its laid-out size.
    helix::ui::ExactSizeImage exact_image_{
        "PrinterImageWidget",
        [this]() -> lv_obj_t* {
            return widget_obj_ ? lv_obj_find_by_name(widget_obj_, "printer_image") : nullptr;
        },
        [](lv_obj_t* img, const std::string& path) { lv_image_set_src(img, path.c_str()); }};

    /// Re-resolves the image when the printer type settles mid-session:
    /// auto-detection finishes after the home panel is built on a fresh
    /// install, so attach()'s one-shot resolve still shows the generic
    /// silhouette unless a type change re-triggers it.
    ObserverGuard printer_type_observer_;

    /// Re-resolve when the detected filament systems change: a database entry's
    /// `image_variants` can pick another image once a backend registers, which
    /// happens after connect.
    std::vector<ObserverGuard> hardware_observers_;
    void refresh_if_detected_image_changed();

    /// Re-resolves the picture and redoes what classify_printer_image_change()
    /// says changed since the last apply; `force` treats it as new pixels.
    void apply_printer_image(bool force);
    void schedule_image_refresh();
    void schedule_cache_check();
    void check_or_generate_cache();

    void handle_printer_manager_clicked();

    /// Live callouts: PrinterState -> chip subjects -> measured layout.
    void arm_callout_observers();
    void update_callouts();         ///< subjects -> chip text/shown; relayout on change
    void schedule_callout_layout(); ///< one-shot deferred apply_callout_layout()
    void cancel_callout_timer();
    void apply_callout_layout(); ///< measure, decide, position
    /// Moves and sizes printer_image to `moved`, or back to filling its
    /// container when null; refreshes the image only when the rect changes.
    void place_printer_image(const CalloutRect* moved);
    void set_glow_pulse(bool on); ///< start/stop the bed glow's opacity pulse
    void handle_callout_clicked(CalloutKind kind);
    /// Recovers the widget from a chip's event (chip -> callout_layer -> printer_container).
    static PrinterImageWidget* widget_from_chip(lv_event_t* e);
    static void route_callout_click(lv_event_t* e, CalloutKind kind);

    lv_timer_t* callout_timer_ = nullptr;
    // Own copies of the granted span: on_size_changed() may be reached directly,
    // which leaves the base class's recorded size at zero.
    int callout_colspan_ = 0;
    int callout_rowspan_ = 0;
    int callout_spin_pct_ = -1; ///< speed the fan icons spin at; -1 = not yet applied
    helix::ui::CalloutPresenter presenter_;
    std::vector<ObserverGuard> callout_observers_;
    /// Each text chip's full text, indexed by CalloutKind. Its subject carries the
    /// compact form while the chips are pinned; measuring reads this.
    std::array<std::string, 6> callout_full_text_{};
    SubjectLifetime bed_temp_lt_, bed_target_lt_, chamber_temp_lt_, chamber_target_lt_;
    helix::ui::HeaterIconBinder nozzle_binder_, bed_binder_, chamber_binder_;
};

} // namespace helix
