// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_context_menu.h"

#include <functional>
#include <string>

namespace helix::ui {

/**
 * @brief Context menu for switching between configured printers
 *
 * Shows a popup near the navbar printer badge listing all configured printers
 * with a checkmark on the active one, plus an "Add Printer" button that
 * launches the setup wizard.
 */
class PrinterSwitchMenu : public ContextMenu {
  public:
    enum class MenuAction {
        SWITCH,
        ADD_PRINTER,
        CANCELLED,
    };

    using SwitchCallback = std::function<void(MenuAction action, const std::string& printer_id)>;

    PrinterSwitchMenu() = default;

    void show(lv_obj_t* parent, lv_obj_t* near_widget);

    void set_switch_callback(SwitchCallback callback) { switch_callback_ = std::move(callback); }

    static void register_callbacks();

  protected:
    const char* xml_component_name() const override { return "printer_switch_menu"; }
    void on_created(lv_obj_t* menu) override;
    void on_backdrop_clicked() override;

  private:
    void populate_printer_list();
    void handle_printer_selected(const std::string& printer_id);
    void handle_add_printer();
    void dispatch_switch_action(MenuAction action, const std::string& printer_id = "");

    SwitchCallback switch_callback_;

    static PrinterSwitchMenu* s_active_instance_;
    static bool s_callbacks_registered_;

    static PrinterSwitchMenu* get_active_instance();
    static void on_backdrop_cb(lv_event_t* e);
    static void on_add_printer_cb(lv_event_t* e);
    static void on_printer_row_cb(lv_event_t* e);
};

}  // namespace helix::ui
