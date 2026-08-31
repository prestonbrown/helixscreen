// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include "spoolman_types.h"

#include <functional>
#include <lvgl.h>
#include <string>

namespace helix::ui {

/**
 * @file ui_spoolman_context_menu.h
 * @brief Context menu for Spoolman spool operations
 *
 * Displays a popup menu near a spool row with options to set active,
 * edit, print label, or delete. Extends the generic ContextMenu with
 * Spoolman-specific actions and spool info display.
 *
 * ## Usage:
 * @code
 * helix::ui::SpoolmanContextMenu menu;
 * menu.set_action_callback([](MenuAction action, int spool_id) {
 *     switch (action) {
 *         case MenuAction::SET_ACTIVE: // set active spool...
 *         case MenuAction::EDIT:       // show edit modal...
 *         case MenuAction::PRINT_LABEL: // print label...
 *         case MenuAction::DUPLICATE:  // create a copy...
 *         case MenuAction::ARCHIVE:    // archive (hide from list)...
 *         case MenuAction::DELETE:     // show delete confirmation...
 *     }
 * });
 * menu.show_for_spool(parent, spool, row_widget);
 * @endcode
 */
class SpoolmanContextMenu : public ContextMenu {
    HELIX_CONTEXT_MENU_KIND(SpoolmanContextMenu)

  public:
    enum class MenuAction {
        CANCELLED,   ///< User dismissed menu without action
        SET_ACTIVE,  ///< Set this spool as active
        EDIT,        ///< Edit spool properties
        PRINT_LABEL, ///< Print a label for this spool
        DUPLICATE,   ///< Create a fresh copy of this spool
        ARCHIVE,     ///< Archive this spool (hide it from the list)
        DELETE       ///< Delete this spool
    };

    using ActionCallback = std::function<void(MenuAction action, int spool_id)>;

    SpoolmanContextMenu();
    ~SpoolmanContextMenu() override;

    // Non-copyable
    SpoolmanContextMenu(const SpoolmanContextMenu&) = delete;
    SpoolmanContextMenu& operator=(const SpoolmanContextMenu&) = delete;

    // Movable
    SpoolmanContextMenu(SpoolmanContextMenu&& other) noexcept;
    SpoolmanContextMenu& operator=(SpoolmanContextMenu&& other) noexcept;

    /**
     * @brief Show context menu near a spool row widget
     * @param parent Parent screen for the menu
     * @param spool Spool info for display and action context
     * @param near_widget Widget to position menu near (typically spool row)
     * @param allow_archive Offer the Archive action. The panel passes false for
     *        the currently active spool — archiving it makes no sense while it
     *        feeds the printer.
     * @return true if menu was shown successfully
     */
    bool show_for_spool(lv_obj_t* parent, const SpoolInfo& spool, lv_obj_t* near_widget,
                        bool allow_archive = true);

    /**
     * @brief Get spool ID the menu is currently shown for
     */
    [[nodiscard]] int get_spool_id() const {
        return get_item_index();
    }

    /**
     * @brief Set callback for menu actions
     */
    void set_action_callback(ActionCallback callback);

  protected:
    const char* xml_component_name() const override {
        return "spoolman_context_menu";
    }
    void on_created(lv_obj_t* menu_obj) override;
    /// A tap outside a single-select action menu chooses nothing, so it reports
    /// CANCELLED through this menu's own callback rather than the base's.
    void on_backdrop_clicked() override;

  private:
    // === Spoolman-specific state ===
    ActionCallback action_callback_;
    SpoolInfo pending_spool_;     ///< Spool info stored between show and on_created
    bool archive_allowed_ = true; ///< show_for_spool's allow_archive, consumed in on_created

    /**
     * @brief Common dispatch: hide, then invoke the callback with the spool id
     */
    void dispatch_spoolman_action(MenuAction action);

    // === Event Handlers ===
    void handle_set_active();
    void handle_edit();
    void handle_print_label();
    void handle_duplicate();
    void handle_archive();
    void handle_delete();

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;

    // === Static Callbacks ===
    /// The menu on screen as a SpoolmanContextMenu, or nullptr. Thin wrapper over
    /// ContextMenu::active_as() that also logs the unexpected empty case.
    static SpoolmanContextMenu* get_active_instance();
    static void on_set_active_cb(lv_event_t* e);
    static void on_edit_cb(lv_event_t* e);
    static void on_print_label_cb(lv_event_t* e);
    static void on_duplicate_cb(lv_event_t* e);
    static void on_archive_cb(lv_event_t* e);
    static void on_delete_cb(lv_event_t* e);
};

} // namespace helix::ui
