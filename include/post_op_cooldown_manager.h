// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "heater_hold.h"
#include "lvgl.h"

#include <atomic>
#include <string>
#include <vector>

/**
 * @brief Singleton manager for post-filament-operation heater cooldown
 *
 * After filament load/unload/swap operations, the extruder heater may be left on
 * indefinitely. This manager provides a unified cooldown path — all callers
 * (FilamentPanel, AMS sidebar, AMS backends) funnel through schedule()/cancel()
 * instead of implementing their own cooldown timers.
 *
 * - schedule() starts a configurable delay timer (default 120s)
 * - cancel() cancels a pending cooldown (user manually heats or new op starts)
 * - At fire time, checks: extruder target > 0 AND print state != PRINTING/PAUSED
 * - Calling schedule() resets any existing pending timer
 *
 * Thread safety: schedule() and cancel() are safe to call from any thread —
 * they defer to the LVGL main thread via queue_update().
 */
class PostOpCooldownManager {
  public:
    static PostOpCooldownManager& instance();

    /// Initialize the manager. Call once during startup.
    void init();

    /// Schedule a cooldown after the configured delay.
    /// Restarts the pending timer, which then turns off every heater scheduled
    /// since it last fired or was cancelled. Safe to call from any thread.
    /// @param heater Klipper extruder name to turn off; empty for the active one.
    void schedule(std::string heater = {});

    /// Cancel any pending cooldown. Call when user manually heats
    /// or a new filament operation starts.
    void cancel();

    /// Take @p heater out of the pending cooldown, leaving any other heater's.
    void cancel(std::string heater);

    /// Run a pending cooldown now, while the printer can still hear it.
    /// Main thread only.
    void flush_before_disconnect();

    /// Shutdown — cancel timer, release resources. Call during app teardown.
    void shutdown();

    /// True while a cooldown timer is pending. Test-observability only; read on
    /// the LVGL main thread after draining the update queue (schedule()/cancel()
    /// mutate timer_ from a queued main-thread lambda).
    [[nodiscard]] bool has_pending_timer() const noexcept {
        return timer_ != nullptr;
    }

    /// The heaters the pending cooldown turns off; "" is the active extruder.
    /// Test-observability only, read like has_pending_timer().
    [[nodiscard]] const std::vector<std::string>& pending_heaters() const noexcept {
        return heaters_;
    }

  private:
    PostOpCooldownManager() = default;
    ~PostOpCooldownManager() = default;
    PostOpCooldownManager(const PostOpCooldownManager&) = delete;
    PostOpCooldownManager& operator=(const PostOpCooldownManager&) = delete;

    // TIMER_DTOR_OK: the callback reaches the owner through
    // PostOpCooldownManager::instance() rather than a captured `this`, so
    // destroying the manager cannot leave the timer holding freed memory.
    /// The timer is spent: turn off what it was owed. Main thread only.
    void fire();

    lv_timer_t* timer_ = nullptr;
    helix::HeaterHold heater_hold_;
    std::vector<std::string> heaters_; ///< What timer_ turns off; "" is the active extruder
    std::atomic<bool> initialized_{false};
};
