// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_virtual_list.h"

#include "../catch_amalgamated.hpp"

using helix::ui::compute_window;
using helix::ui::VirtualWindow;
using helix::ui::whole_rows;

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

TEST_CASE("compute_window over row tops agrees with the fixed stride", "[virtual_list][window]") {
    for (int total : {1, 2, 7, 100}) {
        for (int scroll : {-50, 0, 1, 49, 50, 333, 4770, 100000}) {
            for (int stride : {1, 44, 57}) {
                for (int overscan : {0, 2}) {
                    // Past the end the fixed stride keeps counting rows that do not
                    // exist; row tops stop at the last one.
                    if (scroll >= total * stride)
                        continue;
                    std::vector<int> tops;
                    for (int i = 0; i <= total; i++)
                        tops.push_back(i * stride);
                    const VirtualWindow a = compute_window(scroll, 240, stride, total, overscan);
                    const VirtualWindow b = compute_window(scroll, 240, tops, overscan);
                    INFO("total " << total << " scroll " << scroll << " stride " << stride);
                    CHECK(b.first == a.first);
                    CHECK(b.last == a.last);
                    CHECK(b.leading_px == a.leading_px);
                    CHECK(b.trailing_px == a.trailing_px);
                }
            }
        }
    }
}

TEST_CASE("compute_window over row tops follows each row's own height", "[virtual_list][window]") {
    // Heights 40, 100, 40, 40, 200, 40.
    const std::vector<int> tops = {0, 40, 140, 180, 220, 420, 460};

    SECTION("a tall row alone fills the viewport") {
        const VirtualWindow w = compute_window(250, 100, tops, 0);
        CHECK(w.first == 4);
        CHECK(w.last == 5);
        CHECK(w.leading_px == 220);
        CHECK(w.trailing_px == 40);
    }
    SECTION("short rows are counted by their own height") {
        const VirtualWindow w = compute_window(150, 60, tops, 1);
        CHECK(w.first == 1);
        CHECK(w.last == 5);
        CHECK(w.leading_px == 40);
        CHECK(w.trailing_px == 40);
    }
    SECTION("no rows") {
        const VirtualWindow w = compute_window(0, 100, std::vector<int>{0}, 2);
        CHECK(w.first == 0);
        CHECK(w.last == 0);
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

TEST_CASE("whole_rows table", "[virtual_list][window]") {
    // Rows of stride 50 below a 10 px gap: row r's card spans [50r + 10, 50r + 50).
    struct WholeCase {
        const char* name;
        int scroll_y, viewport_h, total, first, last;
    };
    const WholeCase cases[] = {
        {"top: rows ending by the bottom edge", 0, 200, 100, 0, 4},
        {"a row cut at the bottom is not whole", 0, 190, 100, 0, 3},
        {"its gap scrolled off, the top row is whole", 10, 190, 100, 0, 4},
        {"a pixel into the card, the top row is cut", 11, 190, 100, 1, 4},
        {"bottom of the list", 4800, 200, 100, 96, 100},
        {"viewport shorter than a card", 15, 30, 100, 1, 1},
        {"overscroll above the top", -40, 200, 100, 0, 3},
        {"empty list", 0, 200, 0, 0, 0},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const VirtualWindow w = whole_rows(c.scroll_y, c.viewport_h, 50, 10, c.total);
            CHECK(w.first == c.first);
            CHECK(w.last == c.last);
        }
    }
}
