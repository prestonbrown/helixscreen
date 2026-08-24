// tests/test_helpers/scoped_test_mode.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app_globals.h"
#include "runtime_config.h"

/// RAII guard for the process-global RuntimeConfig::test_mode flag.
///
/// `test_mode` is the master switch behind every `should_mock_*()` predicate, so
/// a test that sets it on the global and never puts it back changes the
/// behaviour of every test that runs after it in the same process. That is what
/// prestonbrown/helixscreen#1287 turned out to be: a single unrestored
/// `get_runtime_config()->test_mode = true` in a capabilities characterization
/// test left four unrelated tool_state/tool_switcher cases failing, but only
/// when a filter happened to schedule them after it. Each passed alone, which is
/// the expensive part - the failure looks like a bug in the victim.
///
/// Deliberately RAII rather than a restore at the end of the test body: these
/// tests use SECTIONs, and Catch2 aborts a failing REQUIRE by throwing, so a
/// trailing assignment is skipped exactly when a test has already gone wrong -
/// turning one red assertion into a cascade in later tests.
///
/// `HelixTestFixture::reset_all()` deliberately does NOT reset `test_mode`,
/// because tests that set it expect it to hold for the duration of their own
/// TEST_CASE. That makes restoring it the setter's responsibility, which is what
/// this guard is for.
class ScopedTestMode {
  public:
    explicit ScopedTestMode(bool enabled = true)
        : config_(get_runtime_config()), saved_(config_->test_mode) {
        config_->test_mode = enabled;
    }
    ~ScopedTestMode() {
        config_->test_mode = saved_;
    }

    ScopedTestMode(const ScopedTestMode&) = delete;
    ScopedTestMode& operator=(const ScopedTestMode&) = delete;
    ScopedTestMode(ScopedTestMode&&) = delete;
    ScopedTestMode& operator=(ScopedTestMode&&) = delete;

  private:
    RuntimeConfig* config_;
    bool saved_;
};
