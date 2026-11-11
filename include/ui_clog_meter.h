// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "clog_meter_model.h"

#include <optional>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix::ui {

/**
 * @brief Compact arc meter for clog/flowguard/buffer fault detection
 *
 * Embedded in the AMS loaded card and the AMS sidebar. Driven by AmsState's
 * clog_meter_* subjects. Auto-hides when mode == 0 (no clog detection backend).
 * Modes:
 *   1 = encoder (0-100 clog%), color gradient from safe to danger
 *   2 = flowguard (symmetrical, -100..+100 tangle..clog)
 *   3 = buffer fault countdown (0-100 fault proximity)
 *
 * The home widget draws the same subjects as a horizontal scale: see UiClogBar
 * (ui_clog_bar.h), whose labels clog_bar_body.xml binds directly and whose
 * danger band and peak tick clog_bar_geometry() places (#1017).
 */
class UiClogMeter {
  public:
    explicit UiClogMeter(lv_obj_t* parent);
    ~UiClogMeter();

    UiClogMeter(const UiClogMeter&) = delete;
    UiClogMeter& operator=(const UiClogMeter&) = delete;

    [[nodiscard]] lv_obj_t* get_root() const {
        return root_;
    }
    [[nodiscard]] bool is_valid() const {
        return root_ != nullptr;
    }

    void resize_arc();

  private:
    /// Redraw the arc for one sample: range and mode, sweep and colour.
    void apply(const ClogMeterSample& s);
    void update_arc_color(const ClogMeterSample& s);

    static void on_card_size_changed(lv_event_t* e);

    helix::ui::WidgetRef root_;
    lv_obj_t* arc_ = nullptr;
    lv_obj_t* arc_container_ = nullptr;
    bool in_resize_ = false;

    /// Constructed last, once the named lookups above have succeeded.
    std::optional<ClogMeterModel> model_;
};

} // namespace helix::ui
