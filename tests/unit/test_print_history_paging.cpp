// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_history_paging.cpp
 * @brief Older history pages in behind the newest-first cache
 *
 * A load is capped (kCompleteJobLimit), so a printer with more jobs than the
 * cap has history the cache cannot show. load_older() fetches the page before
 * the oldest cached job; ensure_covers_since() keeps paging until a window is
 * covered or the printer runs out of jobs.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/print_history_data.h"
#include "../../include/print_history_manager.h"
#include "../../include/printer_state.h"
#include "../../include/ui_filename_utils.h"
#include "../../include/ui_update_queue.h"
#include "../test_helpers/history_call_counting_api.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

constexpr double kNow = 1'000'000'000.0;
constexpr double kHour = 3600.0;

/// `count` jobs, newest first, one an hour, the newest `first_age_hours` old.
std::vector<PrintHistoryJob> jobs_from(int first_index, int count, double first_age_hours) {
    std::vector<PrintHistoryJob> out;
    for (int i = 0; i < count; ++i) {
        PrintHistoryJob job;
        job.job_id = "job" + std::to_string(first_index + i);
        job.filename = "part" + std::to_string(first_index + i) + ".gcode";
        job.start_time = kNow - (first_age_hours + i) * kHour;
        out.push_back(job);
    }
    return out;
}

class PagingFixture {
  public:
    PagingFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        update_queue_init();
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<ScriptedHistoryMoonrakerAPI>(client_, printer_state_);
        manager_ = std::make_unique<PrintHistoryManager>(api_.get(), &client_);
        pump();
        lv_subject_set_int(printer_state_.network_state().get_printer_connection_state_subject(),
                           static_cast<int>(ConnectionState::CONNECTED));
        pump();
    }

    ~PagingFixture() {
        manager_.reset();
        api_.reset();
        client_.disconnect();
        UpdateQueueTestAccess::drain(UpdateQueue::instance());
        update_queue_shutdown();
    }

  protected:
    void pump() {
        for (int i = 0; i < 5; ++i) {
            UpdateQueueTestAccess::drain(UpdateQueue::instance());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    /// A whole-history load that came back full: the limit, not the printer,
    /// ended it.
    void install_capped(int count) {
        PrintHistoryManagerTestAccess::set_loaded_jobs(*manager_, jobs_from(0, count, 0.0),
                                                       HistoryScope::COMPLETE, count);
    }

    ScriptedHistoryAPI& history() {
        return api_->scripted();
    }

    helix::PrinterState printer_state_;
    MoonrakerClientMock client_;
    std::unique_ptr<ScriptedHistoryMoonrakerAPI> api_;
    std::unique_ptr<PrintHistoryManager> manager_;
};

} // namespace

TEST_CASE_METHOD(PagingFixture, "load_older asks for the page before the oldest cached job",
                 "[history_manager][paging]") {
    install_capped(3);
    REQUIRE_FALSE(manager_->holds_every_job());

    manager_->load_older();
    REQUIRE(history().requests.size() == 1);
    CHECK(history().requests[0].limit == PrintHistoryManager::kOlderPageJobs);
    // Just past the oldest cached job, so jobs sharing its start time are kept.
    CHECK(history().requests[0].before > manager_->get_jobs().back().start_time);
    CHECK(history().requests[0].before < manager_->get_jobs().back().start_time + 1.0);

    // A short page: the printer has no more jobs than these.
    history().answer(jobs_from(3, 2, 3.0));
    pump();

    CHECK(manager_->get_jobs().size() == 5);
    CHECK(manager_->get_jobs().back().job_id == "job4");
    CHECK(manager_->holds_every_job());
    CHECK(manager_->covers_since(0.0));
}

TEST_CASE_METHOD(PagingFixture, "a full older page leaves the cache short of every job",
                 "[history_manager][paging]") {
    install_capped(3);
    manager_->load_older();
    history().answer(jobs_from(3, PrintHistoryManager::kOlderPageJobs, 3.0));
    pump();

    CHECK(manager_->get_jobs().size() == 3 + PrintHistoryManager::kOlderPageJobs);
    CHECK_FALSE(manager_->holds_every_job());
}

TEST_CASE_METHOD(PagingFixture, "a job already cached is not appended twice",
                 "[history_manager][paging]") {
    install_capped(3);
    manager_->load_older();
    auto page = jobs_from(2, 3, 2.0); // job2 overlaps the cache
    history().answer(page);
    pump();

    int job2 = 0;
    for (const auto& j : manager_->get_jobs()) {
        job2 += j.job_id == "job2" ? 1 : 0;
    }
    CHECK(job2 == 1);
    CHECK(manager_->get_jobs().size() == 5);
}

TEST_CASE_METHOD(PagingFixture, "load_older does nothing once every job is cached",
                 "[history_manager][paging]") {
    PrintHistoryManagerTestAccess::set_loaded_jobs(*manager_, jobs_from(0, 3, 0.0),
                                                   HistoryScope::COMPLETE, 10);
    REQUIRE(manager_->holds_every_job());
    manager_->load_older();
    CHECK(history().requests.empty());
}

TEST_CASE_METHOD(PagingFixture, "ensure_covers_since pages until the window is covered",
                 "[history_manager][paging]") {
    const int page = PrintHistoryManager::kOlderPageJobs;
    install_capped(3); // the last 3 hours
    const double window = kNow - (3.0 + page + 10.0) * kHour;
    REQUIRE_FALSE(manager_->covers_since(window));

    manager_->ensure_covers_since(window);
    REQUIRE(history().requests.size() == 1);
    history().answer(jobs_from(3, page, 3.0)); // still newer than the window
    pump();

    // Not covered yet, so the manager asks again on its own.
    REQUIRE(history().requests.size() == 1);
    CHECK_FALSE(manager_->covers_since(window));
    history().answer(jobs_from(3 + page, page, 3.0 + page)); // reaches past it
    pump();

    CHECK(manager_->covers_since(window));
    CHECK(history().requests.empty());
}

TEST_CASE_METHOD(PagingFixture, "an older page is dropped when a full load replaced the cache",
                 "[history_manager][paging]") {
    install_capped(3);
    manager_->load_older();
    REQUIRE(history().requests.size() == 1);

    // A history event refetched the newest jobs while the page was out.
    PrintHistoryManagerTestAccess::complete_fetch(*manager_, jobs_from(100, 3, 0.0),
                                                  HistoryScope::COMPLETE, 3);
    history().answer(jobs_from(3, 2, 3.0));
    pump();

    REQUIRE(manager_->get_jobs().size() == 3);
    CHECK(manager_->get_jobs().front().job_id == "job100");
}

TEST_CASE_METHOD(PagingFixture, "paging stops at the cached-job budget and coverage stays partial",
                 "[history_manager][paging]") {
    PrintHistoryManagerTestAccess::set_job_budget(*manager_, 5);
    install_capped(3);
    const double window = kNow - 1000.0 * kHour;

    manager_->ensure_covers_since(window);
    REQUIRE(history().requests.size() == 1);
    history().answer(jobs_from(3, PrintHistoryManager::kOlderPageJobs, 3.0));
    pump();

    // Over budget now: no further page, and the window is not claimed covered.
    CHECK(history().requests.empty());
    CHECK_FALSE(manager_->covers_since(window));
    CHECK_FALSE(manager_->holds_every_job());

    manager_->load_older();
    CHECK(history().requests.empty());
}

TEST_CASE_METHOD(PagingFixture, "a full page of jobs already cached ends paging",
                 "[history_manager][paging]") {
    install_capped(3);
    const double window = kNow - 1000.0 * kHour;
    manager_->ensure_covers_since(window);
    REQUIRE(history().requests.size() == 1);

    // A server ignoring before= answers with the newest jobs again: a full page,
    // nothing new. Asking again would get the same page forever.
    std::vector<PrintHistoryJob> same;
    for (int i = 0; i < PrintHistoryManager::kOlderPageJobs; ++i) {
        same.push_back(jobs_from(i % 3, 1, static_cast<double>(i % 3))[0]);
    }
    history().answer(same);
    pump();

    CHECK(history().requests.empty());
    CHECK(manager_->get_jobs().size() == 3);
    CHECK_FALSE(manager_->holds_every_job()); // stopped, not complete

    manager_->load_older();
    CHECK(history().requests.empty());
}

TEST_CASE_METHOD(PagingFixture, "a job sharing the boundary start time is not skipped",
                 "[history_manager][paging]") {
    install_capped(3);
    manager_->load_older();
    REQUIRE(history().requests.size() == 1);

    // job2 is the oldest cached; job2b started at the same instant.
    PrintHistoryJob twin = manager_->get_jobs().back();
    twin.job_id = "job2b";
    std::vector<PrintHistoryJob> page{manager_->get_jobs().back(), twin};
    history().answer(page);
    pump();

    CHECK(manager_->get_jobs().size() == 4);
    CHECK(manager_->get_jobs().back().job_id == "job2b");
}

TEST_CASE_METHOD(PagingFixture, "a reload nobody asked to cover does not page back",
                 "[history_manager][paging]") {
    install_capped(3);
    manager_->ensure_covers_since(kNow - 1000.0 * kHour);
    REQUIRE(history().requests.size() == 1);
    history().answer(jobs_from(3, PrintHistoryManager::kOlderPageJobs, 3.0));
    pump();
    REQUIRE(history().requests.size() == 1); // still paging toward the window

    // A history event replaces the cache with the newest jobs. The window that
    // wanted coverage is not asking any more, so nothing pages back.
    history().answer({}); // the in-flight page lands after the reload; dropped
    PrintHistoryManagerTestAccess::complete_fetch(*manager_, jobs_from(0, 3, 0.0),
                                                  HistoryScope::COMPLETE, 3);
    pump();
    CHECK(history().requests.empty());
}

TEST_CASE_METHOD(PagingFixture, "a rewritten job on an older page shows as the original's print",
                 "[history_manager][paging][reprint]") {
    FileMetadata meta;
    meta.modified = 42.0;
    api_->metadata_table().table["parts/benchy.gcode"] = meta;

    install_capped(3);
    manager_->load_older();
    auto page = jobs_from(3, 1, 3.0);
    page[0].filename = helix::gcode::make_rewritten_gcode_path("parts/benchy.gcode");
    history().answer(page);
    pump();

    const PrintHistoryJob& job = manager_->get_jobs().back();
    CHECK(job.filename == "parts/benchy.gcode");
    CHECK(job.exists);
    CHECK(job.modified == 42.0);
    CHECK(api_->metadata_table().calls == 1);
}

// A whole load larger than one wire page goes out as successive pages, so no single reply
// outgrows what a weak link and a small receive window carry in one frame.
TEST_CASE_METHOD(PagingFixture, "a load larger than a wire page arrives as successive pages",
                 "[history_manager][paging][wire_page]") {
    PrintHistoryManagerTestAccess::set_wire_page_jobs(*manager_, 10);
    manager_->ensure_loaded(HistoryScope::RECENT);
    REQUIRE(history().requests.size() == 1);
    CHECK(history().requests[0].limit == 10);
    CHECK(history().requests[0].start == 0);

    history().answer(jobs_from(0, 10, 0.0));
    pump();
    CHECK_FALSE(manager_->is_loaded(HistoryScope::RECENT));
    REQUIRE(history().requests.size() == 1);
    CHECK(history().requests[0].start == 10);

    // A short page is the end of the printer's history.
    history().answer(jobs_from(10, 4, 10.0));
    pump();
    CHECK(history().requests.empty());
    CHECK(manager_->is_loaded(HistoryScope::RECENT));
    CHECK(manager_->get_jobs().size() == 14);
    CHECK(manager_->holds_every_job());
}

TEST_CASE_METHOD(PagingFixture, "paged loading stops at the load's own limit",
                 "[history_manager][paging][wire_page]") {
    PrintHistoryManagerTestAccess::set_wire_page_jobs(*manager_, 20);
    manager_->ensure_loaded(HistoryScope::RECENT);
    int pages = 0;
    while (!history().requests.empty() && pages < 10) {
        const auto& r = history().requests[0];
        CHECK(r.limit <= 20);
        history().answer(jobs_from(r.start, r.limit, r.start));
        pump();
        ++pages;
    }
    CHECK(pages == 3); // 20 + 20 + 10
    CHECK(manager_->get_jobs().size() == PrintHistoryManager::kRecentJobLimit);
    CHECK_FALSE(manager_->holds_every_job());
}

// History waits for discovery: a stalled history reply on a new connection must not hold
// up discovery, and every reconnect would otherwise ask for it again first.
TEST_CASE_METHOD(PagingFixture, "history waits for discovery on each connection",
                 "[history_manager][paging][discovery_gate]") {
    manager_->hold_until_discovery();
    manager_->ensure_loaded(HistoryScope::RECENT);
    CHECK(history().requests.empty());

    manager_->on_discovery_complete();
    REQUIRE(history().requests.size() == 1);
    history().answer(jobs_from(0, 3, 0.0));
    pump();
    REQUIRE(manager_->is_loaded(HistoryScope::RECENT));

    // The connection drops and comes back: nothing goes out before discovery again.
    lv_subject_set_int(printer_state_.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::DISCONNECTED));
    pump();
    lv_subject_set_int(printer_state_.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    pump();
    manager_->ensure_loaded(HistoryScope::RECENT);
    CHECK(history().requests.empty());
    manager_->on_discovery_complete();
    CHECK(history().requests.size() == 1);
}
