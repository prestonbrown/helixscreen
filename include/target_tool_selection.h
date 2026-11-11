// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include <cstdint>
#include <functional>

namespace helix::ui {

/// The tool a temperature or filament surface aims at. One per surface.
/// A tool change anywhere moves it to the mounted tool; reopening the
/// surface does not reset it.
class TargetToolSelection {
  public:
    /// ToolState's active tool changed. >= 0 moves the selection there;
    /// -1 (empty carriage) keeps the user's pick.
    void on_active_tool_changed(int active_tool);
    /// The user chose @p tool on this surface.
    void pick(int tool);
    /// The selection clamped to [0, tool_count); 0 when nothing was ever
    /// chosen or the pick no longer exists.
    [[nodiscard]] int current(int tool_count) const;

  private:
    int selected_ = -1;
};

/// What the Pick up / Park button beside a tool selector offers.
enum class TargetToolAction : uint8_t { None, PickUp, Park };

/// @p multiple_heads is ToolState::has_multiple_toolheads(); hub lanes into
/// one head get None. A target that is mounted offers Park only when the
/// printer can park; otherwise None.
[[nodiscard]] TargetToolAction decide_target_tool_action(int target_tool, int active_tool,
                                                         bool multiple_heads, bool supports_park);

/// How long a requested tool change may take before the move waiting on it is
/// dropped. A hot-end changer confirms the swap from its dock sensors, which
/// can take tens of seconds after the change is accepted.
inline constexpr uint32_t kToolArrivalTimeoutMs = 120000;

/// Runs a step once ToolState reports a given tool on the carriage. Main
/// thread only. At most one wait is armed; start() replaces it.
class ToolArrivalWait {
  public:
    ToolArrivalWait() = default;
    ~ToolArrivalWait();
    ToolArrivalWait(const ToolArrivalWait&) = delete;
    ToolArrivalWait& operator=(const ToolArrivalWait&) = delete;

    /// @p on_arrived runs as soon as the active tool reads @p tool (at once
    /// when it already does); @p on_timeout runs after @p timeout_ms otherwise.
    /// Never both, and neither after cancel() or destruction.
    void start(int tool, uint32_t timeout_ms, std::function<void()> on_arrived,
               std::function<void()> on_timeout);
    void cancel();
    [[nodiscard]] bool armed() const {
        return static_cast<bool>(on_arrived_);
    }

  private:
    void finish(bool arrived);
    static void on_timer(lv_timer_t* timer);

    int tool_ = -1;
    std::function<void()> on_arrived_;
    std::function<void()> on_timeout_;
    ObserverGuard observer_;
    LvglTimerGuard timer_;
};

} // namespace helix::ui
