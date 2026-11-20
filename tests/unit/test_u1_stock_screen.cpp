// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_u1_stock_screen.cpp
 * @brief The Snapmaker U1 hook's SSH and stock-screen handoff to the app.
 *
 * The surface function is observed through the sticky-warning test hook,
 * which sees the strings the production notification channel receives.
 */

#include "../ui_test_utils.h"
#include "core/update_queue.h"
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

template <typename Fn> std::vector<std::string> sticky_warnings(Fn&& fn) {
    std::vector<std::string> out;
    helix::ui::set_test_notification_sticky_warning_hook(
        [&](const std::string& msg) { out.push_back(msg); });
    fn();
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::set_test_notification_sticky_warning_hook(nullptr);
    return out;
}

} // namespace

TEST_CASE("u1 ssh: only the hook's 1 means SSH is down", "[u1-stock-screen]") {
    CHECK(helix::u1::root_ssh_down("1"));
    CHECK_FALSE(helix::u1::root_ssh_down(nullptr));
    CHECK_FALSE(helix::u1::root_ssh_down(""));
    CHECK_FALSE(helix::u1::root_ssh_down("0"));
    CHECK_FALSE(helix::u1::root_ssh_down("10"));
}

TEST_CASE("u1 ssh: a hook report of SSH down becomes one sticky warning", "[u1-stock-screen]") {
    const auto warnings = sticky_warnings([] {
        EnvVarGuard down(helix::u1::kRootSshDownEnv, "1");
        helix::u1::surface_root_ssh_down_from_hook();
    });
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("SSH did not start although Root access is on") != std::string::npos);
}

TEST_CASE("u1 ssh: no hook report shows nothing", "[u1-stock-screen]") {
    const auto warnings = sticky_warnings([] {
        EnvVarGuard unset(helix::u1::kRootSshDownEnv, nullptr);
        helix::u1::surface_root_ssh_down_from_hook();
    });
    CHECK(warnings.empty());
}
