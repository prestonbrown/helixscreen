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
// anomaly codes, and ends with the callback it found put back. A case that installs its
// own callback replaces it for the rest of that case.
//
// HELIX_ANOMALY_TALLY=<dir> also counts every anomaly code, strict or not, per case:
// each case that reported any appends "file<TAB>case<TAB>code<TAB>count" lines to
// <dir>/tally-<pid>.tsv, so a sharded sweep's files can be summed per test file. A
// refused observer also counts under "refused:<subject>", named from the debug
// registry, or "refused:PrinterState@global+<offset>" for an unnamed Subject inside
// the process-wide PrinterState, "refused:static+0x<offset>" for one in static
// storage, or "refused:Subject@heap".

#include "../test_helpers/strict_anomalies.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/subject.h"
#include "helix_lvgl_anomaly.h"
#include "lvgl/lvgl_private.h"
#include "lvgl_log_handler.h"
#include "printer_state.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

std::atomic<bool> g_in_case{false};
std::thread::id g_case_thread;
int g_tolerated = 0;
lv_log_print_g_cb_t g_previous_print_cb = nullptr;
const char* g_tally_dir = nullptr;
std::map<std::string, int> g_case_tally;

void on_anomaly(const char* code, const char* context) {
    if (g_tally_dir != nullptr && code != nullptr && g_in_case.load(std::memory_order_acquire) &&
        std::this_thread::get_id() == g_case_thread) {
        ++g_case_tally[code];
    }
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

std::string refused_subject_label(const char* buf) {
    const char* p = std::strstr(buf, "(ptr=");
    if (p == nullptr) {
        return "?";
    }
    auto* subject = reinterpret_cast<lv_subject_t*>(std::strtoull(p + 5, nullptr, 16));
    if (const auto* info = SubjectDebugRegistry::instance().lookup(subject)) {
        return info->name;
    }
    auto* ps = reinterpret_cast<char*>(&get_printer_state());
    auto* at = reinterpret_cast<char*>(helix::Subject::owner_of(subject));
    if (at != nullptr && at >= ps && at < ps + sizeof(helix::PrinterState)) {
        return "PrinterState@global+" + std::to_string(at - ps);
    }
    if (at == nullptr) {
        return "?";
    }
    // A Subject in static storage names its owner through the binary's symbol table:
    // `nm -C` resolves the offset printed here.
    Dl_info self{};
    if (dladdr(reinterpret_cast<void*>(&refused_subject_label), &self) != 0) {
        const auto offset = static_cast<std::uintptr_t>(at - static_cast<char*>(self.dli_fbase));
        if (offset < (std::uintptr_t{1} << 30)) {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "static+0x%zx", static_cast<size_t>(offset));
            return hex;
        }
    }
    return "Subject@heap";
}

void on_lvgl_log(lv_log_level_t level, const char* buf) {
    if (level != LV_LOG_LEVEL_WARN || buf == nullptr) {
        return;
    }
    if (const char* code = helix::logging::anomaly_code_for_warning(buf)) {
        if (g_tally_dir != nullptr && std::strcmp(code, "observer_refused_deinit_subject") == 0) {
            ++g_case_tally["refused:" + refused_subject_label(buf)];
        }
        helix_lvgl_anomaly(code, buf);
    }
}

class StrictAnomalyListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
        helix_lvgl_anomaly_set_observer(&on_anomaly);
        const char* dir = std::getenv("HELIX_ANOMALY_TALLY");
        g_tally_dir = (dir != nullptr && *dir != '\0') ? dir : nullptr;
    }

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        helix::test::allowed_anomalies().clear();
        lv_init_safe();
        g_previous_print_cb = LV_GLOBAL_DEFAULT()->custom_log_print_cb;
        lv_log_register_print_cb(&on_lvgl_log);
        g_case_thread = std::this_thread::get_id();
        g_in_case.store(true, std::memory_order_release);
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        g_in_case.store(false, std::memory_order_release);
        write_tally(*stats.testInfo);
        if (lv_is_initialized() && LV_GLOBAL_DEFAULT()->custom_log_print_cb == &on_lvgl_log) {
            lv_log_register_print_cb(g_previous_print_cb);
        }
    }

    static void write_tally(Catch::TestCaseInfo const& info) {
        if (g_tally_dir == nullptr || g_case_tally.empty()) {
            return;
        }
        const std::string path =
            std::string(g_tally_dir) + "/tally-" + std::to_string(::getpid()) + ".tsv";
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            for (const auto& [code, count] : g_case_tally) {
                std::fprintf(f, "%s\t%s\t%s\t%d\n", info.lineInfo.file, info.name.c_str(),
                             code.c_str(), count);
            }
            std::fclose(f);
        }
        g_case_tally.clear();
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
