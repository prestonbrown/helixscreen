// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_widget_ref.h"

#include "lvgl.h"

#include <functional>
#include <string>

/**
 * @file ui_fan_dial.h
 * @brief Reusable fan dial widget with 270-degree rotary arc control
 *
 * Self-contained widget that can be used anywhere fan speed control is needed.
 * Uses callback-based API for loose coupling with parent components.
 *
 * Features:
 * - Bambu-style 270-degree arc dial
 * - Center percentage display
 * - Off/On toggle switch with state label
 * - Callback-based notification of speed changes
 *
 * Usage:
 *   FanDial dial(parent, "Part Fan", "fan", 50);
 *   dial.set_on_speed_changed([](const std::string& id, int pct) {
 *       // Send G-code to set fan speed
 *   });
 */
class FanDial {
  public:
    using SpeedCallback = std::function<void(const std::string& fan_id, int speed_percent)>;
    using IconClickCallback = std::function<void(const std::string& fan_id)>;

    /**
     * @brief Create a FanDial widget
     * @param parent LVGL parent object
     * @param name Display name (e.g., "Part Fan")
     * @param fan_id Identifier passed to callbacks (e.g., "fan" or "fan_generic chamber")
     * @param initial_speed Initial speed percentage (0-100)
     * @param label_bottom If true, fan name label appears below the arc instead of above
     */
    FanDial(lv_obj_t* parent, const std::string& name, const std::string& fan_id,
            int initial_speed = 0, bool label_bottom = false);
    ~FanDial();

    // Disable copy, allow move
    FanDial(const FanDial&) = delete;
    FanDial& operator=(const FanDial&) = delete;
    FanDial(FanDial&& other) noexcept;
    FanDial& operator=(FanDial&& other) noexcept;

    /**
     * @brief Set speed from external source (e.g., printer state update)
     * @param percent Speed 0-100
     */
    void set_speed(int percent);

    /**
     * @brief Get current displayed speed
     * @return Current speed percentage (0-100)
     */
    [[nodiscard]] int get_speed() const;

    /**
     * @brief Set callback for when user changes speed via dial or buttons
     * @param callback Function called with (fan_id, speed_percent)
     */
    void set_on_speed_changed(SpeedCallback callback);

    /**
     * @brief Set callback for when user clicks the fan icon
     * @param callback Function called with (fan_id)
     */
    void set_on_icon_clicked(IconClickCallback callback);

    /**
     * @brief Get the root LVGL object for this widget
     * @return Root lv_obj_t pointer
     */
    [[nodiscard]] lv_obj_t* get_root() const {
        return root_;
    }

    /**
     * @brief Get the fan identifier
     * @return Fan ID string
     */
    [[nodiscard]] const std::string& get_fan_id() const {
        return fan_id_;
    }

    /**
     * @brief Get the display name
     * @return Fan name string
     */
    [[nodiscard]] const std::string& get_name() const {
        return name_;
    }

    /**
     * @brief Refresh fan icon spin animation based on current speed and animation setting
     *
     * Call when the global animations_enabled setting changes.
     */
    void refresh_animation();

  private:
    void update_speed_label(int percent);
    void update_onoff_state(int percent);
    void animate_speed_label(int from, int to);
    void handle_arc_changed();
    void handle_arc_released();

    void handle_switch_changed();
    void handle_icon_clicked();

    void cancel_pending_send();
    void flush_pending_send();

    // Static callbacks
    static void on_arc_value_changed(lv_event_t* e);
    static void on_arc_released(lv_event_t* e);
    static void on_switch_changed(lv_event_t* e);
    static void on_icon_clicked(lv_event_t* e);
    static void label_anim_exec_cb(void* var, int32_t value);
    static void anim_completed_cb(lv_anim_t* anim);
    static void on_debounce_timer(lv_timer_t* t);

    void update_knob_glow(int percent);
    void update_fan_animation(int speed_pct);

    helix::ui::WidgetRef root_;
    helix::ui::WidgetRef arc_;
    lv_obj_t* speed_label_ = nullptr;
    helix::ui::WidgetRef fan_icon_;
    helix::ui::WidgetRef onoff_switch_;
    lv_obj_t* onoff_label_ = nullptr;

    std::string name_;
    std::string fan_id_;
    int current_speed_ = 0;
    SpeedCallback on_speed_changed_;
    IconClickCallback on_icon_clicked_;
    bool syncing_ = false;         // Prevent callback loops during set_speed()
    uint32_t last_user_input_ = 0; // Tick of last user interaction (for suppression window)

    // Debounce: coalesce mid-drag VALUE_CHANGED bursts into one Moonraker send.
    // Fires after 500 ms of slider idleness, OR immediately on touch RELEASED.
    lv_timer_t* debounce_timer_ = nullptr;
    int pending_speed_ = 0;
    bool has_pending_send_ = false;
};

/**
 * @brief Register fan dial XML event callbacks
 *
 * Must be called before creating any FanDial widgets via XML.
 * Registers: on_fan_dial_switch_changed
 */
void register_fan_dial_callbacks();
