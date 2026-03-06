// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

// Forward declarations
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix::ui {

/**
 * @brief Compact arc meter for clog/flowguard/buffer fault detection
 *
 * Embedded in the AMS loaded card. Driven by AmsState clog_meter_* subjects.
 * Auto-hides when mode == 0 (no clog detection backend).
 * Modes:
 *   1 = encoder (0-100 clog%), color gradient from safe to danger
 *   2 = flowguard (symmetrical, -100..+100 tangle..clog)
 *   3 = AFC buffer (0-100 fault proximity)
 */
class UiClogMeter {
  public:
    explicit UiClogMeter(lv_obj_t* parent);
    ~UiClogMeter();

    // Non-copyable, non-movable
    UiClogMeter(const UiClogMeter&) = delete;
    UiClogMeter& operator=(const UiClogMeter&) = delete;

    [[nodiscard]] lv_obj_t* get_root() const { return root_; }
    [[nodiscard]] bool is_valid() const { return root_ != nullptr; }

    void set_fill_mode(bool fill);
    void resize_arc();

  private:
    void setup_observers();
    void on_mode_changed(int mode);
    void on_value_changed(int value);
    void on_warning_changed(int warning);
    void update_arc_color();
    void update_safe_state();

    // Enhanced fill-mode widgets
    void create_enhanced_widgets();
    void update_danger_zone(int threshold);
    void update_peak_marker(int peak);
    int value_to_angle(int value) const;

    static void on_card_size_changed(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* arc_ = nullptr;
    lv_obj_t* arc_container_ = nullptr;
    int current_mode_ = 0;
    int current_value_ = 0;
    int current_warning_ = 0;
    bool in_resize_ = false;
    bool fill_mode_ = false;

    // Enhanced fill-mode widgets (created programmatically)
    lv_obj_t* danger_arc_ = nullptr;
    lv_obj_t* peak_arc_ = nullptr;
    lv_obj_t* label_left_ = nullptr;
    lv_obj_t* label_right_ = nullptr;
    lv_obj_t* center_label_ = nullptr;
    lv_obj_t* safe_icon_ = nullptr; // check_circle icon for safe state
    lv_obj_t* value_text_ = nullptr; // XML-bound clog_value_text (hidden in fill mode)

    ObserverGuard mode_obs_;
    ObserverGuard value_obs_;
    ObserverGuard warning_obs_;
    ObserverGuard danger_obs_;
    ObserverGuard peak_obs_;
    ObserverGuard center_text_obs_;
    ObserverGuard label_left_obs_;
    ObserverGuard label_right_obs_;
};

} // namespace helix::ui
