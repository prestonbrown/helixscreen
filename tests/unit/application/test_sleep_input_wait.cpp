// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The sleeping main loop waits on input and queued work instead of polling
// (prestonbrown/helixscreen#2037).

#include "lvgl_test_fixture.h"
#include "main_loop_wake.h"
#include "sleep_input_wait.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "../../catch_amalgamated.hpp"

using helix::InputWakeSource;
using helix::SleepInputWait;

namespace {

/// A pipe standing in for an evdev node: readable once something is written.
struct FakeEvdev {
    int fds[2] = {-1, -1};
    lv_indev_t* indev = nullptr;
    int reads = 0;

    FakeEvdev() {
        REQUIRE(pipe(fds) == 0);
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(indev, this);
        lv_indev_set_read_cb(indev, [](lv_indev_t* i, lv_indev_data_t* data) {
            auto* self = static_cast<FakeEvdev*>(lv_indev_get_user_data(i));
            ++self->reads;
            char buf[16];
            while (read(self->fds[0], buf, sizeof(buf)) > 0) {
            }
            data->state = LV_INDEV_STATE_RELEASED;
        });
    }
    ~FakeEvdev() {
        if (indev) {
            lv_indev_delete(indev);
        }
        close(fds[0]);
        close(fds[1]);
    }
    FakeEvdev(const FakeEvdev&) = delete;
    FakeEvdev& operator=(const FakeEvdev&) = delete;

    void touch() {
        REQUIRE(write(fds[1], "x", 1) == 1);
    }
    bool timer_paused() const {
        return lv_timer_get_paused(lv_indev_get_read_timer(indev));
    }
    InputWakeSource source() const {
        return {indev, fds[0]};
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "asleep, input is read when its fd turns readable",
                 "[sleep][sleep_input_wait]") {
    helix::main_loop_wake::drain();
    FakeEvdev touch;
    SleepInputWait wait;
    wait.begin({touch.source()});
    REQUIRE(wait.active());
    CHECK(touch.timer_paused());

    CHECK_FALSE(wait.wait(0));
    CHECK(touch.reads == 0);

    touch.touch();
    CHECK(wait.wait(1000));
    CHECK(touch.reads == 1);

    wait.end();
    CHECK_FALSE(touch.timer_paused());
}

TEST_CASE_METHOD(LVGLTestFixture, "queued work cuts a sleeping wait short",
                 "[sleep][sleep_input_wait]") {
    helix::main_loop_wake::drain();
    SleepInputWait wait;
    wait.begin({});
    CHECK_FALSE(wait.wait(0));

    helix::main_loop_wake::notify();
    CHECK(wait.wait(1000));
    // Drained by the wait, so the next one sleeps again.
    CHECK_FALSE(wait.wait(0));
    wait.end();
}

TEST_CASE_METHOD(LVGLTestFixture, "the input wait leaves alone what it did not pause",
                 "[sleep][sleep_input_wait]") {
    helix::main_loop_wake::drain();
    FakeEvdev already_paused;
    lv_timer_pause(lv_indev_get_read_timer(already_paused.indev));
    FakeEvdev gone;
    InputWakeSource gone_source = gone.source();
    FakeEvdev no_fd;

    SleepInputWait wait;
    wait.begin({already_paused.source(), gone_source, {no_fd.indev, -1}});
    CHECK_FALSE(no_fd.timer_paused());

    lv_indev_delete(gone.indev);
    gone.indev = nullptr;
    gone.touch();
    CHECK_FALSE(wait.wait(0));

    wait.end();
    CHECK(already_paused.timer_paused());
}
