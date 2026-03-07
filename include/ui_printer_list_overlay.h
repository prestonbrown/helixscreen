// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"

#include <string>

namespace helix::ui {

class PrinterListOverlay : public OverlayBase {
  public:
    PrinterListOverlay() = default;
    ~PrinterListOverlay() override = default;

    void init_subjects() override {}
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivate() override;

    const char* get_name() const override { return "Printer List"; }

    void show(lv_obj_t* parent_screen);

    void handle_add_printer();
    void handle_switch_printer(const std::string& printer_id);
    void handle_delete_printer(const std::string& printer_id);

  private:
    void populate_printer_list();

    static bool s_callbacks_registered_;
    static std::string s_pending_delete_id_;  // Printer ID pending delete confirmation

    static void on_add_printer_cb(lv_event_t* e);
    static void on_printer_row_cb(lv_event_t* e);
    static void on_delete_printer_cb(lv_event_t* e);
    static void on_delete_confirm_cb(lv_event_t* e);
    static void on_delete_cancel_cb(lv_event_t* e);
};

PrinterListOverlay& get_printer_list_overlay();

}  // namespace helix::ui
