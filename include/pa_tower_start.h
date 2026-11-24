// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What both pressure advance tuners ask before they print a test tower: filament in the
// toolhead, and one confirmation that also stands for "the bed is clear".

#include "async_lifetime_guard.h"

#include <functional>
#include <string>

namespace helix::ui {

/// Whether the app knows the tower's head holds no filament. @p tool is the tool the tower
/// mounts, -1 for the machine's only head. False when nothing reports a loaded state: an
/// unknown head never blocks a tower.
[[nodiscard]] bool tower_head_empty(int tool);

/// Refuse a tower on an empty head: tell the user why and offer the Filament panel to load.
/// Returns true when it refused.
bool refuse_empty_toolhead(int tool, LifetimeToken owner);

/// "Print the test tower?" with @p message. Start marks the bed cleared, as the generic
/// "Is the bed clear?" question would, and then runs @p start. @p owner gates the dialog's
/// callbacks.
void confirm_tower_print(const std::string& message, LifetimeToken owner,
                         std::function<void()> start);

} // namespace helix::ui
