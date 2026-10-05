// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises patches/lvgl_event_stack_array.patch, shipped LVGL code with
//                 no HelixScreen header to include

/**
 * @file test_event_addr_on_stack.cpp
 * @brief The event-slot stack bound holds for a stack at the top of the address space.
 *
 * lv_event_mark_deleted() sets `deleted` only on slots lv_event_addr_on_stack()
 * accepts. A 32-bit userland on a 64-bit kernel puts the main stack just below
 * 0xff000000, where `sp + 16 MB` overflows. If the bound wraps, every slot is
 * rejected, an object deleted from inside its own event is never flagged, and
 * dispatch walks the freed object (prestonbrown/helixscreen#1601).
 *
 * @see lib/lvgl/src/misc/lv_event.c lv_event_mark_deleted()
 */

#include "lvgl/lvgl.h"
#include "misc/lv_event_private.h"

#include <cstdint>

#include "../catch_amalgamated.hpp"

namespace {
constexpr uintptr_t kSpan = uintptr_t{16} << 20;
} // namespace

TEST_CASE("event slot stack bound accepts frames above sp", "[lvgl][event_stack]") {
    const uintptr_t sp = 0x7ffd0000u;
    CHECK(lv_event_addr_on_stack(sp, sp));
    CHECK(lv_event_addr_on_stack(sp + 0x3dc, sp));
    CHECK(lv_event_addr_on_stack(sp + kSpan - 1, sp));
    CHECK_FALSE(lv_event_addr_on_stack(sp + kSpan, sp));
    CHECK_FALSE(lv_event_addr_on_stack(sp - sizeof(void*), sp));
    CHECK_FALSE(lv_event_addr_on_stack(0x1000, sp));
}

TEST_CASE("event slot stack bound does not wrap near the top of the address space",
          "[lvgl][event_stack]") {
    // Same layout as the field report (slot 0x3dc above sp, sp within 16 MB of
    // the top), placed at this host's UINTPTR_MAX so a 64-bit host wraps too.
    const uintptr_t sp = UINTPTR_MAX - (0xffffffffu - 0xffb68e30u);
    CHECK(lv_event_addr_on_stack(sp + 0x3dc, sp));
    CHECK(lv_event_addr_on_stack(UINTPTR_MAX - 7, sp));
    CHECK_FALSE(lv_event_addr_on_stack(sp - 4, sp));
    CHECK_FALSE(lv_event_addr_on_stack(0x00b68e00u, sp));
}
