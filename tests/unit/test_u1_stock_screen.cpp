// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_u1_stock_screen.cpp
 * @brief The Snapmaker U1 hook's SSH and stock-screen handoff to the app.
 *
 * The test build stubs the notification channel, so only the pure
 * decisions are tested here.
 */

#include "../ui_test_utils.h"
#include "u1_stock_screen.h"

#include <cstdlib>
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
