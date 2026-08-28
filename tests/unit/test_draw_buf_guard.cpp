// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// safe_draw_buf_destroy() is the one correct way to free an lv_draw_buf_t a
// parallel draw unit may still be reading (#929: v0.99.54 telemetry pinned the
// faulting source pointer to a freed mmap'd cache_buf_). The sequence was
// hand-copied across six sites in the two G-code renderers and had already
// drifted - lv_draw_wait_for_finish() was missing entirely from
// GCodeGLESRenderer::clear_cached_frame(), which frees the same buffer the
// destructor guards while citing this bug number.
//
// The drain is the part that cannot be inspected from outside, so it is
// measured here rather than assumed: LVGL's lv_draw_wait_for_finish() walks
// the draw-unit list calling each unit's wait_for_finish_cb, so a unit whose
// callback counts its own invocations reports whether the drain happened. That
// costs nothing at runtime and fails in a plain build, without ASan.

#include "../lvgl_test_fixture.h"
#include "lv_draw_buf_guard.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/draw/lv_draw_private.h"
#include "system/crash_handler.h"

#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

int g_wait_calls = 0;

int32_t spy_wait_for_finish(lv_draw_unit_t*) {
    g_wait_calls++;
    return 0;
}

/// Never takes work. dispatch_cb is the one callback lv_draw_dispatch() calls
/// without a null check, so it has to exist even though this unit draws
/// nothing.
int32_t spy_dispatch(lv_draw_unit_t*, lv_layer_t*) {
    return LV_DRAW_UNIT_IDLE;
}

/// A draw unit that records every lv_draw_wait_for_finish() the code under test
/// performs.
///
/// Registered and unregistered around one test rather than left in place: LVGL
/// has no lv_draw_delete_unit(), units live until lv_deinit(), and the test
/// binary never deinits. A permanent extra unit would take unit_cnt from 1 to
/// 2, which switches lv_draw_get_available_task() to the multi-unit
/// independence path for every later test in the shard.
class WaitForFinishSpy {
  public:
    WaitForFinishSpy() {
        unit_ = static_cast<lv_draw_unit_t*>(lv_draw_create_unit(sizeof(lv_draw_unit_t)));
        REQUIRE(unit_ != nullptr);
        unit_->name = "helix_test_wait_spy";
        unit_->dispatch_cb = spy_dispatch;
        unit_->wait_for_finish_cb = spy_wait_for_finish;
        g_wait_calls = 0;
    }

    ~WaitForFinishSpy() {
        // lv_draw_create_unit() pushes onto the head, and nothing else in this
        // test creates one, so the spy is still the head.
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
        return g_wait_calls;
    }

  private:
    lv_draw_unit_t* unit_ = nullptr;
};

/// Breadcrumb categories are truncated to 7 characters (crash_handler.cpp's
/// `char category[8]`), so a probe tag has to fit or the assertion below hunts
/// for a string the ring never held. Production's own tags ("cache_buf",
/// "gles_draw_buf") are truncated the same way.
constexpr const char* kDestroyTag = "tstbufA";
constexpr const char* kNullTag = "tstbufB";

/// The breadcrumb ring as lines, mirroring the pipe+dump_to_fd helper in
/// test_crash_handler.cpp.
std::vector<std::string> capture_breadcrumb_lines() {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    crash_handler::breadcrumb::dump_to_fd(fds[1]);
    ::close(fds[1]);
    std::string all;
    char chunk[4096];
    ssize_t n;
    while ((n = ::read(fds[0], chunk, sizeof(chunk))) > 0) {
        all.append(chunk, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    std::vector<std::string> lines;
    size_t pos = 0, nl;
    while ((nl = all.find('\n', pos)) != std::string::npos) {
        lines.push_back(all.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

size_t count_lines_containing(const std::vector<std::string>& lines, const std::string& needle) {
    size_t hits = 0;
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            hits++;
        }
    }
    return hits;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "safe_draw_buf_destroy drains the draw units before freeing",
                 "[gcode][drawbuf][929]") {
    WaitForFinishSpy spy;

    lv_draw_buf_t* buf = lv_draw_buf_create(16, 16, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);
    REQUIRE(spy.waits() == 0);

    helix::safe_draw_buf_destroy(buf, "test_buf");

    // The whole point of the helper: no in-flight draw task can still be
    // reading the buffer by the time lv_draw_buf_destroy() runs. Dropping the
    // lv_draw_wait_for_finish() call - which is exactly what
    // GCodeGLESRenderer::clear_cached_frame() had done - leaves this at 0.
    CHECK(spy.waits() == 1);

    // And the caller is not left holding the pointer it just handed over.
    CHECK(buf == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_draw_buf_destroy leaves the #929 breadcrumbs",
                 "[gcode][drawbuf][929]") {
    // Breadcrumbs are what turned #929 from an unattributable SIGSEGV into a
    // fix, and only one of the six hand-copied sites had them. Both sides of
    // the destroy are marked so a crash inside the drain is distinguishable
    // from one inside the free.
    lv_draw_buf_t* buf = lv_draw_buf_create(8, 8, LV_COLOR_FORMAT_ARGB8888, 0);
    REQUIRE(buf != nullptr);

    helix::safe_draw_buf_destroy(buf, kDestroyTag);
    REQUIRE(buf == nullptr);

    const auto crumbs = capture_breadcrumb_lines();
    CHECK(count_lines_containing(crumbs, std::string(kDestroyTag) + " destroy_pre") == 1);
    CHECK(count_lines_containing(crumbs, std::string(kDestroyTag) + " destroy_post") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_draw_buf_destroy on a null buffer does nothing at all",
                 "[gcode][drawbuf][929]") {
    // Every renderer teardown calls this unconditionally, most of them on a
    // buffer that was never allocated. A drain per null pointer would join the
    // draw units on paths that have nothing to free, and a breadcrumb per null
    // pointer would flood the ring that #929 is diagnosed from.
    WaitForFinishSpy spy;

    lv_draw_buf_t* buf = nullptr;
    helix::safe_draw_buf_destroy(buf, kNullTag);

    CHECK(buf == nullptr);
    CHECK(spy.waits() == 0);

    const auto crumbs = capture_breadcrumb_lines();
    CHECK(count_lines_containing(crumbs, kNullTag) == 0);
}
