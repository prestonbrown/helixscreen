// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <optional>

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file enclosure_top.h
 * @brief Firmwares that report whether a removable enclosure top is fitted.
 *
 * Some printers ship an optional lid or top module that the firmware powers and
 * detects, so whether it is on the printer right now can be read from a status
 * object. The answer is live: the top can be lifted off and put back while the
 * printer is connected.
 *
 * This is the one module that knows which firmwares report such a top and where
 * they keep the reading. Generic code asks the capability questions below and
 * never names a firmware. Adding another is a row in the provider table.
 */
namespace helix::enclosure_top {

/// True when this firmware reports whether an enclosure top is fitted.
[[nodiscard]] bool firmware_reports_enclosure_top(const PrinterDiscovery& hw);

/// Status objects to subscribe, as a Moonraker `objects` map (object name to
/// the list of fields read), for the subscription builder. Empty when the
/// firmware reports no enclosure top.
[[nodiscard]] nlohmann::json required_status_objects(const PrinterDiscovery& hw);

/// Whether an enclosure top is fitted, read out of a Moonraker status frame.
///
/// nullopt is "this frame says nothing": the printer reports no enclosure top,
/// the object or field is absent (as it is from any delta frame where it did not
/// change), or the field has a type no firmware sends. Callers must keep their
/// last answer on nullopt, never read it as "removed".
[[nodiscard]] std::optional<bool> fitted_from_status(const PrinterDiscovery& hw,
                                                     const nlohmann::json& status);

} // namespace helix::enclosure_top
