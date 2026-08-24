// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file test_rediscovery_fingerprint.cpp
/// @brief Fingerprint stability across a re-discovery on the SAME connection.
///
/// notify_klippy_ready re-triggers the full discovery pass unconditionally
/// (moonraker_client.cpp, "Klippy ready" -> invoke_connected_callback). That
/// replay is meant to be absorbed by the hw_changed fingerprint gate in
/// Application::on_discovery_complete (#1117), which skips LED chip
/// population, printer-type auto-detect, heater-role heal + config save, the
/// validator toast, the targeted reconfig wizard, and telemetry snapshots
/// whenever the hardware shape is unchanged.
///
/// The gate only works if compute_hardware_fingerprint() returns the SAME
/// value for two discovery passes over identical Moonraker responses. Every
/// case in test_hardware_fingerprint.cpp builds a PrinterDiscovery by hand, so
/// none of them exercise the real sequence -- they would stay green even if
/// the live pipeline produced a different fingerprint on every replay.
///
/// This drives MoonrakerDiscoverySequence itself, repeatedly, in one process,
/// with no disconnect in between -- which is exactly what a klippy restart
/// looks like (a klippy restart does not drop the WebSocket, so neither
/// clear_cache() nor reset_identified() runs between passes).

#include "../lvgl_test_fixture.h"
#include "hardware_fingerprint.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"

#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Exposes the REAL discovery sequence. MoonrakerClientMock overrides
/// discover_printer() with mock logic; this reaches past it to
/// MoonrakerClient::discover_printer -> discovery_.start(), while send_jsonrpc()
/// still resolves through the mock's method handler registry.
/// Same approach as TestDiscoveryClient in test_discovery_klippy_gate.cpp.
class RediscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real(std::function<void()> on_complete,
                               std::function<void(const std::string&)> on_error) {
        MoonrakerClient::discover_printer(std::move(on_complete), std::move(on_error));
    }
};

/// What one discovery pass produced, captured from the on_discovery_complete
/// callback -- the same value Application::on_discovery_complete fingerprints.
struct Pass {
    bool completed = false;
    bool errored = false;
    std::string error_reason;
    size_t fingerprint = 0;
    helix::PrinterDiscovery hw;
};

Pass run_pass(RediscoveryClient& client) {
    Pass p;
    client.set_on_discovery_complete(
        [&p](const helix::PrinterDiscovery& hw, const nlohmann::json& /*initial_status*/) {
            p.hw = hw;
            p.fingerprint = helix::compute_hardware_fingerprint(hw);
        });
    client.discover_printer_real([&p]() { p.completed = true; },
                                 [&p](const std::string& reason) {
                                     p.errored = true;
                                     p.error_reason = reason;
                                 });
    return p;
}

std::string join(const std::vector<std::string>& v) {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i)
            os << ", ";
        os << v[i];
    }
    return os.str();
}

/// Name the field that moved, so a failure is actionable instead of "two
/// size_t hashes differ".
std::string describe_delta(const helix::PrinterDiscovery& a, const helix::PrinterDiscovery& b) {
    std::ostringstream os;
    auto vec = [&os](const char* name, const std::vector<std::string>& x,
                     const std::vector<std::string>& y) {
        if (x != y) {
            os << "\n  " << name << ": [" << join(x) << "] -> [" << join(y) << "]";
        }
    };
    auto str = [&os](const char* name, const std::string& x, const std::string& y) {
        if (x != y) {
            os << "\n  " << name << ": '" << x << "' -> '" << y << "'";
        }
    };
    vec("heaters", a.heaters(), b.heaters());
    vec("fans", a.fans(), b.fans());
    vec("sensors", a.sensors(), b.sensors());
    vec("leds", a.leds(), b.leds());
    vec("steppers", a.steppers(), b.steppers());
    vec("filament_sensors", a.filament_sensor_names(), b.filament_sensor_names());
    vec("printer_objects", a.printer_objects(), b.printer_objects());
    str("hostname", a.hostname(), b.hostname());
    str("mcu", a.mcu(), b.mcu());
    str("kinematics", a.kinematics(), b.kinematics());

    // macros() is an unordered_set -- report set difference, not order.
    const auto& ma = a.macros();
    const auto& mb = b.macros();
    if (ma != mb) {
        std::vector<std::string> only_a, only_b;
        for (const auto& m : ma) {
            if (mb.find(m) == mb.end())
                only_a.push_back(m);
        }
        for (const auto& m : mb) {
            if (ma.find(m) == ma.end())
                only_b.push_back(m);
        }
        os << "\n  macros: " << ma.size() << " -> " << mb.size() << " (lost: [" << join(only_a)
           << "], gained: [" << join(only_b) << "])";
    }

    std::string out = os.str();
    return out.empty() ? std::string("\n  (no field-level difference found; the delta is in a "
                                     "capability flag or another hashed field)")
                       : out;
}

} // namespace

// ============================================================================

TEST_CASE("Re-discovery on the same connection keeps the hardware fingerprint stable",
          "[discovery][hardware_fingerprint][rediscovery]") {
    LVGLTestFixture fixture;

    RediscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    // Three passes, no disconnect between them: pass 1 is the WebSocket-connect
    // discovery, passes 2 and 3 are notify_klippy_ready replays.
    Pass p1 = run_pass(client);
    Pass p2 = run_pass(client);
    Pass p3 = run_pass(client);

    INFO("pass1 completed=" << p1.completed << " errored=" << p1.errored << " (" << p1.error_reason
                            << ")");
    INFO("pass2 completed=" << p2.completed << " errored=" << p2.errored << " (" << p2.error_reason
                            << ")");
    INFO("pass3 completed=" << p3.completed << " errored=" << p3.errored << " (" << p3.error_reason
                            << ")");
    REQUIRE(p1.completed);
    REQUIRE(p2.completed);
    REQUIRE(p3.completed);

    // Guard against a vacuous pass: an empty PrinterDiscovery would hash
    // identically every time and prove nothing.
    INFO("pass1 objects=" << p1.hw.printer_objects().size() << " heaters=" << p1.hw.heaters().size()
                          << " fans=" << p1.hw.fans().size()
                          << " macros=" << p1.hw.macros().size());
    REQUIRE(p1.hw.printer_objects().size() > 5);
    REQUIRE_FALSE(p1.hw.heaters().empty());

    // The mock answers printer.objects.list from self->hardware(), which is the
    // very PrinterDiscovery the sequence overwrites via parse_objects(). So
    // "identical Moonraker responses" is something to PROVE here, not assume:
    // if the object list is stable across passes then every pass received the
    // same input and the feedback loop is inert. Real Moonraker returns a
    // config-derived list that does not depend on what we parsed last time.
    INFO("objects.list input drifted between passes" << describe_delta(p1.hw, p2.hw));
    REQUIRE(p1.hw.printer_objects() == p2.hw.printer_objects());
    REQUIRE(p2.hw.printer_objects() == p3.hw.printer_objects());

    // The actual contract: same hardware in, same fingerprint out. If this
    // fails, hw_changed is true on every klippy_ready replay in the field and
    // the #1117 gate is inert.
    INFO("fingerprint moved between pass 1 and pass 2 (0x" << std::hex << p1.fingerprint << " -> 0x"
                                                           << p2.fingerprint << std::dec << ")"
                                                           << describe_delta(p1.hw, p2.hw));
    REQUIRE(p1.fingerprint == p2.fingerprint);

    INFO("fingerprint moved between pass 2 and pass 3 (0x" << std::hex << p2.fingerprint << " -> 0x"
                                                           << p3.fingerprint << std::dec << ")"
                                                           << describe_delta(p2.hw, p3.hw));
    REQUIRE(p2.fingerprint == p3.fingerprint);
}

TEST_CASE("Re-discovery preserves identity fields that parse_objects clears",
          "[discovery][hardware_fingerprint][rediscovery]") {
    LVGLTestFixture fixture;

    // PrinterDiscovery::parse_objects() opens with clear(), which wipes the
    // whole struct -- hostname, mcu and kinematics included. Those are set by
    // LATER steps of the sequence (printer.info, machine.system_info, the MCU
    // queries) and they feed the fingerprint. A replay that re-clears them but
    // fails to repopulate one would move the fingerprint without any hardware
    // having changed. Asserted separately from the hash so the cause is legible.
    RediscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);

    Pass p1 = run_pass(client);
    Pass p2 = run_pass(client);
    REQUIRE(p1.completed);
    REQUIRE(p2.completed);

    INFO("identity field lost or changed on replay" << describe_delta(p1.hw, p2.hw));
    CHECK(p1.hw.hostname() == p2.hw.hostname());
    CHECK(p1.hw.mcu() == p2.hw.mcu());
    CHECK(p1.hw.kinematics() == p2.hw.kinematics());
}
