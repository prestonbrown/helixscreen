// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mpc_detection.cpp
 * @brief Unit tests for Kalico detection and heater control type query
 *
 * Tests:
 * - PrinterDiscovery::is_kalico() flag behavior
 * - MoonrakerAdvancedAPI::get_heater_control_type() queries
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_discovery.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Global LVGL Initialization (called once)
// ============================================================================

namespace {
struct LVGLInitializerMPCDetection {
    LVGLInitializerMPCDetection() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerMPCDetection lvgl_init;

/// Scoped HELIX_MOCK_KALICO override. is_mock_kalico() re-reads the environment on
/// every printer.objects.query, so setting it around a call is enough — no restart,
/// no cached copy. Mirrors ScopedProbeType in test_mock_probe_discovery.cpp.
class ScopedKalico {
  public:
    explicit ScopedKalico(const char* value) {
        if (const char* prev = std::getenv("HELIX_MOCK_KALICO")) {
            had_prev_ = true;
            prev_ = prev;
        }
        setenv("HELIX_MOCK_KALICO", value, 1);
    }
    ~ScopedKalico() {
        if (had_prev_) {
            setenv("HELIX_MOCK_KALICO", prev_.c_str(), 1);
        } else {
            unsetenv("HELIX_MOCK_KALICO");
        }
    }

    ScopedKalico(const ScopedKalico&) = delete;
    ScopedKalico& operator=(const ScopedKalico&) = delete;

  private:
    bool had_prev_ = false;
    std::string prev_;
};
} // namespace

// ============================================================================
// PrinterDiscovery is_kalico tests
// ============================================================================

TEST_CASE("PrinterDiscovery::is_kalico() returns false by default", "[mpc_detection]") {
    PrinterDiscovery discovery;
    REQUIRE_FALSE(discovery.is_kalico());
}

TEST_CASE("PrinterDiscovery::is_kalico() returns true after set_is_kalico(true)",
          "[mpc_detection]") {
    PrinterDiscovery discovery;
    discovery.set_is_kalico(true);
    REQUIRE(discovery.is_kalico());
}

TEST_CASE("PrinterDiscovery::is_kalico() cleared on clear()", "[mpc_detection]") {
    PrinterDiscovery discovery;
    discovery.set_is_kalico(true);
    REQUIRE(discovery.is_kalico());
    discovery.clear();
    REQUIRE_FALSE(discovery.is_kalico());
}

// ============================================================================
// Heater Control Type Query Tests
// ============================================================================

class MPCDetectionTestFixture {
  public:
    MPCDetectionTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
    }
    ~MPCDetectionTestFixture() {
        api_.reset();
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

TEST_CASE_METHOD(MPCDetectionTestFixture,
                 "get_heater_control_type returns pid for default extruder", "[mpc_detection]") {
    std::atomic<bool> cb_fired{false};
    std::string control_type;

    api_->advanced().get_heater_control_type(
        "extruder",
        [&](const std::string& type) {
            control_type = type;
            cb_fired.store(true);
        },
        [&](const MoonrakerError&) { cb_fired.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(cb_fired.load());
    REQUIRE(control_type == "pid");
}

TEST_CASE_METHOD(MPCDetectionTestFixture, "get_heater_control_type returns pid for heater_bed",
                 "[mpc_detection]") {
    std::atomic<bool> cb_fired{false};
    std::string control_type;

    api_->advanced().get_heater_control_type(
        "heater_bed",
        [&](const std::string& type) {
            control_type = type;
            cb_fired.store(true);
        },
        [&](const MoonrakerError&) { cb_fired.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(cb_fired.load());
    REQUIRE(control_type == "pid");
}

TEST_CASE_METHOD(MPCDetectionTestFixture,
                 "get_heater_control_type errors for a heater absent from configfile.settings",
                 "[mpc_detection]") {
    // A heater the config never declares takes the `!settings.contains(heater)`
    // early return in MoonrakerAdvancedAPI::get_heater_control_type() — on_error,
    // never on_complete. Reporting a fabricated "pid" for a heater that does not
    // exist would let a caller size a PID-tune UI for nothing.
    std::atomic<bool> error_fired{false};
    std::atomic<bool> success_fired{false};
    std::string error_message;

    api_->advanced().get_heater_control_type(
        "nonexistent_heater", [&](const std::string&) { success_fired.store(true); },
        [&](const MoonrakerError& err) {
            error_message = err.message;
            error_fired.store(true);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(error_fired.load());
    REQUIRE_FALSE(success_fired.load());
    REQUIRE(error_message.find("not in config") != std::string::npos);
}

TEST_CASE_METHOD(MPCDetectionTestFixture, "get_heater_control_type returns mpc on a Kalico printer",
                 "[mpc_detection]") {
    // The mpc branch is the whole reason this query exists: Kalico's MPC heater model
    // has no PID constants, so a PID-tune UI must not be offered. HELIX_MOCK_KALICO=1
    // is what flips the mock's configfile.settings.extruder.control to "mpc"
    // (moonraker_client_mock_objects.cpp), so this drives the real API against a real
    // Kalico-shaped response rather than re-parsing the JSON by hand.
    ScopedKalico kalico("1");

    std::atomic<bool> success_fired{false};
    std::atomic<bool> error_fired{false};
    std::string control_type;

    api_->advanced().get_heater_control_type(
        "extruder",
        [&](const std::string& type) {
            control_type = type;
            success_fired.store(true);
        },
        [&](const MoonrakerError&) { error_fired.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(success_fired.load());
    REQUIRE_FALSE(error_fired.load());
    REQUIRE(control_type == "mpc");
}
