// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "belt_cutter_calibration.h"

namespace helix {
class IMoonrakerAPI;
class PrinterDiscovery;
} // namespace helix

namespace helix::ui {

/// Confirms a motorized belt tensioning run and lets the user pick which belts
/// it covers (helix::belt_cutter_cal).
class BeltTensionAutoModal : public Modal {
  public:
    const char* get_name() const override {
        return "Auto Belt Tension";
    }
    const char* component_name() const override {
        return "belt_tension_auto_modal";
    }

    /// One-shot owned show over the active screen. False if the XML failed.
    static bool show_owned();

  protected:
    void on_show() override;
    void on_ok() override;
};

/// Registers the Calibration rows' "on_belt_tension_auto_clicked" and
/// "on_cutter_calibration_clicked" callbacks.
void init_belt_cutter_cal_row_handlers();

/// Send the printer's belt tensioning for @p axes. Refused while a print holds
/// the machine; a printer without motorized tensioners sends nothing.
void start_belt_tension(IMoonrakerAPI* api, const PrinterDiscovery& hw,
                        belt_cutter_cal::BeltAxes axes);

/// Send the printer's cutter calibration. Refused while a print holds the
/// machine; a printer without one sends nothing.
void start_cutter_calibration(IMoonrakerAPI* api, const PrinterDiscovery& hw);

} // namespace helix::ui
