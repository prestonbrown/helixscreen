// tests/test_helpers/scoped_runtime_config.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app_globals.h"
#include "runtime_config.h"

/// RAII snapshot of the process-global RuntimeConfig.
///
/// The global config is what every `should_mock_*()` predicate reads, so a test
/// that flips a flag on it and never puts it back changes behaviour for every
/// test scheduled after it in the same process. That is what
/// prestonbrown/helixscreen#1287 was: one unrestored `test_mode = true` failed
/// four unrelated tool_state/tool_switcher cases under a filter that happened to
/// order them after it, while each passed alone.
///
/// Construct one, then set whatever flags the test needs:
///
/// ```cpp
/// ScopedRuntimeConfig scoped_config;
/// get_runtime_config()->test_mode = true;
/// get_runtime_config()->use_real_wifi = false;
/// ```
///
/// The destructor restores the whole struct, so a test cannot forget the flag it
/// set second. That is the reason this snapshots everything rather than taking a
/// list of fields: the per-flag guards this replaced each covered exactly the
/// fields their author remembered, and adding a flag to a test meant remembering
/// to widen its guard too.
///
/// RAII rather than a restore at the end of the test body: Catch2 aborts a
/// failing REQUIRE by throwing, so a trailing assignment is skipped exactly when
/// a test has already gone wrong - turning one red assertion into a cascade in
/// every test that follows.
///
/// `HelixTestFixture::reset_all()` deliberately does not reset `test_mode`,
/// because a test that sets it expects it to hold for its own TEST_CASE. That
/// makes restoring it the setter's responsibility, which is what this is for.
///
/// RuntimeConfig is plain members, so the copy is a shallow snapshot. The two
/// `const char*` fields (`select_file`, `gcode_test_file`) are restored as
/// pointers, which is the intended semantics - the guard puts back whatever the
/// test found, it does not own string storage.
class ScopedRuntimeConfig {
  public:
    ScopedRuntimeConfig() : config_(get_runtime_config()), saved_(*config_) {}
    ~ScopedRuntimeConfig() {
        *config_ = saved_;
    }

    ScopedRuntimeConfig(const ScopedRuntimeConfig&) = delete;
    ScopedRuntimeConfig& operator=(const ScopedRuntimeConfig&) = delete;
    ScopedRuntimeConfig(ScopedRuntimeConfig&&) = delete;
    ScopedRuntimeConfig& operator=(ScopedRuntimeConfig&&) = delete;

  private:
    RuntimeConfig* config_;
    RuntimeConfig saved_;
};
