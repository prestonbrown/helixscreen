// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include <functional>
#include <lvgl.h>

// Forward declaration
class AmsBackend;

namespace helix::ui {

/**
 * @file ui_ams_selector_menu.h
 * @brief Happy Hare "selector context menu" for the filament-path canvas
 *
 * Opened by tapping the selector/hub box on the filament-path canvas. Offers
 * selector-level maintenance operations: home selector, check all slots, raw
 * servo positioning (Up/Move/Down), manual selector jog (Prev/Next), a
 * gear-motor sync toggle, and recover.
 *
 * This class only *presents* the menu and reports the user's choice via a
 * callback (SelectorAction). It does NOT invoke the backend directly — the
 * canvas→panel→menu→backend dispatch wiring lives elsewhere.
 *
 * Mirrors AmsContextMenu (the per-slot menu): callbacks dispatched through
 * ContextMenu::active(), capability gating in on_created(), and ContextMenu
 * positioning.
 *
 * ## Usage:
 * @code
 * helix::ui::AmsSelectorMenu menu;
 * menu.set_action_callback([](SelectorAction action) {
 *     switch (action) {
 *         case SelectorAction::HOME: // MMU_HOME...
 *         case SelectorAction::CHECK_SLOTS: // check_all_gates()...
 *         // ...
 *     }
 * });
 * menu.show_at(parent, anchor, click_pt, backend);
 * @endcode
 */
class AmsSelectorMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(AmsSelectorMenu)

  public:
    enum class SelectorAction {
        CANCELLED,     ///< User dismissed menu without action
        HOME,          ///< Home the selector (MMU_HOME)
        CHECK_SLOTS,   ///< Check all slots (MMU_CHECK_GATE)
        SERVO_UP,      ///< Raise servo
        SERVO_MOVE,    ///< Move servo to engage/move position
        SERVO_DOWN,    ///< Lower servo
        JOG_PREV,      ///< Jog selector to previous position
        JOG_NEXT,      ///< Jog selector to next position
        GEAR_SYNC_ON,  ///< Enable gear-motor sync
        GEAR_SYNC_OFF, ///< Disable gear-motor sync
        RECOVER        ///< Recover selector/MMU state
    };

    using ActionCallback = std::function<void(SelectorAction)>;

    AmsSelectorMenu();
    ~AmsSelectorMenu() override;

    // Non-copyable
    AmsSelectorMenu(const AmsSelectorMenu&) = delete;
    AmsSelectorMenu& operator=(const AmsSelectorMenu&) = delete;

    // Movable
    AmsSelectorMenu(AmsSelectorMenu&& other) noexcept;
    AmsSelectorMenu& operator=(AmsSelectorMenu&& other) noexcept;

    /**
     * @brief Show the selector context menu near the selector/hub widget
     * @param parent Parent screen for the menu
     * @param anchor Widget to position the menu near
     * @param click_pt Display-coordinate click point (for positioning)
     * @param backend Backend pointer for capability gating (Type B / servo, etc.)
     * @return true if menu was shown successfully
     */
    bool show_at(lv_obj_t* parent, lv_obj_t* anchor, lv_point_t click_pt, AmsBackend* backend);

    /**
     * @brief Set callback for menu actions
     */
    void set_action_callback(ActionCallback callback);

  protected:
    const char* xml_component_name() const override {
        return "ams_selector_menu";
    }
    // menu_card_name() is the base's "context_menu": the card comes from the
    // shared <context_menu_card>, which names its own <view> that.
    void on_created(lv_obj_t* menu_obj) override;
    /// A tap outside a single-select action menu chooses nothing, so it reports
    /// CANCELLED through this menu's own callback rather than the base's.
    void on_backdrop_clicked() override;

  private:
    ActionCallback action_callback_;
    AmsBackend* backend_ = nullptr;

    /**
     * @brief Common pattern: hide, then invoke the callback with the action
     */
    void dispatch_selector_action(SelectorAction action);

    // === Event Handlers ===
    void handle_home();
    void handle_check_slots();
    void handle_servo_up();
    void handle_servo_move();
    void handle_servo_down();
    void handle_jog_prev();
    void handle_jog_next();
    void handle_gear_sync();
    void handle_recover();

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;

    // === Static Callbacks (instance lookup via ContextMenu::active()) ===
    /// The menu on screen as an AmsSelectorMenu, or nullptr. Thin wrapper over
    /// ContextMenu::active_as() that also logs the unexpected empty case.
    static AmsSelectorMenu* get_active_instance();
    static void on_home_cb(lv_event_t* e);
    static void on_check_cb(lv_event_t* e);
    static void on_servo_up_cb(lv_event_t* e);
    static void on_servo_move_cb(lv_event_t* e);
    static void on_servo_down_cb(lv_event_t* e);
    static void on_jog_prev_cb(lv_event_t* e);
    static void on_jog_next_cb(lv_event_t* e);
    static void on_gear_sync_cb(lv_event_t* e);
    static void on_recover_cb(lv_event_t* e);
};

} // namespace helix::ui
