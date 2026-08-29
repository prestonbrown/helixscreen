// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <vector>

namespace helix::printer {

/**
 * @brief Shared AFC capability flags
 *
 * Provides a single source of truth for AFC system capabilities,
 * used by both AmsBackendAfc and AmsBackendMock.
 */
struct AfcCapabilities {
    bool supports_endless_spool = true;
    bool supports_bypass = true;
    bool supports_purge = true;
    TipMethod tip_method = TipMethod::CUT;
};

/**
 * @brief Get the canonical AFC section definitions
 *
 * Returns the 7 standard AFC sections in display order.
 * Both AmsBackendAfc and AmsBackendMock use these as their
 * section definitions rather than duplicating them.
 *
 * @return Vector of DeviceSection structs ordered by display_order
 */
std::vector<DeviceSection> afc_default_sections();

/**
 * @brief Get the static AFC action definitions
 *
 * Returns actions whose metadata does not depend on runtime config.
 * Dynamic actions (per-lane calibration, config-dependent settings)
 * are added by the backends themselves.
 *
 * @return Vector of DeviceAction structs with default values
 */
std::vector<DeviceAction> afc_default_actions();

/**
 * @brief Get the default AFC capability flags
 * @return AfcCapabilities with standard AFC values
 */
AfcCapabilities afc_default_capabilities();

} // namespace helix::printer
