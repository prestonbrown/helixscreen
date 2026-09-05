// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_api.h"
#include "moonraker_history_api.h"

#include <atomic>
#include <utility>

namespace helix {

/// Records the history requests that actually reach the client.
///
/// Lets a test assert on requests issued rather than on their downstream
/// effects, which is the only way to tell a load apart from a no-op on a mock
/// whose history is short enough that every scope returns the same jobs.
class HistoryCallCountingAPI : public MoonrakerHistoryAPI {
  public:
    explicit HistoryCallCountingAPI(helix::IMoonrakerClient& client)
        : MoonrakerHistoryAPI(client) {}

    void get_history_list(int limit, int start, double since, double before,
                          HistoryListCallback on_success, ErrorCallback on_error) override {
        ++calls;
        last_limit.store(limit);
        MoonrakerHistoryAPI::get_history_list(limit, start, since, before, std::move(on_success),
                                              std::move(on_error));
    }

    std::atomic<int> calls{0};
    std::atomic<int> last_limit{0};
};

/// MoonrakerAPI that installs the counting history API in place of the real one.
class HistoryCallCountingMoonrakerAPI : public MoonrakerAPI {
  public:
    HistoryCallCountingMoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        // history_api_ is protected; swap in the counting implementation.
        history_api_ = std::make_unique<HistoryCallCountingAPI>(client);
    }

    [[nodiscard]] int history_list_calls() const {
        return counting()->calls.load();
    }

    /// Job limit the most recent request carried.
    [[nodiscard]] int history_last_limit() const {
        return counting()->last_limit.load();
    }

  private:
    [[nodiscard]] const HistoryCallCountingAPI* counting() const {
        return static_cast<const HistoryCallCountingAPI*>(history_api_.get());
    }
};

} // namespace helix
