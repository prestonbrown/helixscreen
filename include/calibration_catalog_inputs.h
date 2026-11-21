// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fills the calibration catalog's inputs from the live printer.

#include "calibration_catalog.h"

#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::calibration {

/// The catalog inputs for the connected printer: its capabilities, its Z offset
/// strategy, its probe and its database tiers. Main thread only.
[[nodiscard]] CatalogInputs catalog_inputs_from(const PrinterState& state);

/// applicable_steps() for the connected printer.
[[nodiscard]] std::vector<StepInfo> current_printer_steps(const PrinterState& state);

} // namespace helix::calibration
