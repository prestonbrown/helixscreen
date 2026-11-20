// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_catalog.h"
#include "printer_detector.h"
#include "printer_profile_state.h"
#include "probe_sensor_types.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using helix::ZOffsetCalibrationStrategy;

namespace {

std::vector<std::string> ids(const std::vector<StepInfo>& steps) {
    std::vector<std::string> out;
    for (const auto& s : steps) {
        out.emplace_back(s.id);
    }
    return out;
}

const StepInfo* step_named(const std::vector<StepInfo>& steps, const std::string& id) {
    for (const auto& s : steps) {
        if (id == s.id) {
            return &s;
        }
    }
    return nullptr;
}

/// A K2 Plus on Jacobean firmware: CFS cutter, motorized belts, a probe, an
/// accelerometer, measured pressure advance, corexy.
CatalogInputs k2_jacobean() {
    CatalogInputs in;
    in.has_probe = true;
    in.has_bed_mesh = true;
    in.has_accelerometer = true;
    in.can_tension_belts = true;
    in.can_calibrate_cutter = true;
    in.has_pa_cal = true;
    in.supports_belt_compare = true;
    in.z_strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    in.nozzle_contact_probe = true;
    in.tier_overrides = tier_overrides_from(
        PrinterDetector::get_calibration_step_tiers("Creality K2 Plus (Jacobean firmware)"));
    return in;
}

} // namespace

TEST_CASE("a printer with no calibration hardware still offers its heaters and PA tuning",
          "[calibration_catalog]") {
    CHECK(ids(applicable_steps(CatalogInputs{})) ==
          std::vector<std::string>{"heaters", "pa_tuning"});
}

TEST_CASE("each step appears only with the capability that runs it", "[calibration_catalog]") {
    struct Case {
        void (*enable)(CatalogInputs&);
        const char* id;
    };
    const Case cases[] = {
        {[](CatalogInputs& in) { in.can_tension_belts = true; }, "belt_tension"},
        {[](CatalogInputs& in) { in.can_calibrate_cutter = true; }, "cutter"},
        {[](CatalogInputs& in) { in.has_probe = true; }, "z_offset"},
        {[](CatalogInputs& in) { in.has_bed_mesh = true; }, "bed_mesh"},
        {[](CatalogInputs& in) { in.has_pa_cal = true; }, "pressure_advance"},
        {[](CatalogInputs& in) { in.has_screws_tilt = true; }, "bed_screws"},
        {[](CatalogInputs& in) { in.has_tool_offset_cal = true; }, "tool_offsets"},
        {[](CatalogInputs& in) { in.has_stepper_resonance_cal = true; }, "vertical_artifact"},
        {[](CatalogInputs& in) { in.supports_ripple_check = true; }, "vibration_check"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.id);
        CatalogInputs in;
        CHECK(step_named(applicable_steps(in), c.id) == nullptr);
        c.enable(in);
        CHECK(step_named(applicable_steps(in), c.id) != nullptr);
    }
}

TEST_CASE("input shaping needs an accelerometer; belt compare also needs belt-path kinematics",
          "[calibration_catalog]") {
    CatalogInputs in;
    in.supports_belt_compare = true;
    CHECK(step_named(applicable_steps(in), "input_shaper") == nullptr);
    CHECK(step_named(applicable_steps(in), "belt_compare") == nullptr);
    in.has_accelerometer = true;
    CHECK(step_named(applicable_steps(in), "input_shaper") != nullptr);
    CHECK(step_named(applicable_steps(in), "belt_compare") != nullptr);
    in.supports_belt_compare = false;
    CHECK(step_named(applicable_steps(in), "input_shaper") != nullptr);
    CHECK(step_named(applicable_steps(in), "belt_compare") == nullptr);
}

TEST_CASE("Z offset steps aside where another routine owns the Z offset", "[calibration_catalog]") {
    CatalogInputs in;
    in.has_probe = true;
    REQUIRE(step_named(applicable_steps(in), "z_offset") != nullptr);

    in.z_offset_reapplied_per_head = true;
    CHECK(step_named(applicable_steps(in), "z_offset") == nullptr);
    in.z_offset_reapplied_per_head = false;

    // Both halves are needed: tool offset calibration alone keeps the paper test.
    in.has_tool_offset_cal = true;
    CHECK(step_named(applicable_steps(in), "z_offset") != nullptr);
    in.hide_manual_z_calibration = true;
    CHECK(step_named(applicable_steps(in), "z_offset") == nullptr);
    in.has_tool_offset_cal = false;
    CHECK(step_named(applicable_steps(in), "z_offset") != nullptr);
}

TEST_CASE("Z offset asks for paper and hands only for a paper test", "[calibration_catalog]") {
    struct Case {
        ZOffsetCalibrationStrategy strategy;
        bool contact;
        bool paper;
    };
    const Case cases[] = {
        {ZOffsetCalibrationStrategy::PROBE_CALIBRATE, false, true},
        {ZOffsetCalibrationStrategy::ENDSTOP, false, true},
        {ZOffsetCalibrationStrategy::FIRMWARE_MANAGED, false, false},
        {ZOffsetCalibrationStrategy::PROBE_CALIBRATE, true, false},
        {ZOffsetCalibrationStrategy::ENDSTOP, true, false},
    };
    for (const auto& c : cases) {
        CAPTURE(static_cast<int>(c.strategy), c.contact);
        CatalogInputs in;
        in.has_probe = true;
        in.z_strategy = c.strategy;
        in.nozzle_contact_probe = c.contact;
        const auto steps = applicable_steps(in);
        const StepInfo* z = step_named(steps, "z_offset");
        REQUIRE(z != nullptr);
        CHECK(z->needs_hands == c.paper);
        CHECK(((z->prereqs & kPrereqPaper) != 0) == c.paper);
        CHECK(((prerequisites_for(steps) & kPrereqPaper) != 0) == c.paper);
    }
}

TEST_CASE("only probes that touch with the nozzle skip the paper test", "[calibration_catalog]") {
    using helix::sensors::probe_contacts_nozzle;
    using helix::sensors::ProbeSensorType;
    CHECK(probe_contacts_nozzle(ProbeSensorType::TAP));
    CHECK(probe_contacts_nozzle(ProbeSensorType::SMART_EFFECTOR));
    CHECK(probe_contacts_nozzle(ProbeSensorType::PRTOUCH_V2));
    CHECK(probe_contacts_nozzle(ProbeSensorType::LOADCELL));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::STANDARD));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::BLTOUCH));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::KLICKY));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::EDDY_CURRENT));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::CARTOGRAPHER));
    CHECK_FALSE(probe_contacts_nozzle(ProbeSensorType::BEACON));
}

TEST_CASE("every step runs in the fixed order", "[calibration_catalog]") {
    CatalogInputs in;
    in.has_probe = true;
    in.has_bed_mesh = true;
    in.has_accelerometer = true;
    in.can_tension_belts = true;
    in.can_calibrate_cutter = true;
    in.has_pa_cal = true;
    in.has_tool_offset_cal = true;
    in.has_screws_tilt = true;
    in.has_stepper_resonance_cal = true;
    in.supports_belt_compare = true;
    in.supports_ripple_check = true;
    const auto steps = applicable_steps(in);
    CHECK(ids(steps) == std::vector<std::string>{
                            "belt_tension", "cutter", "z_offset", "heaters", "bed_mesh",
                            "input_shaper", "pressure_advance", "belt_compare", "bed_screws",
                            "tool_offsets", "vertical_artifact", "pa_tuning", "vibration_check"});
    for (size_t i = 1; i < steps.size(); ++i) {
        CHECK(steps[i - 1].order < steps[i].order);
    }
}

TEST_CASE("the K2 Plus on Jacobean firmware gets its database tiers", "[calibration_catalog]") {
    const auto steps = applicable_steps(k2_jacobean());
    CHECK(ids(steps) == std::vector<std::string>{"belt_tension", "cutter", "z_offset", "heaters",
                                                 "bed_mesh", "input_shaper", "pressure_advance",
                                                 "belt_compare", "pa_tuning"});
    const auto tier = [&](const char* id) {
        const StepInfo* s = step_named(steps, id);
        REQUIRE(s != nullptr);
        return s->tier;
    };
    CHECK(tier("cutter") == Tier::Required);
    CHECK(tier("heaters") == Tier::Required);
    CHECK(tier("bed_mesh") == Tier::Required);
    CHECK(tier("z_offset") == Tier::Recommended);
    CHECK(tier("belt_tension") == Tier::Recommended);
    CHECK(tier("input_shaper") == Tier::Recommended);
    CHECK(tier("pressure_advance") == Tier::Optional);
    // Not named in its database entry: the default tier.
    CHECK(tier("belt_compare") == Tier::Optional);
    // Its probe touches with the nozzle: no paper.
    CHECK((prerequisites_for(steps) & kPrereqPaper) == 0);
}

TEST_CASE("a printer without database tiers gets the defaults", "[calibration_catalog]") {
    CHECK(PrinterDetector::get_calibration_step_tiers("Some Random Printer").empty());
    CatalogInputs in;
    in.can_calibrate_cutter = true;
    const auto steps = applicable_steps(in);
    REQUIRE(step_named(steps, "cutter") != nullptr);
    CHECK(step_named(steps, "cutter")->tier == Tier::Recommended);
    CHECK(step_named(steps, "heaters")->tier == Tier::Recommended);
    CHECK(step_named(steps, "pa_tuning")->tier == Tier::OnRequest);
}

TEST_CASE("database tier entries with an unknown step or tier are dropped",
          "[calibration_catalog]") {
    const auto overrides = tier_overrides_from({{"cutter", "required"},
                                                {"cutter_typo", "required"},
                                                {"heaters", "mandatory"},
                                                {"bed_mesh", "on_request"}});
    REQUIRE(overrides.size() == 2);
    CHECK(overrides[0] == TierOverride{"cutter", Tier::Required});
    CHECK(overrides[1] == TierOverride{"bed_mesh", Tier::OnRequest});

    // A tier for a step the printer lacks does not make the step appear.
    CatalogInputs in;
    in.tier_overrides = overrides;
    CHECK(step_named(applicable_steps(in), "cutter") == nullptr);
}

TEST_CASE("tiers parse from their database spelling only", "[calibration_catalog]") {
    CHECK(parse_tier("required") == Tier::Required);
    CHECK(parse_tier("recommended") == Tier::Recommended);
    CHECK(parse_tier("optional") == Tier::Optional);
    CHECK(parse_tier("on_request") == Tier::OnRequest);
    CHECK_FALSE(parse_tier("Required").has_value());
    CHECK_FALSE(parse_tier("").has_value());
    CHECK_FALSE(parse_tier("on request").has_value());
}

TEST_CASE("prerequisites are the union of the steps' and time totals add up",
          "[calibration_catalog]") {
    StepInfo a;
    a.minutes = 5;
    a.needs_hands = true;
    a.prereqs = kPrereqBedClear | kPrereqPaper;
    StepInfo b;
    b.minutes = 7;
    b.prereqs = kPrereqBedClear | kPrereqAccelerometer;
    StepInfo c;
    c.minutes = 3;
    c.needs_hands = true;
    c.prereqs = kPrereqFilamentLoaded;
    const std::vector<StepInfo> steps{a, b, c};
    CHECK(prerequisites_for(steps) ==
          (kPrereqBedClear | kPrereqPaper | kPrereqAccelerometer | kPrereqFilamentLoaded));
    CHECK(total_minutes(steps) == 15);
    CHECK(hands_on_minutes(steps) == 8);
    CHECK(prerequisites_for({}) == kPrereqNone);
    CHECK(total_minutes({}) == 0);
}

TEST_CASE("the Calibration screen lists its tiles in the catalog's run order",
          "[calibration_catalog]") {
    std::ifstream f("ui_xml/calibration_hub_overlay.xml");
    REQUIRE(f.good());
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string xml = buf.str();
    // Each tile's step id, in document order.
    std::vector<std::string> tiles;
    const std::string tag = "<calibration_hub_tile";
    for (size_t at = xml.find(tag); at != std::string::npos; at = xml.find(tag, at + 1)) {
        const size_t end = xml.find("/>", at);
        const size_t step = xml.find(" step=\"", at);
        REQUIRE(step < end);
        const size_t open = step + 7;
        tiles.push_back(xml.substr(open, xml.find('"', open) - open));
    }
    const auto& ids = all_step_ids();
    CHECK(tiles == std::vector<std::string>(ids.begin(), ids.end()));
}
