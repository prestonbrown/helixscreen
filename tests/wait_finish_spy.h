// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WaitForFinishSpy: a draw unit that records every lv_draw_wait_for_finish()
// the code under test performs, so a teardown that claims to drain the draw
// units before freeing a buffer can be held to it in a plain build (see
// tests/unit/test_draw_buf_guard.cpp for the original use).
//
// Include after catch_amalgamated.hpp — the constructor asserts.

#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/draw/lv_draw_private.h"

#include "catch_amalgamated.hpp"

// One spy is alive per test, so a single shared counter is sufficient; the
// constructor zeroes it.
inline int g_wait_spy_calls = 0;

class WaitForFinishSpy {
  public:
    WaitForFinishSpy() {
        unit_ = static_cast<lv_draw_unit_t*>(lv_draw_create_unit(sizeof(lv_draw_unit_t)));
        REQUIRE(unit_ != nullptr);
        unit_->name = "helix_test_wait_spy";
        unit_->dispatch_cb = spy_dispatch;
        unit_->wait_for_finish_cb = spy_wait_for_finish;
        g_wait_spy_calls = 0;
    }

    ~WaitForFinishSpy() {
        // lv_draw_create_unit() pushes onto the head, and nothing else in a
        // test creates one while the spy is alive, so the spy is still the
        // head.
        lv_draw_global_info_t& info = LV_GLOBAL_DEFAULT()->draw_info;
        if (info.unit_head == unit_) {
            info.unit_head = unit_->next;
            info.unit_cnt--;
            lv_free(unit_);
        }
    }

    WaitForFinishSpy(const WaitForFinishSpy&) = delete;
    WaitForFinishSpy& operator=(const WaitForFinishSpy&) = delete;

    int waits() const {
        return g_wait_spy_calls;
    }

  private:
    // Never takes work. dispatch_cb is the one callback lv_draw_dispatch()
    // calls without a null check, so it has to exist even though this unit
    // draws nothing.
    static int32_t spy_dispatch(lv_draw_unit_t*, lv_layer_t*) {
        return LV_DRAW_UNIT_IDLE;
    }

    static int32_t spy_wait_for_finish(lv_draw_unit_t*) {
        g_wait_spy_calls++;
        return 0;
    }

    lv_draw_unit_t* unit_ = nullptr;
};
