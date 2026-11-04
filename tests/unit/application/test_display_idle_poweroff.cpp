// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_idle_poweroff.cpp
 * @brief Tests for #1049 — two-stage idle on no-backlight devices + real power-off.
 *
 * Bug 1 (root cause): the Sleep>=Dim coupling in DisplaySettingsManager was only
 *        enforced when has_dimming_control() was true, so no-backlight devices
 *        (the reporter's Pi, Backlight-None) could persist Sleep < Dim and starve
 *        the screensaver stage. The coupling must also fire when a screensaver is
 *        enabled (the screensaver IS the dim-stage visual there). Verified via
 *        DisplaySettingsManager setters + init cross-validation, plus the awake
 *        path reaching the screensaver at the Dim timeout on a no-backlight device.
 *
 * Bug 2: When the screensaver is OFF and there is no hardware backlight blank,
 *        idle entry must perform a real, capability-gated display power-off
 *        (fbdev FBIOBLANK FB_BLANK_POWERDOWN) via the display backend, with the
 *        software overlay remaining the fallback when no power-off exists.
 *
 * In these tests no DisplayManager singleton is constructed, so
 * DisplaySettingsManager::has_dimming_control() returns false — i.e. the
 * no-backlight Pi case. screensaver_enabled() is driven via the screensaver_type
 * setting. The power-off seam is exercised through a fake DisplayBackend injected
 * via DisplayManager's test hooks.
 */

#include "backlight_backend.h"
#include "config.h"
#include "display/lv_display_private.h" // flush_cb access for the #1049 swap assertion
#include "display_backend.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lvgl_test_fixture.h"
#include "misc/lv_timer_private.h" // read timer period
#include "screen_hide_hold.h"
#include "splash_screen_manager.h"
#include "test_helpers/display_manager_test_access.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "../../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

namespace {

// Minimal fake backend that records power_off()/power_on() and blank/unblank
// calls so the idle-entry path can be asserted without real hardware.
class FakePowerOffBackend : public DisplayBackend {
  public:
    explicit FakePowerOffBackend(bool supports_power_off,
                                 DisplayBackendType type = DisplayBackendType::FBDEV)
        : m_supports(supports_power_off), m_type(type) {}

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return m_type;
    }
    const char* name() const override {
        return "FakePowerOff";
    }
    bool is_available() const override {
        return true;
    }

    bool supports_power_off() const override {
        return m_supports;
    }
    bool power_off() override {
        ++power_off_calls;
        return m_supports;
    }
    bool power_on() override {
        ++power_on_calls;
        return m_supports;
    }

    bool blank_display() override {
        ++blank_calls;
        return true;
    }
    bool unblank_display() override {
        ++unblank_calls;
        return true;
    }

    void request_full_upload() override {
        ++full_upload_requests;
    }

    int power_off_calls = 0;
    int power_on_calls = 0;
    int blank_calls = 0;
    int unblank_calls = 0;
    int full_upload_requests = 0;

  private:
    bool m_supports;
    DisplayBackendType m_type;
};

// Fake controllable backlight (e.g. the Snapmaker U1's Sysfs pwm-backlight):
// is_available() == true, set_brightness(0) cleanly turns the panel off.
class FakeBacklight : public BacklightBackend {
  public:
    explicit FakeBacklight(bool available) : m_available(available) {}

    bool set_brightness(int percent) override {
        last_brightness = percent;
        return m_available;
    }
    int get_brightness() const override {
        return m_available ? last_brightness : -1;
    }
    bool is_available() const override {
        return m_available;
    }
    const char* name() const override {
        return "FakeBacklight";
    }

    int last_brightness = 100;

  private:
    bool m_available;
};

// Sentinel flush callback. The headless LVGLTestFixture display has a null
// flush_cb, so to observe the #1049 flush swap (real → no-op → real) we install
// this stable, identifiable callback first and compare against it.
void test_sentinel_flush_cb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px*/) {
    lv_display_flush_ready(disp);
}

} // namespace

// ============================================================================
// Bug 1 — Sleep>=Dim coupling must hold on no-backlight + screensaver devices
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "screensaver_enabled drives the Sleep>=Dim coupling on no-backlight devices",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();

    SECTION("screensaver ON: setting sleep < dim is clamped up to dim") {
        config->set<int>("/display/screensaver_type", 1); // Flying Toasters → enabled
        config->set<int>("/display/dim_sec", 600);
        config->set<int>("/display/sleep_sec", 600);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();

        // No DisplayManager → has_dimming_control() is false; the screensaver is
        // the only reason the coupling should fire.
        REQUIRE_FALSE(DisplaySettingsManager::instance().has_dimming_control());
        REQUIRE(DisplaySettingsManager::instance().screensaver_enabled());
        REQUIRE(DisplaySettingsManager::instance().should_couple_sleep_to_dim());

        // Reporter's exact mistake: sleep=60 < dim=600. Must be forced up to 600.
        DisplaySettingsManager::instance().set_display_sleep_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);
    }

    SECTION("screensaver OFF + no backlight: short sleep is NOT clamped") {
        config->set<int>("/display/screensaver_type", 0); // OFF
        config->set<int>("/display/dim_sec", 600);
        config->set<int>("/display/sleep_sec", 1200);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();

        REQUIRE_FALSE(DisplaySettingsManager::instance().screensaver_enabled());
        REQUIRE_FALSE(DisplaySettingsManager::instance().should_couple_sleep_to_dim());

        // With neither dimming nor screensaver, a short sleep is legitimate
        // (e.g. AD5M/AD5X binary backlight) and must NOT be clamped.
        DisplaySettingsManager::instance().set_display_sleep_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 60);
    }

    // Re-init rather than leaving the singleton torn down. A deinitialised
    // DisplaySettingsManager withdraws settings_animations_enabled from the XML
    // registry, so HelixTestFixture::reset_all()'s force-animations-off lookup
    // silently finds nothing and the next fixture's init_subjects() restores the
    // platform default (animations ON). Modal exits then animate for 150ms and
    // every later modal test that pumps less than that sees the outgoing dialog.
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "raising dim above sleep bumps sleep up when a screensaver is enabled",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1);
    config->set<int>("/display/dim_sec", 60);
    config->set<int>("/display/sleep_sec", 120);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    // Raising dim above the current sleep must bump sleep up to match.
    DisplaySettingsManager::instance().set_display_dim_sec(300);
    REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 300);
    REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 300);

    // Re-init rather than leaving the singleton torn down. A deinitialised
    // DisplaySettingsManager withdraws settings_animations_enabled from the XML
    // registry, so HelixTestFixture::reset_all()'s force-animations-off lookup
    // silently finds nothing and the next fixture's init_subjects() restores the
    // platform default (animations ON). Modal exits then animate for 150ms and
    // every later modal test that pumps less than that sees the outgoing dialog.
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "init cross-validation self-corrects a persisted sleep<dim on no-backlight + screensaver",
    "[application][display][sleep][screensaver][1049]") {
    // Reproduces bundle 3VDWMLYQ exactly: no backlight, screensaver enabled,
    // persisted sleep=60 < dim=600. On the next launch (init_subjects) the
    // config must self-correct to sleep>=dim instead of starving the screensaver.
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1);
    config->set<int>("/display/dim_sec", 600);
    config->set<int>("/display/sleep_sec", 60); // inconsistent persisted value

    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);
    REQUIRE(config->get<int>("/display/sleep_sec", -1) == 600);

    // Re-init rather than leaving the singleton torn down. A deinitialised
    // DisplaySettingsManager withdraws settings_animations_enabled from the XML
    // registry, so HelixTestFixture::reset_all()'s force-animations-off lookup
    // silently finds nothing and the next fixture's init_subjects() restores the
    // platform default (animations ON). Modal exits then animate for 150ms and
    // every later modal test that pumps less than that sees the outgoing dialog.
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();
}

// ============================================================================
// Bug 1 — awake path reaches the screensaver at the Dim timeout (no backlight)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "awake path starts the screensaver at the Dim timeout on a no-backlight device",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1); // enabled
    config->set<int>("/display/dim_sec", 600);
    config->set<int>("/display/sleep_sec", 1200);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    DisplayManager mgr; // no init() → m_backlight is null (no-backlight device)
    // Drive the dim stage deterministically with a 1s dim timeout; the sleep
    // timeout stays at 1200s so the sleep branch does not fire. With no backlight,
    // can_dim is false — the dim/screensaver branch is reached only because a
    // screensaver is enabled (the #1049 guard on display_manager.cpp:829-830).
    mgr.set_dim_timeout(1);

    // Advance LVGL's tick past the 1s dim timeout. In headless tests the tick is
    // manual (process_lvgl drives lv_tick_inc) — a wall-clock sleep would NOT
    // move lv_display_get_inactive_time(), so use process_lvgl, not delay().
    lv_display_trigger_activity(nullptr);
    process_lvgl(1300);

    REQUIRE_FALSE(mgr.is_display_sleeping());
    mgr.check_display_sleep();

    // The dim/screensaver branch must have been reached and the screensaver
    // started (display marked dimmed, not slept).
    REQUIRE(mgr.is_display_dimmed());
    REQUIRE_FALSE(mgr.is_display_sleeping());

    mgr.wake_display();
    // Re-init rather than leaving the singleton torn down. A deinitialised
    // DisplaySettingsManager withdraws settings_animations_enabled from the XML
    // registry, so HelixTestFixture::reset_all()'s force-animations-off lookup
    // silently finds nothing and the next fixture's init_subjects() restores the
    // platform default (animations ON). Modal exits then animate for 150ms and
    // every later modal test that pumps less than that sees the outgoing dialog.
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();
}

// ============================================================================
// Bug 2 — real display power-off seam (fbdev FB_BLANK_POWERDOWN)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep powers off the panel when backend supports it (screensaver OFF)",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Software-blank strategy (no hardware backlight blank), like the Pi/HDMI bundle.
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    // init() sets this from backend->supports_power_off(); mirror that here.
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 1);
    REQUIRE(raw->blank_calls == 0); // power-off is not the FB_BLANK_NORMAL hardware-blank path

    // Wake-side restore must power the panel back on (the full wake_display()
    // also calls lv_refr_now(), which can't run under the headless test display).
    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep falls back to software overlay when no power-off capability",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    // Backend reports no power-off capability → init() would leave this false.
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 0); // capability gate: not supported → not called

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 0); // nothing to power on — overlay was used
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep keeps hardware-blank path when backlight blanks (no power-off)",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Hardware-blank devices (AD5M/Allwinner) must keep using blank_display(),
    // not the new power_off() path.
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, true);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->blank_calls == 1);
    REQUIRE(raw->power_off_calls == 0);

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->unblank_calls == 1); // hardware-blank path unblanks, never power_on
    REQUIRE(raw->power_on_calls == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "DRM backend power-off flows through the same generic seam (CB1/Pi HDMI)",
                 "[application][display][sleep][poweroff][drm][1049]") {
    // The reporter's Pi and the CB1 verification device render via DRM (DPMS is
    // the real power-off there). DisplayManager's power-off path is backend-type
    // agnostic — it calls m_backend->power_off()/power_on() regardless of type —
    // so a DRM-typed backend reporting supports_power_off() must drive exactly the
    // same path as the fbdev case. (The actual drmModeConnectorSetProperty DPMS
    // call lives in DisplayBackendDRM and can't run in CI without a DRM master.)
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false); // no backlight blank (CB1)
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 1); // DRM DPMS-off path, not the software overlay
    REQUIRE(raw->blank_calls == 0);

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 1); // DPMS-on before lv_refr_now on wake (#303)
}

// ============================================================================
// Bug 2 — power-off gate: LAST RESORT only (Snapmaker U1 regression guard)
// ============================================================================

TEST_CASE("power-off gate: only with NO hardware blank AND NO usable backlight",
          "[application][display][sleep][poweroff][1049][1594][1708]") {
    // Pure decision matrix for should_use_power_off(use_hw_blank,
    // has_usable_backlight, backend_supports_power_off).

    // No hw blank, no backlight, backend can power off -> power off (HDMI / CB1).
    REQUIRE(helix::should_use_power_off(false, false, true));

    // Usable backlight present -> never power off, even with a DPMS-capable
    // backend (U1, AD5X, K1/K2, Pi DSI all have one).
    REQUIRE_FALSE(helix::should_use_power_off(false, true, true));

    // Hardware blank present -> never power off (AD5M/Allwinner).
    REQUIRE_FALSE(helix::should_use_power_off(true, false, true));

    // Backend can't power off -> never (falls back to software overlay).
    REQUIRE_FALSE(helix::should_use_power_off(false, false, false));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "usable backlight suppresses power-off so enter_sleep dims instead",
                 "[application][display][sleep][poweroff][1049][1708]") {
    // A working backlight (is_available()==true) + a DPMS-capable DRM backend
    // reporting Hardware blank: false, the U1 shape. The gate must refuse
    // power-off, and enter_sleep must turn the backlight off instead of calling
    // power_off(), which would wedge the VOP2 CRTC there.
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw_backend = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_backlight(mgr,
                                            std::make_unique<FakeBacklight>(/*available=*/true));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);

    // Run the real init() gate against the injected state.
    bool use_power_off = DisplayManagerTestAccess::compute_use_power_off(mgr);
    REQUIRE_FALSE(use_power_off);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw_backend->power_off_calls == 0); // never DPMS-off a backlight device
    REQUIRE(raw_backend->blank_calls == 0);     // not a hardware-blank device either
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "no usable backlight keeps power-off enabled (reporter HDMI / CB1)",
                 "[application][display][sleep][poweroff][1049]") {
    // Backlight-None device with a DPMS-capable backend: power-off is the only way
    // to cut the panel, so the gate must KEEP it enabled.
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw_backend = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Backlight present but NOT available (Backlight-None in production).
    DisplayManagerTestAccess::set_backlight(mgr,
                                            std::make_unique<FakeBacklight>(/*available=*/false));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);

    bool use_power_off = DisplayManagerTestAccess::compute_use_power_off(mgr);
    REQUIRE(use_power_off);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw_backend->power_off_calls == 1); // power-off still used on no-backlight devices
}

// ============================================================================
// #1049 regression — power-off must neutralize the flush so the next page-flip
// can't re-assert DPMS-on and relight the panel ("HDMI5 doesn't sleep anymore;
// comes back on at the home screen"). DRM DPMS-off is undone by the very next
// LVGL commit; pausing the refresh timer is insufficient because any
// invalidation resumes it. The fix swaps the flush callback for a no-op and
// disables invalidation while powered off, restoring both on wake.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "power-off sleep suppresses LVGL flush so no page-flip relights the panel",
                 "[application][display][sleep][poweroff][drm][1049]") {
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    // Give the manager a real display so the suppression acts on something. The
    // LVGLTestFixture owns the default SDL display.
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb); // stand in for the real page-flip
    lv_display_flush_cb_t real_cb = disp->flush_cb;
    REQUIRE(real_cb == test_sentinel_flush_cb);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    // Panel powered off AND rendering neutralized: the flush callback is swapped
    // away from the real (page-flipping) one so no commit can relight the panel.
    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 1);
    REQUIRE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    REQUIRE(disp->flush_cb != real_cb);

    // Wake-side restore must re-enable rendering BEFORE power-on (so the post-wake
    // lv_refr_now reaches the panel) and put the real flush callback back.
    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    REQUIRE(disp->flush_cb == real_cb);
    REQUIRE(raw->power_on_calls == 1);

    // Clear the injected display so the manager dtor doesn't reason about a
    // fixture-owned display (it only nulls the pointer, but keep state tidy).
    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "software-overlay fallback keeps the flush live (overlay must render)",
                 "[application][display][sleep][poweroff][1049]") {
    // When the backend can't actually power off, enter_sleep paints a black
    // software overlay instead — that overlay MUST still render, so the flush must
    // NOT be suppressed. Suppressing it here would freeze a stale frame and the
    // "sleep" would never visually take effect.
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);
    lv_display_flush_cb_t real_cb = disp->flush_cb;

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 0); // capability gate: overlay path
    REQUIRE_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr)); // overlay needs the flush
    REQUIRE(disp->flush_cb == real_cb);                                // flush untouched

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(disp->flush_cb == real_cb);
    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "hardware-blank path keeps the flush live (AD5M/Allwinner)",
                 "[application][display][sleep][poweroff][1049]") {
    // Hardware-blank devices blank the panel at the controller; LVGL keeps
    // rendering normally. The flush must not be suppressed there.
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, true);

    lv_display_t* disp = lv_display_get_default();
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);
    lv_display_flush_cb_t real_cb = disp->flush_cb;

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(raw->blank_calls == 1);
    REQUIRE(raw->power_off_calls == 0);
    REQUIRE_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    REQUIRE(disp->flush_cb == real_cb);

    DisplayManagerTestAccess::restore_display_output(mgr);
    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

// ============================================================================
// The software sleep overlay hides the screen beneath it
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "software sleep overlay hides the screen until it is removed",
                 "[application][display][sleep][screen_hide]") {
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    DisplayManager mgr;
    DisplayManagerTestAccess::set_backend(
        mgr, std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, false);
    lv_obj_t* screen = lv_screen_active();
    REQUIRE_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

    DisplayManagerTestAccess::enter_sleep(mgr, 60);
    REQUIRE(DisplayManagerTestAccess::last_sleep_mechanism(mgr) ==
            helix::SleepMechanism::SoftwareOverlay);
    REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) != nullptr);
    CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

    SECTION("the wake-side restore shows the screen again") {
        DisplayManagerTestAccess::restore_display_output(mgr);
        CHECK(DisplayManagerTestAccess::sleep_overlay(mgr) == nullptr);
        CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("a repeated create or destroy keeps the hold balanced") {
        DisplayManagerTestAccess::create_sleep_overlay(mgr);
        DisplayManagerTestAccess::destroy_sleep_overlay(mgr);
        CHECK(DisplayManagerTestAccess::sleep_overlay(mgr) == nullptr);
        CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

        DisplayManagerTestAccess::destroy_sleep_overlay(mgr);
        CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
    }

    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a display manager destroyed asleep takes its overlay and hold with it",
                 "[application][display][sleep][screen_hide]") {
    lv_obj_t* screen = lv_screen_active();
    const uint32_t top_children = lv_obj_get_child_count(lv_layer_top());
    {
        DisplayManager mgr;
        DisplayManagerTestAccess::set_backend(
            mgr, std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false));
        DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
        DisplayManagerTestAccess::set_use_power_off(mgr, false);
        DisplayManagerTestAccess::enter_sleep(mgr, 60);
        REQUIRE(DisplayManagerTestAccess::sleep_overlay(mgr) != nullptr);
        REQUIRE(lv_obj_get_child_count(lv_layer_top()) == top_children + 1);
    }
    CHECK(lv_obj_get_child_count(lv_layer_top()) == top_children);
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());
    CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
}

// ============================================================================
// Refresh period across sleep and input rebuilds
// ============================================================================

#include "refresh_period_hold.h"
#include "refresh_timing_env.h"
#include "test_helpers/refresh_period_hold_test_access.h"
#include "test_helpers/scoped_env.h"
#if HELIX_ENABLE_SCREENSAVER
#include "screensaver.h"
#endif

namespace {

void noop_pointer_read(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
}

lv_indev_t* create_noop_pointer() {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, noop_pointer_read);
    return indev;
}

/// A backend whose pointer is a real LVGL input device, so an input rebuild creates one.
class IndevBackend : public FakePowerOffBackend {
  public:
    IndevBackend() : FakePowerOffBackend(/*supports_power_off=*/false) {}

    lv_indev_t* create_input_pointer() override {
        return create_noop_pointer();
    }
};

} // namespace

#if HELIX_ENABLE_SCREENSAVER
TEST_CASE_METHOD(LVGLTestFixture,
                 "entering sleep gives back a running screensaver's refresh period",
                 "[application][display][sleep][refresh_period]") {
    helix::ScopedTimerPeriods timers;
    helix::ScopedEnv global("HELIX_REFR_PERIOD_MS");
    helix::ScopedEnv scope("HELIX_REFR_PERIOD_SCOPE");
    helix::ScopedEnv saver("HELIX_SCREENSAVER_REFR_PERIOD_MS");
    unsetenv("HELIX_REFR_PERIOD_MS");
    unsetenv("HELIX_REFR_PERIOD_SCOPE");
    setenv("HELIX_SCREENSAVER_REFR_PERIOD_MS", "16", 1);
    helix::apply_refresh_timing(helix::refresh_timing_from_env());
    const uint32_t baseline = helix::default_refr_timer_period();
    REQUIRE(baseline != 16);

    DisplayManager mgr;
    DisplayManagerTestAccess::set_backend(
        mgr, std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, false);
    struct StopSavers {
        ~StopSavers() {
            ScreensaverManager::instance().stop();
        }
    } stop_savers;

    auto& savers = ScreensaverManager::instance();
    savers.start(ScreensaverType::FLYING_TOASTERS);
    REQUIRE(savers.is_active());
    DisplayManagerTestAccess::set_screensaver_active(mgr, true);
    CHECK(helix::default_refr_timer_period() == 16);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    CHECK_FALSE(savers.is_active());
    CHECK_FALSE(helix::active_refresh_period_hold().is_held());
    CHECK(helix::default_refr_timer_period() == baseline);
    CHECK(helix::anim_timer_period() == baseline);

    DisplayManagerTestAccess::restore_display_output(mgr);
}
#endif

TEST_CASE_METHOD(LVGLTestFixture, "input setup paces its new devices by the refresh timing",
                 "[application][display][refresh_period]") {
    helix::ScopedTimerPeriods timers;
    helix::RefreshTiming timing;
    timing.refr_period_ms = 20;
    timing.scope_all = GENERATE(true, false);
    CAPTURE(timing.scope_all);

    lv_indev_t* probe = create_noop_pointer();
    const uint32_t default_read_period = lv_indev_get_read_timer(probe)->period;
    lv_indev_delete(probe);
    REQUIRE(default_read_period != 20);

    DisplayManager mgr;
    DisplayManagerTestAccess::set_backend(mgr, std::make_unique<IndevBackend>());
    DisplayManagerTestAccess::set_refresh_timing(mgr, timing);
    struct DeletePointer {
        DisplayManager& dm;
        ~DeletePointer() {
            DisplayManagerTestAccess::delete_pointer_input(dm);
        }
    } delete_pointer{mgr};

    DisplayManagerTestAccess::create_input_devices(mgr);

    lv_indev_t* pointer = mgr.pointer_input();
    REQUIRE(pointer != nullptr);
    const lv_timer_t* read = lv_indev_get_read_timer(pointer);
    REQUIRE(read != nullptr);
    CHECK(read->period == (timing.scope_all ? 20u : default_read_period));
    CHECK(helix::default_refr_timer_period() == 20);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "input setup keeps a held screensaver refresh period until release",
                 "[application][display][refresh_period]") {
    helix::ScopedTimerPeriods timers;
    helix::RefreshTiming timing;
    timing.refr_period_ms = 20;
    timing.screensaver_refr_period_ms = 16;
    helix::apply_refresh_timing(timing);
    REQUIRE(helix::default_refr_timer_period() == 20);

    DisplayManager mgr;
    DisplayManagerTestAccess::set_backend(mgr, std::make_unique<IndevBackend>());
    DisplayManagerTestAccess::set_refresh_timing(mgr, timing);
    struct DeletePointer {
        DisplayManager& dm;
        ~DeletePointer() {
            DisplayManagerTestAccess::delete_pointer_input(dm);
        }
    } delete_pointer{mgr};
    struct ReleaseHold {
        ~ReleaseHold() {
            helix::active_refresh_period_hold().release();
        }
    } release_hold;

    auto& hold = helix::active_refresh_period_hold();
    hold.acquire();
    REQUIRE(helix::default_refr_timer_period() == 16);

    DisplayManagerTestAccess::create_input_devices(mgr);

    CHECK(hold.is_held());
    CHECK(helix::default_refr_timer_period() == 16);
    CHECK(helix::anim_timer_period() == 16);

    hold.release();
    CHECK(helix::default_refr_timer_period() == 20);
    CHECK(helix::anim_timer_period() == 20);
}

// ============================================================================
// A backend that presents only the areas LVGL flushes keeps stale pixels wherever
// the image changed without a flush reaching it, so those paths ask for a full upload.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "a color transform change asks the backend to upload the whole next frame",
                 "[application][display][egl_upload]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_display(mgr, lv_display_get_default());
    REQUIRE(raw->full_upload_requests == 0);

    mgr.set_color_transform(0.8f, 20, 0);
    CHECK(raw->full_upload_requests == 1);

    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "waking from power-off sleep asks the backend to upload the whole next frame",
                 "[application][display][sleep][poweroff][egl_upload]") {
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, true);
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);
    REQUIRE(DisplayManagerTestAccess::is_flush_suppressed(mgr));

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(disp->flush_cb == test_sentinel_flush_cb);
    CHECK(raw->full_upload_requests == 1);

    // Nothing is swapped out on a second wake, so nothing asks again.
    DisplayManagerTestAccess::restore_display_output(mgr);
    CHECK(raw->full_upload_requests == 1);

    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "putting back a swapped-out flush callback asks the backend for a full upload",
                 "[application][display][egl_upload]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(
        disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    REQUIRE(disp->flush_cb != test_sentinel_flush_cb);

    mgr.restore_flush_cb(test_sentinel_flush_cb);
    CHECK(disp->flush_cb == test_sentinel_flush_cb);
    CHECK(raw->full_upload_requests == 1);

    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "the splash handoff's flush restore asks the backend for a full upload",
                 "[application][display][egl_upload]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);

    const pid_t splash_pid = fork();
    if (splash_pid == 0) {
        pause();
        _exit(0);
    }
    REQUIRE(splash_pid > 0);

    helix::application::SplashScreenManager splash;
    splash.start(splash_pid);
    splash.begin_render_suppression(splash_pid);
    CHECK(disp->flush_cb != test_sentinel_flush_cb);

    splash.restore_flush_callback(&mgr);
    CHECK(disp->flush_cb == test_sentinel_flush_cb);
    CHECK(raw->full_upload_requests == 1);

    // Only a saved callback is restored, so the failsafe's restore adds no upload.
    splash.arm_invalidation_failsafe(0);
    splash.run_handoff_frame(&mgr,
                             helix::application::SplashScreenManager::kInvalidationFailsafeMs);
    CHECK(raw->full_upload_requests == 1);

    kill(splash_pid, SIGKILL);
    waitpid(splash_pid, nullptr, 0);
    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a wake restores rendering even when the panel branch cannot run",
                 "[application][display][poweroff][flush_restore]") {
    // Suppression is engaged by enter_sleep() alone. Undoing it must not depend on
    // which panel mechanism is still available at wake: a no-op flush left installed
    // freezes the panel on its last frame while LVGL keeps painting, and every later
    // wake looks successful.
    DisplayManager mgr;
    auto backend =
        std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true, DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);
    lv_display_flush_cb_t real_cb = disp->flush_cb;

    DisplayManagerTestAccess::enter_sleep(mgr, 60);
    REQUIRE(raw->power_off_calls == 1);
    REQUIRE(DisplayManagerTestAccess::is_flush_suppressed(mgr));

    // The backend goes away before the wake (a swap, or teardown racing a wake).
    DisplayManagerTestAccess::set_backend(mgr, nullptr);

    DisplayManagerTestAccess::restore_display_output(mgr);

    CHECK_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    CHECK(disp->flush_cb == real_cb);

    DisplayManagerTestAccess::set_display(mgr, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "panel power-off hides the screen until the flush is restored",
                 "[application][display][sleep][poweroff][screen_hide]") {
    // Nothing is drawn while the panel is off, so a widget waiting on its own
    // draw must see the screen as hidden rather than stalled.
    REQUIRE_FALSE(helix::active_screen_hide_hold().is_held());
    DisplayManager mgr;
    DisplayManagerTestAccess::set_backend(
        mgr, std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    DisplayManagerTestAccess::set_use_power_off(mgr, true);
    lv_display_t* disp = lv_display_get_default();
    DisplayManagerTestAccess::set_display(mgr, disp);
    lv_display_set_flush_cb(disp, test_sentinel_flush_cb);
    lv_obj_t* screen = lv_screen_active();
    REQUIRE_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

    DisplayManagerTestAccess::enter_sleep(mgr, 60);
    REQUIRE(DisplayManagerTestAccess::last_sleep_mechanism(mgr) ==
            helix::SleepMechanism::PanelPowerOff);
    REQUIRE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

    DisplayManagerTestAccess::restore_display_output(mgr);
    CHECK_FALSE(DisplayManagerTestAccess::is_flush_suppressed(mgr));
    CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(helix::active_screen_hide_hold().is_held());

    DisplayManagerTestAccess::set_display(mgr, nullptr);
}
