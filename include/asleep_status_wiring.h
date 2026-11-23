// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>

class DisplayManager;   // NAMESPACE_OK: declared in the global namespace
class MoonrakerManager; // NAMESPACE_OK: declared in the global namespace

namespace helix {

/// Holds telemetry status and widget-bound observers while the display sleeps (#2037).
/// Sleep turns both holds on; status keeps arriving and every other observer still sees
/// each value. Before the first frame after a sleep, the held status is applied, the
/// widgets are brought to the current values and the UpdateQueue is drained, so that
/// frame shows current values. @p moonraker is read at each call, since a printer switch
/// can replace the manager.
void wire_asleep_status(DisplayManager& display, std::function<MoonrakerManager*()> moonraker);

} // namespace helix
