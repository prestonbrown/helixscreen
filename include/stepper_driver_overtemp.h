// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace helix::sensors {

/// How hot a stepper driver says it is, from its overtemperature flags.
/// Ordered: a higher level is a worse state.
enum class DriverOvertemp : uint8_t {
    None,     ///< no flag set, or the driver is not being polled (motors off)
    Warning,  ///< prewarning or a temperature threshold flag (otpw, t120 .. t157)
    Shutdown, ///< the driver shut its outputs off (ot)
};

/**
 * @brief Read a TMC driver's `drv_status` field.
 *
 * Klipper publishes the non-zero DRV_STATUS fields as an object, or null while
 * the driver is not polled. `ot` is the shutdown flag; `otpw` and the TMC220x
 * `t120`/`t143`/`t150`/`t157` thresholds are warnings. Any other shape reads None.
 */
[[nodiscard]] DriverOvertemp classify_drv_status(const nlohmann::json& drv_status);

struct DriverOvertempAlert {
    std::string klipper_name; ///< "tmc2209 stepper_z"
    DriverOvertemp level = DriverOvertemp::None;
};

/**
 * @brief The drivers whose level rose between two readings, worst level each.
 *
 * A driver rising from None or Warning to a higher level is an alert; one that
 * stays at, or falls back from, its level is not. A driver that clears to None
 * alerts again on its next rise. Names absent from @p before read as None.
 */
[[nodiscard]] std::vector<DriverOvertempAlert>
driver_overtemp_rises(const std::map<std::string, DriverOvertemp>& before,
                      const std::map<std::string, DriverOvertemp>& now);

} // namespace helix::sensors
