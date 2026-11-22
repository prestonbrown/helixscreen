// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fills the calibration catalog's inputs from the live printer.

#include "calibration_catalog.h"

#include <string>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::calibration {

/// The catalog inputs for the connected printer: its capabilities, its Z offset
/// strategy, its probe and its database tiers. Main thread only.
[[nodiscard]] CatalogInputs catalog_inputs_from(const PrinterState& state);

/// One heater of the connected printer, priced for a calibration at @p target_c
/// from the shared thermal model and what the printer runs. Main thread only.
[[nodiscard]] HeaterPricing price_heater(PrinterState& state, const std::string& heater,
                                         int target_c);

/// The heaters the Heaters step calibrates, in its order: the active extruder,
/// then the bed where there is one. Main thread only.
[[nodiscard]] std::vector<HeaterPricing> heater_pricing(PrinterState& state);

/// applicable_steps() for the connected printer, the Heaters step priced by
/// heaters_step_minutes(). Main thread only.
[[nodiscard]] std::vector<StepInfo> current_printer_steps(PrinterState& state);

} // namespace helix::calibration
