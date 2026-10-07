// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "clog_meter_model.h"

#include <optional>

// Forward declarations
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
 *   3 = AFC buffer (0-100 fault proximity)
 *
 * The home widget draws the same subjects as a horizontal scale instead — see
 * UiClogBar (ui_clog_bar.h). This class had a second "fill mode" presentation
 * built for that widget, with a danger-zone arc, a peak-hold marker and
 * endpoint labels; the bar replaced it (#1017) and it went with it. The
 * readouts it drew are not lost — clog_bar_body.xml binds the labels directly,
 * and clog_bar_geometry() places the danger band and peak tick.
 */
class UiClogMeter {
  public:
    explicit UiClogMeter(lv_obj_t* parent);
    ~UiClogMeter();

    // Non-copyable, non-movable
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
    /// Redraw the arc for one sample: range and mode, sweep, colour, and the
    /// check icon that stands in when there is nothing to report.
    void apply(const ClogMeterSample& s);
    void update_arc_color(const ClogMeterSample& s);
    void update_safe_state(const ClogMeterSample& s);

    static void on_card_size_changed(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* arc_ = nullptr;
    lv_obj_t* arc_container_ = nullptr;
    bool in_resize_ = false;

    lv_obj_t* safe_icon_ = nullptr;  ///< check_circle shown when there is nothing to report
    lv_obj_t* value_text_ = nullptr; ///< XML-bound clog_value_text, hidden in the safe state

    /// Constructed last, once the named lookups above have succeeded.
    std::optional<ClogMeterModel> model_;
};

} // namespace helix::ui
