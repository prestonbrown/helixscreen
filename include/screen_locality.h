// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace helix {

/**
 * @brief Telemetry label for where the display runs relative to the printer
 *
 * "local" means HelixScreen and Klipper share a host, which is the precondition
 * for every feature that needs klippy's UDS socket. "remote" is everything else,
 * including an unconfigured host - absence of evidence is not evidence of
 * co-location, and guessing "local" would inflate the statistic this exists to
 * measure.
 *
 * @param host The configured moonraker_host string
 * @return The literal "local" or "remote". Never null, never allocates.
 */
const char* screen_locality_for_host(std::string_view host);

} // namespace helix
