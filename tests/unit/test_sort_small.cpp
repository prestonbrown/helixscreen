// SPDX-License-Identifier: GPL-3.0-or-later

#include "sort_small.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::sort_small;

TEST_CASE("sort_small orders ascending by default", "[sort_small]") {
    std::vector<int> v{5, 3, 9, 1, 3, 7};
    sort_small(v);
    CHECK(v == std::vector<int>{1, 3, 3, 5, 7, 9});
}

TEST_CASE("sort_small leaves empty and one-element ranges alone", "[sort_small]") {
    std::vector<std::string> empty;
    sort_small(empty);
    CHECK(empty.empty());

    std::vector<std::string> one{"only"};
    sort_small(one);
    CHECK(one == std::vector<std::string>{"only"});
}

TEST_CASE("sort_small keeps equal elements in input order", "[sort_small]") {
    // Keyed on .first only; .second records the input position.
    std::vector<std::pair<int, int>> v{{2, 0}, {1, 1}, {2, 2}, {1, 3}, {0, 4}, {2, 5}};
    sort_small(v, [](const auto& a, const auto& b) { return a.first < b.first; });
    CHECK(v == std::vector<std::pair<int, int>>{{0, 4}, {1, 1}, {1, 3}, {2, 0}, {2, 2}, {2, 5}});
}

TEST_CASE("sort_small honours a descending comparator", "[sort_small]") {
    std::vector<std::string> v{"b", "a", "c"};
    sort_small(v, [](const std::string& a, const std::string& b) { return a > b; });
    CHECK(v == std::vector<std::string>{"c", "b", "a"});
}

TEST_CASE("sort_small sorts an iterator range inside a larger array", "[sort_small]") {
    std::array<int, 6> a{9, 4, 2, 8, 1, 0};
    sort_small(a.begin() + 1, a.begin() + 5);
    CHECK(a == std::array<int, 6>{9, 1, 2, 4, 8, 0});
}

TEST_CASE("sort_small handles already sorted and reversed input", "[sort_small]") {
    std::vector<int> sorted{1, 2, 3, 4};
    sort_small(sorted);
    CHECK(sorted == std::vector<int>{1, 2, 3, 4});

    std::vector<int> reversed{4, 3, 2, 1};
    sort_small(reversed);
    CHECK(reversed == std::vector<int>{1, 2, 3, 4});
}
