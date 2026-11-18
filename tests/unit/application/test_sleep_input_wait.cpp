// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The sleeping main loop waits on input and queued work instead of polling
// (prestonbrown/helixscreen#2037).

#include "core/update_queue.h"
#include "input_wake_source_list.h"
#include "lvgl_test_fixture.h"
#include "main_loop_wake.h"
#include "sleep_input_wait.h"

#include <atomic>
#include <fcntl.h>
#include <poll.h>
#include <thread>
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

TEST_CASE_METHOD(LVGLTestFixture, "a wake source whose fd closed is dropped, not polled again",
                 "[sleep][sleep_input_wait]") {
    helix::main_loop_wake::drain();
    FakeEvdev dev;
    const int fd = dup(dev.fds[0]);
    REQUIRE(fd >= 0);

    SleepInputWait wait;
    wait.begin({{dev.indev, fd}});
    REQUIRE(dev.timer_paused());
    close(fd);

    // POLLNVAL answers at once; a source kept after it would make every wait return at once.
    wait.wait(0);
    CHECK_FALSE(dev.timer_paused());
    CHECK_FALSE(wait.wait(0));
    CHECK(dev.reads == 0);
    wait.end();
}

TEST_CASE_METHOD(LVGLTestFixture, "a backend's wake sources forget a device LVGL deleted",
                 "[sleep][sleep_input_wait]") {
    helix::InputWakeSourceList list;
    lv_indev_t* kept = lv_indev_create();
    lv_indev_t* gone = lv_indev_create();
    list.add(kept, 7);
    list.add(gone, 8);
    REQUIRE(list.sources().size() == 2);

    lv_indev_delete(gone);
    REQUIRE(list.sources().size() == 1);
    CHECK(list.sources()[0].indev == kept);
    CHECK(list.sources()[0].fd == 7);

    // A device created after the delete may reuse the freed address; it is not a source.
    lv_indev_t* reused = lv_indev_create();
    CHECK(list.sources().size() == 1);
    lv_indev_delete(reused);
    lv_indev_delete(kept);
    CHECK(list.sources().empty());
}

namespace {
std::atomic<int> g_enqueue_hook_calls{0};
void count_enqueue() {
    ++g_enqueue_hook_calls;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "work queued from another thread calls the enqueue hook",
                 "[sleep][sleep_input_wait][update_queue]") {
    auto& queue = helix::ui::UpdateQueue::instance();
    g_enqueue_hook_calls = 0;
    queue.set_enqueue_hook(&count_enqueue);
    std::thread([] { helix::ui::queue_update("test::from_bg", [] {}); }).join();
    CHECK(g_enqueue_hook_calls == 1);

    queue.set_enqueue_hook(nullptr);
    std::thread([] { helix::ui::queue_update("test::from_bg_unhooked", [] {}); }).join();
    CHECK(g_enqueue_hook_calls == 1);
    queue.drain();
}
