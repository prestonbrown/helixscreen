// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

#include <string>

namespace helix::settings {

/**
 * @class PrinterTypeOverlay
 * @brief Overlay for correcting the printer model
 *
 * Detection declines to persist a type it is not confident about, and it can
 * still land on the wrong near-neighbour when it is. This is the in-place
 * correction, so a wrong model never forces deleting and re-adding the
 * printer (prestonbrown/helixscreen#1284).
 *
 * Picking a model here goes through PrinterDetector::apply_type_choice(), the
 * same call the wizard's identify step makes, so the preset merge behaves
 * identically on both paths.
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_printer_type_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class PrinterTypeOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Printer Model";
    }
    const char* xml_component() const override {
        return "printer_type_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

    //
    // === Event Handlers (public for the callback table) ===
    //

    void handle_type_selected(const std::string& type_name);

  protected:
    void before_show() override;

  private:
    void populate_type_list();
    void update_selection_indicator(const std::string& active_type);

    /// Kinematics filter captured at show() time, matching the wizard's list.
    std::string kinematics_filter_;
};

inline PrinterTypeOverlay& get_printer_type_overlay() {
    return lazy_global<PrinterTypeOverlay>("PrinterTypeOverlay");
}

} // namespace helix::settings
