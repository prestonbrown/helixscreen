// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_m300_install_gate.cpp
 * @brief Which signals install the M300 (Klipper gcode beeper) backend
 *
 * try_install_m300_backend() is the whole sound path for a buzzer-only
 * machine: no local audio hardware exists, so until it runs, every
 * SoundManager::play() is a silent no-op. These tests pin the install
 * decision at the PrinterState::set_hardware() wiring level:
 *
 *   - a beeper output_pin in objects/list          (long-standing signal)
 *   - a gcode_macro M300 in objects/list           (macro-only buzzers)
 *   - a forced-on speaker capability override      (firmware-native M300)
 *
 * and the suppressors:
 *
 *   - no Moonraker client yet (install defers, capability subject still set)
 *   - a forced-off override (user says the printer has no speaker)
 *
 * The macro arm is not hypothetical: Z-Mod's AD5X config implements M300 as a
 * shell-command macro with no output_pin object at all, so output_pin-only
 * detection left that whole machine class silent.
 */

#include "ui_update_queue.h"

#include "../test_helpers/printer_state_test_access.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "sound_manager.h"
#include "test_fixtures.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using helix::PrinterDiscovery;

namespace {

/// Return SoundManager to its pre-test state on every exit path. It is a
/// process-global singleton, so a backend left installed here leaks a live
/// sequencer thread into every later test in the shard and makes their
/// "no backend" assertions lie. Shutdown BEFORE clearing the client, the same
/// ordering Application::shutdown uses — the M300 drop path joins the
/// sequencer thread while the captured client is still valid.
struct SoundManagerClean {
    ~SoundManagerClean() {
        auto& sm = SoundManager::instance();
        sm.shutdown();
        sm.set_moonraker_client(nullptr);
    }
};

lv_subject_t* speaker_subject() {
    return lv_xml_get_subject(NULL, "printer_has_speaker");
}

} // namespace

TEST_CASE_METHOD(MoonrakerTestFixture, "M300 backend install signals",
                 "[sound][m300][capabilities]") {
    SoundManagerClean clean;
    auto& sm = SoundManager::instance();

    // Re-init the fixture's subjects WITH XML-scope registration so
    // speaker_subject() resolves to this instance's subjects. The fixture ctor
    // registered without XML scope, and init_subjects() guards on
    // subjects_initialized_, so reset first — the same sequence the
    // capabilities characterization test uses.
    PrinterStateTestAccess::reset(state());
    state().init_subjects(true);

    sm.set_moonraker_client(&client());

    SECTION("output_pin beeper installs the M300 backend") {
        PrinterDiscovery hw;
        nlohmann::json objects = {"output_pin beeper"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_speaker()); // setup reached the detection branch

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(speaker_subject()) == 1);
        REQUIRE(sm.has_backend());
    }

    SECTION("gcode_macro M300 installs the M300 backend") {
        PrinterDiscovery hw;
        nlohmann::json objects = {"gcode_macro M300"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_speaker()); // macro detection fired

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(speaker_subject()) == 1);
        REQUIRE(sm.has_backend());
    }

    SECTION("a non-M300 macro is not a speaker") {
        PrinterDiscovery hw;
        nlohmann::json objects = {"gcode_macro M356"};
        hw.parse_objects(objects);
        REQUIRE_FALSE(hw.has_speaker());

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(speaker_subject()) == 0);
        REQUIRE_FALSE(sm.has_backend());
    }

    SECTION("forced-on override installs M300 with no detected signal") {
        PrinterDiscovery hw; // no beeper objects at all
        PrinterStateTestAccess::set_capability_override(state(), capability::SPEAKER,
                                                        OverrideState::ENABLE);
        REQUIRE_FALSE(hw.has_speaker()); // only the override claims a speaker

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        // The override must not be a silent no-op: forcing the speaker
        // capability on is how a firmware-native M300 printer gets sound.
        REQUIRE(lv_subject_get_int(speaker_subject()) == 1);
        REQUIRE(sm.has_backend());
    }

    SECTION("forced-off override suppresses M300 despite a detected beeper") {
        PrinterDiscovery hw;
        nlohmann::json objects = {"output_pin beeper"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_speaker());
        PrinterStateTestAccess::set_capability_override(state(), capability::SPEAKER,
                                                        OverrideState::DISABLE);

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        // DISABLE means "treat this printer as having no speaker": no
        // capability subject, no backend sending gcode.
        REQUIRE(lv_subject_get_int(speaker_subject()) == 0);
        REQUIRE_FALSE(sm.has_backend());
    }

    SECTION("AUTO with no speaker signal installs nothing") {
        PrinterDiscovery hw;
        REQUIRE_FALSE(hw.has_speaker());

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(speaker_subject()) == 0);
        REQUIRE_FALSE(sm.has_backend());
    }

    SECTION("no Moonraker client: capability is set but nothing installs") {
        sm.set_moonraker_client(nullptr);

        PrinterDiscovery hw;
        nlohmann::json objects = {"output_pin beeper"};
        hw.parse_objects(objects);
        REQUIRE(hw.has_speaker());

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        // The install defers (a client may arrive later), but the capability
        // subject must already reflect the detected beeper.
        REQUIRE(lv_subject_get_int(speaker_subject()) == 1);
        REQUIRE_FALSE(sm.has_backend());
    }
}
