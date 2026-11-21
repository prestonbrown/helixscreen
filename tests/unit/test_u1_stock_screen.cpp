// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_u1_stock_screen.cpp
 * @brief The Snapmaker U1 hook's SSH and stock-screen handoff to the app.
 *
 * The test build stubs the notification channel, so only the pure
 * decisions are tested here.
 */

#include "../ui_test_utils.h"
#include "test_helpers/unique_temp_dir.h"
#include "u1_stock_screen.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Set (or, with nullptr, unset) an env var for one scope and restore it.
struct EnvVarGuard {
    std::string name;
    std::string old;
    bool was_set;

    EnvVarGuard(const char* var, const char* value) : name(var) {
        const char* prev = std::getenv(var);
        was_set = prev != nullptr;
        if (was_set) {
            old = prev;
        }
        if (value != nullptr) {
            ::setenv(var, value, 1);
        } else {
            ::unsetenv(var);
        }
    }
    ~EnvVarGuard() {
        if (was_set) {
            ::setenv(name.c_str(), old.c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

} // namespace

TEST_CASE("u1 ssh: only the hook's 1 means SSH is down", "[u1-stock-screen]") {
    CHECK(helix::u1::root_ssh_down("1"));
    CHECK_FALSE(helix::u1::root_ssh_down(nullptr));
    CHECK_FALSE(helix::u1::root_ssh_down(""));
    CHECK_FALSE(helix::u1::root_ssh_down("0"));
    CHECK_FALSE(helix::u1::root_ssh_down("10"));
}

TEST_CASE("u1 restore: offered only where the hook named a trigger path", "[u1-stock-screen]") {
    CHECK(helix::u1::restore_stock_screen_offered(
        "/home/lava/printer_data/config/helixscreen-restore-stock"));
    CHECK_FALSE(helix::u1::restore_stock_screen_offered(nullptr));
    CHECK_FALSE(helix::u1::restore_stock_screen_offered(""));
}

TEST_CASE("u1 restore: the trigger file is created where the hook looks", "[u1-stock-screen]") {
    namespace fs = std::filesystem;
    const fs::path dir = helix::test::unique_temp_dir("u1_restore");
    fs::create_directories(dir);
    const std::string trigger = (dir / "helixscreen-restore-stock").string();
    REQUIRE_FALSE(fs::exists(trigger));

    CHECK(helix::u1::write_restore_trigger(trigger));
    CHECK(fs::exists(trigger));

    // A directory that does not exist: nothing would restore, and the caller
    // must be told.
    CHECK_FALSE(helix::u1::write_restore_trigger((dir / "missing" / "trigger").string()));
    fs::remove_all(dir);
}

TEST_CASE("u1 restore: refused while a job holds the machine or its state is unknown",
          "[u1-stock-screen]") {
    using helix::u1::RestoreStockVerdict;
    const char* trigger = "/home/lava/printer_data/config/helixscreen-restore-stock";
    CHECK(helix::u1::decide_restore_stock(trigger, true, false) == RestoreStockVerdict::Proceed);
    CHECK(helix::u1::decide_restore_stock(trigger, true, true) == RestoreStockVerdict::JobActive);
    // Before Moonraker and print_stats arrive the job state reads STANDBY, so
    // "no job" is not yet known, whatever job_active says.
    CHECK(helix::u1::decide_restore_stock(trigger, false, false) ==
          RestoreStockVerdict::StateUnknown);
    CHECK(helix::u1::decide_restore_stock(trigger, false, true) ==
          RestoreStockVerdict::StateUnknown);
    CHECK(helix::u1::decide_restore_stock(nullptr, true, false) == RestoreStockVerdict::NotOffered);
    CHECK(helix::u1::decide_restore_stock("", false, true) == RestoreStockVerdict::NotOffered);
}
