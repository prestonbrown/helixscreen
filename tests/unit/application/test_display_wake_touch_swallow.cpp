// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_wake_touch_swallow.cpp
 * @brief The touch that wakes the display acts on nothing until the finger lifts
 *
 * Asleep, dimmed or under a screensaver, a press wakes the display at once and is
 * then swallowed for as long as the finger stays down: no click, long press or
 * scroll on whatever lies beneath it. The next touch is an ordinary one.
 *
 * Each case runs on both input chains DisplayManager builds. FBDEV stands for every
 * embedded backend, whose pointer gets the sleep-aware read wrapper; SDL (desktop and
 * Android) keeps its own read callback, so its wake touch reaches LVGL and the wake
 * has to cancel it there.
 *
 * Harness: the clock, the reads and the idle tick are driven by hand, in the order the
 * main loop runs them (indev read, then check_display_sleep(), then the update queue).
 * The fixture's pump runs only one-shot timers, so timers the wake itself creates are
 * fired here when due, as lv_timer_handler() would.
 */

#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "app_constants.h"
#include "backlight_backend.h"
#include "display_backend.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "misc/lv_timer_private.h" // period/last_run/timer_cb: fires the wake's own timers
#include "test_helpers/display_manager_test_access.h"
#include "wake_touch_latch.h"

#if HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

#include <algorithm>
#include <memory>
#include <vector>

#include "../../catch_amalgamated.hpp"

namespace {

constexpr int kReadStepMs = 10;

// The button the wake touch lands on, well inside the 800x480 fixture display.
constexpr lv_point_t kOnButton{120, 120};

// A scrollable list to the right of the button, and a drag down its middle.
constexpr lv_point_t kListTop{500, 120};
constexpr lv_point_t kListDragged{500, 300};

lv_indev_data_t g_raw{};

void raw_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_raw.point;
    data->state = g_raw.state;
}

/// A backend whose pointer reads g_raw. FBDEV gets the embedded input chain, SDL its own.
class RawPointerBackend : public DisplayBackend {
  public:
    explicit RawPointerBackend(DisplayBackendType type) : m_type(type) {}

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, raw_read);
        return indev;
    }
    DisplayBackendType type() const override {
        return m_type;
    }
    const char* name() const override {
        return "RawPointer";
    }
    bool is_available() const override {
        return true;
    }

  private:
    DisplayBackendType m_type;
};

/// A dimmable backlight, so the idle tick can dim again right after a wake.
class DimmableBacklight : public BacklightBackend {
  public:
    bool set_brightness(int percent) override {
        brightness = percent;
        return true;
    }
    int get_brightness() const override {
        return brightness;
    }
    bool is_available() const override {
        return true;
    }
    const char* name() const override {
        return "Dimmable";
    }
    bool supports_dimming() const override {
        return true;
    }
    int brightness = 100;
};

struct Counts {
    int pressed = 0;
    int clicked = 0;
    int long_pressed = 0;
    int scroll_begin = 0;
};

Counts g_counts;

void count_cb(lv_event_t* e) {
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        ++g_counts.pressed;
        break;
    case LV_EVENT_CLICKED:
        ++g_counts.clicked;
        break;
    case LV_EVENT_LONG_PRESSED:
        ++g_counts.long_pressed;
        break;
    case LV_EVENT_SCROLL_BEGIN:
        ++g_counts.scroll_begin;
        break;
    default:
        break;
    }
}

enum class Idle { Sleep, Dim, Screensaver };

const char* idle_name(Idle idle) {
    switch (idle) {
    case Idle::Sleep:
        return "sleep";
    case Idle::Dim:
        return "dim";
    case Idle::Screensaver:
        return "screensaver";
    }
    return "?";
}

/// A DisplayManager with its pointer built the way init() builds it, a button and a
/// scrollable list under it, and the idle state the case asks for.
class WakeChain {
  public:
    explicit WakeChain(DisplayBackendType type) {
        ensure_displays_never_block_on_flush();
        g_counts = Counts{};
        g_raw = lv_indev_data_t{};
        g_raw.point = {0, 0};
        DisplayManagerTestAccess::set_active_instance(&mgr);
        DisplayManagerTestAccess::set_backend(mgr, std::make_unique<RawPointerBackend>(type));
        // A 1 s dim timeout against an idle clock go_idle() leaves 5 s old: a wake that
        // forgot to count as activity would dim again on the next tick.
        DisplayManagerTestAccess::set_backlight(mgr, std::make_unique<DimmableBacklight>());
        mgr.set_dim_timeout(1);
        DisplayManagerTestAccess::create_input_devices(mgr);
        pointer_ = mgr.pointer_input();
        REQUIRE(pointer_ != nullptr);
        remember_timers();
    }
    ~WakeChain() {
#if HELIX_ENABLE_SCREENSAVER
        ScreensaverManager::instance().stop();
#endif
        helix::ui::UpdateQueue::instance().drain();
        DisplayManagerTestAccess::set_display_sleeping(mgr, false);
        DisplayManagerTestAccess::set_display_dimmed(mgr, false);
        DisplayManagerTestAccess::delete_pointer_input(mgr);
        DisplayManagerTestAccess::set_active_instance(previous_);
        g_raw = lv_indev_data_t{};
    }
    WakeChain(const WakeChain&) = delete;
    WakeChain& operator=(const WakeChain&) = delete;

    void build_targets(lv_obj_t* screen) {
        button = lv_button_create(screen);
        lv_obj_set_pos(button, 60, 60);
        lv_obj_set_size(button, 160, 160);
        for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_CLICKED, LV_EVENT_LONG_PRESSED}) {
            lv_obj_add_event_cb(button, count_cb, code, nullptr);
        }

        list = lv_obj_create(screen);
        lv_obj_set_pos(list, 400, 60);
        lv_obj_set_size(list, 200, 300);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_t* content = lv_obj_create(list);
        lv_obj_set_size(content, 150, 2000);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(list, count_cb, LV_EVENT_SCROLL_BEGIN, nullptr);
        lv_obj_update_layout(screen);
    }

    /// Puts the display into @p idle with the last input long past, as the idle tick
    /// would have left it.
    void go_idle(Idle idle) {
        lv_display_trigger_activity(nullptr);
        lv_tick_inc(5000);
        switch (idle) {
        case Idle::Sleep:
            DisplayManagerTestAccess::set_display_sleeping(mgr, true);
            break;
        case Idle::Dim:
            DisplayManagerTestAccess::set_display_dimmed(mgr, true);
            break;
        case Idle::Screensaver:
#if HELIX_ENABLE_SCREENSAVER
            ScreensaverManager::instance().start(ScreensaverType::FLYING_TOASTERS);
            DisplayManagerTestAccess::set_display_dimmed(mgr, true);
            DisplayManagerTestAccess::set_screensaver_active(mgr, true);
            remember_timers();
#endif
            break;
        }
        REQUIRE(asleep_or_dimmed());
        mgr.check_display_sleep();
        REQUIRE(asleep_or_dimmed()); // nothing but a touch may wake it
    }

    bool asleep_or_dimmed() const {
        return mgr.is_display_sleeping() || mgr.is_display_dimmed();
    }

    /// One main-loop pass with the finger reporting @p state at @p point.
    void step(lv_indev_state_t state, lv_point_t point) {
        g_raw.state = state;
        g_raw.point = point;
        lv_tick_inc(kReadStepMs);
        lv_indev_read(pointer_);
        mgr.check_display_sleep();
        helix::ui::UpdateQueue::instance().drain();
        fire_due_new_timers();
    }

    void hold(lv_indev_state_t state, lv_point_t point, int ms) {
        for (int elapsed = 0; elapsed < ms; elapsed += kReadStepMs) {
            step(state, point);
        }
    }

    /// Down at @p at for @p ms, then up, then a few idle reads.
    void tap(lv_point_t at, int ms) {
        hold(LV_INDEV_STATE_PRESSED, at, ms);
        hold(LV_INDEV_STATE_RELEASED, at, 50);
    }

    DisplayManager mgr;
    lv_obj_t* button = nullptr;
    lv_obj_t* list = nullptr;

  private:
    void remember_timers() {
        known_timers_.clear();
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            known_timers_.push_back(t);
        }
    }

    bool known(lv_timer_t* t) const {
        return std::find(known_timers_.begin(), known_timers_.end(), t) != known_timers_.end();
    }

    /// Runs, once each, the timers created since the chain was built that are due now.
    void fire_due_new_timers() {
        std::vector<lv_timer_t*> fired;
        for (;;) {
            lv_timer_t* due = nullptr;
            for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
                if (known(t) || std::find(fired.begin(), fired.end(), t) != fired.end() ||
                    t->paused || t->timer_cb == nullptr) {
                    continue;
                }
                if (lv_tick_elaps(t->last_run) >= t->period) {
                    due = t;
                    break;
                }
            }
            if (!due) {
                return;
            }
            fired.push_back(due);
            due->last_run = lv_tick_get();
            due->timer_cb(due);
        }
    }

    DisplayManager* previous_ = DisplayManager::instance();
    lv_indev_t* pointer_ = nullptr;
    std::vector<lv_timer_t*> known_timers_;
};

std::vector<Idle> idle_modes() {
    std::vector<Idle> modes{Idle::Sleep, Idle::Dim};
#if HELIX_ENABLE_SCREENSAVER
    modes.push_back(Idle::Screensaver);
#endif
    return modes;
}

} // namespace

// ============================================================================
// The latch on its own
// ============================================================================

namespace {
using S = helix::WakeTouchLatch::Sample;
constexpr uint32_t kWakeAt = 1000;
constexpr uint32_t kWindowEnd = kWakeAt + helix::WakeTouchLatch::kRearmMs;
} // namespace

TEST_CASE("the wake latch swallows the waking touch until it lifts", "[display][wake_swallow]") {
    helix::WakeTouchLatch latch;
    int asked = 0;
    bool waking = true;
    auto wakes = [&](int32_t, int32_t) {
        ++asked;
        return waking;
    };

    // The finger rests at (5,6), then lands at (100,100) and drags while the display sleeps.
    CHECK_FALSE(latch.filter(S{false, 5, 6}, kWakeAt - 30, wakes).pressed);
    S out = latch.filter(S{true, 100, 100}, kWakeAt, wakes);
    CHECK_FALSE(out.pressed);
    CHECK(out.x == 5);
    CHECK(out.y == 6);
    CHECK(latch.swallowing());

    waking = false; // the display is awake from here on
    out = latch.filter(S{true, 140, 300}, kWakeAt + 100, wakes);
    CHECK_FALSE(out.pressed);
    CHECK(out.x == 5); // a swallowed drag does not move the pointer either
    CHECK(out.y == 6);
    // Still held past the window: still swallowed.
    CHECK_FALSE(latch.filter(S{true, 140, 300}, kWindowEnd + 50, wakes).pressed);
    CHECK(asked == 1); // one wake decision per touch

    // A lift past the window ends it and is reported where it happened.
    out = latch.filter(S{false, 140, 300}, kWindowEnd + 80, wakes);
    CHECK_FALSE(out.pressed);
    CHECK(out.x == 140);
    CHECK_FALSE(latch.swallowing());

    // The next press passes unchanged.
    out = latch.filter(S{true, 20, 30}, kWindowEnd + 120, wakes);
    CHECK(out.pressed);
    CHECK(out.x == 20);
    CHECK(out.y == 30);
    CHECK(asked == 2);
}

TEST_CASE("the wake latch swallows contact chatter inside the re-arm window",
          "[display][wake_swallow]") {
    helix::WakeTouchLatch latch;
    bool waking = true;
    auto wakes = [&](int32_t, int32_t) { return waking; };

    latch.filter(S{false, 5, 6}, kWakeAt - 30, wakes);
    CHECK_FALSE(latch.filter(S{true, 100, 100}, kWakeAt, wakes).pressed);
    waking = false;

    // Press, release, press, release, all inside the window: nothing gets through.
    CHECK_FALSE(latch.filter(S{false, 100, 100}, kWakeAt + 30, wakes).pressed);
    CHECK_FALSE(latch.filter(S{true, 100, 100}, kWakeAt + 60, wakes).pressed);
    CHECK_FALSE(latch.filter(S{false, 100, 100}, kWakeAt + 90, wakes).pressed);
    CHECK_FALSE(latch.filter(S{true, 100, 100}, kWakeAt + 120, wakes).pressed);
    CHECK(latch.swallowing());

    // A chatter press still down at the end of the window stays swallowed until it lifts.
    CHECK_FALSE(latch.filter(S{true, 100, 100}, kWindowEnd + 10, wakes).pressed);
    CHECK_FALSE(latch.filter(S{false, 100, 100}, kWindowEnd + 40, wakes).pressed);
    CHECK_FALSE(latch.swallowing());

    // A deliberate tap after the window is an ordinary press.
    CHECK(latch.filter(S{true, 100, 100}, kWindowEnd + 300, wakes).pressed);
}

TEST_CASE("a release inside the window, then quiet, ends the latch at the next press",
          "[display][wake_swallow]") {
    // A device that reads only on events reports nothing between the lift and the next
    // touch, so the press after the window has to end the latch itself.
    helix::WakeTouchLatch latch;
    bool waking = true;
    auto wakes = [&](int32_t, int32_t) { return waking; };

    CHECK_FALSE(latch.filter(S{true, 100, 100}, kWakeAt, wakes).pressed);
    waking = false;
    CHECK_FALSE(latch.filter(S{false, 100, 100}, kWakeAt + 50, wakes).pressed);
    REQUIRE(latch.swallowing());

    CHECK(latch.filter(S{true, 300, 200}, kWindowEnd + 500, wakes).pressed);
    CHECK_FALSE(latch.swallowing());
}

TEST_CASE("the wake latch changes nothing while the display is awake", "[display][wake_swallow]") {
    helix::WakeTouchLatch latch;
    auto awake = [](int32_t, int32_t) { return false; };

    CHECK(latch.filter(S{true, 1, 2}, 0, awake).pressed);
    CHECK(latch.filter(S{true, 3, 4}, 10, awake).x == 3);
    CHECK_FALSE(latch.filter(S{false, 3, 4}, 20, awake).pressed);
    CHECK_FALSE(latch.swallowing());
}

// ============================================================================
// The live input chain
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a held wake touch wakes the display on the press and clicks nothing",
                 "[application][display][input][wake][wake_swallow]") {
    const DisplayBackendType backend = GENERATE(DisplayBackendType::FBDEV, DisplayBackendType::SDL);
    const Idle idle = GENERATE(from_range(idle_modes()));
    CAPTURE(backend == DisplayBackendType::SDL ? "sdl" : "fbdev", idle_name(idle));

    WakeChain chain(backend);
    chain.build_targets(test_screen());
    chain.go_idle(idle);

    chain.hold(LV_INDEV_STATE_PRESSED, kOnButton, 30);
    CHECK_FALSE(chain.asleep_or_dimmed()); // woken by the press, not the release

    // Held well past the long-press threshold from the touch-down, so a wake that left
    // the press live would fire LONG_PRESSED before the lift.
    chain.hold(LV_INDEV_STATE_PRESSED, kOnButton,
               static_cast<int>(helix::app_constants::input::kLongPressMs) + 200 - 30);
    chain.hold(LV_INDEV_STATE_RELEASED, kOnButton, 50);

    CHECK(g_counts.clicked == 0);
    CHECK(g_counts.long_pressed == 0);
    if (backend == DisplayBackendType::FBDEV) {
        CHECK(g_counts.pressed == 0); // the wrapped chain hides the touch entirely
    }
    CHECK_FALSE(chain.asleep_or_dimmed());
}

TEST_CASE_METHOD(LVGLTestFixture, "a quick wake tap clicks nothing",
                 "[application][display][input][wake][wake_swallow]") {
    const DisplayBackendType backend = GENERATE(DisplayBackendType::FBDEV, DisplayBackendType::SDL);
    const Idle idle = GENERATE(from_range(idle_modes()));
    CAPTURE(backend == DisplayBackendType::SDL ? "sdl" : "fbdev", idle_name(idle));

    WakeChain chain(backend);
    chain.build_targets(test_screen());
    chain.go_idle(idle);

    chain.tap(kOnButton, 50);

    CHECK(g_counts.clicked == 0);
    CHECK_FALSE(chain.asleep_or_dimmed());
}

TEST_CASE_METHOD(LVGLTestFixture, "the touch after the wake touch clicks normally",
                 "[application][display][input][wake][wake_swallow]") {
    const DisplayBackendType backend = GENERATE(DisplayBackendType::FBDEV, DisplayBackendType::SDL);
    const Idle idle = GENERATE(from_range(idle_modes()));
    CAPTURE(backend == DisplayBackendType::SDL ? "sdl" : "fbdev", idle_name(idle));

    WakeChain chain(backend);
    chain.build_targets(test_screen());
    chain.go_idle(idle);

    chain.tap(kOnButton, 300);
    REQUIRE(g_counts.clicked == 0);

    chain.tap(kOnButton, 50);
    CHECK(g_counts.clicked == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "a drag that wakes the display scrolls nothing",
                 "[application][display][input][wake][wake_swallow]") {
    const DisplayBackendType backend = GENERATE(DisplayBackendType::FBDEV, DisplayBackendType::SDL);
    const Idle idle = GENERATE(from_range(idle_modes()));
    CAPTURE(backend == DisplayBackendType::SDL ? "sdl" : "fbdev", idle_name(idle));

    WakeChain chain(backend);
    chain.build_targets(test_screen());
    chain.go_idle(idle);

    // Down at the top of the list, then drag 180 px down it over 300 ms and lift.
    chain.hold(LV_INDEV_STATE_PRESSED, kListTop, 100);
    for (int i = 1; i <= 30; ++i) {
        chain.step(LV_INDEV_STATE_PRESSED,
                   {kListTop.x, kListTop.y + (kListDragged.y - kListTop.y) * i / 30});
    }
    chain.hold(LV_INDEV_STATE_PRESSED, kListDragged, 100);
    chain.hold(LV_INDEV_STATE_RELEASED, kListDragged, 50);

    CHECK(g_counts.scroll_begin == 0);
    CHECK(lv_obj_get_scroll_y(chain.list) == 0);
    CHECK_FALSE(chain.asleep_or_dimmed());

    // Control: the same drag on an awake display does scroll, so the case above is not
    // vacuous. Dragging up reveals the content below.
    chain.hold(LV_INDEV_STATE_PRESSED, kListDragged, 50);
    for (int i = 1; i <= 30; ++i) {
        chain.step(LV_INDEV_STATE_PRESSED,
                   {kListTop.x, kListDragged.y - (kListDragged.y - kListTop.y) * i / 30});
    }
    chain.hold(LV_INDEV_STATE_RELEASED, kListTop, 50);
    CHECK(g_counts.scroll_begin >= 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "contact chatter on the wake touch clicks nothing",
                 "[application][display][input][wake][wake_swallow]") {
    // The wrapped chain only: the latch's re-arm window lives in the read wrapper.
    const Idle idle = GENERATE(from_range(idle_modes()));
    CAPTURE(idle_name(idle));

    WakeChain chain(DisplayBackendType::FBDEV);
    chain.build_targets(test_screen());
    chain.go_idle(idle);

    // Press, release, press, release, 30 ms each: all inside the re-arm window.
    chain.hold(LV_INDEV_STATE_PRESSED, kOnButton, 30);
    chain.hold(LV_INDEV_STATE_RELEASED, kOnButton, 30);
    chain.hold(LV_INDEV_STATE_PRESSED, kOnButton, 30);
    chain.hold(LV_INDEV_STATE_RELEASED, kOnButton, 300);
    CHECK(g_counts.pressed == 0);
    CHECK(g_counts.clicked == 0);
    CHECK_FALSE(chain.asleep_or_dimmed());

    // A deliberate tap after the window clicks once.
    chain.tap(kOnButton, 50);
    CHECK(g_counts.clicked == 1);
}
