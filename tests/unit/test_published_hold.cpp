// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "published_hold.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE("republish_held publishes the next image before it lets go of the old",
          "[print_select][detail_view][published_hold]") {
    std::weak_ptr<int> old_alive;
    std::vector<bool> old_alive_at_publish;
    std::vector<int> published;
    auto publish = [&](const std::shared_ptr<int>& next) {
        old_alive_at_publish.push_back(!old_alive.expired());
        published.push_back(next ? *next : 0);
    };

    std::shared_ptr<int> held = std::make_shared<int>(1);
    old_alive = held;
    helix::republish_held(held, std::make_shared<int>(2), publish);
    REQUIRE(published == std::vector<int>{2});
    CHECK(old_alive_at_publish.front()); // still readable while the widget switched
    CHECK(old_alive.expired());          // and released once it had
    REQUIRE(held);
    CHECK(*held == 2);

    // Releasing publishes nothing, then frees.
    old_alive = held;
    helix::republish_held(held, std::shared_ptr<int>(), publish);
    CHECK(published.back() == 0);
    CHECK(old_alive_at_publish.back());
    CHECK(old_alive.expired());
    CHECK_FALSE(held);
}
