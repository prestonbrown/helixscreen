// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_test_fixture.h"

#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "test_helpers/refresh_period_hold_test_access.h"
#include "test_helpers/screen_hide_hold_test_access.h"
#include "theme_manager.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

using namespace helix;
using namespace helix::ui;

// Static member definitions
std::once_flag LVGLTestFixture::s_init_flag;
bool LVGLTestFixture::s_initialized = false;
lv_display_t* LVGLTestFixture::s_display = nullptr;
bool LVGLTestFixture::s_queue_initialized = false;

// Display buffer - static to persist across test cases
// Size: width * 10 lines for partial rendering mode
// IMPORTANT: Must be aligned to LV_DRAW_BUF_ALIGN (typically 4 or 8 bytes)
// Using alignas(64) for maximum compatibility with all platforms
alignas(64) static lv_color_t s_display_buf[kTestDisplayWidth * 10];

/**
 * @brief Flush callback for virtual display (no-op for testing)
 */
static void test_display_flush_cb(lv_display_t* disp, const lv_area_t* /*area*/,
                                  uint8_t* /*px_map*/) {
    // No actual rendering needed for tests
    lv_display_flush_ready(disp);
}

/**
 * @brief The screen teardown switches to while deleting the test screen
 *
 * One for the whole process: something has to stay active during the delete,
 * and LVGL sizes every screen on the display at each resolution change
 * (update_resolution() sends LV_EVENT_SIZE_CHANGED down the whole screen
 * list), so a fresh throwaway screen per case makes every later change walk
 * more and more dead trees.
 */
static lv_obj_t* blank_screen() {
    static lv_obj_t* screen = lv_obj_create(nullptr);
    return screen;
}

// The display is a process-wide singleton, so anything a previous test in
// this shard did to it - or to the default-display slot - survives into the
// next case, and every responsive decision downstream inherits it:
// theme_manager_init() republishes the breakpoint subjects from
// lv_display_get_default(), and that slot is not the fixture's by right -
// test translation units create a display in static initialisers before
// main(), and LVGL promotes the most recently created remaining display when
// one is deleted.
//
// Restore order is load-bearing: rotation first, because a rotated display
// is wrong on its own and the resolution getters swap their axes under
// ROTATION_90/270 while lv_display_set_resolution() no-ops when the raw
// pixel fields already match - so putting the geometry back from "what the
// getters report" against a rotated display writes nothing and leaves the
// axes swapped. Pixels and the default slot next.
//
// The derived layout state (breakpoint subjects, XML px tokens, fonts) is
// repainted only when something actually moved. Resolving the px tokens
// walks ui_xml once per tier - seven scans - and dev/test builds read them
// from disk by design (the compiled token table is installed-builds-only),
// so an unconditional refresh would tax every fixture construction in the
// shard. The repaint also cannot restore everything derived: font tiers are
// monotonic (AssetManager never unregisters faces - static .rodata with live
// widget pointers), so a case that raises the tier leaves the extra faces
// available for the rest of the shard. That exclusion is by design and
// process-wide; only a fresh process gets the 800x480 font set back.
void LVGLTestFixture::reclaim_display() {
    if (s_display == nullptr) {
        return;
    }
    bool geometry_moved = false;
    if (lv_display_get_rotation(s_display) != LV_DISPLAY_ROTATION_0) {
        lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_0);
        geometry_moved = true;
    }
    const int32_t w = lv_display_get_horizontal_resolution(s_display);
    const int32_t h = lv_display_get_vertical_resolution(s_display);
    if (w != kTestDisplayWidth || h != kTestDisplayHeight) {
        lv_display_set_resolution(s_display, kTestDisplayWidth, kTestDisplayHeight);
        geometry_moved = true;
    }
    if (lv_display_get_default() != s_display) {
        lv_display_set_default(s_display);
    }
    // A subject can go stale with the pixels untouched - a scope that
    // republished from moved geometry and restored only the pixels, or a
    // direct subject write (tests/test_helpers/scoped_breakpoint.h). One
    // subject read is cheap; disagreeing with the display-derived tier
    // means the derived state needs the repaint.
    if (!geometry_moved) {
        lv_subject_t* const bp = theme_manager_get_breakpoint_subject();
        if (bp == nullptr || bp->type != LV_SUBJECT_TYPE_INT) {
            return;
        }
        if (lv_subject_get_int(bp) == to_int(breakpoint_for(responsive_dimension(s_display)))) {
            return;
        }
    }
    theme_manager_refresh_layout_constants(s_display);
}

LVGLTestFixture::LVGLTestFixture() : m_test_screen(nullptr) {
    ensure_lvgl_initialized();

    // Hand this case the display state a fresh process would start with; a
    // test that wants a different display for its own body scopes that
    // inside the body (ScopedResolution, or its own display).
    reclaim_display();

    // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
    // Per L053/L054: Tests using UpdateQueue need proper lifecycle
    if (!s_queue_initialized) {
        helix::ui::update_queue_init();
        s_queue_initialized = true;
    }

    // Components register on first use, as in production. Installed per case:
    // XML teardown (deinit_xml_subjects) removes the loader.
    helix::register_xml_on_first_use();

    m_test_screen = create_test_screen();
}

LVGLTestFixture::~LVGLTestFixture() {
    // Drop every animation still registered, before anything else.
    //
    // LVGL's animation list is process-global and is NOT owned by the screen, so
    // deleting m_test_screen below does not reach an animation whose var is not a
    // widget. A test that starts an animation and returns before it finishes —
    // legitimately, e.g. to assert partway progress — leaves it live in that list,
    // and the NEXT test's process_lvgl() runs it against the previous test's dead
    // stack frame. ASan caught exactly that as a stack-use-after-return in
    // test_fixture_animation_pump.cpp: an AnimProbe stack local from one TEST_CASE
    // written by anim_timer during the following one.
    //
    // Clearing here rather than in the offending test kills the whole class: any
    // test may end with an animation in flight without contaminating its successor.
    // Safe at teardown — the only callback lv_anim_delete_all() invokes is
    // deleted_cb, never exec_cb or ready_cb, so it cannot itself touch a dead var.
    lv_anim_delete_all();

    // The shared screen hold is process-wide. A test that fails between acquire and
    // release would leave it held, so the next test's first acquire would not hide
    // its screen. Reset it while the screen it may have hidden still exists.
    helix::ScreenHideHoldTestAccess::reset(helix::active_screen_hide_hold());

    // The refresh period hold is process-wide as well, and it changes the shared display's
    // refresh timer. A leaked hold would leave every later test refreshing at its period,
    // with that period still configured.
    helix::RefreshPeriodHoldTestAccess::reset(helix::active_refresh_period_hold());

    // Clean up the test screen
    if (m_test_screen != nullptr) {
        // Switch to a different screen before deleting if this is active
        lv_obj_t* active = lv_screen_active();
        if (active == m_test_screen) {
            lv_screen_load(blank_screen());
        }
        lv_obj_delete(m_test_screen);
        m_test_screen = nullptr;
    }

    // Per L053/L054: Drain pending callbacks before shutdown
    helix::ui::UpdateQueue::instance().drain();

    // Shutdown queue
    helix::ui::update_queue_shutdown();

    // Reset static flag for next test
    s_queue_initialized = false;
}

void LVGLTestFixture::ensure_lvgl_initialized() {
    std::call_once(s_init_flag, []() {
        // Initialize LVGL library (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create virtual display for headless testing
        s_display = lv_display_create(kTestDisplayWidth, kTestDisplayHeight);
        if (s_display != nullptr) {
            lv_display_set_buffers(s_display, s_display_buf, nullptr, sizeof(s_display_buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(s_display, test_display_flush_cb);
        }
        // The app registers globals.xml before the theme. The global scope is
        // first-registration-wins, so a theme init that ran first would let a
        // component-local const of the same name (card_height) shadow it for the
        // rest of the process.
        lv_xml_register_component_from_file("A:ui_xml/globals.xml");

        // The fixture base swept the display list before this one existed, and
        // the first test to run is entitled to the same guarantee as the rest.
        ensure_displays_never_block_on_flush();

        s_initialized = true;
    });
}

lv_obj_t* LVGLTestFixture::create_test_screen() {
    // Clean up existing screen if any
    if (m_test_screen != nullptr) {
        lv_obj_delete(m_test_screen);
    }

    // Create new screen and make it active
    m_test_screen = lv_obj_create(nullptr);
    if (m_test_screen != nullptr) {
        lv_screen_load(m_test_screen);
    }

    return m_test_screen;
}

// Advances LVGL's virtual clock only, so it costs CPU and no real time. Another
// thread gets no guaranteed time slice from it; wait_until() is the helper that
// yields to other threads.
void LVGLTestFixture::process_lvgl(int ms) {
    if (ms > 0) {
        UITest::advance_ms(static_cast<uint32_t>(ms));
    }
}

bool LVGLTestFixture::wait_until(const std::function<bool()>& condition, uint32_t timeout_ms,
                                 uint32_t poll_ms) {
    if (poll_ms == 0) {
        poll_ms = 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        // Advance the virtual clock so timers and animations can come due, then
        // drain the queue and run whatever is ready. Without the tick the test
        // binary's clock never moves — nothing but zero-period one-shots fires.
        lv_tick_inc(poll_ms);
        lv_timer_handler_safe();

        if (condition()) {
            return true;
        }

        // Checked after the pump so a zero timeout still gets one full pass.
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        // Real time, so the thread we are waiting on gets to run.
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }

    spdlog::warn("[LVGLTestFixture] wait_until() timed out after {}ms", timeout_ms);
    return false;
}

bool LVGLTestFixture::advance_until(const std::function<bool()>& condition, uint32_t budget_ms,
                                    uint32_t step_ms) {
    if (UITest::advance_until(condition, budget_ms, step_ms)) {
        return true;
    }
    spdlog::warn("[LVGLTestFixture] advance_until() ran {}ms of virtual time without the "
                 "condition holding",
                 budget_ms);
    return false;
}
