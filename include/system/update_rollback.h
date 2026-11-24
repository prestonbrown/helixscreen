// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

namespace helix::update {

/// Can this install put the previous version back on its own when an automatic
/// install does not come up healthy? Install overnight is offered only where it can.
bool auto_rollback_supported();

/// Replaces auto_rollback_supported()'s answer for tests; std::nullopt restores it.
void set_auto_rollback_supported_for_test(std::optional<bool> value);

} // namespace helix::update
