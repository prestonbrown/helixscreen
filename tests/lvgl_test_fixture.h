// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/update_queue.h"
#include "helix_test_fixture.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <mutex>

/**
 * @brief Test display dimensions (standard 800x480 touchscreen)
 */
constexpr int kTestDisplayWidth = 800;
constexpr int kTestDisplayHeight = 480;

/**
 * @brief Drive a display to a size for the duration of a scope, then put it back
 *
 * A resize is the only production path that reaches
 * theme_manager_refresh_layout_constants(), and the rest of the suite assumes
 * the fixture's kTestDisplayWidth x kTestDisplayHeight.
 *
 * The destructor restores the pixels only. The derived state those pixels
 * feed - XML px constants, the ui_breakpoint / ui_breakpoint_v /
 * ui_is_portrait subjects, font tiers - is process-global, and
 * helix::widget_size::current_breakpoint() reads the subject rather than the
 * display, so a scope whose refresh outlives it would decide layout for every
 * later test sharing this display; that repaint happens once, conditionally,
 * at the next case boundary (LVGLTestFixture::reclaim_display()). Mid-case
 * successors still see the scope's tier until then.
 *
 * It deliberately does NOT refresh on construction. The content-fit sweep
 * (test_widget_content_fits) measures widgets across eight geometries inside
 * these scopes and needs the token set to hold still while it does; a test that
 * wants the tier to follow the pixels calls
 * theme_manager_refresh_layout_constants() itself after opening the scope.
 * For the breakpoint subject alone, use tests/test_helpers/scoped_breakpoint.h.
 */
class ScopedResolution {
  public:
    ScopedResolution(lv_display_t* disp, int32_t w, int32_t h)
        : disp_(disp), w0_(lv_display_get_horizontal_resolution(disp)),
          h0_(lv_display_get_vertical_resolution(disp)) {
        lv_display_set_resolution(disp_, w, h);
    }
    ~ScopedResolution() {
        lv_display_set_resolution(disp_, w0_, h0_);
    }

    ScopedResolution(const ScopedResolution&) = delete;
    ScopedResolution& operator=(const ScopedResolution&) = delete;

  private:
    lv_display_t* disp_;
    int32_t w0_;
    int32_t h0_;
};

/**
 * @brief Shared LVGL test fixture base class for Catch2 tests
 *
 * Provides thread-safe singleton LVGL initialization with automatic cleanup.
 * Use as a base class with TEST_CASE_METHOD for tests requiring LVGL:
 *
 * @code
 * TEST_CASE_METHOD(LVGLTestFixture, "Test name", "[tags]") {
 *     lv_obj_t* obj = lv_obj_create(test_screen());
 *     process_lvgl(100);  // Process for 100ms
 *     REQUIRE(obj != nullptr);
 * }
 * @endcode
 *
 * Key features:
 * - Thread-safe LVGL initialization (only once per test run)
 * - Virtual display buffer for headless testing
 * - Helper methods for LVGL timer processing
 * - Test screen creation with automatic cleanup
 */
class LVGLTestFixture : public HelixTestFixture {
    static bool s_queue_initialized;

  public:
    /**
     * @brief Construct fixture and ensure LVGL is initialized
     *
     * Creates a fresh test screen for each test case.
     * Also initializes the UpdateQueue (L053/L054 pattern).
     */
    LVGLTestFixture();

    /**
     * @brief Destroy fixture and clean up test objects
     *
     * Drains pending callbacks and shuts down UpdateQueue (L053/L054 pattern).
     */
    virtual ~LVGLTestFixture();

    // Non-copyable, non-movable
    LVGLTestFixture(const LVGLTestFixture&) = delete;
    LVGLTestFixture& operator=(const LVGLTestFixture&) = delete;
    LVGLTestFixture(LVGLTestFixture&&) = delete;
    LVGLTestFixture& operator=(LVGLTestFixture&&) = delete;

    /**
     * @brief Put the shared display back the way a fresh process finds it
     *
     * Restores rotation, then resolution, then the default-display slot, then
     * (only when the geometry moved or the breakpoint subject disagrees with
     * it - the repaint walks ui_xml seven times) the derived layout state:
     * breakpoint subjects, XML px tokens, switch presets. Font tiers are the
     * one derived thing NOT restored: AssetManager never unregisters faces,
     * so a case that raised the tier leaves the extra faces available for
     * the rest of the shard; only a fresh process gets the fixture-geometry
     * font set back. Exactly what the constructor runs; public and static so
     * a case that moves the display mid-body can hand whatever runs next the
     * same state without constructing a second fixture inside a live one.
     */
    static void reclaim_display();

    /**
     * @brief Advance LVGL's VIRTUAL clock by @p ms and pump timers
     * @param ms Duration of *simulated* time to advance
     *
     * Drives animations, transitions, and anything else keyed off lv_tick_get()
     * — the test binary has no tick source of its own (see below), so this is
     * the only thing that makes LVGL's clock move.
     *
     * NOT a wall-clock wait, and NOT a way to wait on another thread: it
     * never sleeps, so no real time passes beyond the CPU the timers take. A
     * loop that accumulates the nominal `ms` toward a timeout burns its whole
     * budget at once and may never yield to the thread it is waiting on; the
     * test then fails past the end of the fixture's lifetime, so teardown
     * effects look like they happened mid-test. Use wait_until() for that, or
     * better, join the worker (e.g. ThumbnailProcessor::wait_for_completion())
     * and then drain.
     */
    void process_lvgl(int ms);

    /**
     * @brief Wait for @p condition, advancing virtual time AND yielding for real
     * @param condition Predicate, re-evaluated after every pump
     * @param timeout_ms Real-world budget, measured on steady_clock
     * @param poll_ms Virtual tick step and real sleep per iteration
     * @return true if the condition held before the timeout
     *
     * The correct helper for "a background thread will eventually publish this":
     * each pass advances the tick by @p poll_ms, drains the UpdateQueue, runs
     * ready timers, checks the predicate, then sleeps @p poll_ms of *real* time
     * so the other thread can actually run. The condition is evaluated at least
     * once even with a zero timeout.
     */
    bool wait_until(const std::function<bool()>& condition, uint32_t timeout_ms = 5000,
                    uint32_t poll_ms = 5);

    /**
     * @brief Advance VIRTUAL time until @p condition holds, never sleeping
     * @param condition Predicate, re-evaluated after every pump
     * @param budget_ms Virtual budget: how far LVGL's clock may move
     * @param step_ms Virtual tick step per iteration
     * @return true if the condition held within the budget
     *
     * For waits that only LVGL timers and the UpdateQueue can end. Nothing
     * sleeps, so a 2s virtual timer costs microseconds, and no other thread
     * gets a time slice from it: when a real thread has to publish the result,
     * use wait_until(). The condition is evaluated at least once.
     */
    bool advance_until(const std::function<bool()>& condition, uint32_t budget_ms,
                       uint32_t step_ms = 5);

    /**
     * @brief Get the test screen for this fixture
     * @return Test screen object (created fresh for each test)
     *
     * Use this as the parent for widgets created during tests.
     */
    lv_obj_t* test_screen() const {
        return m_test_screen;
    }

    /**
     * @brief Create a new test screen and set it as active
     * @return Newly created screen object
     *
     * Call this if you need a fresh screen within a test.
     * The previous screen is automatically cleaned up.
     */
    lv_obj_t* create_test_screen();

  protected:
    /**
     * @brief Ensure LVGL is initialized (thread-safe, called once)
     *
     * Creates virtual display buffer for headless testing.
     * Safe to call multiple times - initialization happens only once.
     */
    static void ensure_lvgl_initialized();

    lv_obj_t* m_test_screen; ///< Test screen for this fixture instance

    // Static initialization state
    static std::once_flag s_init_flag;
    static bool s_initialized;
    static lv_display_t* s_display;
};
