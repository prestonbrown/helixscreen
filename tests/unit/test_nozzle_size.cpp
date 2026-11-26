// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_size.cpp
 * @brief The nozzle-size capability layer: which heads a printer has, how each
 *        head's size can change, and the one resolver every consumer reads.
 */

#include "moonraker_client_mock.h"
#include "nozzle_size.h"
#include "snapmaker_nozzle_properties.h"

#include <cmath>
#include <limits>

#include "../catch_amalgamated.hpp"

using namespace helix::nozzle_size;

namespace {

PrinterInputs mainline(std::vector<std::optional<double>> config) {
    PrinterInputs in;
    in.config_diameters = std::move(config);
    return in;
}

} // namespace

TEST_CASE("nozzle size: a single-extruder mainline printer edits its config", "[nozzle_size]") {
    const Capability cap = nozzle_capability(mainline({0.4}));
    REQUIRE(cap.heads.size() == 1);
    const HeadCapability& h = cap.heads[0];
    CHECK(h.extruder_index == 0);
    CHECK(h.extruder == "extruder");
    CHECK(h.current == std::optional<double>(0.4));
    CHECK(h.settable);
    CHECK(h.mechanism == Mechanism::ConfigEdit);
    CHECK(h.restart_required);
    CHECK(h.idle_only);
    CHECK(h.choices == std::vector<double>(kCommonSizes.begin(), kCommonSizes.end()));
}

TEST_CASE("nozzle size: a two-extruder config gives each head its own entry", "[nozzle_size]") {
    const Capability cap = nozzle_capability(mainline({0.4, 0.6}));
    REQUIRE(cap.heads.size() == 2);
    CHECK(cap.heads[1].extruder_index == 1);
    CHECK(cap.heads[1].extruder == "extruder1");
    CHECK(cap.heads[1].current == std::optional<double>(0.6));
    CHECK(cap.heads[1].mechanism == Mechanism::ConfigEdit);
}

TEST_CASE("nozzle size: a registered SET_NOZZLE_DIAMETER covers extruder only", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4, 0.6});
    in.set_nozzle_diameter_registered = true;
    const Capability cap = nozzle_capability(in);
    REQUIRE(cap.heads.size() == 2);
    CHECK(cap.heads[0].mechanism == Mechanism::KlipperCommand);
    CHECK(cap.heads[0].settable);
    CHECK_FALSE(cap.heads[0].restart_required);
    CHECK(cap.heads[1].mechanism == Mechanism::ConfigEdit);
    CHECK(cap.heads[1].restart_required);
}

TEST_CASE("nozzle size: vendor nozzle records make every head a vendor call with the "
          "firmware's list",
          "[nozzle_size]") {
    PrinterInputs in;
    in.live_diameters = {0.4, 0.4, 0.6, 0.4};
    in.vendor_records_present = true;
    in.vendor_choices = {0.2, 0.4, 0.6, 0.8};
    const Capability cap = nozzle_capability(in);
    REQUIRE(cap.heads.size() == 4);
    for (const auto& h : cap.heads) {
        CHECK(h.mechanism == Mechanism::VendorRpc);
        CHECK(h.settable);
        CHECK(h.idle_only);
        CHECK_FALSE(h.restart_required);
        CHECK(h.choices == std::vector<double>{0.2, 0.4, 0.6, 0.8});
    }
    CHECK(cap.heads[2].current == std::optional<double>(0.6));
    for (const auto& h : cap.heads) {
        CHECK(h.flow_variants == std::vector<std::string>{"Standard", "High Flow"});
    }
}

TEST_CASE("nozzle size: only a vendor head offers flow variants", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4});
    in.set_nozzle_diameter_registered = true;
    CHECK(nozzle_capability(in).heads[0].flow_variants.empty());
    CHECK(nozzle_capability(mainline({0.4})).heads[0].flow_variants.empty());
}

TEST_CASE("nozzle size: lane tools on one extruder are one head", "[nozzle_size]") {
    PrinterInputs in = mainline({0.6});
    const std::vector<std::string> lanes = {"extruder", "extruder", "extruder", "extruder"};
    REQUIRE(nozzle_capability(in).heads.size() == 1);
    for (int tool = 0; tool < 4; ++tool) {
        CHECK(head_nozzle_for_tool(in, lanes, tool) == std::optional<double>(0.6));
    }
    CHECK_FALSE(head_nozzle_for_tool(in, lanes, 4).has_value());
}

TEST_CASE("nozzle size: each tool of a changer resolves to its own extruder", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4, 0.6});
    const std::vector<std::string> heads = {"extruder", "extruder1"};
    CHECK(head_nozzle_for_tool(in, heads, 1) == std::optional<double>(0.6));
    CHECK(head_nozzle_for_tool(in, {}, 1) == std::optional<double>(0.6));
    CHECK_FALSE(head_nozzle_for_tool(in, {"", "extruder1"}, 0).has_value());
}

TEST_CASE("nozzle size: a firmware-supervised config is read-only", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4});
    in.config_supervised = true;
    const Capability cap = nozzle_capability(in);
    REQUIRE(cap.heads.size() == 1);
    CHECK(cap.heads[0].mechanism == Mechanism::None);
    CHECK_FALSE(cap.heads[0].settable);
    CHECK(cap.heads[0].current == std::optional<double>(0.4));
}

TEST_CASE("nozzle size: a live diameter beats the config", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4, 0.4});
    in.live_diameters = {std::nullopt, 0.8};
    CHECK(head_nozzle(in, 0) == std::optional<double>(0.4));
    CHECK(head_nozzle(in, 1) == std::optional<double>(0.8));
    CHECK(nozzle_capability(in).heads[1].current == std::optional<double>(0.8));
}

TEST_CASE("nozzle size: a live NaN, zero or negative is ignored", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4, 0.4, 0.4});
    in.live_diameters = {std::numeric_limits<double>::quiet_NaN(), 0.0, -0.6};
    for (int tool = 0; tool < 3; ++tool) {
        CHECK(head_nozzle(in, tool) == std::optional<double>(0.4));
    }
}

TEST_CASE("nozzle size: head_nozzle is empty for an unknown head or no data", "[nozzle_size]") {
    CHECK_FALSE(head_nozzle(PrinterInputs{}, 0).has_value());
    CHECK_FALSE(head_nozzle(mainline({0.4}), 1).has_value());
    CHECK_FALSE(head_nozzle(mainline({0.4}), -1).has_value());
}

TEST_CASE("nozzle size: resolve is the one live-over-config rule", "[nozzle_size]") {
    CHECK(resolve(0.6, 0.4) == std::optional<double>(0.6));
    CHECK(resolve(std::nullopt, 0.4) == std::optional<double>(0.4));
    CHECK(resolve(0.0, 0.4) == std::optional<double>(0.4));
    CHECK(resolve(0.6, std::nullopt) == std::optional<double>(0.6));
    CHECK_FALSE(resolve(std::nullopt, std::nullopt).has_value());
    CHECK_FALSE(resolve(std::numeric_limits<double>::infinity(), std::nullopt).has_value());
}

namespace {

PrinterInputs k2_like() {
    PrinterInputs in = mainline({0.4});
    in.set_nozzle_diameter_registered = true;
    return in;
}

PrinterInputs u1_like() {
    PrinterInputs in;
    in.live_diameters = {0.4, 0.4, 0.4, 0.4};
    in.live_volume_types = {"standard", "standard", "standard", "standard"};
    in.vendor_records_present = true;
    in.vendor_choices = {0.2, 0.4, 0.6, 0.8};
    return in;
}

} // namespace

TEST_CASE("nozzle size plan: a config edit names the section, key and value", "[nozzle_size]") {
    const ChangePlan plan = plan_change(mainline({0.4, 0.4}), 1, 0.6);
    REQUIRE(plan.kind == ChangePlan::Kind::ConfigEdit);
    CHECK(plan.section == "extruder1");
    CHECK(plan.key == "nozzle_diameter");
    CHECK(plan.value == "0.6");
    CHECK(plan.restart_required);
}

TEST_CASE("nozzle size plan: the registered command is sent for extruder only", "[nozzle_size]") {
    PrinterInputs in = k2_like();
    in.config_diameters.push_back(0.4);
    const ChangePlan head0 = plan_change(in, 0, 0.25);
    REQUIRE(head0.kind == ChangePlan::Kind::KlipperCommand);
    CHECK(head0.gcode == "SET_NOZZLE_DIAMETER VALUE=0.25");
    CHECK_FALSE(head0.restart_required);
    CHECK(plan_change(in, 1, 0.6).kind == ChangePlan::Kind::ConfigEdit);
}

TEST_CASE("nozzle size plan: a vendor head gets the firmware's call", "[nozzle_size]") {
    const ChangePlan plan = plan_change(u1_like(), 2, 0.6);
    REQUIRE(plan.kind == ChangePlan::Kind::VendorRpc);
    CHECK(plan.method == helix::snapmaker::nozzle::kMethod);
    CHECK(plan.params["extruder"] == 2);
    CHECK(plan.params["diameter"] == 0.6);
}

TEST_CASE("nozzle size plan: a vendor flow change names the head's own diameter", "[nozzle_size]") {
    const ChangePlan plan = plan_flow_change(u1_like(), 1, "high_flow");
    REQUIRE(plan.kind == ChangePlan::Kind::VendorRpc);
    CHECK(plan.params["extruder"] == 1);
    CHECK(plan.params["diameter"] == 0.4);
    CHECK(plan.params["volume_type"] == "high_flow");
    // No flow choice on a head without flow variants.
    CHECK(plan_flow_change(mainline({0.4}), 0, "high_flow").kind == ChangePlan::Kind::Refused);
}

TEST_CASE("nozzle size plan: refusals", "[nozzle_size]") {
    SECTION("while printing or paused") {
        for (PrinterInputs in : {mainline({0.4}), u1_like()}) {
            in.printing = true;
            CHECK(plan_change(in, 0, 0.6).kind == ChangePlan::Kind::Refused);
        }
        PrinterInputs k2 = k2_like();
        k2.printing = true;
        // The command applies at once and has no idle rule of its own.
        CHECK(plan_change(k2, 0, 0.6).kind == ChangePlan::Kind::KlipperCommand);
    }
    SECTION("a supervised config") {
        PrinterInputs in = mainline({0.4});
        in.config_supervised = true;
        const ChangePlan plan = plan_change(in, 0, 0.6);
        CHECK(plan.kind == ChangePlan::Kind::Refused);
        CHECK_FALSE(plan.reason.empty());
    }
    SECTION("a size outside the list") {
        CHECK(plan_change(mainline({0.4}), 0, 0.45).kind == ChangePlan::Kind::Refused);
        CHECK(plan_change(u1_like(), 0, 0.5).kind == ChangePlan::Kind::Refused);
    }
    SECTION("an unknown head") {
        CHECK(plan_change(mainline({0.4}), 3, 0.6).kind == ChangePlan::Kind::Refused);
        CHECK(plan_change(PrinterInputs{}, 0, 0.6).kind == ChangePlan::Kind::Refused);
    }
    SECTION("the size the head already has") {
        CHECK(plan_change(mainline({0.4}), 0, 0.4).kind == ChangePlan::Kind::Refused);
        CHECK(plan_change(u1_like(), 0, 0.4).kind == ChangePlan::Kind::Refused);
    }
}

TEST_CASE("nozzle size: the command list says whether the size command exists", "[nozzle_size]") {
    using nlohmann::json;
    CHECK(help_lists_size_command(json{{"result", {{"SET_NOZZLE_DIAMETER", "Set it"}}}}) == true);
    CHECK(help_lists_size_command(json{{"result", {{"G28", "Home"}}}}) == false);
    CHECK_FALSE(help_lists_size_command(json{{"error", "x"}}).has_value());
}

TEST_CASE("nozzle size: discovery finds the command on the K2 persona only", "[nozzle_size]") {
    {
        MoonrakerClientMock plain;
        probe_size_command(plain);
        CHECK_FALSE(size_command_registered());
    }
    {
        MoonrakerClientMock k2(MoonrakerClientMock::PrinterType::CREALITY_K2_PLUS);
        probe_size_command(k2);
        CHECK(size_command_registered());
        k2.gcode_script("SET_NOZZLE_DIAMETER VALUE=0.6");
        CHECK(k2.nozzle_properties(0).first == Catch::Approx(0.6));
    }
    set_size_command_registered(false);
}
