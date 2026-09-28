// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_scroll_click_guard.cpp
 * @brief The post-scroll click guard's decision, and its place on the live input chain
 *
 * The first half drives ScrollClickGuard directly. The second builds a pointer
 * through the same DisplayManager setup init() runs and reads it through the
 * callback LVGL would call, so the guard counts only if that chain runs it.
 */

#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "scroll_click_guard.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/display_manager_test_access.h"
#include "test_helpers/scoped_env.h"

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ScrollClickGuard;

namespace {

constexpr lv_point_t DOWN{100, 100};
constexpr lv_point_t SCROLLED{100, 200};

ScrollClickGuard enabled_guard(uint32_t cooldown_ms = 80) {
    ScrollClickGuard guard;
    guard.enabled = true;
    guard.cooldown_ms = cooldown_ms;
    guard.scroll_limit_px = 10;
    return guard;
}

/// Feeds one sample and returns the state the guard hands on.
lv_indev_state_t feed(ScrollClickGuard& guard, lv_indev_state_t state, lv_point_t point,
                      uint32_t now_ms) {
    guard.filter(state, point, now_ms);
    return state;
}

/// A touch that goes down at DOWN, moves to @p to and lifts at @p release_ms.
void touch(ScrollClickGuard& guard, lv_point_t to, uint32_t release_ms) {
    feed(guard, LV_INDEV_STATE_PRESSED, DOWN, release_ms - 40);
    feed(guard, LV_INDEV_STATE_PRESSED, to, release_ms - 20);
    feed(guard, LV_INDEV_STATE_RELEASED, to, release_ms);
}

} // namespace

TEST_CASE("scroll guard suppresses a press inside the cooldown after a scroll",
          "[input][scroll_guard]") {
    auto guard = enabled_guard();
    touch(guard, SCROLLED, 1000);

    lv_indev_state_t state = LV_INDEV_STATE_PRESSED;
    CHECK(guard.filter(state, SCROLLED, 1079));
    CHECK(state == LV_INDEV_STATE_RELEASED);
}

TEST_CASE("scroll guard delivers a press once the cooldown has passed", "[input][scroll_guard]") {
    auto guard = enabled_guard();
    touch(guard, SCROLLED, 1000);

    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 1080) == LV_INDEV_STATE_PRESSED);
}

TEST_CASE("scroll guard delivers a held press once the cooldown runs out under it",
          "[input][scroll_guard]") {
    auto guard = enabled_guard();
    touch(guard, SCROLLED, 1000);

    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 1030) == LV_INDEV_STATE_RELEASED);
    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 1060) == LV_INDEV_STATE_RELEASED);
    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 1090) == LV_INDEV_STATE_PRESSED);
    // Once delivered it is an ordinary press, so later samples of it pass too.
    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 1120) == LV_INDEV_STATE_PRESSED);
}

TEST_CASE("scroll guard never suppresses a press that follows a tap", "[input][scroll_guard]") {
    auto guard = enabled_guard();
    // One pixel short of the scroll limit on both axes, which LVGL does not scroll either.
    touch(guard, lv_point_t{DOWN.x + 9, DOWN.y - 9}, 1000);

    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, DOWN, 1010) == LV_INDEV_STATE_PRESSED);
}

TEST_CASE("scroll guard counts a touch as a scroll at exactly the limit", "[input][scroll_guard]") {
    auto guard = enabled_guard();
    touch(guard, lv_point_t{DOWN.x - 10, DOWN.y}, 1000);

    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, DOWN, 1010) == LV_INDEV_STATE_RELEASED);
}

TEST_CASE("scroll guard changes nothing while disabled", "[input][scroll_guard]") {
    auto guard = enabled_guard();
    guard.enabled = false;
    touch(guard, SCROLLED, 1000);

    lv_indev_state_t state = LV_INDEV_STATE_PRESSED;
    CHECK_FALSE(guard.filter(state, SCROLLED, 1001));
    CHECK(state == LV_INDEV_STATE_PRESSED);
}

TEST_CASE("scroll guard measures the cooldown across a tick wrap", "[input][scroll_guard]") {
    auto guard = enabled_guard();
    touch(guard, SCROLLED, UINT32_MAX - 10);

    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 20) == LV_INDEV_STATE_RELEASED);
    CHECK(feed(guard, LV_INDEV_STATE_PRESSED, SCROLLED, 70) == LV_INDEV_STATE_PRESSED);
}

TEST_CASE("scroll guard settings come from config unless the environment overrides them",
          "[input][scroll_guard]") {
    SECTION("saved values") {
        auto guard = ScrollClickGuard::from_settings(true, 150, nullptr, nullptr, 12);
        CHECK(guard.enabled);
        CHECK(guard.cooldown_ms == 150);
        CHECK(guard.scroll_limit_px == 12);
    }
    SECTION("HELIX_SCROLL_GUARD enables with 1 or true") {
        CHECK(ScrollClickGuard::from_settings(false, 80, "1", nullptr, 10).enabled);
        CHECK(ScrollClickGuard::from_settings(false, 80, "true", nullptr, 10).enabled);
    }
    SECTION("HELIX_SCROLL_GUARD disables with anything else") {
        CHECK_FALSE(ScrollClickGuard::from_settings(true, 80, "0", nullptr, 10).enabled);
        CHECK_FALSE(ScrollClickGuard::from_settings(true, 80, "yes", nullptr, 10).enabled);
    }
    SECTION("HELIX_SCROLL_GUARD_COOLDOWN_MS wins over the saved cooldown") {
        CHECK(ScrollClickGuard::from_settings(true, 80, nullptr, "200", 10).cooldown_ms == 200);
    }
    SECTION("the cooldown is clamped to 20-500 ms") {
        CHECK(ScrollClickGuard::from_settings(true, 5, nullptr, nullptr, 10).cooldown_ms == 20);
        CHECK(ScrollClickGuard::from_settings(true, 80, nullptr, "900", 10).cooldown_ms == 500);
    }
}

// ============================================================================
// The live chain
// ============================================================================

namespace {

lv_indev_data_t g_driver_sample{};

void scripted_driver_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_driver_sample.point;
    data->state = g_driver_sample.state;
}

/// A framebuffer backend: the type that gets the embedded input chain.
class ScriptedFbdevBackend : public DisplayBackend {
  public:
    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        lv_indev_t* indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, scripted_driver_read);
        return indev;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::FBDEV;
    }
    const char* name() const override {
        return "ScriptedFbdev";
    }
    bool is_available() const override {
        return true;
    }
};

/// Puts the shared Config document back as it was.
struct ConfigSnapshot {
    helix::Config* config = helix::Config::get_instance();
    json saved = helix::ConfigTestAccess::data(*config);
    ~ConfigSnapshot() {
        helix::ConfigTestAccess::data(*config) = saved;
    }
};

/// A DisplayManager whose pointer went through the setup init() runs, on an
/// embedded backend, with the scroll guard saved on or off.
class LiveChain {
  public:
    explicit LiveChain(bool guard_on) {
        snapshot_.config->set<bool>("/input/scroll_guard", guard_on);
        snapshot_.config->set<int>("/input/scroll_guard_cooldown_ms", 150);
        DisplayManagerTestAccess::set_active_instance(&mgr);
        DisplayManagerTestAccess::set_backend(mgr, std::make_unique<ScriptedFbdevBackend>());
        DisplayManagerTestAccess::rebuild_input_after_backend_swap(mgr);
        pointer_ = mgr.pointer_input();
        REQUIRE(pointer_ != nullptr);
        // What LVGL's read timer would call for this device.
        read_ = lv_indev_get_read_cb(pointer_);
    }
    ~LiveChain() {
        DisplayManagerTestAccess::delete_pointer_input(mgr);
        DisplayManagerTestAccess::set_active_instance(previous_);
        g_driver_sample = lv_indev_data_t{};
    }
    LiveChain(const LiveChain&) = delete;
    LiveChain& operator=(const LiveChain&) = delete;

    /// Advances the clock, has the driver report @p state at @p point, and returns
    /// the state the chain hands LVGL.
    lv_indev_state_t sample(lv_indev_state_t state, lv_point_t point, uint32_t advance_ms) {
        lv_tick_inc(advance_ms);
        g_driver_sample.state = state;
        g_driver_sample.point = point;
        lv_indev_data_t data{};
        read_(pointer_, &data);
        return data.state;
    }

    void scroll_and_lift() {
        sample(LV_INDEV_STATE_PRESSED, DOWN, 10);
        sample(LV_INDEV_STATE_PRESSED, SCROLLED, 20);
        sample(LV_INDEV_STATE_RELEASED, SCROLLED, 20);
    }

    DisplayManager mgr;

  private:
    helix::ScopedEnv env_guard_{"HELIX_SCROLL_GUARD", nullptr};
    helix::ScopedEnv env_cooldown_{"HELIX_SCROLL_GUARD_COOLDOWN_MS", nullptr};
    ConfigSnapshot snapshot_;
    DisplayManager* previous_ = DisplayManager::instance();
    lv_indev_t* pointer_ = nullptr;
    lv_indev_read_cb_t read_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the pointer DisplayManager sets up runs the scroll guard",
                 "[input][scroll_guard][application][display]") {
    const bool guard_on = GENERATE(true, false);
    CAPTURE(guard_on);
    LiveChain chain(guard_on);

    chain.scroll_and_lift();

    // 100 ms after the lift: past the 80 ms default, inside the configured 150.
    const lv_indev_state_t ghost = chain.sample(LV_INDEV_STATE_PRESSED, SCROLLED, 100);
    CHECK(ghost == (guard_on ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED));

    chain.sample(LV_INDEV_STATE_RELEASED, SCROLLED, 10);
    CHECK(chain.sample(LV_INDEV_STATE_PRESSED, SCROLLED, 50) == LV_INDEV_STATE_PRESSED);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a press inside the scroll guard cooldown still wakes the display",
                 "[input][scroll_guard][application][display][sleep]") {
    LiveChain chain(/*guard_on=*/true);
    chain.scroll_and_lift();
    DisplayManagerTestAccess::set_display_sleeping(chain.mgr, true);
    REQUIRE_FALSE(DisplayManagerTestAccess::wake_requested(chain.mgr));

    // Inside the cooldown, so the guard would suppress this press if it ran first.
    CHECK(chain.sample(LV_INDEV_STATE_PRESSED, SCROLLED, 30) == LV_INDEV_STATE_RELEASED);
    CHECK(DisplayManagerTestAccess::wake_requested(chain.mgr));

    DisplayManagerTestAccess::set_display_sleeping(chain.mgr, false);
}
