// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_fan_dial.h"
#include "ui_observer_guard.h"

#include "overlay_base.h"
#include "printer_state.h"
#include "ui/animated_value.h"

#include <memory>
#include <vector>

class IMoonrakerAPI;

/**
 * @file ui_fan_control_overlay.h
 * @brief Full-screen overlay for controlling all printer fans
 *
 * Displays all discovered fans with appropriate controls:
 * - Controllable fans (part fan, generic fans): FanDial widgets with arc control
 * - Auto-controlled fans (heater_fan, controller_fan): Status cards with AUTO badge
 *
 * Layout:
 * - Top section (~55%): Controllable fans with rotary dial controls
 * - Divider: "Auto-Controlled" label
 * - Bottom section: Auto fans with status display
 *
 * @see FanDial for the rotary dial widget
 * @see fan_control_overlay.xml for layout definition
 */
class FanControlOverlay : public OverlayBase {
  public:
    /**
     * @brief Construct FanControlOverlay with injected dependencies
     * @param printer_state Reference to helix::PrinterState for fan data
     */
    explicit FanControlOverlay(helix::PrinterState& printer_state);
    ~FanControlOverlay() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for XML binding
     *
     * No local subjects needed - we use PrinterState's fans_version subject.
     */
    void init_subjects() override;

    /**
     * @brief Create overlay UI from XML
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Register XML event callbacks
     *
     * Registers back button callback.
     */
    void register_callbacks() override;

    /**
     * @brief Get human-readable overlay name
     * @return "Fan Control"
     */
    [[nodiscard]] const char* get_name() const override {
        return "Fan Control";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Subscribes to fans_version subject and refreshes fan display.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is hidden
     *
     * Unsubscribes from fans_version subject.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    /**
     * @brief Set IMoonrakerAPI for sending fan commands
     * @param api Pointer to IMoonrakerAPI (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

  private:
    /**
     * @brief Populate fan widgets from helix::PrinterState
     *
     * Creates FanDial widgets for controllable fans and
     * fan_status_card components for auto-controlled fans.
     */
    void populate_fans();

    /**
     * @brief Update fan speed displays from helix::PrinterState
     *
     * Called when fans_version subject changes to refresh
     * current speed values on all fan widgets.
     */
    void update_fan_speeds();

    /**
     * @brief Send fan speed command to printer
     * @param object_name Moonraker object name (e.g., "fan", "fan_generic chamber")
     * @param speed_percent Speed 0-100%
     */
    void send_fan_speed(const std::string& object_name, int speed_percent);

    /**
     * @brief Subscribe to all per-fan speed subjects
     */
    void subscribe_to_fan_speeds();

    /**
     * @brief Unsubscribe from all per-fan speed subjects
     */
    void unsubscribe_from_fan_speeds();

    //
    // === Injected Dependencies ===
    //

    helix::PrinterState& printer_state_;
    IMoonrakerAPI* api_ = nullptr;

    //
    // === Widget References ===
    //

    lv_obj_t* fans_container_ = nullptr; ///< Single flex-wrap container for all fans

    //
    // === Animated FanDial Instances ===
    //

    /**
     * @brief Pairs a FanDial with its speed animation
     *
     * AnimatedValue observes the per-fan speed subject and smoothly animates
     * the dial when speed changes arrive from the printer. Respects the
     * animations_enabled user setting.
     */
    struct AnimatedFanDial {
        std::unique_ptr<FanDial> dial;
        std::string object_name; ///< Moonraker object name for subject lookup
        helix::ui::AnimatedValue<int> animation;
    };
    std::vector<AnimatedFanDial> animated_fan_dials_;

    //
    // === Auto Fan Card Tracking ===
    //

    struct AutoFanCard {
        std::string object_name;
        lv_obj_t* card = nullptr;
        lv_obj_t* speed_label = nullptr;
        lv_obj_t* arc = nullptr;      ///< Arc widget for live speed updates
        lv_obj_t* fan_icon = nullptr; ///< Fan icon for spin animation
        int last_speed_pct = 0;       ///< Cached speed for animation refresh
    };
    std::vector<AutoFanCard> auto_fan_cards_;

    //
    // === Observer Guards ===
    //

    ObserverGuard fans_observer_;                    ///< Structural changes (fan discovery)
    std::vector<ObserverGuard> fan_speed_observers_; ///< Per-fan speed changes
    ObserverGuard anim_settings_observer_;           ///< Animation settings changes
    bool fans_rebuild_pending_ = false; ///< Coalesces rapid fans_version observer notifications

    //
    // === Fan Icon Spin Animation ===
    //

    void update_auto_fan_animation(AutoFanCard& card, int speed_pct);
    void refresh_all_auto_fan_animations();
};

/**
 * @brief Get global FanControlOverlay instance
 * @return Reference to singleton instance
 * @throws std::runtime_error if not initialized
 */
FanControlOverlay& get_fan_control_overlay();

/**
 * @brief Initialize global FanControlOverlay instance
 * @param printer_state Reference to global helix::PrinterState
 */
void init_fan_control_overlay(helix::PrinterState& printer_state);
