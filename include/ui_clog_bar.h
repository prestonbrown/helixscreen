// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "clog_meter_model.h"

#include <optional>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix::ui {

/**
 * @brief Horizontal FlowGuard scale — the wide counterpart to UiClogMeter.
 *
 * Drives the measured half of clog_bar_body.xml from the same AmsState
 * `clog_meter_*` subjects the arc uses: the fill, the value marker, the
 * peak-hold tick and the danger shading, all laid out inside clog_bar_track by
 * clog_bar_geometry() (clog_meter_geometry.h). Every label on the page is bound
 * in XML, so nothing here touches text.
 *
 * Both presentations stay: the arc is what fits the AMS sidebar and the loaded
 * card, and this is what fits a 2x1 home widget (#1017).
 */
class UiClogBar {
  public:
    explicit UiClogBar(lv_obj_t* parent);
    ~UiClogBar();

    UiClogBar(const UiClogBar&) = delete;
    UiClogBar& operator=(const UiClogBar&) = delete;

    [[nodiscard]] lv_obj_t* get_root() const {
        return root_;
    }
    [[nodiscard]] bool is_valid() const {
        return root_ != nullptr;
    }

    /// Re-lay the bar against the track's current width. Called on the track's
    /// own SIZE_CHANGED and by the owning widget's on_size_changed().
    void relayout();

  private:
    static void on_track_size_changed(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* track_ = nullptr;
    lv_obj_t* fill_ = nullptr;
    lv_obj_t* marker_ = nullptr;
    lv_obj_t* peak_ = nullptr;
    lv_obj_t* danger_lo_ = nullptr;
    lv_obj_t* danger_hi_ = nullptr;
    lv_obj_t* threshold_lo_ = nullptr;
    lv_obj_t* threshold_hi_ = nullptr;

    /// Constructed last, once the named lookups above have succeeded, so its
    /// change callback never runs against a half-found widget tree.
    std::optional<ClogMeterModel> model_;
};

} // namespace helix::ui
