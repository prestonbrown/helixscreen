// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "platform_capabilities.h"

#include <optional>

#include "hv/json.hpp"

namespace helix::config_testing {

/// Force a platform tier for v15→v16 screensaver migration testing.
/// Pass std::nullopt to restore live detection. Production code must never call this.
void set_forced_tier_for_migration(std::optional<helix::PlatformTier> tier);

/// Run Config::init()'s display-migration step over @p data: root-level
/// display_* -> /display/, then /display/{calibration,touch_device} -> /input/.
///
/// Both migrations are file-static in config.cpp, so this is the only way a test
/// can exercise the real code instead of restating it. init() calls the same
/// underlying function, so the two cannot drift.
///
/// @param data JSON config data to migrate (modified in place)
/// @return true if either migration changed @p data
bool run_display_migrations_for_test(nlohmann::json& data);

} // namespace helix::config_testing
