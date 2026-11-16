// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include "bed_drying.h"
#include "helix_shared_callback.h"

#include <functional>
#include <string>

namespace helix {
class BedDryingController;
}

namespace helix::ui {

/// Starts drying filament on the heated bed (prestonbrown/helixscreen#1730):
/// a material from the heated-bed table, capped for this printer, an
/// optional chamber dryer alongside, and an acknowledgement before Start. The
/// rest of the flow (unload offer, clearance move, place prompt) follows from
/// on_ok() through BedDryingController.
class BedDryingModal : public Modal {
  public:
    const char* get_name() const override {
        return "Bed Drying";
    }
    const char* component_name() const override {
        return "bed_drying_modal";
    }

    /// One-shot owned show over the active screen. False without a controller.
    static bool show_owned();

    /// "PLA  70°C  12h": the material as this printer will run it.
    static std::string material_label(const bed_drying::Material& m, int bed_c);

  protected:
    void on_show() override;
    void on_ok() override;
};

/// Unload through the shared filament ladder, then @p then once the toolhead
/// is clear. A failed unload, or a filament-system one that never starts, stops
/// the flow; "nothing loaded" goes straight on.
void unload_before_drying(BedDryingController& ctrl, helix::SharedCallback<void()> then);

/// The unload offer, then the clearance move, then the place prompt.
void start_bed_drying_flow(const bed_drying::Material& material, bool with_appliance);

/// What tapping the banner does in each run state: stop, remove early, remove.
void on_bed_drying_banner_clicked();

/// The cooled-bed "remove the spools" prompt; wired to the controller at startup.
void show_bed_drying_remove_prompt();

/// The alarm for a print taking hold of the machine while spools are latched
/// on the bed; wired to the controller at startup.
void show_spools_on_bed_print_alarm();

/// Registers the XML callbacks the banner and the entry rows use.
void register_bed_drying_callbacks();

} // namespace helix::ui
