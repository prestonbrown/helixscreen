// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_stats_widget_lifetime_totals.cpp
 * @brief The "Lifetime Print Stats" widget must report the SERVER's totals,
 *        not the sum of the capped local job cache (#1272).
 *
 * PrintHistoryManager::fetch() pulls at most `limit` jobs (500 in production).
 * Summing that cache silently truncates every printer with a longer history —
 * the reporter saw 500 prints / 209h next to Mainsail's 764.
 *
 * The cap is reproduced here in miniature: fetch(3) against a mock whose
 * history holds more than three jobs, so the cache sum and the server totals
 * disagree exactly the way they do at 500. Weekly mode has no server-side
 * equivalent (`server.history.totals` is lifetime-only) and must keep using
 * the filtered cache.
 */

#include "../../include/app_globals.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/panel_widget_manager.h"
#include "../../include/print_history_data.h"
#include "../../include/print_history_manager.h"
#include "../../include/printer_state.h"
#include "../lvgl_test_fixture.h"
#include "src/ui/panel_widgets/print_stats_widget.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

const char* subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

/// Display rule the widget uses for a duration, restated independently here so
/// the expectation is not read back out of the implementation.
std::string format_hours(uint64_t seconds) {
    char buf[24];
    unsigned long hours = static_cast<unsigned long>(seconds / 3600);
    unsigned long mins = static_cast<unsigned long>((seconds % 3600) / 60);
    if (hours >= 100) {
        std::snprintf(buf, sizeof(buf), "%luh", hours);
    } else {
        std::snprintf(buf, sizeof(buf), "%luh %lum", hours, mins);
    }
    return buf;
}

class PrintStatsLifetimeFixture : public LVGLTestFixture {
  public:
    PrintStatsLifetimeFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, printer_state_);
        manager_ = std::make_unique<PrintHistoryManager>(api_.get(), &client_);

        set_moonraker_api(api_.get());
        set_print_history_manager(manager_.get());

        // Widget-owned subjects register lazily.
        PanelWidgetManager::instance().init_widget_subjects();
        lv_subject_t* view_mode = lv_xml_get_subject(nullptr, "print_stats_view_mode");
        REQUIRE(view_mode != nullptr);
        lv_subject_set_int(view_mode, 0); // 0 = lifetime
    }

    ~PrintStatsLifetimeFixture() override {
        set_print_history_manager(nullptr);
        set_moonraker_api(nullptr);
        manager_.reset();
        api_.reset();
        client_.disconnect();
    }

  protected:
    /// Read the server-computed totals directly — the same numbers Mainsail shows.
    PrintHistoryTotals fetch_server_totals() {
        PrintHistoryTotals totals;
        std::atomic<bool> done{false};
        api_->history().get_history_totals(
            [&](const PrintHistoryTotals& t) {
                totals = t;
                done = true;
            },
            [&](const MoonrakerError&) { done = true; });
        REQUIRE(wait_until([&]() { return done.load(); }));
        return totals;
    }

    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintHistoryManager> manager_;
};

} // namespace

TEST_CASE_METHOD(PrintStatsLifetimeFixture,
                 "print_stats lifetime totals come from the server, not the capped job cache",
                 "[print_stats][history][1272]") {
    PrintHistoryTotals server = fetch_server_totals();
    REQUIRE(server.total_jobs > 3); // otherwise the cap below can't bite

    // Cache capped below the real history size — same shape as >500 jobs.
    manager_->fetch(3);
    REQUIRE(wait_until([&]() { return manager_->is_loaded(); }));
    REQUIRE(manager_->get_jobs().size() == 3);

    uint64_t cached_time = 0;
    for (const auto& job : manager_->get_jobs()) {
        cached_time += static_cast<uint64_t>(job.total_duration);
    }
    REQUIRE(cached_time < server.total_time);

    PrintStatsWidget widget;
    lv_obj_t* obj = lv_obj_create(test_screen());
    widget.attach(obj, test_screen());
    widget.on_activate();

    const std::string expected_prints = std::to_string(server.total_jobs);
    const std::string expected_time = format_hours(server.total_time);

    bool settled = wait_until([&]() {
        return expected_prints == subject_text("print_stats_total_prints") &&
               expected_time == subject_text("print_stats_total_time");
    });

    INFO("total_prints=" << subject_text("print_stats_total_prints") << " total_time="
                         << subject_text("print_stats_total_time") << " (cache holds "
                         << manager_->get_jobs().size() << " jobs / " << cached_time << "s)");
    CHECK(settled);
    CHECK(expected_prints == std::string(subject_text("print_stats_total_prints")));
    CHECK(expected_time == std::string(subject_text("print_stats_total_time")));
    CHECK(std::to_string(server.total_time / 3600) + "h" ==
          std::string(subject_text("print_stats_total_time_short")));

    widget.detach();
    lv_obj_delete(obj);
}

TEST_CASE_METHOD(PrintStatsLifetimeFixture,
                 "print_stats refetches lifetime totals after a grid rebuild recycles it",
                 "[print_stats][history][1272]") {
    PrintHistoryTotals server = fetch_server_totals();
    REQUIRE(server.total_jobs > 3);

    manager_->fetch(3);
    REQUIRE(wait_until([&]() { return manager_->is_loaded(); }));

    PrintStatsWidget widget;

    // First attach: activate, then tear down before the in-flight totals reply
    // is drained, exactly as a grid rebuild does. The reply is dropped by the
    // lifetime guard — nothing may stay latched that blocks the next request.
    lv_obj_t* first = lv_obj_create(test_screen());
    widget.attach(first, test_screen());
    widget.on_activate();
    widget.detach();
    lv_obj_delete(first);

    // Same C++ instance re-attached into the rebuilt grid (supports_reuse()).
    lv_obj_t* second = lv_obj_create(test_screen());
    widget.attach(second, test_screen());
    widget.on_activate();

    const std::string expected_prints = std::to_string(server.total_jobs);
    bool settled =
        wait_until([&]() { return expected_prints == subject_text("print_stats_total_prints"); });
    INFO("total_prints=" << subject_text("print_stats_total_prints"));
    CHECK(settled);

    widget.detach();
    lv_obj_delete(second);
}

TEST_CASE_METHOD(PrintStatsLifetimeFixture,
                 "print_stats weekly mode still aggregates the cached job list",
                 "[print_stats][history][1272]") {
    PrintHistoryTotals server = fetch_server_totals();
    REQUIRE(server.total_jobs > 3);

    manager_->fetch(3);
    REQUIRE(wait_until([&]() { return manager_->is_loaded(); }));

    lv_subject_t* view_mode = lv_xml_get_subject(nullptr, "print_stats_view_mode");
    REQUIRE(view_mode != nullptr);
    lv_subject_set_int(view_mode, 1); // weekly

    PrintStatsWidget widget;
    lv_obj_t* obj = lv_obj_create(test_screen());
    widget.attach(obj, test_screen());
    widget.on_activate();
    process_lvgl(20);

    // `server.history.totals` is lifetime-only, so a 7-day window can only come
    // from the cache. The mock spreads jobs one per day going back, so a 3-job
    // cache is entirely inside the last week.
    const auto weekly_jobs = manager_->get_jobs().size();
    CHECK(std::to_string(weekly_jobs) == std::string(subject_text("print_stats_total_prints")));
    CHECK(std::string(subject_text("print_stats_total_prints")) !=
          std::to_string(server.total_jobs));

    lv_subject_set_int(view_mode, 0);
    widget.detach();
    lv_obj_delete(obj);
}
