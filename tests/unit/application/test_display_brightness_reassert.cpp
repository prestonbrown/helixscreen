// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The post-boot brightness re-assert is scheduled by the board row, not by which
// backlight driver happens to be in use.

#include "backlight_backend.h"
#include "board_identity.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/armed_timers.h"
#include "test_helpers/display_manager_test_access.h"
#include "test_helpers/scoped_board.h"

#include <memory>

#include "../../catch_amalgamated.hpp"

namespace {

// A driver whose name is not the one the old gate keyed on.
class NamedBacklight : public BacklightBackend {
  public:
    bool set_brightness(int) override {
        return true;
    }
    int get_brightness() const override {
        return 100;
    }
    bool is_available() const override {
        return true;
    }
    const char* name() const override {
        return "Sysfs";
    }
};

int timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        ++n;
    }
    return n;
}

int scheduled_by(helix::board::Board board) {
    helix::test::ScopedBoard scoped(board);
    DisplayManager dm;
    DisplayManagerTestAccess::set_backlight(dm, std::make_unique<NamedBacklight>());
    const int before = timer_count();
    DisplayManagerTestAccess::schedule_brightness_reassert(dm);
    return timer_count() - before;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "brightness re-assert is scheduled on FlashForge boards only",
                 "[display][board]") {
    using helix::board::Board;
    for (Board b : {Board::Ad5m, Board::Ad5x, Board::Creator5, Board::Creator5Pro}) {
        INFO("board: " << helix::board::info(b).key);
        CHECK(scheduled_by(b) == 1);
    }
    for (Board b : {Board::Generic, Board::K1, Board::K2, Board::Cc1, Board::SnapmakerU1}) {
        INFO("board: " << helix::board::info(b).key);
        CHECK(scheduled_by(b) == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "a destroyed DisplayManager leaves no brightness re-assert armed",
                 "[display][board]") {
    // The re-assert's callback dereferences its DisplayManager, so a timer that survives
    // the manager fires into freed memory 20 s later, in whichever test is running then.
    helix::test::ScopedBoard scoped(helix::board::Board::Ad5m);
    const void* dm_addr = nullptr;
    {
        DisplayManager dm;
        dm_addr = &dm;
        DisplayManagerTestAccess::set_backlight(dm, std::make_unique<NamedBacklight>());
        DisplayManagerTestAccess::schedule_brightness_reassert(dm);
        REQUIRE(helix::test::armed_timer_count(dm_addr) == 1);
    }
    process_lvgl(1); // a cancelled timer is reaped on the next handler pass
    CHECK(helix::test::armed_timer_count(dm_addr) == 0);
}
