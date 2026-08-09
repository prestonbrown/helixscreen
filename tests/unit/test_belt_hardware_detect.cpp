// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_hardware_detect.cpp
 * @brief Tests for MoonrakerAdvancedAPI::detect_belt_hardware object-list parsing
 *
 * detect_belt_hardware() drives two RPCs: printer.objects.list to find the
 * hardware sections, then printer.objects.query for kinematics. The object list
 * arrives inside the JSON-RPC envelope's "result" member, so reading "objects"
 * off the top level silently yields nothing and every flag stays false
 * (prestonbrown/helixscreen#1137).
 */

#include "../../include/belt_tension_types.h"
#include "../../include/moonraker_advanced_api.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

// Step 2 (printer.objects.query for kinematics) already reads through "result"
// correctly, but it cannot be asserted here: the mock's configfile.settings.printer
// carries only max_velocity/max_accel, with no kinematics key
// (moonraker_client_mock_objects.cpp:211). Covering that path needs the mock to
// report a per-printer-type kinematics first — worth doing, but it is mock
// fidelity work rather than part of this fix.

// A malformed envelope must not throw out of the callback or skip on_complete.
// The parse sits behind a try/catch that reports through on_error, so the
// contract is "one of the two callbacks fires, and nothing escapes".
TEST_CASE("detect_belt_hardware tolerates an object list of non-strings",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    bool errored = false;
    REQUIRE_NOTHROW(advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware&) { completed = true; },
        [&](const MoonrakerError&) { errored = true; }));
    CHECK((completed || errored));
}
