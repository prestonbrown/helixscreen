// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_virtual_list.h"

#include "../catch_amalgamated.hpp"

using helix::ui::compute_window;
using helix::ui::VirtualWindow;

namespace {
struct Case {
    const char* name;
    int scroll_y, viewport_h, stride, total, overscan;
    int first, last, leading, trailing;
};
} // namespace

TEST_CASE("compute_window table", "[virtual_list][window]") {
    // stride 50 throughout unless the case is about the stride itself
    const Case cases[] = {
        {"empty list", 0, 200, 50, 0, 2, 0, 0, 0, 0},
        {"negative total", 0, 200, 50, -3, 2, 0, 0, 0, 0},
        {"top, overscan clamped at start", 0, 200, 50, 100, 2, 0, 7, 0, 93 * 50},
        {"middle, overscan at both ends", 1000, 200, 50, 100, 2, 18, 27, 18 * 50, 73 * 50},
        {"bottom, overscan clamped at end", 4800, 200, 50, 100, 2, 94, 100, 94 * 50, 0},
        {"partial last row still counts", 4770, 200, 50, 100, 0, 95, 100, 95 * 50, 0},
        {"partial first row is included", 75, 100, 50, 100, 0, 1, 4, 50, 96 * 50},
        {"list shorter than viewport", 0, 1000, 50, 3, 2, 0, 3, 0, 0},
        {"scroll beyond end keeps the last row", 99999, 200, 50, 10, 1, 9, 10, 9 * 50, 0},
        {"negative scroll acts as zero", -300, 200, 50, 100, 1, 0, 6, 0, 94 * 50},
        {"zero stride does not divide by zero", 10, 5, 0, 4, 0, 4 - 1, 4, 3, 0},
        {"zero viewport", 100, 0, 50, 100, 0, 2, 3, 100, 97 * 50},
        {"single row", 0, 200, 50, 1, 2, 0, 1, 0, 0},
    };

    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            VirtualWindow w =
                compute_window(c.scroll_y, c.viewport_h, c.stride, c.total, c.overscan);
            CHECK(w.first == c.first);
            CHECK(w.last == c.last);
            CHECK(w.leading_px == c.leading);
            CHECK(w.trailing_px == c.trailing);
        }
    }
}

TEST_CASE("compute_window: any non-empty list yields a non-empty window",
          "[virtual_list][window]") {
    for (int total : {1, 2, 7, 100}) {
        for (int scroll : {-50, 0, 1, 333, 100000}) {
            for (int stride : {0, 1, 44, 57}) {
                VirtualWindow w = compute_window(scroll, 240, stride, total, 2);
                CHECK(w.first >= 0);
                CHECK(w.first < w.last);
                CHECK(w.last <= total);
            }
        }
    }
}

TEST_CASE("assign_pool_slots keeps slots whose item stays in the window", "[virtual_list][pool]") {
    using helix::ui::assign_pool_slots;

    SECTION("one row scrolls out, one in: only one slot changes") {
        std::vector<ssize_t> items = {10, 11, 12, 13, 14};
        const auto order = assign_pool_slots(items, 11, 16);
        CHECK(order == std::vector<size_t>{1, 2, 3, 4, 0});
        CHECK(items == std::vector<ssize_t>{15, 11, 12, 13, 14});
    }
    SECTION("scrolling back reuses the slot that left") {
        std::vector<ssize_t> items = {15, 11, 12, 13, 14};
        const auto order = assign_pool_slots(items, 10, 15);
        CHECK(order == std::vector<size_t>{0, 1, 2, 3, 4});
        CHECK(items == std::vector<ssize_t>{10, 11, 12, 13, 14});
    }
    SECTION("a window smaller than the pool frees the rest") {
        std::vector<ssize_t> items = {-1, 4, 5, 6};
        const auto order = assign_pool_slots(items, 5, 7);
        CHECK(order == std::vector<size_t>{2, 3});
        CHECK(items == std::vector<ssize_t>{-1, -1, 5, 6});
    }
    SECTION("a window larger than the pool shows what fits") {
        std::vector<ssize_t> items = {-1, -1};
        const auto order = assign_pool_slots(items, 0, 9);
        CHECK(order == std::vector<size_t>{0, 1});
        CHECK(items == std::vector<ssize_t>{0, 1});
    }
    SECTION("two slots claiming one item: the second is reassigned") {
        std::vector<ssize_t> items = {3, 3, 4};
        const auto order = assign_pool_slots(items, 3, 6);
        CHECK(order == std::vector<size_t>{0, 2, 1});
        CHECK(items == std::vector<ssize_t>{3, 5, 4});
    }
}
