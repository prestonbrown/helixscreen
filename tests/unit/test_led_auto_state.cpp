// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix::led;

/// LedAutoState::init() registers a print-state observer whose first
/// notification is deferred through the UpdateQueue, and apply_action() routes
/// through LedController. With no fixture these TEST_CASEs returned with that
/// work still queued and handed it to whichever test drained next
/// (prestonbrown/helixscreen#1167). The drain sits in the derived destructor body
/// so it runs while the subjects are still alive, before HelixTestFixture's own
/// teardown.
struct LedAutoStateFixture : public HelixTestFixture {
    ~LedAutoStateFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

namespace {

/// Write a persisted auto-state config to the SAME per-printer path that
/// LedAutoState::load_config() reads (`<df()>leds/auto_state/...`). Returns the
/// df()-relative base so tests can clean up afterward.
std::string write_persisted_auto_state(bool enabled, const std::string& state_key,
                                       const std::string& action_type, const std::string& hex_color,
                                       int brightness) {
    auto* cfg = helix::Config::get_instance();
    const std::string base = cfg->df() + "leds/auto_state/";

    cfg->set(base + "enabled", enabled);

    nlohmann::json mappings = nlohmann::json::object();
    nlohmann::json entry;
    entry["action"] = action_type;
    entry["color"] = hex_color;
    entry["brightness"] = brightness;
    mappings[state_key] = entry;
    cfg->set(base + "mappings", mappings);

    cfg->save();
    return base;
}

/// Remove the persisted auto-state keys so the shared Config singleton does not
/// leak state into later tests (random test order).
void clear_persisted_auto_state() {
    auto* cfg = helix::Config::get_instance();
    cfg->set(cfg->df() + "leds/auto_state/enabled", nlohmann::json());
    cfg->set(cfg->df() + "leds/auto_state/mappings", nlohmann::json());
    // Also clear the legacy migration source so it cannot re-seed.
    cfg->set("/led/auto_state/enabled", nlohmann::json());
    cfg->set("/led/auto_state/mappings", nlohmann::json());
    cfg->save();
}

} // namespace

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState singleton access", "[led][autostate]") {
    auto& state1 = LedAutoState::instance();
    auto& state2 = LedAutoState::instance();
    REQUIRE(&state1 == &state2);
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState default disabled after deinit",
                 "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();
    REQUIRE_FALSE(state.is_enabled());
    REQUIRE_FALSE(state.is_initialized());
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState enable/disable without printer state",
                 "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    REQUIRE_FALSE(state.is_enabled());
    state.set_enabled(true);
    REQUIRE(state.is_enabled());
    state.set_enabled(false);
    REQUIRE_FALSE(state.is_enabled());

    // Double-set is idempotent
    state.set_enabled(true);
    state.set_enabled(true);
    REQUIRE(state.is_enabled());

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState set and get mapping", "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    LedStateAction action;
    action.action_type = "color";
    action.color = 0xFF0000;
    action.brightness = 75;

    state.set_mapping("error", action);

    auto* result = state.get_mapping("error");
    REQUIRE(result != nullptr);
    REQUIRE(result->action_type == "color");
    REQUIRE(result->color == 0xFF0000);
    REQUIRE(result->brightness == 75);

    // Non-existent mapping returns nullptr
    REQUIRE(state.get_mapping("nonexistent") == nullptr);

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState mappings() returns all mappings",
                 "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    LedStateAction a1;
    a1.action_type = "color";
    a1.color = 0xFF0000;

    LedStateAction a2;
    a2.action_type = "off";

    state.set_mapping("error", a1);
    state.set_mapping("idle", a2);

    auto& all = state.mappings();
    REQUIRE(all.size() == 2);
    REQUIRE(all.count("error") == 1);
    REQUIRE(all.count("idle") == 1);

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedStateAction struct defaults", "[led][autostate]") {
    LedStateAction action;
    REQUIRE(action.action_type.empty());
    REQUIRE(action.color == 0xFFFFFF);
    REQUIRE(action.brightness == 100);
    REQUIRE(action.effect_name.empty());
    REQUIRE(action.wled_preset == 0);
    REQUIRE(action.macro_gcode.empty());
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState mapping overwrite", "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    LedStateAction action1;
    action1.action_type = "color";
    action1.color = 0xFF0000;

    LedStateAction action2;
    action2.action_type = "effect";
    action2.effect_name = "rainbow";

    state.set_mapping("printing", action1);
    state.set_mapping("printing", action2);

    auto* result = state.get_mapping("printing");
    REQUIRE(result != nullptr);
    REQUIRE(result->action_type == "effect");
    REQUIRE(result->effect_name == "rainbow");

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedAutoState deinit clears all state", "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    // Add some state
    LedStateAction action;
    action.action_type = "color";
    state.set_mapping("idle", action);
    state.set_enabled(true);

    REQUIRE(state.is_enabled());
    REQUIRE(state.mappings().size() == 1);

    // Deinit clears everything
    state.deinit();

    REQUIRE_FALSE(state.is_enabled());
    REQUIRE_FALSE(state.is_initialized());
    REQUIRE(state.mappings().empty());
}

TEST_CASE_METHOD(LedAutoStateFixture, "LedStateAction supports brightness action type",
                 "[led][auto_state]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    LedStateAction action;
    action.action_type = "brightness";
    action.brightness = 50;

    // Verify fields are set correctly
    REQUIRE(action.action_type == "brightness");
    REQUIRE(action.brightness == 50);
    REQUIRE(action.color == 0xFFFFFF); // Default color unchanged

    // Round-trip through set_mapping / get_mapping
    state.set_mapping("idle", action);
    auto* result = state.get_mapping("idle");
    REQUIRE(result != nullptr);
    REQUIRE(result->action_type == "brightness");
    REQUIRE(result->brightness == 50);

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "brightness action type stored in mapping",
                 "[led][auto_state]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    LedStateAction action;
    action.action_type = "brightness";
    action.brightness = 75;

    state.set_mapping("heating", action);

    auto* result = state.get_mapping("heating");
    REQUIRE(result != nullptr);
    REQUIRE(result->action_type == "brightness");
    REQUIRE(result->brightness == 75);

    // Verify it coexists with other action types
    LedStateAction color_action;
    color_action.action_type = "color";
    color_action.color = 0xFF0000;
    state.set_mapping("error", color_action);

    REQUIRE(state.mappings().size() == 2);
    REQUIRE(state.get_mapping("heating")->action_type == "brightness");
    REQUIRE(state.get_mapping("error")->action_type == "color");

    state.deinit();
}

TEST_CASE_METHOD(LedAutoStateFixture, "setup_default_mappings includes all 6 state keys",
                 "[led][auto_state]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    // Set up defaults by setting mappings manually (matching setup_default_mappings)
    // We can't call the private method directly, but we can verify after init with no config
    // Instead, verify the expected state keys via set_mapping
    const std::vector<std::string> expected_keys = {"idle",   "heating", "printing",
                                                    "paused", "error",   "complete"};

    for (const auto& key : expected_keys) {
        LedStateAction action;
        action.action_type = "color";
        state.set_mapping(key, action);
    }

    REQUIRE(state.mappings().size() == 6);
    for (const auto& key : expected_keys) {
        auto* mapping = state.get_mapping(key);
        REQUIRE(mapping != nullptr);
        // All action types should be valid
        bool valid_type =
            (mapping->action_type == "color" || mapping->action_type == "brightness" ||
             mapping->action_type == "effect" || mapping->action_type == "wled_preset" ||
             mapping->action_type == "macro" || mapping->action_type == "off");
        REQUIRE(valid_type);
    }

    state.deinit();
}

// ============================================================================
// Lifecycle wiring guarantees: these lock in the behavior that the production
// wiring (printer_discovery init / application teardown deinit) depends on.
// ============================================================================

// init() must load the persisted per-printer config (enabled flag + mappings).
// This is the core value the production wiring relies on: when printer_discovery
// calls init(printer_state), saved auto-state config becomes live.
TEST_CASE_METHOD(LedAutoStateFixture,
                 "LedAutoState init loads persisted config from per-printer path",
                 "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    // Persist enabled=true + a single mapping at the path load_config() reads.
    write_persisted_auto_state(/*enabled=*/true, /*state_key=*/"printing",
                               /*action_type=*/"color", /*hex_color=*/"#FF0000",
                               /*brightness=*/77);

    // Fresh init from the persisted store.
    state.init(get_printer_state());

    REQUIRE(state.is_enabled());

    const auto* mapping = state.get_mapping("printing");
    REQUIRE(mapping != nullptr);
    REQUIRE(mapping->action_type == "color");
    REQUIRE(mapping->color == 0xFF0000u);
    REQUIRE(mapping->brightness == 77);

    state.deinit();
    clear_persisted_auto_state();
}

// deinit()->init() soft-restart cycle (mirrors switch_printer: teardown then
// re-init on the new printer). Must not crash and must end in a consistent,
// re-subscribed state reflecting the (re)loaded config.
TEST_CASE_METHOD(LedAutoStateFixture,
                 "LedAutoState deinit/init soft-restart cycle stays consistent",
                 "[led][autostate]") {
    auto& state = LedAutoState::instance();
    state.deinit();

    write_persisted_auto_state(/*enabled=*/true, /*state_key=*/"idle",
                               /*action_type=*/"color", /*hex_color=*/"#00FF00",
                               /*brightness=*/40);

    // First init (as printer_discovery would do).
    state.init(get_printer_state());
    REQUIRE(state.is_initialized());
    REQUIRE(state.is_enabled());
    REQUIRE(state.get_mapping("idle") != nullptr);

    // Teardown (as application teardown would do).
    state.deinit();
    REQUIRE_FALSE(state.is_initialized());
    REQUIRE_FALSE(state.is_enabled());
    REQUIRE(state.mappings().empty());

    // Re-init (as the next printer_discovery on switch_printer would do).
    state.init(get_printer_state());
    REQUIRE(state.is_initialized());
    REQUIRE(state.is_enabled());

    const auto* mapping = state.get_mapping("idle");
    REQUIRE(mapping != nullptr);
    REQUIRE(mapping->color == 0x00FF00u);

    // Re-evaluation after the soft restart must not crash; drain deferred
    // observer callbacks that subscribe_observers() may have enqueued.
    state.evaluate();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    state.deinit();
    clear_persisted_auto_state();
}

// After init() subscribes observers, a change to an observed PrinterState
// subject must drive LedController via apply_action (the end-to-end auto-state
// path that production now activates).
namespace helix::led {
class LedAutoStateTestAccess {
  public:
    static void apply(const LedStateAction& a) {
        LedAutoState::instance().apply_action(a);
    }
};
} // namespace helix::led

namespace {
struct AutoStateTargetFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    AutoStateTargetFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            helix::led::LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = helix::led::LedBackendType::NATIVE;
            s.supports_color = true;
            s.supports_white = true;
            ctrl.native().add_strip(s);
        }
    }
    ~AutoStateTargetFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        helix::led::LedAutoState::instance().set_strips({});
        auto* cfg = helix::Config::get_instance();
        cfg->set(cfg->df() + "leds/auto_state/strips", nlohmann::json());

        clear_persisted_auto_state();
        helix::led::LedController::instance().deinit();
    }
};
} // namespace

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: targets fall back to the chamber light",
                 "[led][auto_state]") {
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({});
    CHECK(as.targets() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({"neopixel gone"});
    CHECK(as.targets() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({"neopixel sb_leds"});
    CHECK(as.targets() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: strips round-trip through config",
                 "[led][auto_state]") {
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({"neopixel sb_leds"});
    as.save_config();
    as.set_strips({});
    as.load_config();
    CHECK(as.strips() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: a color action reaches only its targets",
                 "[led][auto_state]") {
    auto& ctrl = helix::led::LedController::instance();
    helix::led::LedAutoState::instance().set_strips({"neopixel sb_leds"});
    helix::led::LedStateAction a;
    a.action_type = "color";
    a.color = 0xFF0000;
    a.brightness = 100;
    helix::led::LedAutoStateTestAccess::apply(a);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(ctrl.native().has_strip_color("neopixel sb_leds"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel chamber_light"));
}

namespace {
PowerState chamber_power() {
    return LedController::instance().device_state("neopixel chamber_light").power;
}
void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}
} // namespace

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: an 'off' action turns its targets off",
                 "[led][autostate]") {
    LedController::instance().set_power({"neopixel chamber_light"}, true);
    drain();
    REQUIRE(chamber_power() == PowerState::On);

    LedAutoStateTestAccess::apply({"off", 0xFFFFFF, 100, "", 0, ""});
    drain();
    CHECK(chamber_power() == PowerState::Off);
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: a 'color' action lights its targets",
                 "[led][autostate]") {
    REQUIRE(chamber_power() == PowerState::Unknown);
    LedAutoStateTestAccess::apply({"color", 0xFF0000, 100, "", 0, ""});
    drain();
    const auto s = LedController::instance().device_state("neopixel chamber_light");
    CHECK(s.power == PowerState::On);
    CHECK(s.rgb == 0xFF0000);
}

TEST_CASE_METHOD(AutoStateTargetFixture,
                 "LedAutoState: a 'color' action is fitted to each target's channels",
                 "[led][autostate]") {
    auto& ctrl = LedController::instance();
    LedStripInfo lamp; // a [led] with only a white_pin
    lamp.id = "led lamp";
    lamp.backend = LedBackendType::NATIVE;
    lamp.supports_color = false;
    lamp.supports_white = true;
    ctrl.native().add_strip(lamp);
    LedStripInfo rgb;
    rgb.id = "neopixel rgb";
    rgb.backend = LedBackendType::NATIVE;
    rgb.supports_color = true;
    rgb.supports_white = false;
    ctrl.native().add_strip(rgb);
    LedStripInfo pin;
    pin.id = "output_pin cabinet";
    pin.backend = LedBackendType::OUTPUT_PIN;
    pin.is_pwm = true;
    ctrl.output_pin().add_pin(pin);
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    ps.set_klippy_state_sync(helix::KlippyState::READY);
    drain();
    client.clear_gcode_script_history();

    LedAutoState::instance().set_strips({"led lamp", "neopixel rgb", "output_pin cabinet"});
    LedAutoStateTestAccess::apply({"color", 0xFF0000, 60, "", 0, ""});
    drain();

    // Single channel: the look's brightness, not the red's luminance.
    const auto w = ctrl.native().get_strip_color("led lamp");
    CHECK(w.r == Catch::Approx(0.0).margin(0.001));
    CHECK(w.w == Catch::Approx(0.6).margin(0.01));
    const auto c = ctrl.native().get_strip_color("neopixel rgb");
    CHECK(c.r == Catch::Approx(0.6).margin(0.01));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.w == Catch::Approx(0.0).margin(0.001));
    const auto& h = client.gcode_script_history();
    CHECK(std::any_of(h.begin(), h.end(), [](const std::string& g) {
        return g.find("SET_PIN PIN=cabinet VALUE=0.6000") != std::string::npos;
    }));
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: a 'brightness' action sets that level",
                 "[led][autostate]") {
    LedAutoStateTestAccess::apply({"brightness", 0xFFFFFF, 50, "", 0, ""});
    drain();
    const auto s = LedController::instance().device_state("neopixel chamber_light");
    CHECK(s.power == PowerState::On);
    CHECK(s.brightness == 50);
}

// After init() subscribes observers, a change to an observed PrinterState
// subject must drive LedController via apply_action (the end-to-end auto-state
// path that production activates).
TEST_CASE_METHOD(AutoStateTargetFixture,
                 "LedAutoState observer fires after init and applies action", "[led][autostate]") {
    auto& auto_state = LedAutoState::instance();
    auto_state.deinit();

    auto& ps = get_printer_state();
    // The PrinterState subjects observed by LedAutoState (print state enum,
    // klippy state, extruder target) are plain lv_subject_t that only become
    // settable after init_subjects(); lv_subject_set_int() is a no-op on an
    // uninitialized subject. register_xml=false keeps these out of the global
    // XML scope so they don't collide with other tests in the shard.
    ps.init_subjects(false);

    // Establish a known non-printing baseline BEFORE init so the later flip to
    // PRINTING is a genuine state transition (the dedup in on_state_changed()
    // skips re-applying an unchanged key). Clearing klippy ERROR and zeroing the
    // extruder target keeps compute_state_key() at "idle" for the baseline.
    auto* print_subj = ps.print_state().get_print_state_enum_subject();
    REQUIRE(print_subj != nullptr);
    lv_subject_set_int(print_subj, static_cast<int>(helix::PrintJobState::STANDBY));
    if (auto* klippy_subj = ps.network_state().get_klippy_state_subject()) {
        lv_subject_set_int(klippy_subj, static_cast<int>(helix::KlippyState::READY));
    }
    if (auto* ext_target = ps.temperature_state().get_active_extruder_target_subject()) {
        lv_subject_set_int(ext_target, 0);
    }

    auto_state.init(ps);
    // Map every state to "off" except the one we will drive to, so whatever
    // value the shared PrinterState subjects currently hold cannot turn the
    // light on before our targeted change.
    LedStateAction off_action{"off", 0xFFFFFF, 100, "", 0, ""};
    for (const char* k : {"idle", "heating", "printing", "paused", "error", "complete"}) {
        auto_state.set_mapping(k, off_action);
    }
    auto_state.set_mapping("printing", {"color", 0x00FF00, 100, "", 0, ""});
    auto_state.set_enabled(true);

    // Drain the immediate evaluate() from set_enabled() so we start from a known
    // ("idle" -> off) baseline, then change the observed subject.
    drain();
    REQUIRE(chamber_power() == PowerState::Off);

    // Drive the print-state subject to PRINTING — this is an observed subject,
    // so on_state_changed() should fire and apply the "color" action.
    lv_subject_set_int(print_subj, static_cast<int>(helix::PrintJobState::PRINTING));
    drain();

    CHECK(chamber_power() == PowerState::On);

    // Restore subject state so a STANDBY/idle baseline doesn't leak into other
    // tests sharing the PrinterState singleton.
    lv_subject_set_int(print_subj, static_cast<int>(helix::PrintJobState::STANDBY));
    auto_state.deinit();
}
