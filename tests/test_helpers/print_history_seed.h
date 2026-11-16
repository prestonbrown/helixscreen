// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../test_helpers/print_history_manager_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix_shared_callback.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_history_api.h"
#include "print_activity.h"
#include "print_history_manager.h"
#include "printer_state.h"

#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// Answers lifetime totals from the test when it holds some, else from the mock.
class TotalsHistoryAPI : public MoonrakerHistoryAPI {
  public:
    explicit TotalsHistoryAPI(helix::IMoonrakerClient& client) : MoonrakerHistoryAPI(client) {}

    void get_history_totals(HistoryTotalsCallback on_success, ErrorCallback on_error) override {
        if (totals) {
            on_success(*totals);
            return;
        }
        MoonrakerHistoryAPI::get_history_totals(std::move(on_success), std::move(on_error));
    }

    std::optional<PrintHistoryTotals> totals;
};

class TotalsMoonrakerAPI : public MoonrakerAPI {
  public:
    TotalsMoonrakerAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        history_api_ = std::make_unique<TotalsHistoryAPI>(client);
    }

    TotalsHistoryAPI& totals_api() {
        return static_cast<TotalsHistoryAPI&>(*history_api_);
    }
};

/// A connected mock client, a Moonraker API whose lifetime totals the test can set, and
/// a PrintHistoryManager, installed on the app globals for the seed's lifetime. What the
/// print stats tile reads its week and its lifetime line from.
struct PrintHistorySeed {
    /// @p settle drains the update queue and pumps LVGL; the owning fixture supplies it.
    PrintHistorySeed(helix::PrinterState& state, helix::SharedCallback<void()> settle)
        : settle_(std::move(settle)),
          history_client(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        history_client.connect("ws://mock/websocket", []() {}, []() {});
        history_api = std::make_unique<TotalsMoonrakerAPI>(history_client, state);
        history_manager = std::make_unique<PrintHistoryManager>(history_api.get(), &history_client);
        set_moonraker_api(history_api.get());
        set_print_history_manager(history_manager.get());
    }

    /// Brings the socket up. Separate from the constructor because the staleness watcher
    /// is queued with the socket down and stales any cache it finds when it runs, so the
    /// queue drains first.
    void connect(helix::PrinterState& state) {
        settle_();
        lv_subject_set_int(state.network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::CONNECTED));
        settle_();
    }

    ~PrintHistorySeed() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        set_print_history_manager(nullptr);
        set_moonraker_api(nullptr);
        history_manager.reset();
        history_api.reset();
        history_client.disconnect();
    }

    PrintHistorySeed(const PrintHistorySeed&) = delete;
    PrintHistorySeed& operator=(const PrintHistorySeed&) = delete;

    /// The totals the mock reports, read the way the tile reads them.
    PrintHistoryTotals mock_totals() {
        std::optional<PrintHistoryTotals> got;
        history_api->history().get_history_totals([&](const PrintHistoryTotals& t) { got = t; },
                                                  [](const MoonrakerError&) {});
        return got.value_or(PrintHistoryTotals{});
    }

    /// Install @p jobs as the whole cached history.
    void seed(std::vector<PrintHistoryJob> jobs) {
        PrintHistoryManagerTestAccess::set_loaded_jobs(*history_manager, std::move(jobs),
                                                       HistoryScope::RECENT,
                                                       PrintHistoryManager::kRecentJobLimit);
        settle_();
    }

    /// A job that finished @p days_ago local days back, ten seconds after that day began.
    static PrintHistoryJob job(int days_ago, double print_seconds, double total_seconds = 0.0) {
        const ActivityDayStarts starts = local_day_starts(std::time(nullptr));
        PrintHistoryJob j;
        j.job_id = "job-" + std::to_string(days_ago) + "-" + std::to_string(print_seconds);
        j.filename = "a.gcode";
        j.status = PrintJobStatus::COMPLETED;
        // Days outside the activity window step back from its oldest start by whole
        // days; they only need to land before the window, not on a local midnight.
        const int oldest = kActivityDays - 1;
        const double day_start = days_ago <= oldest
                                     ? starts[static_cast<size_t>(oldest - days_ago)]
                                     : starts[0] - static_cast<double>(days_ago - oldest) * 86400.0;
        j.start_time = day_start + 10.0;
        j.end_time = j.start_time + print_seconds;
        j.print_duration = print_seconds;
        j.total_duration = total_seconds > 0 ? total_seconds : print_seconds;
        return j;
    }

  private:
    helix::SharedCallback<void()> settle_;

  public:
    MoonrakerClientMock history_client;
    std::unique_ptr<TotalsMoonrakerAPI> history_api;
    std::unique_ptr<PrintHistoryManager> history_manager;
};

} // namespace helix
