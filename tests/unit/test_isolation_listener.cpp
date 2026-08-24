// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-test isolation guard.
//
// Many flaky failures in the single-process test binary come from a test that
// mutates PROCESS-GLOBAL state and never restores it: the working directory
// (chdir) or the HELIX_DATA_DIR / HELIX_CONFIG_DIR env vars that drive
// find_readable()/get_data_dir(). A later test in the same process then can't
// locate assets/config/* and fails with confusing "file not found" symptoms
// (e.g. MacroManager returns 0 macros, default_layout.json falls back to
// hardcoded anchors). These are invisible in isolation and shard-dependent in
// CI.
//
// This listener captures CWD + the data/config env vars before each TEST_CASE
// and reports any test that leaves them changed — pinpointing the leaking test
// by name, deterministically, in a single run. It does not fail the run (so it
// can also serve as a passive regression tripwire); grep stderr for
// "[ISOLATION-LEAK]".

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/live_thread_count.h"
#include "../test_helpers/update_queue_test_access.h"
#include "http_executor.h"
#include "thumbnail_processor.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include "../catch_amalgamated.hpp"

namespace {

std::string current_cwd() {
    std::array<char, 4096> buf{};
    const char* r = getcwd(buf.data(), buf.size());
    return r ? std::string(r) : std::string("<getcwd-failed>");
}

std::string env_or(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string("<unset>");
}

// Live thread count for this process. A test that spawns a background thread
// (e.g. an hv::EventLoopThread) and does not join it before returning leaks it;
// the loop later fires a callback on freed state and crashes a *different* test
// (UAF in hio_get / a freed observer). This count can't be auto-healed (we can't
// safely kill a thread), but naming the leaking test makes the otherwise-
// nondeterministic crash diagnosable in one run.
//
// Implementation lives in tests/test_helpers/live_thread_count.h so individual
// tests can assert thread-neutrality of a specific operation with the same
// measure this listener uses (prestonbrown/helixscreen#1146, #1212).
using helix::test::live_thread_count;

// Count occurrences of `key`, preserving first-seen order.
void tally(std::vector<std::pair<std::string, size_t>>& counts, const std::string& key) {
    auto it = std::find_if(counts.begin(), counts.end(),
                           [&key](const auto& e) { return e.first == key; });
    if (it == counts.end()) {
        counts.emplace_back(key, 1);
    } else {
        ++it->second;
    }
}

// "a, b x3, c" from a tally.
std::string join(const std::vector<std::pair<std::string, size_t>>& counts) {
    std::string detail;
    for (const auto& [key, n] : counts) {
        if (!detail.empty()) {
            detail += ", ";
        }
        detail += key;
        if (n > 1) {
            detail += " x" + std::to_string(n);
        }
    }
    return detail;
}

std::string basename(const char* path) {
    std::string p(path);
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

class IsolationListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const& /*info*/) override {
        // Route the default spdlog logger to a null sink BEFORE any worker
        // thread starts. spdlog's default-logger API (spdlog::debug/info/...)
        // reads the registry pointer WITHOUT a lock, so set_default_logger()
        // may never race a logging thread — including the process-lifetime
        // HttpExecutor/ThumbnailProcessor workers started just below
        // (nightly TSAN: data race on registry::s_instance from the
        // ActivePrintMediaManager fixtures, which used to swap the default
        // logger per test). Installed exactly once here, single-threaded.
        // Capture-style tests still swap in their own logger and restore this
        // one — those swaps stay, but this baseline removes the per-test one.
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        auto null_logger = std::make_shared<spdlog::logger>("null", null_sink);
        spdlog::set_default_logger(std::move(null_logger));

        // Pre-warm process-wide worker pools ONCE before any per-test thread
        // baseline is captured. ThumbnailProcessor is a Meyer's singleton whose
        // constructor eagerly starts an HThreadPool worker (MIN_WORKER_THREADS=1)
        // that lives for the whole process. The first test to reach a thumbnail
        // fetch (get_thumbnail_cache() -> ThumbnailProcessor::instance()) would
        // otherwise be flagged as "leaking" that thread — a false positive, since
        // a process-lifetime pool thread is not an unjoined per-test thread.
        // Spawning it here folds it into every test's threads_ baseline, so the
        // delta check only catches genuine per-test thread leaks.
        (void)helix::ThumbnailProcessor::instance();

        // Same rationale for the HttpExecutor lanes: fast() (4 workers) and
        // slow() (1 worker) are process-wide pools whose worker threads start
        // lazily on first use and live for the process. The first test to reach
        // an async network/REST call (e.g. EthernetManager::get_info_async ->
        // HttpExecutor::fast().start()) would otherwise be flagged as leaking
        // all 4 fast-lane workers. start_all() is the app-init entry point and
        // start() is idempotent, so warming both lanes here folds their threads
        // into the baseline.
        helix::http::HttpExecutor::start_all();
    }

    void testRunEnded(Catch::TestRunStats const& /*stats*/) override {
        // Production teardown calls ObserverGuard::invalidate_all() before LVGL is
        // torn down so surviving singletons release their observers instead of
        // calling lv_observer_remove() on freed state. The test binary has no such
        // hook, so global panel singletons (e.g. PrintStatusPanel) held in
        // function-local statics destruct during __run_exit_handlers and crash in
        // lv_ll_remove (SIGSEGV at exit, after every test passed). Bumping the
        // epoch here makes those at-exit ObserverGuard::reset() calls release
        // safely. No tests run after this point, so skipping the removes is free.
        ObserverGuard::invalidate_all();
    }

    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        // The Config singleton outlives every test, so whatever the previous
        // test left in it — a path pointing at a deleted temp dir, an LED
        // selection, a stale active_printer_id_ — is what this test starts
        // with. Resetting HERE rather than only in HelixTestFixture is what
        // makes the guarantee unconditional: plenty of TEST_CASEs (all of
        // test_led_config.cpp, for one) use no fixture at all.
        helix::test::reset_config_singleton();

        name_ = info.name;
        cwd_ = current_cwd();
        data_dir_ = env_or("HELIX_DATA_DIR");
        config_dir_ = env_or("HELIX_CONFIG_DIR");
        threads_ = live_thread_count();
    }

    void testCaseEnded(Catch::TestCaseStats const& /*stats*/) override {
        // Report AND auto-heal: a leaked CWD / data-dir env breaks find_readable()
        // for every later test in the process (cascading "file not found"
        // failures). Restoring here makes that failure class structurally
        // impossible even if a future test forgets to clean up, while the warning
        // ensures the offending test still gets fixed at the source.
        if (cwd_ != current_cwd()) {
            warn("cwd", cwd_, current_cwd());
            (void)chdir(cwd_.c_str());
        }
        heal_env("HELIX_DATA_DIR", data_dir_);
        heal_env("HELIX_CONFIG_DIR", config_dir_);

        // A test that returns with callbacks still queued hands them to the NEXT
        // test: HelixTestFixture's ctor drains the queue before the new test
        // body runs. Any subject or observer that died with the leaking test is
        // then walked as freed memory — SIGSEGV in lv_subject_notify /
        // lv_ll_get_next, blamed on whichever innocent test happened to
        // construct a fixture next. Naming the leaker here turns that
        // shard-order-dependent crash into a deterministic report.
        //
        // Report AND auto-heal, like the cwd/env checks above. Heal by DISCARDING,
        // never by draining: this hook runs after the test's fixture has already
        // been destroyed, so executing the callbacks here would be the very
        // use-after-free we are preventing. Draining is only correct inside the
        // owning fixture's destructor body, while its subjects are still alive.
        //
        // Discarding makes the failure class structurally impossible — no test
        // can hand queued work to the next one — while the warning ensures the
        // leaking test still gets fixed at the source.
        // The tags name the producers, which is what makes a report actionable:
        // a tag pointing at a process singleton is benign unflushed work, while
        // one closing over a per-test object is a real UAF awaiting the next drain.
        if (std::vector<helix::ui::UpdateQueueTestAccess::DroppedCallback> dropped =
                helix::ui::UpdateQueueTestAccess::discard_pending(
                    helix::ui::UpdateQueue::instance());
            !dropped.empty()) {
            // Collapse repeats — a loop-driven test queues the same tag N times —
            // but match tags EXACTLY. A substring test would hide any producer
            // whose tag is a prefix of one already listed.
            std::vector<std::pair<std::string, size_t>> counts;
            std::vector<std::pair<std::string, size_t>> sites;
            for (const auto& d : dropped) {
                tally(counts, d.tag);
                if (d.file != nullptr) {
                    tally(sites, basename(d.file) + ":" + std::to_string(d.line));
                }
            }
            std::fprintf(stderr,
                         "\n[ISOLATION-LEAK] test \"%s\" left %zu queued UpdateQueue "
                         "callback(s); discarded. Producers: %s\n",
                         name_.c_str(), dropped.size(), join(counts).c_str());
            // Untagged producers are anonymous in the line above, and
            // "<untagged> x44" cannot be acted on. The call sites go on their
            // OWN line, with a distinct prefix, so scripts/check_update_queue_leaks.py
            // keeps parsing the report exactly as before — its regex matches the
            // [ISOLATION-LEAK] line only, and the untagged ceiling it enforces
            // must stay keyed on "<untagged>", not on a file:line that moves
            // whenever anything above the call site is edited.
            if (!sites.empty()) {
                std::fprintf(stderr, "[ISOLATION-LEAK-SITES] untagged from: %s\n",
                             join(sites).c_str());
            }
        }

        // Thread leaks can't be healed; settle briefly to avoid flagging a thread
        // that is mid-exit, then report a genuine increase.
        int now = live_thread_count();
        if (threads_ >= 0 && now > threads_) {
            for (int i = 0; i < 20 && live_thread_count() > threads_; ++i) {
                usleep(5000); // up to 100ms for a joining/exiting thread to clear
            }
            now = live_thread_count();
            if (now > threads_) {
                std::fprintf(stderr,
                             "\n[ISOLATION-LEAK] test \"%s\" leaked %d thread(s): %d -> %d "
                             "(likely an unjoined hv::EventLoopThread → later UAF crash)\n",
                             name_.c_str(), now - threads_, threads_, now);
            }
        }
    }

  private:
    void warn(const char* what, const std::string& before, const std::string& after) {
        std::fprintf(stderr,
                     "\n[ISOLATION-LEAK] test \"%s\" did not restore %s: \"%s\" -> \"%s\" "
                     "(auto-healed)\n",
                     name_.c_str(), what, before.c_str(), after.c_str());
    }

    void heal_env(const char* var, const std::string& before) {
        std::string after = env_or(var);
        if (before == after) {
            return;
        }
        warn(var, before, after);
        if (before == "<unset>") {
            unsetenv(var);
        } else {
            setenv(var, before.c_str(), 1);
        }
    }

    std::string name_;
    std::string cwd_;
    std::string data_dir_;
    std::string config_dir_;
    int threads_ = -1;
};

} // namespace

CATCH_REGISTER_LISTENER(IsolationListener)
