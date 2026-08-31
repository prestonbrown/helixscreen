// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_coalesced_timer.h"

#include <functional>
#include <string>

namespace helix::ui {

/// Quiet period shared by every search input. One constant, not three kept in
/// sync by comments.
inline constexpr uint32_t kDefaultSearchDebounceMs = 300;

/// Search-input policy on top of CoalescedTimer, shared by the search inputs
/// (Spoolman panel, history list, AMS edit overlay's spool picker). Adds two
/// things to the underlying one-shot coalescing: the callback carries the
/// query text, and an EMPTY query applies immediately - clearing a filter must
/// not wait out the delay. All timing, cancel, and teardown semantics belong
/// to CoalescedTimer; this class holds no timer of its own.
class SearchDebounce {
  public:
    using Callback = std::function<void(const std::string& query)>;

    SearchDebounce(Callback callback, uint32_t delay_ms = kDefaultSearchDebounceMs);

    /// Restart the debounce countdown for a new query; fires the callback
    /// synchronously instead when the query is empty.
    void schedule(std::string query);

    /// Drop a pending trigger without firing.
    void cancel() {
        timer_.cancel();
    }

    /// True while a trigger is armed.
    bool pending() const {
        return timer_.pending();
    }

  private:
    CoalescedTimer timer_;
    Callback callback_;
    std::string query_;
};

} // namespace helix::ui
