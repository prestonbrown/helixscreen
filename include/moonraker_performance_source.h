// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "performance_source.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

class IMoonrakerAPI;

namespace helix {
namespace perf {

/**
 * @brief Production IPerformanceSource backed by Moonraker WebSocket notifications.
 *
 * Subscribes to notify_proc_stat_update for host CPU/memory/throttle data, and
 * dynamically discovers + subscribes to all MCU Klipper objects for per-MCU load
 * and retransmit counters.  All notification callbacks fire on the MoonrakerClient
 * WebSocket thread and are marshalled to the main thread via AsyncLifetimeGuard::bg_cb
 * before touching any members or invoking the SampleCallback.
 */
class MoonrakerPerformanceSource : public IPerformanceSource {
  public:
    explicit MoonrakerPerformanceSource(IMoonrakerAPI* api);
    ~MoonrakerPerformanceSource() override;

    void start() override;
    void stop() override;
    void set_callback(SampleCallback cb) override {
        cb_ = std::move(cb);
    }

  private:
    /// Called on main thread with the body from a proc_stats notification or
    /// the initial /machine/proc_stats REST snapshot.
    void on_proc_stat_payload(const json& body);

    /// Run the initial REST proc_stats fetch. Fired on every MoonrakerClient
    /// connected-observer signal (WS open / Klippy ready). MCU live updates
    /// flow through the main MoonrakerDiscoverySequence subscription — see
    /// notify_status_update registration in start().
    void run_initial_handshake();

    /// Called on main thread for each MCU status payload.
    void on_mcu_status_update(const std::string& object_name, const json& payload);

    /// Returns a human-readable throttle string or "" if bits == 0.
    static std::string format_throttle_text(uint32_t bits, const std::vector<std::string>& flags);

    IMoonrakerAPI* api_;
    SampleCallback cb_;
    AsyncLifetimeGuard lifetime_;
    bool running_ = false;

    /// Accumulated sample — updated by both proc_stat and MCU callbacks.
    PerfSample latest_;

    /// Per-MCU awake-time tracking for load computation.
    struct McuRunningState {
        double last_awake = 0.0;
        std::chrono::steady_clock::time_point last_t;
        uint64_t retrans = 0;
        bool initialized = false;
    };
    std::unordered_map<std::string, McuRunningState> mcu_state_;

    /// ID for the notify_status_update subscription (for MCU live updates).
    /// notify_callbacks_ is the channel that MoonrakerClient::dispatch_status_update
    /// fans out to — it carries both the initial subscription snapshot and live
    /// frames. method_callbacks_ would skip the initial snapshot.
    uint64_t status_sub_id_ = 0;
};

} // namespace perf
} // namespace helix
