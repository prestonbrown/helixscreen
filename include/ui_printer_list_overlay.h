// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "overlay_base.h"
#include "static_panel_registry.h"

#include <string>

namespace helix::ui {

class PrinterListOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Printer List";
    }
    const char* xml_component() const override {
        return "printer_list_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

    void handle_add_printer();
    void handle_switch_printer(const std::string& printer_id);
    void handle_delete_printer(const std::string& printer_id);

  private:
    void populate_printer_list();

    /// The active row's connection dot, colored by printer_connection_state.
    WidgetRef active_dot_;
    ObserverGuard active_dot_observer_;
};

inline PrinterListOverlay& get_printer_list_overlay() {
    return lazy_global<PrinterListOverlay>("PrinterListOverlay");
}

} // namespace helix::ui
