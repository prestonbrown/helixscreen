// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file session_wiring.h
 * @brief The discovery wiring both builds share: desktop's PrinterSession and the ESP32
 *        firmware's app_boot register their discovery callbacks through wire_discovery().
 */

#include "hardware_fingerprint.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"

#include <functional>

namespace helix {

struct DiscoveryContext;

/// What differs between the builds' discovery passes.
struct DiscoveryHooks {
    /// The session's hardware-change record. Outlives the callbacks; the session resets it
    /// when a printer scope ends.
    HardwareChangeTracker& changes;
    /// False drops a queued pass (the session is shutting down). Null: always alive.
    std::function<bool()> alive;
    /// A discovery cycle starts: the hardware is known, the subsystems are not built yet.
    std::function<void()> begin_cycle;
    /// The core steps have run. Desktop runs its tail steps here.
    std::function<void(DiscoveryContext&)> after_core;
};

/// Registers @p client's hardware-discovered and discovery-complete callbacks. Each queues
/// its work to the UI thread and drops it there when the HTTP epoch has moved (the work
/// belongs to the previous printer) or @p hooks.alive says the session is gone. The first
/// copies the hardware into @p api and initialises the subsystems from it; the second
/// copies it again, notes its fingerprint, runs the core discovery steps, calls
/// @p hooks.after_core and releases the print history.
void wire_discovery(IMoonrakerAPI& api, IMoonrakerClient& client, DiscoveryHooks hooks);

} // namespace helix
