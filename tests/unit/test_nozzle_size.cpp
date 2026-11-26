// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_size.cpp
 * @brief The nozzle-size capability layer: which heads a printer has, how each
 *        head's size can change, and the one resolver every consumer reads.
 */

#include "nozzle_size.h"

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
    const Capability cap = capability(mainline({0.4}));
    REQUIRE(cap.heads.size() == 1);
    const HeadCapability& h = cap.heads[0];
    CHECK(h.tool == 0);
    CHECK(h.extruder == "extruder");
    CHECK(h.current == std::optional<double>(0.4));
    CHECK(h.settable);
    CHECK(h.mechanism == Mechanism::ConfigEdit);
    CHECK(h.restart_required);
    CHECK(h.idle_only);
    CHECK(h.choices == std::vector<double>(kCommonSizes.begin(), kCommonSizes.end()));
}

TEST_CASE("nozzle size: a two-extruder config gives each head its own entry", "[nozzle_size]") {
    const Capability cap = capability(mainline({0.4, 0.6}));
    REQUIRE(cap.heads.size() == 2);
    CHECK(cap.heads[1].tool == 1);
    CHECK(cap.heads[1].extruder == "extruder1");
    CHECK(cap.heads[1].current == std::optional<double>(0.6));
    CHECK(cap.heads[1].mechanism == Mechanism::ConfigEdit);
}

TEST_CASE("nozzle size: a registered SET_NOZZLE_DIAMETER covers extruder only", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4, 0.6});
    in.set_nozzle_diameter_registered = true;
    const Capability cap = capability(in);
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
    const Capability cap = capability(in);
    REQUIRE(cap.heads.size() == 4);
    for (const auto& h : cap.heads) {
        CHECK(h.mechanism == Mechanism::VendorRpc);
        CHECK(h.settable);
        CHECK(h.idle_only);
        CHECK_FALSE(h.restart_required);
        CHECK(h.choices == std::vector<double>{0.2, 0.4, 0.6, 0.8});
    }
    CHECK(cap.heads[2].current == std::optional<double>(0.6));
}

TEST_CASE("nozzle size: a firmware-supervised config is read-only", "[nozzle_size]") {
    PrinterInputs in = mainline({0.4});
    in.config_supervised = true;
    const Capability cap = capability(in);
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
    CHECK(capability(in).heads[1].current == std::optional<double>(0.8));
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
