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
#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace helix {

struct DiscoveryContext;
class PanelFactory;

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

/// The services a session runs, set up after the panel subjects and before the shell, so
/// nothing a panel raises while it builds is lost: E-stop and print abort over @p api, the
/// software keyboard on @p screen, notifications and toasts (draining the warnings queued
/// before the UI existed), custom printer images, the theme's light/dark availability,
/// post-operation cooldown and filament-consumption tracking.
void init_session_services(IMoonrakerAPI* api, lv_obj_t* screen);

/// Creates app_layout (the navbar and the resident panels' widgets) on @p screen, starts the
/// printer status icon, and wires navigation and the navbar's printer switch and add
/// callbacks. Returns app_layout, or null when the layout is structurally broken (logged).
lv_obj_t* create_app_layout(lv_obj_t* screen,
                            std::function<void(const std::string&)> on_switch_printer,
                            std::function<void()> on_add_printer);

/// Finds and sets up the panels in @p app_layout through @p panels, then creates the print
/// status overlay and the keypad. False on a structural failure (logged).
bool setup_app_panels(lv_obj_t* app_layout, lv_obj_t* screen, PanelFactory& panels);

} // namespace helix
