// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_runtime_config.h"
#include "backlight_backend.h"
#include "config.h"
#include "runtime_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

// RAII guard to temporarily enable test mode on global RuntimeConfig
/// Forces test_mode on for the scope; ScopedRuntimeConfig puts the whole
/// config back on the way out.
struct TestModeGuard {
    ScopedRuntimeConfig scoped_config;
    explicit TestModeGuard(RuntimeConfig* r) {
        r->test_mode = true;
    }
};

// ============================================================================
// brightness_cli_command (#972) — Creality Sonic Pad `brightness` helper
// ============================================================================

// Forward declaration of the pure command builder (defined in backlight_backend.cpp,
// outside the __linux__ guard so it is testable on any host).
namespace helix::backlight_internal {
std::string brightness_cli_command(int percent, int floor_percent = 0);
// Pure percent -> raw mapping shared by every backend (also defined in
// backlight_backend.cpp).
int raw_from_percent(int percent, int max_raw, int floor_percent);
int build_floor_percent(bool dev_disp_backend, int build_floor);
int backlight_floor_percent(int build_floor);
} // namespace helix::backlight_internal

TEST_CASE("brightness_cli_command: zero or negative powers the backlight off",
          "[api][backlight][cli]") {
    REQUIRE(helix::backlight_internal::brightness_cli_command(0) == "brightness -s 0");
    REQUIRE(helix::backlight_internal::brightness_cli_command(-10) == "brightness -s 0");
}

TEST_CASE("brightness_cli_command: positive powers on and scales 0-100 to 0-255",
          "[api][backlight][cli]") {
    REQUIRE(helix::backlight_internal::brightness_cli_command(100) ==
            "brightness -s 1; brightness -d 255");
    REQUIRE(helix::backlight_internal::brightness_cli_command(50) ==
            "brightness -s 1; brightness -d 127");
    // Smallest on-level never truncates to 0 (would read as "off").
    REQUIRE(helix::backlight_internal::brightness_cli_command(1) ==
            "brightness -s 1; brightness -d 2");
}

TEST_CASE("brightness_cli_command: floor lifts the lowest on-levels",
          "[api][backlight][cli][1709]") {
    REQUIRE(helix::backlight_internal::brightness_cli_command(0, 20) == "brightness -s 0");
    // Any on-level at or below the slider minimum lands exactly on the floor.
    REQUIRE(helix::backlight_internal::brightness_cli_command(1, 20) ==
            "brightness -s 1; brightness -d 51");
    REQUIRE(helix::backlight_internal::brightness_cli_command(10, 20) ==
            "brightness -s 1; brightness -d 51");
    REQUIRE(helix::backlight_internal::brightness_cli_command(100, 20) ==
            "brightness -s 1; brightness -d 255");
}

// ============================================================================
// raw_from_percent (#1709): percent -> raw with a per-panel floor
// ============================================================================

TEST_CASE("raw_from_percent: no floor keeps the plain linear map", "[api][backlight][1709]") {
    REQUIRE(helix::backlight_internal::raw_from_percent(0, 255, 0) == 0);
    REQUIRE(helix::backlight_internal::raw_from_percent(10, 255, 0) == 25);
    REQUIRE(helix::backlight_internal::raw_from_percent(50, 255, 0) == 127);
    REQUIRE(helix::backlight_internal::raw_from_percent(100, 255, 0) == 255);
}

TEST_CASE("raw_from_percent: with a floor, the slider range spans [floor, max]",
          "[api][backlight][1709]") {
    // 20% of 255 = 51, the boundary the K2 panel still renders visibly (#1709).
    // The slider's minimum (10) is the floor itself, so the dimmest setting a
    // user can pick is the dimmest the panel shows. Off stays a true 0 (sleep).
    REQUIRE(helix::backlight_internal::raw_from_percent(0, 255, 20) == 0);
    REQUIRE(helix::backlight_internal::raw_from_percent(1, 255, 20) == 51);
    REQUIRE(helix::backlight_internal::raw_from_percent(10, 255, 20) == 51);
    REQUIRE(helix::backlight_internal::raw_from_percent(55, 255, 20) == 153);
    REQUIRE(helix::backlight_internal::raw_from_percent(100, 255, 20) == 255);
}

TEST_CASE("build floor: the sysfs panel takes it, the /dev/disp panel does not",
          "[api][backlight][1709]") {
    // The floor is measured on community K2 firmware's sysfs backlight; stock
    // firmware's Allwinner /dev/disp panel stays lit to raw 6 of 255.
    REQUIRE(helix::backlight_internal::build_floor_percent(false, 20) == 20);
    REQUIRE(helix::backlight_internal::build_floor_percent(true, 20) == 0);
}

TEST_CASE("raw_from_percent: a requested-on level never maps to off", "[api][backlight][1709]") {
    // Small raw ranges truncate toward 0; a 0 the caller did not ask for reads
    // as "off" on the panel.
    REQUIRE(helix::backlight_internal::raw_from_percent(1, 10, 0) == 1);
    REQUIRE(helix::backlight_internal::raw_from_percent(1, 100, 0) == 1);
}

// Restores the floor key on the way out so one case's floor cannot leak into
// another case's backend in this binary.
struct ScopedBacklightFloor {
    helix::Config* config_ = helix::Config::get_instance();
    explicit ScopedBacklightFloor(int floor) {
        config_->set<int>("/display/backlight_floor_percent", floor);
    }
    ~ScopedBacklightFloor() {
        config_->set<int>("/display/backlight_floor_percent", 0);
    }
};

// ============================================================================
// BacklightBackend::supports_hardware_blank() Tests
// ============================================================================

TEST_CASE("BacklightBackend supports_hardware_blank defaults to false", "[api][backlight]") {
    // Ensure test_mode is off (prior tests may have left it enabled)
    get_runtime_config()->test_mode = false;
    // Factory creates None backend (no hardware). Key invariant: non-Allwinner
    // backends must NOT claim hardware blank support.
    auto backend = BacklightBackend::create();
    REQUIRE(backend != nullptr);
    REQUIRE_FALSE(backend->supports_hardware_blank());
}

TEST_CASE("BacklightBackend factory creates None backend without test mode", "[api][backlight]") {
    // Ensure test_mode is off (prior tests may have left it enabled)
    get_runtime_config()->test_mode = false;
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

TEST_CASE("backlight_floor_percent: the settings override wins on every backend",
          "[api][backlight][1709]") {
    {
        ScopedBacklightFloor floor(15);
        REQUIRE(helix::backlight_internal::backlight_floor_percent(20) == 15);
        REQUIRE(helix::backlight_internal::backlight_floor_percent(0) == 15);
    }
    helix::Config::get_instance()->get_json("/display").erase("backlight_floor_percent");
    REQUIRE(helix::backlight_internal::backlight_floor_percent(20) == 20);
    REQUIRE(helix::backlight_internal::backlight_floor_percent(0) == 0);
}

TEST_CASE("Sysfs backend honors /display/backlight_floor_percent",
          "[api][backlight][sysfs][1709]") {
    ScopedBacklightFloor floor(20);
    FakeSysfsBacklight fake(255);
    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    // Lowest nonzero percent lands at the floor, not at a near-black level.
    REQUIRE(backend->set_brightness(1));
    REQUIRE(fake.read_file("brightness") == "51"); // the floor

    // Sleep's 0 still writes a true off.
    REQUIRE(backend->set_brightness(0));
    REQUIRE(fake.read_file("brightness") == "0");
    REQUIRE(fake.read_file("bl_power") == "1");
}

#ifdef __linux__
TEST_CASE("Sysfs backend reports a write the kernel rejects", "[api][backlight][sysfs][1595]") {
    // /dev/full opens successfully and fails every write() with ENOSPC, which is
    // the shape of a sysfs attribute whose store handler refuses the value. A
    // buffered stream defers the write to flush, so stream state inspected before
    // closing still reads clean and a rejected write reports success.
    FakeSysfsBacklight fake;
    fs::remove(fake.device_dir / "brightness");
    fs::create_symlink("/dev/full", fake.device_dir / "brightness");

    auto backend = BacklightBackend::create_sysfs(fake.base_dir.string());
    REQUIRE(backend->is_available());

    REQUIRE_FALSE(backend->set_brightness(50));
}
#endif

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

TEST_CASE("Binary backlight maps any non-zero brightness to ON", "[api][backlight][sysfs]") {
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

TEST_CASE("None backend supports_dimming() returns false without simulate", "[api][backlight]") {
    // Ensure test_mode is off (prior tests may have left it enabled)
    get_runtime_config()->test_mode = false;
    // Production None backend: no dimming
    auto backend = BacklightBackend::create();
    REQUIRE(std::string(backend->name()) == "None");
    REQUIRE_FALSE(backend->supports_dimming());
}
