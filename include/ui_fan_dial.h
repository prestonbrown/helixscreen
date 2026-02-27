// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
 * - Off/On quick-set buttons
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
     */
    FanDial(lv_obj_t* parent, const std::string& name, const std::string& fan_id,
            int initial_speed = 0);
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
     * @brief Make the dial read-only (for auto-controlled fans)
     *
     * Disables arc interaction, hides the knob, changes indicator to muted color,
     * and replaces Off/On buttons with an "Auto" label.
     */
    void set_read_only(bool read_only);

    /**
     * @brief Refresh fan icon spin animation based on current speed and animation setting
     *
     * Call when the global animations_enabled setting changes.
     */
    void refresh_animation();

  private:
    void update_speed_label(int percent);
    void update_button_states(int percent);
    void animate_speed_label(int from, int to);
    void handle_arc_changed();

    void handle_off_clicked();
    void handle_on_clicked();
    void handle_icon_clicked();

    // Static callbacks
    static void on_arc_value_changed(lv_event_t* e);
    static void on_off_clicked(lv_event_t* e);
    static void on_on_clicked(lv_event_t* e);
    static void on_icon_clicked(lv_event_t* e);
    static void label_anim_exec_cb(void* var, int32_t value);
    static void anim_completed_cb(lv_anim_t* anim);

    void update_knob_glow(int percent);
    void update_fan_animation(int speed_pct);

    // Fan icon spin animation (matches FanStackWidget pattern)
    static void spin_anim_cb(void* var, int32_t value);
    static void start_spin(lv_obj_t* icon, int speed_pct);
    static void stop_spin(lv_obj_t* icon);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* arc_ = nullptr;
    lv_obj_t* speed_label_ = nullptr;
    lv_obj_t* fan_icon_ = nullptr;
    lv_obj_t* btn_off_ = nullptr;
    lv_obj_t* btn_on_ = nullptr;

    std::string name_;
    std::string fan_id_;
    int current_speed_ = 0;
    SpeedCallback on_speed_changed_;
    IconClickCallback on_icon_clicked_;
    bool syncing_ = false;         // Prevent callback loops during set_speed()
    uint32_t last_user_input_ = 0; // Tick of last user interaction (for suppression window)
};

/**
 * @brief Register fan dial XML event callbacks
 *
 * Must be called before creating any FanDial widgets via XML.
 * Registers: on_fan_dial_value_changed, on_fan_dial_off_clicked, on_fan_dial_on_clicked
 */
void register_fan_dial_callbacks();
