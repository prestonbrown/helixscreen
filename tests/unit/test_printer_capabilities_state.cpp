// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrinterCapabilitiesState::set_hardware: the capability subjects the Printer
// Manager's chips bind, each set from discovery and cleared by a printer
// without it.

#include "../lvgl_test_fixture.h"
#include "capability_overrides.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Capability;
using helix::PrinterCapabilitiesState;
using helix::PrinterDiscovery;
using json = nlohmann::json;

namespace {

int cap(const PrinterCapabilitiesState& caps, Capability c) {
    return lv_subject_get_int(caps.subject(c));
}

} // namespace

TEST_CASE("set_hardware sets the Shake&Tune, fan tach, LED effect and extra fan subjects",
          "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery full;
    full.parse_objects(json{"fan", "fan_generic exhaust", "fan_feedback", "led_effect rainbow",
                            "gcode_macro AXES_SHAPER_CALIBRATION"});
    CapabilityOverrides full_overrides;
    full_overrides.set_hardware(full);

    caps.set_hardware(full, full_overrides);
    CHECK(cap(caps, Capability::HasShakeTune) == 1);
    CHECK(cap(caps, Capability::HasFanFeedback) == 1);
    CHECK(cap(caps, Capability::HasLedEffects) == 1);
    CHECK(cap(caps, Capability::HasExtraFans) == 1);
    CHECK(cap(caps, Capability::HasHeatSoak) == 0);

    SECTION("a printer without them clears every one") {
        PrinterDiscovery bare;
        bare.parse_objects(json{"fan"});
        CapabilityOverrides bare_overrides;
        bare_overrides.set_hardware(bare);

        caps.set_hardware(bare, bare_overrides);
        CHECK(cap(caps, Capability::HasShakeTune) == 0);
        CHECK(cap(caps, Capability::HasFanFeedback) == 0);
        CHECK(cap(caps, Capability::HasLedEffects) == 0);
        CHECK(cap(caps, Capability::HasExtraFans) == 0);
    }

    caps.deinit_subjects();
}

TEST_CASE("set_hardware: only fans the user controls besides part cooling are extra",
          "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery hw;
    hw.parse_objects(json{"fan"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasExtraFans) == 0);

    PrinterDiscovery automatic;
    automatic.parse_objects(json{"fan", "heater_fan hotend_fan", "controller_fan mcu_fan"});
    CapabilityOverrides automatic_overrides;
    automatic_overrides.set_hardware(automatic);
    caps.set_hardware(automatic, automatic_overrides);
    CHECK(cap(caps, Capability::HasExtraFans) == 0);

    PrinterDiscovery generic;
    generic.parse_objects(json{"fan_generic exhaust"});
    CapabilityOverrides generic_overrides;
    generic_overrides.set_hardware(generic);
    caps.set_hardware(generic, generic_overrides);
    CHECK(cap(caps, Capability::HasExtraFans) == 1);

    caps.deinit_subjects();
}

TEST_CASE("set_hardware: heat soak follows its override", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery hw; // no heat soak macro
    hw.parse_objects(json{"fan"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);

    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasHeatSoak) == 0);

    overrides.set_override(capability::HEAT_SOAK, OverrideState::ENABLE);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasHeatSoak) == 1);

    caps.deinit_subjects();
}

TEST_CASE("set_hardware: printer_has_led follows its override", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery hw;
    hw.parse_objects(json{"led chamber_light"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);

    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasLed) == 1);

    overrides.set_override(capability::LED, OverrideState::DISABLE);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HasLed) == 0);

    PrinterDiscovery none;
    none.parse_objects(json{"fan"});
    overrides.set_hardware(none);
    overrides.set_override(capability::LED, OverrideState::ENABLE);
    caps.set_hardware(none, overrides);
    CHECK(cap(caps, Capability::HasLed) == 1);

    caps.deinit_subjects();
}

TEST_CASE("set_hardware bumps the hardware generation", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);
    PrinterDiscovery hw;
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);
    const int before = cap(caps, Capability::HardwareGeneration);
    caps.set_hardware(hw, overrides);
    CHECK(cap(caps, Capability::HardwareGeneration) == before + 1);
    caps.deinit_subjects();
}

TEST_CASE("new capability subjects register under their XML names", "[printer_capabilities]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(true);
    for (const char* name : {"printer_has_shaketune", "printer_has_heat_soak",
                             "printer_has_fan_feedback", "printer_has_led_effects"}) {
        CAPTURE(name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    caps.deinit_subjects();
}

TEST_CASE("enclosure top: set from status, cleared by every new printer",
          "[printer_capabilities][enclosure_top]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(true);
    CHECK(lv_xml_get_subject(nullptr, "printer_enclosure_top_fitted") != nullptr);
    CHECK_FALSE(caps.enclosure_top_fitted());

    caps.set_enclosure_top_fitted(true);
    CHECK(cap(caps, Capability::EnclosureTopFitted) == 1);
    CHECK(caps.enclosure_top_fitted());

    // Another printer that also reports a top must not inherit this one's answer
    // before its own first frame.
    PrinterDiscovery reporting;
    reporting.parse_objects(json{"purifier", "fm175xx_reader"});
    CapabilityOverrides reporting_overrides;
    reporting_overrides.set_hardware(reporting);
    caps.set_hardware(reporting, reporting_overrides);
    CHECK_FALSE(caps.enclosure_top_fitted());

    caps.set_enclosure_top_fitted(true);
    PrinterDiscovery bare;
    bare.parse_objects(json{"fan"});
    CapabilityOverrides bare_overrides;
    bare_overrides.set_hardware(bare);
    caps.set_hardware(bare, bare_overrides);
    CHECK_FALSE(caps.enclosure_top_fitted());

    caps.deinit_subjects();
}

TEST_CASE("enclosure top: a report before the subjects exist is kept",
          "[printer_capabilities][enclosure_top]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.set_enclosure_top_fitted(true);
    CHECK(caps.enclosure_top_fitted());
    caps.init_subjects(false);
    CHECK(cap(caps, Capability::EnclosureTopFitted) == 1);
    caps.deinit_subjects();
}

TEST_CASE("set_hardware publishes whether the firmware keeps a Z offset per head",
          "[printer_capabilities][per_head]") {
    LVGLTestFixture fixture;
    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    PrinterDiscovery changer;
    changer.parse_objects(json{"zmod_color", "gcode_button extruder_grab1", "save_variables",
                               "gcode_macro SAVE_ZMOD_DATA"});
    CapabilityOverrides changer_overrides;
    changer_overrides.set_hardware(changer);
    caps.set_hardware(changer, changer_overrides);
    CHECK(cap(caps, Capability::ZOffsetPerHead) == 1);

    PrinterDiscovery single;
    single.parse_objects(json{"zmod_color", "save_variables", "gcode_macro SAVE_ZMOD_DATA"});
    CapabilityOverrides single_overrides;
    single_overrides.set_hardware(single);
    caps.set_hardware(single, single_overrides);
    CHECK(cap(caps, Capability::ZOffsetPerHead) == 0);

    caps.deinit_subjects();
}
