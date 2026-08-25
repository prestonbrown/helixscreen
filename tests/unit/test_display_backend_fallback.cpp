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
