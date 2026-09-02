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
 *
 * A second case covers the PWM takeover: on a modded AD5M both HelixScreen's
 * PWM sysfs backend and klippy's tone_player write the same buzzer channel,
 * so a REAL has_speaker() signal hands the channel to M300 (single writer),
 * while the forced override and non-PWM backends keep the no-op contract.
 */

#include "ui_update_queue.h"

#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/sound_manager_test_access.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "pwm_sound_backend.h"
#include "sound_backend.h"
#include "sound_manager.h"
#include "test_fixtures.h"

#include <spdlog/spdlog.h>

#include <memory>

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

/// Minimal host-audio stand-in (an SDL/ALSA/jz analogue): owns no sysfs PWM
/// channel, so the M300 gate must never displace it.
class StubHostBackend : public SoundBackend {
  public:
    void set_tone(float /* freq_hz */, float /* amplitude */, float /* duty_cycle */) override {}
    void silence() override {}
};

/// A PWMSoundBackend that never touched real sysfs: uninitialized (no channel
/// to release), but it reports the capability the gate asks about.
std::shared_ptr<PWMSoundBackend> make_pwm_backend() {
    auto pwm = std::make_shared<PWMSoundBackend>("/nonexistent-sysfs-ad5m-gate", 0, 6);
    REQUIRE(pwm->owns_sysfs_pwm_channel()); // setup: this backend is displacable
    return pwm;
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

TEST_CASE_METHOD(MoonrakerTestFixture, "M300 backend displaces the PWM sysfs backend",
                 "[sound][m300][capabilities]") {
    SoundManagerClean clean;
    auto& sm = SoundManager::instance();

    // Re-init the fixture's subjects WITH XML-scope registration (see the
    // install-signals case above for why).
    PrinterStateTestAccess::reset(state());
    state().init_subjects(true);

    sm.set_moonraker_client(&client());

    auto pwm = make_pwm_backend();
    SoundManagerTestAccess::install_backend(sm, pwm);

    PrinterDiscovery hw;
    nlohmann::json objects = {"output_pin beeper"};
    hw.parse_objects(objects);
    REQUIRE(hw.has_speaker());

    SECTION("real detection swaps PWM out for M300") {
        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        auto active = SoundManagerTestAccess::backend(sm);
        REQUIRE(active != nullptr);
        REQUIRE(active->needs_moonraker_client()); // M300 owns the channel now
        REQUIRE(active.get() != pwm.get());        // the PWM backend is gone
        REQUIRE(sm.has_backend());
    }

    SECTION("forced-on override alone leaves PWM installed") {
        PrinterDiscovery empty_hw; // no beeper objects at all
        PrinterStateTestAccess::set_capability_override(state(), capability::SPEAKER,
                                                        OverrideState::ENABLE);
        REQUIRE_FALSE(empty_hw.has_speaker()); // only the override claims a speaker

        state().set_hardware(empty_hw);
        UpdateQueue::instance().drain();

        // The override may still install M300 when NOTHING is installed, but
        // it must not take the buzzer channel away from PWM: it says "this
        // printer has a speaker", not "klippy answers M300".
        auto active = SoundManagerTestAccess::backend(sm);
        REQUIRE(active.get() == pwm.get());
        REQUIRE_FALSE(active->needs_moonraker_client());
    }

    SECTION("a non-PWM backend is never displaced") {
        auto stub = std::make_shared<StubHostBackend>();
        SoundManagerTestAccess::install_backend(sm, stub);

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        REQUIRE(SoundManagerTestAccess::backend(sm).get() == stub.get());
    }

    SECTION("no client: the swap is refused and PWM stays") {
        sm.set_moonraker_client(nullptr);

        state().set_hardware(hw);
        UpdateQueue::instance().drain();

        // M300 cannot install without a client to send gcode through — and
        // the box must not go soundless because the swap tore PWM down first.
        REQUIRE(SoundManagerTestAccess::backend(sm).get() == pwm.get());
    }

    SECTION("clearing the client after a swap restores a host backend") {
        state().set_hardware(hw);
        UpdateQueue::instance().drain();
        REQUIRE(SoundManagerTestAccess::backend(sm)->needs_moonraker_client());

        // What this environment's eager probe can provide (scoped: released
        // before the drop path runs its own probe, so the two never contend
        // on the same audio device).
        bool host_audio_available = false;
        {
            auto probe = SoundManagerTestAccess::create_backend(sm);
            host_audio_available = probe != nullptr;
        }

        sm.set_moonraker_client(nullptr);

        // The M300 backend is dropped (next printer may lack the handler),
        // and the host backend the swap displaced must come back — on a
        // modded AD5M that is the PWM backend, and without the re-probe the
        // box stays silent until reboot.
        auto active = SoundManagerTestAccess::backend(sm);
        if (host_audio_available) {
            REQUIRE(active != nullptr);
            REQUIRE_FALSE(active->needs_moonraker_client());
        } else {
            REQUIRE(active == nullptr);
        }

        // And the gate still works for the next printer: real detection on a
        // fresh client takes the channel again. The recovered host backend is
        // whatever this environment provides (on the AD5M: PWM), so put the
        // PWM stand-in back and prove detection retakes the channel.
        sm.shutdown();
        SoundManagerTestAccess::install_backend(sm, make_pwm_backend());
        sm.set_moonraker_client(&client());
        state().set_hardware(hw);
        UpdateQueue::instance().drain();
        REQUIRE(SoundManagerTestAccess::backend(sm)->needs_moonraker_client());
    }
}

TEST_CASE_METHOD(MoonrakerTestFixture, "M300 drop without host recovery leaves no backend",
                 "[sound][m300][capabilities]") {
    // Application::shutdown clears the SoundManager client with
    // host_recovery=false: the manager is torn down moments later, so
    // re-opening audio hardware and spawning a sequencer thread on the way
    // out would be pure waste. Printer switch / transient disconnect keep the
    // default (true) — there the gcode path is dead and local audio is the
    // only fallback.
    SoundManagerClean clean;
    auto& sm = SoundManager::instance();

    PrinterStateTestAccess::reset(state());
    state().init_subjects(true);

    sm.set_moonraker_client(&client());
    SoundManagerTestAccess::install_backend(sm, make_pwm_backend());

    PrinterDiscovery hw;
    nlohmann::json objects = {"output_pin beeper"};
    hw.parse_objects(objects);
    REQUIRE(hw.has_speaker());

    state().set_hardware(hw);
    UpdateQueue::instance().drain();
    REQUIRE(SoundManagerTestAccess::backend(sm)->needs_moonraker_client()); // M300 active

    sm.set_moonraker_client(nullptr, /*host_recovery=*/false);

    // M300 dropped, and nothing re-probed in its place — even on a host
    // where create_backend() would succeed.
    REQUIRE(SoundManagerTestAccess::backend(sm) == nullptr);
}
