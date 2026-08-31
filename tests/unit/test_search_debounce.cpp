// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for SearchDebounce, the shared search-input debounce. The
// behaviour that matters: a burst of keystrokes triggers the callback ONCE
// (after the delay), clearing fires immediately, cancel drops a pending
// trigger, and a destroyed owner never fires.

#include "ui_search_debounce.h"

#include "../lvgl_ui_test_fixture.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::SearchDebounce;

namespace {
constexpr uint32_t kDelayMs = helix::ui::kDefaultSearchDebounceMs;

class DebounceFixture : public LVGLUITestFixture {
  public:
    std::vector<std::string> fired;
    std::unique_ptr<SearchDebounce> debounce =
        std::make_unique<SearchDebounce>([this](const std::string& q) { fired.push_back(q); });
};
} // namespace

TEST_CASE("SearchDebounce coalesces a keystroke burst into one fire", "[ui][debounce]") {
    DebounceFixture f;
    REQUIRE(f.fired.empty());

    for (const char* q : {"p", "po", "pol", "poly", "polym", "polyma", "polymak", "polymaker"}) {
        f.debounce->schedule(q);
        // Fast typists must never see a synchronous render mid-burst.
        CHECK(f.fired.empty());
    }
    CHECK(f.debounce->pending());

    f.process_lvgl(kDelayMs - 10);
    CHECK(f.fired.empty());

    f.process_lvgl(20);
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0] == "polymaker");
    CHECK_FALSE(f.debounce->pending());
}

TEST_CASE("SearchDebounce empty query applies immediately", "[ui][debounce]") {
    DebounceFixture f;
    f.debounce->schedule("poly");
    f.process_lvgl(kDelayMs);
    REQUIRE(f.fired.size() == 1);

    // Clearing the filter shows everything again - no debounce wait.
    f.debounce->schedule("");
    REQUIRE(f.fired.size() == 2);
    CHECK(f.fired[1].empty());
    CHECK_FALSE(f.debounce->pending());
}

TEST_CASE("SearchDebounce empty query fires immediately with nothing pending", "[ui][debounce]") {
    DebounceFixture f;
    f.debounce->schedule("");
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0].empty());
}

TEST_CASE("SearchDebounce reschedule extends the countdown", "[ui][debounce]") {
    DebounceFixture f;
    f.debounce->schedule("a");
    f.process_lvgl(kDelayMs - 50);
    f.debounce->schedule("ab");
    // Only 250ms since the last keystroke: must not fire yet.
    f.process_lvgl(kDelayMs - 60);
    CHECK(f.fired.empty());
    f.process_lvgl(100);
    REQUIRE(f.fired.size() == 1);
    CHECK(f.fired[0] == "ab");
}

TEST_CASE("SearchDebounce cancel drops the pending trigger", "[ui][debounce]") {
    DebounceFixture f;
    f.debounce->schedule("poly");
    REQUIRE(f.debounce->pending());
    f.debounce->cancel();
    CHECK_FALSE(f.debounce->pending());
    f.process_lvgl(kDelayMs * 2);
    CHECK(f.fired.empty());
}

TEST_CASE("SearchDebounce destructor with pending timer never fires", "[ui][debounce]") {
    DebounceFixture f;
    f.debounce->schedule("poly");
    REQUIRE(f.debounce->pending());
    f.debounce.reset();
    f.process_lvgl(kDelayMs * 2);
    CHECK(f.fired.empty());
}
