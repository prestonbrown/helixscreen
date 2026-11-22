// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "probe_sensor_types.h"

#include <optional>
#include <string>

namespace helix {
class PrinterDiscovery;
}

/// Probes that can find Z by touching the bed with the nozzle, on top of their
/// usual mode. When one is set up that way the nozzle height comes from the touch,
/// so there is no Z offset left to calibrate with paper. One row per probe; generic
/// code asks the questions below and never names a probe.
namespace helix::sensors::touch_z {

/// A status field to subscribe for the touch setup.
struct StatusField {
    std::string object;
    std::string field;
};

/// The status field a probe of @p type reports its touch setup in, if it has one.
[[nodiscard]] std::optional<StatusField> status_field(ProbeSensorType type,
                                                      const std::string& klipper_name);

/// Whether @p status (a status update, keyed by object) says the probe sets Z by
/// touch; nullopt when the update does not say.
[[nodiscard]] std::optional<bool> from_status(ProbeSensorType type, const std::string& klipper_name,
                                              const nlohmann::json& status);

/// Whether the probe's config says it sets Z by touch.
[[nodiscard]] bool from_config(ProbeSensorType type, const std::string& klipper_name,
                               const PrinterDiscovery& hw);

} // namespace helix::sensors::touch_z
