// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"

#include <string>

namespace helix::settings {

/**
 * @class PrinterTypeOverlay
 * @brief Overlay for correcting the printer model
 *
 * Detection declines to persist a type it is not confident about, and it can
 * still land on the wrong near-neighbour when it is. Before this existed the
 * only way to change a model was to delete the printer and add it back, so
 * users either did that or lived with the wrong name (prestonbrown/helixscreen#1284).
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
    PrinterTypeOverlay();
    ~PrinterTypeOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Printer Model";
    }

    void on_activate() override;

    //
    // === UI Creation ===
    //

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    //
    // === Event Handlers (public for static callbacks) ===
    //

    void handle_type_selected(const std::string& type_name);

  private:
    static void on_type_row_clicked(lv_event_t* e);

    void populate_type_list();
    void update_selection_indicator(const std::string& active_type);

    /// Kinematics filter captured at show() time, matching the wizard's list.
    std::string kinematics_filter_;
};

/**
 * @brief Get the singleton printer type overlay instance
 */
PrinterTypeOverlay& get_printer_type_overlay();

} // namespace helix::settings
