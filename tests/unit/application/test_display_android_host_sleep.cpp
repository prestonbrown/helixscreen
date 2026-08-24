// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_android_host_sleep.cpp
 * @brief Tests for #1245 item 3 — Android must be allowed to actually sleep.
 *
 * SDL2 asserts FLAG_KEEP_SCREEN_ON at video init (SDL_video.c disables the
 * screensaver by default) and nothing ever cleared it, so the device could never
 * power its panel down while HelixScreen was foregrounded. Our own "Display
 * Sleep" setting was cosmetic there: no backlight backend is reachable from an
 * untrusted app and DisplayBackendSDL has no blank/power-off, so enter_sleep()
 * fell through to the software overlay — a black LVGL rect over a fully lit,
 * battery-draining panel.
 *
 * The fix maps our Display Sleep setting onto Android's own sleep: at idle we
 * clear keep-screen-on and let the OS power the panel off; on wake we re-assert
 * it. Display Sleep = "Never" must NEVER clear the flag (wall-mounted tablets).
 *
 * The JNI call itself cannot run in the host test binary, so the DECISION is
 * factored into pure statics on DisplayManager and tested here exhaustively —
 * the same shape as should_use_power_off() (#1049). What is verified on the host
 * build: the mechanism selection matrix, that non-Android behaviour is bit-for-bit
 * what it was, and that enter_sleep()/restore_display_output() actually consult
 * the selector rather than re-deriving the branch.
 */

#include "backlight_backend.h"
#include "display_backend.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/display_manager_test_access.h"

#include "../../catch_amalgamated.hpp"

using SleepMechanism = DisplayManager::SleepMechanism;

namespace {

// Stand-in for DisplayBackendSDL on Android: no hardware blank, no power-off.
// Matches what the real SDL backend reports (neither method is overridden).
class FakeSdlBackend : public DisplayBackend {
  public:
    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::SDL;
    }
    const char* name() const override {
        return "FakeSDL";
    }
    bool is_available() const override {
        return true;
    }

    int blank_calls = 0;
    int power_off_calls = 0;

    bool blank_display() override {
        ++blank_calls;
        return true;
    }
    bool power_off() override {
        ++power_off_calls;
        return false;
    }
};

// Hardware-blank / power-off capable backend, so the non-Android branches can be
// re-asserted through the new selector.
class FakeCapableBackend : public DisplayBackend {
  public:
    explicit FakeCapableBackend(bool supports_power_off) : m_supports(supports_power_off) {}

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return DisplayBackendType::FBDEV;
    }
    const char* name() const override {
        return "FakeCapable";
    }
    bool is_available() const override {
        return true;
    }

    bool supports_power_off() const override {
        return m_supports;
    }

    int blank_calls = 0;
    int power_off_calls = 0;
    int power_on_calls = 0;

    bool blank_display() override {
        ++blank_calls;
        return true;
    }
    bool unblank_display() override {
        return true;
    }
    bool power_off() override {
        ++power_off_calls;
        return m_supports;
    }
    bool power_on() override {
        ++power_on_calls;
        return m_supports;
    }

  private:
    bool m_supports;
};

// The pre-#1245 branch order, written out independently of the implementation so
// the "non-Android is untouched" test derives its expectation from the OLD rule
// rather than from the new function it is guarding.
SleepMechanism legacy_mechanism(bool use_hardware_blank, bool can_power_off) {
    if (use_hardware_blank) {
        return SleepMechanism::HardwareBlank;
    }
    if (can_power_off) {
        return SleepMechanism::PanelPowerOff;
    }
    return SleepMechanism::SoftwareOverlay;
}

} // namespace

// ============================================================================
// The pure selector
// ============================================================================

TEST_CASE("Android idle sleep hands the panel to the OS instead of painting an overlay",
          "[application][display][sleep][android][1245]") {
    // The shipping Android configuration: SDL backend, no backlight backend, so
    // neither hardware blank nor panel power-off is available. Before the fix this
    // landed on SoftwareOverlay (black rect over a lit panel).
    REQUIRE(DisplayManager::select_sleep_mechanism(/*is_android=*/true,
                                                   /*use_hardware_blank=*/false,
                                                   /*can_power_off=*/false,
                                                   /*sleep_timeout_sec=*/60) ==
            SleepMechanism::HostSleep);

    // Every finite timeout behaves the same — the value only gates "Never".
    for (int timeout : {1, 30, 300, 3600}) {
        REQUIRE(DisplayManager::select_sleep_mechanism(true, false, false, timeout) ==
                SleepMechanism::HostSleep);
    }
}

TEST_CASE("Display Sleep = Never never hands the panel to the OS (wall-mounted tablet)",
          "[application][display][sleep][android][1245]") {
    // get_display_sleep_sec() returns 0 for "Never"; check_display_sleep() treats
    // <= 0 as disabled. The selector must agree, so no code path can ever clear
    // keep-screen-on for a user who asked the screen to stay on forever.
    REQUIRE(DisplayManager::select_sleep_mechanism(true, false, false, 0) ==
            SleepMechanism::SoftwareOverlay);

    // Defensive: a negative timeout is equally "not a real timeout".
    REQUIRE(DisplayManager::select_sleep_mechanism(true, false, false, -1) ==
            SleepMechanism::SoftwareOverlay);
}

TEST_CASE("host sleep is the last resort — real panel control still wins",
          "[application][display][sleep][android][1245]") {
    // Not reachable on today's Android build (no backlight, no power-off), but the
    // ordering must stay: cutting the panel ourselves is strictly better than
    // handing control to a timeout we don't own.
    REQUIRE(DisplayManager::select_sleep_mechanism(true, /*hw_blank=*/true, false, 60) ==
            SleepMechanism::HardwareBlank);
    REQUIRE(DisplayManager::select_sleep_mechanism(true, false, /*power_off=*/true, 60) ==
            SleepMechanism::PanelPowerOff);
}

TEST_CASE("non-Android sleep selection is bit-for-bit the pre-#1245 behaviour",
          "[application][display][sleep][1245]") {
    // Exhaustive over the whole input space: with is_android=false the selector
    // must reduce exactly to the old if/else-if/else chain, for every timeout
    // including the ones that would trigger host sleep on Android.
    for (bool hw_blank : {false, true}) {
        for (bool power_off : {false, true}) {
            for (int timeout : {-1, 0, 1, 60, 3600}) {
                CAPTURE(hw_blank, power_off, timeout);
                REQUIRE(
                    DisplayManager::select_sleep_mechanism(false, hw_blank, power_off, timeout) ==
                    legacy_mechanism(hw_blank, power_off));
            }
        }
    }
}

// ============================================================================
// Suspend/resume self-heal — m_display_sleeping must not stick
// ============================================================================

TEST_CASE("a suspend/resume cycle while host-sleeping forces a wake",
          "[application][display][sleep][android][1245]") {
    // When Android powers the panel off it pauses the app; turning the panel back
    // on resumes it. Nothing in that round-trip is a touch, so the normal
    // activity-based wake never fires and m_display_sleeping would stay true with
    // keep-screen-on still cleared — the display would keep re-sleeping and the
    // sleep callbacks (camera suspend) would never resume. HelixActivity bumps a
    // resume counter; a change in it while host-sleeping means the panel is back.
    REQUIRE(DisplayManager::host_sleep_needs_wake(/*sleeping_via_host=*/true,
                                                  /*seq_at_sleep=*/7, /*seq_now=*/8));

    // No resume yet — stay asleep. This is the window between clearing the flag
    // and Android's own timeout expiring; waking here would mean never sleeping.
    REQUIRE_FALSE(DisplayManager::host_sleep_needs_wake(true, 7, 7));

    // Not host-sleeping: hardware blank / power-off / overlay devices are
    // unaffected, and a resume must not spuriously wake them.
    REQUIRE_FALSE(DisplayManager::host_sleep_needs_wake(false, 7, 8));
    REQUIRE_FALSE(DisplayManager::host_sleep_needs_wake(false, 7, 7));
}

// ============================================================================
// Wiring — enter_sleep()/restore must go through the selector
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "enter_sleep records the selected mechanism (overlay on host)",
                 "[application][display][sleep][1245]") {
    // Host build: is_android() is false, so an SDL-shaped device (no blank, no
    // power-off) must still land on the software overlay. This is the guard that
    // the Android change did not leak onto desktop/embedded.
    DisplayManager mgr;
    auto backend = std::make_unique<FakeSdlBackend>();
    FakeSdlBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, false);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(DisplayManagerTestAccess::last_sleep_mechanism(mgr) == SleepMechanism::SoftwareOverlay);
    REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) != nullptr); // black rect was painted
    REQUIRE(raw->blank_calls == 0);
    REQUIRE(raw->power_off_calls == 0);
    // Never touched off-Android — the flag belongs to the host window manager.
    REQUIRE(DisplayManagerTestAccess::keep_screen_on(mgr));

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) == nullptr);
    REQUIRE(DisplayManagerTestAccess::keep_screen_on(mgr));
}

TEST_CASE_METHOD(LVGLTestFixture, "hardware-blank device still blanks and paints no overlay",
                 "[application][display][sleep][1245]") {
    // AD5M/Allwinner shape, re-asserted through the new switch.
    DisplayManager mgr;
    auto backend = std::make_unique<FakeCapableBackend>(/*supports_power_off=*/false);
    FakeCapableBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, true);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(DisplayManagerTestAccess::last_sleep_mechanism(mgr) == SleepMechanism::HardwareBlank);
    REQUIRE(raw->blank_calls == 1);
    REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) == nullptr);
    REQUIRE(DisplayManagerTestAccess::keep_screen_on(mgr));
}

TEST_CASE_METHOD(LVGLTestFixture, "a failed power_off() still falls back to the overlay",
                 "[application][display][sleep][1245]") {
    // The capability probe can disagree with reality. enter_sleep() must notice
    // power_off() returning false and degrade to the overlay rather than leaving
    // a lit panel with no visual sleep at all.
    DisplayManager mgr;
    auto backend = std::make_unique<FakeCapableBackend>(/*supports_power_off=*/false);
    FakeCapableBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    // Force the power-off branch even though the backend will refuse.
    DisplayManagerTestAccess::set_use_power_off(mgr, true);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(raw->power_off_calls == 1);
    REQUIRE(DisplayManagerTestAccess::last_sleep_mechanism(mgr) == SleepMechanism::SoftwareOverlay);
    REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) != nullptr);
    REQUIRE_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
}
