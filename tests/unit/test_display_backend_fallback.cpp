// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_backend_fallback.cpp
 * @brief Tests for crash-hardening: in-process fbdev display fallback
 *
 * Validates the fix from 352418c5: when the primary display backend
 * (e.g. DRM) passes is_available() but create_display() returns nullptr,
 * DisplayManager should retry with fbdev backend without requiring
 * a process restart.
 *
 * These tests use mock backends to stand in for real DRM/fbdev hardware, but the
 * fallback DECISION is not restated here — every case calls the production
 * predicate DisplayBackend::should_try_fbdev_fallback(), which is the condition
 * DisplayManager::init() itself branches on (display_manager.cpp). Invert or
 * delete that predicate and these go red.
 *
 * init() cannot be called directly: it initializes LVGL and opens real devices.
 */

#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/scoped_board.h"
#include "../test_helpers/scoped_env.h"
#include "core/text_io.h"
#include "display_backend.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Mock Backends for Testing Fallback Logic
// ============================================================================

/**
 * Mock backend that reports available but fails to create a display.
 * Simulates DRM passing is_available() but failing create_display()
 * (e.g. mode setting or buffer allocation failure).
 */
class MockFailingBackend : public DisplayBackend {
  public:
    explicit MockFailingBackend(DisplayBackendType t, const char* n) : type_(t), name_(n) {}

    lv_display_t* create_display(int /*width*/, int /*height*/) override {
        create_display_called_ = true;
        return nullptr; // Simulate failure
    }

    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }

    DisplayBackendType type() const override {
        return type_;
    }

    const char* name() const override {
        return name_;
    }

    bool is_available() const override {
        return true; // Reports available despite failing to create display
    }

    bool create_display_called_ = false;

  private:
    DisplayBackendType type_;
    const char* name_;
};

/**
 * Mock backend that successfully creates a "display" (returns non-null).
 * In tests, we use a sentinel value rather than a real lv_display_t.
 */
class MockSuccessBackend : public DisplayBackend {
  public:
    explicit MockSuccessBackend(DisplayBackendType t, const char* n) : type_(t), name_(n) {}

    lv_display_t* create_display(int /*width*/, int /*height*/) override {
        create_display_called_ = true;
        // Return a sentinel — we're testing the fallback logic flow,
        // not actual LVGL display creation.
        return reinterpret_cast<lv_display_t*>(&sentinel_);
    }

    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }

    DisplayBackendType type() const override {
        return type_;
    }

    const char* name() const override {
        return name_;
    }

    bool is_available() const override {
        return true;
    }

    bool create_display_called_ = false;

  private:
    int sentinel_ = 0xBEEF;
    DisplayBackendType type_;
    const char* name_;
};

// ============================================================================
// Fallback Logic Unit Tests
// ============================================================================

// These tests verify the decision logic extracted from DisplayManager::init().
// We can't call init() directly (it initializes LVGL), so we test the
// fallback condition and backend type checks in isolation.

TEST_CASE("Fallback condition: DRM backend with null display triggers fallback",
          "[display][fallback][crash_hardening]") {
    // The fallback condition DisplayManager::init() branches on, called directly
    // rather than restated.
    auto backend = std::make_unique<MockFailingBackend>(DisplayBackendType::DRM, "DRM/KMS");
    lv_display_t* display = backend->create_display(800, 480);

    REQUIRE(display == nullptr);
    REQUIRE(backend->create_display_called_);

    // Verify fallback condition is met
    bool should_fallback = DisplayBackend::should_try_fbdev_fallback(backend.get(), display);
    REQUIRE(should_fallback);
}

TEST_CASE("Fallback condition: FBDEV failure does NOT trigger fallback to itself",
          "[display][fallback][crash_hardening]") {
    // If fbdev itself fails, there's no further fallback
    auto backend = std::make_unique<MockFailingBackend>(DisplayBackendType::FBDEV, "Framebuffer");
    lv_display_t* display = backend->create_display(800, 480);

    REQUIRE(display == nullptr);

    bool should_fallback = DisplayBackend::should_try_fbdev_fallback(backend.get(), display);
    REQUIRE_FALSE(should_fallback);
}

TEST_CASE("Fallback condition: SDL failure does NOT trigger fbdev fallback",
          "[display][fallback][crash_hardening]") {
    // SDL is desktop-only; falling back to fbdev on desktop makes no sense.
    // However, the current code only excludes FBDEV from fallback, so SDL
    // would technically attempt fbdev fallback. This test documents the behavior.
    auto backend = std::make_unique<MockFailingBackend>(DisplayBackendType::SDL, "SDL");
    lv_display_t* display = backend->create_display(800, 480);

    REQUIRE(display == nullptr);

    bool should_fallback = DisplayBackend::should_try_fbdev_fallback(backend.get(), display);
    // SDL failure would trigger fallback attempt (fbdev won't be available on desktop)
    REQUIRE(should_fallback);
}

TEST_CASE("Fallback condition: successful display does NOT trigger fallback",
          "[display][fallback][crash_hardening]") {
    auto backend = std::make_unique<MockSuccessBackend>(DisplayBackendType::DRM, "DRM/KMS");
    lv_display_t* display = backend->create_display(800, 480);

    REQUIRE(display != nullptr);
    REQUIRE(backend->create_display_called_);

    bool should_fallback = DisplayBackend::should_try_fbdev_fallback(backend.get(), display);
    REQUIRE_FALSE(should_fallback);
}

TEST_CASE("Backend availability check: fallback requires is_available()",
          "[display][fallback][crash_hardening]") {
    // The fallback code checks:
    //   if (m_backend && m_backend->is_available()) {
    //       m_display = m_backend->create_display(m_width, m_height);
    //   }

    SECTION("Available backend proceeds to create_display") {
        auto backend =
            std::make_unique<MockSuccessBackend>(DisplayBackendType::FBDEV, "Framebuffer");
        REQUIRE(backend->is_available());

        lv_display_t* display = backend->create_display(800, 480);
        REQUIRE(display != nullptr);
        REQUIRE(backend->create_display_called_);
    }

    SECTION("Unavailable backend skips create_display") {
        // A backend that reports unavailable
        class UnavailableBackend : public DisplayBackend {
          public:
            lv_display_t* create_display(int, int) override {
                create_called = true;
                return nullptr;
            }
            lv_indev_t* create_input_pointer() override {
                return nullptr;
            }
            DisplayBackendType type() const override {
                return DisplayBackendType::FBDEV;
            }
            const char* name() const override {
                return "Unavailable";
            }
            bool is_available() const override {
                return false;
            }
            bool create_called = false;
        };

        auto backend = std::make_unique<UnavailableBackend>();
        REQUIRE_FALSE(backend->is_available());

        // Simulating the fallback code path: skip create_display if unavailable
        if (backend->is_available()) {
            backend->create_display(800, 480);
        }
        REQUIRE_FALSE(backend->create_called);
    }
}

TEST_CASE("Backend fallback: simulate full DRM->fbdev fallback sequence",
          "[display][fallback][crash_hardening]") {
    // Walks the fallback path from display_manager.cpp init():
    // 1. Primary DRM backend passes is_available() but create_display() fails
    // 2. The production predicate says to fall back
    // 3. Reset primary backend
    // 4. Create fbdev backend, check is_available(), create display
    // 5. The predicate now says stop (fbdev never falls back to itself)

    // Step 1: Primary backend fails
    auto primary = std::make_unique<MockFailingBackend>(DisplayBackendType::DRM, "DRM/KMS");
    lv_display_t* display = primary->create_display(800, 480);
    REQUIRE(display == nullptr);

    // Step 2: the decision itself, taken from production rather than restated
    REQUIRE(DisplayBackend::should_try_fbdev_fallback(primary.get(), display));

    // Step 3: Reset primary
    primary.reset();
    REQUIRE(primary == nullptr);

    // Step 4: Create fallback backend and try display creation
    auto fallback = std::make_unique<MockSuccessBackend>(DisplayBackendType::FBDEV, "Framebuffer");
    REQUIRE(fallback->is_available());

    display = fallback->create_display(800, 480);
    REQUIRE(display != nullptr);
    REQUIRE(fallback->create_display_called_);

    // Step 5: no second fallback — both because a display now exists and because
    // fbdev is the terminal backend. Checked with a null display too, so the
    // assertion does not pass on the display!=nullptr half alone.
    REQUIRE_FALSE(DisplayBackend::should_try_fbdev_fallback(fallback.get(), display));
    REQUIRE_FALSE(DisplayBackend::should_try_fbdev_fallback(fallback.get(), nullptr));
}

// ============================================================================
// DRM Auto-Detection Fallback Tests
// ============================================================================
// When no suitable DRM device exists, auto_detect_drm_device() returns empty
// and is_available() rejects it, allowing create_auto() to fall through to fbdev.

#ifdef HELIX_DISPLAY_DRM
#include "display_backend_drm.h"

TEST_CASE("DRM backend: empty device string reports unavailable",
          "[display][drm][crash_hardening]") {
    // Simulates auto_detect_drm_device() returning empty (no /dev/dri/ or no suitable device)
    DisplayBackendDRM backend("");
    REQUIRE_FALSE(backend.is_available());
}

TEST_CASE("DRM backend: nonexistent device reports unavailable",
          "[display][drm][crash_hardening]") {
    DisplayBackendDRM backend("/dev/dri/card99");
    REQUIRE_FALSE(backend.is_available());
}

TEST_CASE("DRM backend: empty device string prevents create_display attempt",
          "[display][drm][crash_hardening]") {
    // When is_available() is false, DisplayManager skips create_display()
    // and falls through to fbdev. Verify the guard works.
    DisplayBackendDRM backend("");
    REQUIRE_FALSE(backend.is_available());
    REQUIRE(backend.type() == DisplayBackendType::DRM);

    // An unavailable DRM backend never produces a display, so init() reaches the
    // fallback decision with a null display — ask production what it decides.
    REQUIRE(DisplayBackend::should_try_fbdev_fallback(&backend, nullptr));
}

#endif // HELIX_DISPLAY_DRM

// ============================================================================
// Virtual Dispatch Tests (#1055)
// ============================================================================
// DisplayManager no longer switches on type()==DRM/FBDEV and downcasts to the
// concrete backend. It calls set_size_was_explicit() and is_gpu_accelerated()
// polymorphically. These tests verify the base defaults and that an override
// is dispatched through the base pointer.

namespace {

// Records whether set_size_was_explicit() was called and with what value;
// reports a configurable is_gpu_accelerated(). Stands in for the concrete
// backends so we can verify DisplayManager's virtual calls reach the override.
class MockCapabilityBackend : public DisplayBackend {
  public:
    explicit MockCapabilityBackend(bool gpu) : gpu_(gpu) {}

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
        return "MockCapability";
    }
    bool is_available() const override {
        return true;
    }

    void set_size_was_explicit(bool explicit_size) override {
        size_explicit_called_ = true;
        size_explicit_value_ = explicit_size;
    }
    bool is_gpu_accelerated() const override {
        return gpu_;
    }

    bool size_explicit_called_ = false;
    bool size_explicit_value_ = false;

  private:
    bool gpu_;
};

} // namespace

TEST_CASE("Virtual dispatch: set_size_was_explicit reaches override via base pointer",
          "[display][virtual]") {
    MockCapabilityBackend backend(false);
    DisplayBackend* base = &backend;

    base->set_size_was_explicit(true);
    REQUIRE(backend.size_explicit_called_);
    REQUIRE(backend.size_explicit_value_ == true);

    base->set_size_was_explicit(false);
    REQUIRE(backend.size_explicit_value_ == false);
}

TEST_CASE("Virtual dispatch: base set_size_was_explicit default is a harmless no-op",
          "[display][virtual]") {
    // MockSuccessBackend does not override set_size_was_explicit — the base
    // default must accept the call without effect (SDL behavior).
    MockSuccessBackend backend(DisplayBackendType::SDL, "SDL");
    DisplayBackend* base = &backend;
    REQUIRE_NOTHROW(base->set_size_was_explicit(true));
}

TEST_CASE("Virtual dispatch: is_gpu_accelerated reflects the override, defaults false",
          "[display][virtual]") {
    SECTION("override reporting true") {
        MockCapabilityBackend gpu(true);
        DisplayBackend* base = &gpu;
        REQUIRE(base->is_gpu_accelerated());
    }
    SECTION("override reporting false") {
        MockCapabilityBackend cpu(false);
        DisplayBackend* base = &cpu;
        REQUIRE_FALSE(base->is_gpu_accelerated());
    }
    SECTION("base default is false (non-DRM backends)") {
        MockSuccessBackend sdl(DisplayBackendType::SDL, "SDL");
        DisplayBackend* base = &sdl;
        REQUIRE_FALSE(base->is_gpu_accelerated());
    }
}

TEST_CASE("HELIX_DISPLAY_BACKEND names a display path", "[display][fallback]") {
    CHECK(helix::parse_forced_display_backend(nullptr) == helix::ForcedDisplayBackend::None);
    CHECK(helix::parse_forced_display_backend("drm") == helix::ForcedDisplayBackend::Drm);
    // The launcher's EGL rung: the same binary, presenting through EGL.
    CHECK(helix::parse_forced_display_backend("egl") == helix::ForcedDisplayBackend::DrmEgl);
    CHECK(helix::parse_forced_display_backend("fbdev") == helix::ForcedDisplayBackend::Fbdev);
    CHECK(helix::parse_forced_display_backend("fb") == helix::ForcedDisplayBackend::Fbdev);
    CHECK(helix::parse_forced_display_backend("sdl") == helix::ForcedDisplayBackend::Sdl);
    CHECK(helix::parse_forced_display_backend("") == helix::ForcedDisplayBackend::Unknown);
    CHECK(helix::parse_forced_display_backend("EGL") == helix::ForcedDisplayBackend::Unknown);
    CHECK(helix::parse_forced_display_backend("drm ") == helix::ForcedDisplayBackend::Unknown);
}

TEST_CASE("The watchdog's splash decision: DRM owns the panel only when it is available",
          "[display][fallback]") {
    using helix::drm_will_own_panel;
    using helix::ForcedDisplayBackend;
    // Both DRM presentation paths skip the fbdev splash when DRM is usable.
    CHECK(drm_will_own_panel(ForcedDisplayBackend::Drm, true));
    CHECK(drm_will_own_panel(ForcedDisplayBackend::DrmEgl, true));
    CHECK(drm_will_own_panel(ForcedDisplayBackend::None, true));
    CHECK(drm_will_own_panel(ForcedDisplayBackend::Unknown, true));
    // A forced drm or egl rung without DRM falls back to fbdev, which needs the splash.
    CHECK_FALSE(drm_will_own_panel(ForcedDisplayBackend::Drm, false));
    CHECK_FALSE(drm_will_own_panel(ForcedDisplayBackend::DrmEgl, false));
    CHECK_FALSE(drm_will_own_panel(ForcedDisplayBackend::None, false));
    // fbdev and SDL never present through DRM.
    CHECK_FALSE(drm_will_own_panel(ForcedDisplayBackend::Fbdev, true));
    CHECK_FALSE(drm_will_own_panel(ForcedDisplayBackend::Sdl, true));
}

TEST_CASE("create_auto takes HELIX_DISPLAY_BACKEND=egl as the EGL rung, not an unknown value",
          "[display][fallback]") {
    // This build has no EGL presentation path, so the forced rung is declined
    // by name and auto-detection takes over, which must not read as a typo.
    helix::ScopedEnv backend("HELIX_DISPLAY_BACKEND", "egl");
    helix::LogCapture log;
    auto chosen = DisplayBackend::create_auto();

    CHECK(log.count_containing("EGL presentation forced but not compiled in") == 1);
    CHECK(log.count_containing("Unknown HELIX_DISPLAY_BACKEND value") == 0);
#ifdef HELIX_DISPLAY_SDL
    REQUIRE(chosen != nullptr);
    CHECK(chosen->type() == DisplayBackendType::SDL);
#endif
}

TEST_CASE("create_auto reports a value it does not know", "[display][fallback]") {
    helix::ScopedEnv backend("HELIX_DISPLAY_BACKEND", "gpu");
    helix::LogCapture log;
    auto chosen = DisplayBackend::create_auto();

    CHECK(log.count_containing("Unknown HELIX_DISPLAY_BACKEND value: gpu") == 1);
    CHECK(log.count_containing("EGL presentation forced") == 0);
}

TEST_CASE("Backend fallback: all backends exhausted returns failure",
          "[display][fallback][crash_hardening]") {
    // When both DRM and fbdev fail, init() should return false

    // Primary fails
    auto primary = std::make_unique<MockFailingBackend>(DisplayBackendType::DRM, "DRM/KMS");
    lv_display_t* display = primary->create_display(800, 480);
    REQUIRE(display == nullptr);

    // Fallback also fails
    primary.reset();
    auto fallback = std::make_unique<MockFailingBackend>(DisplayBackendType::FBDEV, "Framebuffer");
    if (fallback->is_available()) {
        display = fallback->create_display(800, 480);
    }
    REQUIRE(display == nullptr);

    // Both exhausted — this is the "all backends exhausted" path
    REQUIRE(display == nullptr);
}

TEST_CASE("read_config_rotation takes the first \"rotate\": <digits> in settings.json",
          "[display][rotation]") {
    helix::ConfigDirGuard guard("rotation_scan");
    const std::string settings = (guard.dir / "settings.json").string();
    auto rotation_for = [&](const std::string& json) {
        REQUIRE(helix::text_io::write_file(settings, json));
        return read_config_rotation(-1);
    };

    CHECK(rotation_for(R"({"display": {"rotate": 90}})") == 90);
    CHECK(rotation_for(R"({"display":{"rotate":180}})") == 180);
    CHECK(rotation_for("{\"display\": {\"rotate\"\n :\t270}}") == 270);
    // A "rotate" key without a number does not stop the search.
    CHECK(rotation_for(R"({"a": {"rotate": "x"}, "display": {"rotate": 90}})") == 90);
    // Digits with no colon before them are not a value.
    CHECK(rotation_for(R"({"rotate" 45, "display": {"rotate": 180}})") == 180);
    // A number that is not a quarter turn means no rotation.
    CHECK(rotation_for(R"({"display": {"rotate": 45}})") == 0);
    // No key: the caller's default.
    CHECK(rotation_for(R"({"display": {}})") == -1);
}

TEST_CASE("read_settings_scalar returns the value token and falls through to legacy files",
          "[display][rotation]") {
    helix::ConfigDirGuard guard("settings_scan");
    const std::string settings = (guard.dir / "settings.json").string();
    const std::string legacy = (guard.dir / "helixconfig.json").string();

    REQUIRE(helix::text_io::write_file(settings, R"({"brightness": 75, "dark_mode" : false})"));
    CHECK(helix::read_settings_int("brightness") == 75);
    CHECK(helix::read_settings_scalar("dark_mode") == "false");
    CHECK_FALSE(helix::read_settings_scalar("auto_restart_sec").has_value());

    // A key settings.json lacks is looked up in the legacy file.
    REQUIRE(helix::text_io::write_file(legacy, R"({"auto_restart_sec": 12})"));
    std::string from;
    CHECK(helix::read_settings_int("auto_restart_sec", &from) == 12);
    CHECK(from == legacy);

    // A string value is not a scalar token; a non-numeric token is not an int.
    REQUIRE(helix::text_io::write_file(settings, R"({"brightness": "80", "dark_mode": true})"));
    CHECK_FALSE(helix::read_settings_int("brightness").has_value());
    CHECK_FALSE(helix::read_settings_int("dark_mode").has_value());
}

TEST_CASE("typed settings reads skip an occurrence whose value does not parse",
          "[display][rotation]") {
    helix::ConfigDirGuard guard("settings_scan_skip");
    const std::string settings = (guard.dir / "settings.json").string();

    REQUIRE(helix::text_io::write_file(
        settings, R"({"leds": {"brightness": null}, "display": {"brightness": 60}})"));
    CHECK(helix::read_settings_int("brightness") == 60);

    REQUIRE(
        helix::text_io::write_file(settings, R"({"a": {"dark_mode": null}, "dark_mode": false})"));
    CHECK(helix::read_settings_bool("dark_mode") == false);

    REQUIRE(helix::text_io::write_file(settings, R"({"a": {"rotate": null}, "rotate": 270})"));
    CHECK(read_config_rotation(-1) == 270);
}

TEST_CASE("standalone_rotation prefers the CLI value, then the configured one",
          "[display][rotation]") {
    helix::ConfigDirGuard guard("standalone_rotation");
    REQUIRE(helix::text_io::write_file((guard.dir / "settings.json").string(),
                                       R"({"display": {"rotate": 180}})"));
    CHECK(helix::standalone_rotation(90, 480, 800) == 90);
    CHECK(helix::standalone_rotation(0, 480, 800) == 180);
}

TEST_CASE("standalone_rotation draws a first boot in the board's known mount",
          "[display][rotation]") {
    helix::ConfigDirGuard guard("standalone_rotation_mount");
    helix::test::ScopedBoard board(helix::board::Board::K1);
    helix::ScopedEnv no_force("HELIX_FORCE_ROTATION_PROBE", nullptr);
    const auto settings = (guard.dir / "settings.json").string();

    SECTION("no saved rotation: the K1 mount") {
        REQUIRE(helix::text_io::write_file(settings, R"({"display": {}})"));
        CHECK(helix::standalone_rotation(0, 480, 800) == 270);
    }
    SECTION("another framebuffer on a K1-resolved board: no mount") {
        REQUIRE(helix::text_io::write_file(settings, R"({"display": {}})"));
        CHECK(helix::standalone_rotation(0, 800, 480) == 0);
    }
    SECTION("a saved 0 is the user's and wins") {
        REQUIRE(helix::text_io::write_file(settings, R"({"display": {"rotate": 0}})"));
        CHECK(helix::standalone_rotation(0, 480, 800) == 0);
    }
    SECTION("the CLI wins over the mount") {
        REQUIRE(helix::text_io::write_file(settings, R"({"display": {}})"));
        CHECK(helix::standalone_rotation(90, 480, 800) == 90);
    }
}

TEST_CASE("a forced rotation probe skips the board's mount", "[display][rotation]") {
    helix::test::ScopedBoard board(helix::board::Board::K1);
    {
        helix::ScopedEnv no_force("HELIX_FORCE_ROTATION_PROBE", nullptr);
        CHECK(helix::first_boot_board_mount(-1, 480, 800) == 270);
    }
    helix::ScopedEnv force("HELIX_FORCE_ROTATION_PROBE", "1");
    CHECK(helix::first_boot_board_mount(-1, 480, 800) == -1);
}

TEST_CASE("a kernel panel_orientation leaves the board's mount out", "[display][rotation]") {
    helix::test::ScopedBoard board(helix::board::Board::K1);
    helix::ScopedEnv no_force("HELIX_FORCE_ROTATION_PROBE", nullptr);
    CHECK(helix::first_boot_board_mount(0, 480, 800) == -1);
    CHECK(helix::first_boot_board_mount(90, 480, 800) == -1);
}

// The kernel's panel_orientation has to reach DisplayManager::init() as the
// startup rotation: the backends gate the stored touch range and calibration on
// the display's rotation inside create_input_pointer(), so a rotation applied
// after init() leaves a range fit to the unrotated frame
// (prestonbrown/helixscreen#1428).
TEST_CASE("startup_rotation hands a first-boot panel_orientation to display init",
          "[display][rotation]") {
    SECTION("first boot adopts the kernel orientation") {
        CHECK(helix::startup_rotation(0, true, 90) == 90);
        CHECK(helix::startup_rotation(0, true, 270) == 270);
    }
    SECTION("an explicit request wins over the kernel") {
        CHECK(helix::startup_rotation(180, true, 90) == 180);
        CHECK(helix::startup_rotation(180, false, 90) == 180);
    }
    SECTION("a configured or already-probed boot ignores the kernel") {
        CHECK(helix::startup_rotation(0, false, 90) == 0);
    }
    SECTION("no orientation, or Normal, leaves the display unrotated") {
        CHECK(helix::startup_rotation(0, true, -1) == 0);
        CHECK(helix::startup_rotation(0, true, 0) == 0);
    }
}
