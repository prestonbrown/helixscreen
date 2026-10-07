// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_slot_pool.h"

#include <cstdlib>
#include <set>

#include "../catch_amalgamated.hpp"

using helix::ThumbnailSlotPool;

namespace {

int g_live = 0;
int g_allocs = 0;
int g_fail_after = -1;

void* counting_alloc(size_t n) {
    if (g_fail_after == 0) {
        return nullptr;
    }
    if (g_fail_after > 0) {
        --g_fail_after;
    }
    ++g_live;
    ++g_allocs;
    return std::malloc(n);
}

void counting_free(void* p) {
    if (p) {
        --g_live;
    }
    std::free(p);
}

void reset_counts() {
    g_live = 0;
    g_allocs = 0;
    g_fail_after = -1;
}

} // namespace

TEST_CASE("slots are allocated as needed, capped, and reused once handed back",
          "[thumbnail][slots]") {
    reset_counts();
    {
        ThumbnailSlotPool pool(1000, 3, counting_alloc, counting_free);
        CHECK(pool.allocated() == 0); // nothing up front

        uint8_t* a = pool.acquire();
        uint8_t* b = pool.acquire();
        uint8_t* c = pool.acquire();
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(c);
        CHECK(std::set<uint8_t*>{a, b, c}.size() == 3);
        CHECK(pool.acquire() == nullptr); // the cap
        CHECK(g_allocs == 3);

        // Scrolling: one card leaves, another arrives, and gets the same buffer.
        pool.release(b);
        CHECK(pool.acquire() == b);
        CHECK(g_allocs == 3);
        pool.release(nullptr); // harmless
    }
    CHECK(g_live == 0); // the pool frees every slot it made
}

TEST_CASE("a slot allocation that fails yields no slot and is tried again later",
          "[thumbnail][slots]") {
    reset_counts();
    {
        ThumbnailSlotPool pool(1000, 2, counting_alloc, counting_free);
        g_fail_after = 0;
        CHECK(pool.acquire() == nullptr);
        CHECK(pool.allocated() == 0);
        g_fail_after = -1;
        CHECK(pool.acquire() != nullptr);
        CHECK(pool.allocated() == 1);
    }
    CHECK(g_live == 0);
}

TEST_CASE("an arena pool takes every slot in one allocation up front", "[thumbnail][slots]") {
    reset_counts();
    {
        ThumbnailSlotPool pool(1000, 3, counting_alloc, counting_free, /*arena=*/true);
        REQUIRE(pool.ok());
        CHECK(g_allocs == 1); // one block, so later decodes cannot fail on fragmentation
        CHECK(pool.allocated() == 3);

        uint8_t* a = pool.acquire();
        uint8_t* b = pool.acquire();
        uint8_t* c = pool.acquire();
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(c);
        CHECK(pool.acquire() == nullptr);
        // Disjoint slices of the one block.
        std::set<uint8_t*> slots{a, b, c};
        CHECK(slots.size() == 3);
        CHECK(*slots.rbegin() - *slots.begin() == 2000);

        pool.release(c);
        CHECK(pool.acquire() == c);
        CHECK(g_allocs == 1);
    }
    CHECK(g_live == 0);
}

TEST_CASE("an arena that cannot be allocated leaves a pool that hands out nothing",
          "[thumbnail][slots]") {
    reset_counts();
    {
        g_fail_after = 0;
        ThumbnailSlotPool pool(1000, 3, counting_alloc, counting_free, /*arena=*/true);
        CHECK_FALSE(pool.ok());
        g_fail_after = -1;
        CHECK(pool.acquire() == nullptr); // no quiet fallback to one slot at a time
        CHECK(g_allocs == 0);
    }
    CHECK(g_live == 0);
}
