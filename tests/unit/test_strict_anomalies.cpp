// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fails the test case that reports a strict anomaly (test_helpers/strict_anomalies.h).
//
// The observer runs inside helix_lvgl_anomaly(), so the failure lands on the line of
// the case that caused it while the case is still running. Catch2 assertions are
// test-thread only: an anomaly from another thread, or from outside any case (static
// teardown, between cases), is written to stderr instead.
//
// observer_refused_deinit_subject reaches helix_lvgl_anomaly() only through an LVGL
// warning, so each case starts with a print callback that maps warnings to their
// anomaly codes. A case that installs its own callback replaces it until the next case.

#include "../test_helpers/strict_anomalies.h"
#include "../ui_test_utils.h"
#include "helix_lvgl_anomaly.h"
#include "lvgl_log_handler.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"

namespace {

std::atomic<bool> g_in_case{false};
std::thread::id g_case_thread;
int g_tolerated = 0;

void on_anomaly(const char* code, const char* context) {
    if (!helix::test::is_strict_anomaly(code)) {
        return;
    }
    const char* ctx = context ? context : "";
    if (!g_in_case.load(std::memory_order_acquire) || std::this_thread::get_id() != g_case_thread) {
        spdlog::error("[StrictAnomaly] {} outside the test thread: {}", code, ctx);
        return;
    }
    if (helix::test::is_anomaly_allowed(code)) {
        ++g_tolerated;
        return;
    }
    // A failure while the run is aborting (--abort, -x) throws; this frame can sit
    // under LVGL's C code or a destructor, so the throw stops here.
    try {
        FAIL_CHECK("strict anomaly " << code << ": " << ctx
                                     << " (opt out: helix::test::AllowAnomaly)");
    } catch (...) {
    }
}

void on_lvgl_log(lv_log_level_t level, const char* buf) {
    if (level != LV_LOG_LEVEL_WARN || buf == nullptr) {
        return;
    }
    if (const char* code = helix::logging::anomaly_code_for_warning(buf)) {
        helix_lvgl_anomaly(code, buf);
    }
}

class StrictAnomalyListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        helix_lvgl_anomaly_set_observer(&on_anomaly);
    }

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        helix::test::allowed_anomalies().clear();
        lv_init_safe();
        lv_log_register_print_cb(&on_lvgl_log);
        g_case_thread = std::this_thread::get_id();
        g_in_case.store(true, std::memory_order_release);
    }

    void testCaseEnded(Catch::TestCaseStats const&) override {
        g_in_case.store(false, std::memory_order_release);
    }

    void testRunEnded(Catch::TestRunStats const&) override {
        helix_lvgl_anomaly_set_observer(nullptr);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(StrictAnomalyListener)

TEST_CASE("AllowAnomaly tolerates only its own code, only while it lives",
          "[strict_anomalies][2116]") {
    CHECK(helix::test::is_strict_anomaly("subject_reinit_while_live"));
    CHECK_FALSE(helix::test::is_strict_anomaly("grid_update_zero_rows"));
    CHECK_FALSE(helix::test::is_strict_anomaly(nullptr));

    const int before = g_tolerated;
    {
        helix::test::AllowAnomaly allow("subject_reinit_while_live");
        helix::test::AllowAnomaly other("observer_null_subject");
        CHECK_FALSE(helix::test::is_anomaly_allowed("observer_refused_deinit_subject"));
        helix_lvgl_anomaly("subject_reinit_while_live", "provoked by this case");
    }
    CHECK(g_tolerated == before + 1);
    CHECK_FALSE(helix::test::is_anomaly_allowed("subject_reinit_while_live"));
    CHECK_FALSE(helix::test::is_anomaly_allowed("observer_null_subject"));
}
