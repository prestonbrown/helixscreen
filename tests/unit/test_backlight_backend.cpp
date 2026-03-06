// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backlight_backend.h"
#include "runtime_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

// RAII guard to temporarily enable test mode on global RuntimeConfig
struct TestModeGuard {
    RuntimeConfig* rc;
    bool prev;
    explicit TestModeGuard(RuntimeConfig* r) : rc(r), prev(r->test_mode) {
        rc->test_mode = true;
    }
    ~TestModeGuard() {
        rc->test_mode = prev;
    }
};

// ============================================================================
// BacklightBackend::supports_hardware_blank() Tests
// ============================================================================

TEST_CASE("BacklightBackend supports_hardware_blank defaults to false", "[api][backlight]") {
    // Factory creates None backend (no hardware). Key invariant: non-Allwinner
    // backends must NOT claim hardware blank support.
    auto backend = BacklightBackend::create();
    REQUIRE(backend != nullptr);
    REQUIRE_FALSE(backend->supports_hardware_blank());
}

TEST_CASE("BacklightBackend factory creates None backend without test mode", "[api][backlight]") {
    // Without test_mode, on a non-Linux (macOS) host, factory falls through to None
    auto backend = BacklightBackend::create();
    REQUIRE(backend != nullptr);
    REQUIRE(std::string(backend->name()) == "None");
    REQUIRE_FALSE(backend->is_available());
}

TEST_CASE("BacklightBackend factory creates Simulated backend in test mode", "[api][backlight]") {
    TestModeGuard guard(get_runtime_config());

    auto backend = BacklightBackend::create();
    REQUIRE(backend != nullptr);
    REQUIRE(std::string(backend->name()) == "Simulated");
    REQUIRE(backend->is_available());
    REQUIRE_FALSE(backend->supports_hardware_blank());

    // Simulated backend round-trips brightness
    REQUIRE(backend->set_brightness(75));
    REQUIRE(backend->get_brightness() == 75);

    REQUIRE(backend->set_brightness(0));
    REQUIRE(backend->get_brightness() == 0);

    REQUIRE(backend->set_brightness(100));
    REQUIRE(backend->get_brightness() == 100);
}

// ============================================================================
// Sysfs backend bl_power tests (Linux only)
// ============================================================================

#ifdef __linux__
namespace fs = std::filesystem;

// RAII helper to create a fake sysfs backlight tree in /tmp
struct FakeSysfsBacklight {
    fs::path base_dir;
    fs::path device_dir;

    explicit FakeSysfsBacklight(int max_brightness = 255) {
        base_dir = fs::temp_directory_path() / ("helix_test_bl_" + std::to_string(getpid()));
        device_dir = base_dir / "test_backlight";
        fs::create_directories(device_dir);

        write_file("max_brightness", std::to_string(max_brightness));
        write_file("brightness", std::to_string(max_brightness));
        write_file("bl_power", "0"); // 0 = on
    }

    ~FakeSysfsBacklight() {
        fs::remove_all(base_dir);
    }

    void write_file(const std::string& name, const std::string& value) const {
        std::ofstream f(device_dir / name);
        f << value;
    }

    std::string read_file(const std::string& name) const {
        std::ifstream f(device_dir / name);
        std::string value;
        f >> value;
        return value;
    }

    FakeSysfsBacklight(const FakeSysfsBacklight&) = delete;
    FakeSysfsBacklight& operator=(const FakeSysfsBacklight&) = delete;
};

TEST_CASE("Sysfs backend discovers fake backlight device", "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake;
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());

    REQUIRE(backend->is_available());
    REQUIRE(std::string(backend->name()) == "Sysfs");
}

TEST_CASE("Sysfs backend set_brightness writes brightness file", "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake(255);
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    REQUIRE(backend->set_brightness(50));
    // 50% of 255 = 127
    REQUIRE(fake.read_file("brightness") == "127");
}

TEST_CASE("Sysfs backend set_brightness(0) sets bl_power off", "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake;
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    // Initially bl_power is on (0)
    REQUIRE(fake.read_file("bl_power") == "0");

    // Setting brightness to 0 should power off the backlight
    REQUIRE(backend->set_brightness(0));
    REQUIRE(fake.read_file("brightness") == "0");
    REQUIRE(fake.read_file("bl_power") == "1");
}

TEST_CASE("Sysfs backend restores bl_power on when brightness > 0", "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake;
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    // Power off
    REQUIRE(backend->set_brightness(0));
    REQUIRE(fake.read_file("bl_power") == "1");

    // Power back on
    REQUIRE(backend->set_brightness(75));
    REQUIRE(fake.read_file("bl_power") == "0");
}

TEST_CASE("Sysfs backend works without bl_power file", "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake;
    // Remove bl_power to simulate a driver that doesn't expose it
    fs::remove(fake.device_dir / "bl_power");

    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    // Should still work — bl_power write is non-fatal
    REQUIRE(backend->set_brightness(0));
    REQUIRE(fake.read_file("brightness") == "0");

    REQUIRE(backend->set_brightness(100));
    REQUIRE(fake.read_file("brightness") == "255");
}
TEST_CASE("Sysfs supports_dimming() returns false when max_brightness=1",
          "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake(1); // Binary backlight (GPIO on/off)
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());
    REQUIRE_FALSE(backend->supports_dimming());
}

TEST_CASE("Binary backlight maps any non-zero brightness to ON",
          "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake(1); // Binary backlight (GPIO on/off)
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    // Any non-zero percent must map to 1 (ON), not truncate to 0 via
    // integer division. This was the root cause of AD5X wake failure (#326).
    REQUIRE(backend->set_brightness(50));
    REQUIRE(fake.read_file("brightness") == "1");

    REQUIRE(backend->set_brightness(10));
    REQUIRE(fake.read_file("brightness") == "1");

    REQUIRE(backend->set_brightness(1));
    REQUIRE(fake.read_file("brightness") == "1");

    REQUIRE(backend->set_brightness(100));
    REQUIRE(fake.read_file("brightness") == "1");

    // 0% must still turn OFF
    REQUIRE(backend->set_brightness(0));
    REQUIRE(fake.read_file("brightness") == "0");
}

TEST_CASE("Sysfs supports_dimming() returns true when max_brightness > 1",
          "[api][backlight][sysfs]") {
    FakeSysfsBacklight fake(255); // PWM backlight with full range
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());
    REQUIRE(backend->supports_dimming());
}

#endif // __linux__

// ============================================================================
// BacklightBackendNone::supports_dimming() Tests
// ============================================================================

TEST_CASE("None backend supports_dimming() mirrors simulate flag", "[api][backlight]") {
    // In test mode (simulated), supports_dimming should be true
    TestModeGuard guard(get_runtime_config());
    auto backend = BacklightBackend::create();
    REQUIRE(std::string(backend->name()) == "Simulated");
    REQUIRE(backend->supports_dimming());
}

TEST_CASE("None backend supports_dimming() returns false without simulate",
          "[api][backlight]") {
    // Production None backend: no dimming
    auto backend = BacklightBackend::create();
    REQUIRE(std::string(backend->name()) == "None");
    REQUIRE_FALSE(backend->supports_dimming());
}
