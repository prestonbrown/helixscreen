// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_screws_tilt_direction_db.cpp
 * @brief The printer-database screw-direction override, end to end
 *
 * Klipper computes CW/CCW from its configured screw_thread. Some vendors ship a
 * screw_thread that disagrees with how the screw physically responds, so the
 * database carries an explicit "screws_tilt_direction" and the parser flips the
 * displayed direction to match reality.
 *
 * This is the test that was missing when the FlashForge AD5X shipped WITHOUT the
 * key while all four of its AD5M-family siblings carried it: the panel told users
 * to tighten when the correct action was to loosen. A data-only omission with no
 * test to catch it. These exercise the whole chain - configured printer name ->
 * PrinterDetector -> database lookup -> parser flip - rather than just asserting
 * the JSON, so a future entry that loses the key fails here.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "calibration_types.h"
#include "config.h"
#include "printer_detector.h"
#include "screws_tilt_parser.h"
#include "wizard_config_paths.h"

#include <string>

#include "../catch_amalgamated.hpp"

// Config, ConfigTestAccess and wizard:: all live in namespace helix.
using namespace helix;

namespace {

class ConfiguredPrinterFixture : public HelixTestFixture {
  protected:
    Config* cfg = Config::get_instance();

    ~ConfiguredPrinterFixture() override {
        // Config is a process-wide singleton; leaving a printer selected here
        // would silently steer every later test in the binary.
        helix::test::reset_config_singleton();
    }

    void select(const std::string& printer_name) {
        cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, printer_name);
    }

    /// Parse one Klipper line the way ScrewsTiltCollector does.
    static ScrewTiltResult parse(const std::string& line) {
        ScrewTiltResult r;
        REQUIRE(helix::parse_screws_tilt_line(line, r));
        return r;
    }
};

constexpr const char* CW_LINE = "// Right Near : x=202.0, y=12.5, z=-4.59750 : adjust CW 01:24";

} // namespace

TEST_CASE_METHOD(ConfiguredPrinterFixture, "AD5X declares the inverted screw direction",
                 "[calibration][screws_tilt]") {
    select("FlashForge Adventurer 5X");
    REQUIRE(PrinterDetector::screws_tilt_direction_override() == "ccw");
}

TEST_CASE_METHOD(ConfiguredPrinterFixture,
                 "AD5X flips Klipper's direction all the way to the displayed string",
                 "[calibration][screws_tilt]") {
    select("FlashForge Adventurer 5X");

    const ScrewTiltResult r = parse(CW_LINE);
    REQUIRE(r.adjustment == "CCW 01:24");
    REQUIRE(r.friendly_adjustment(false) == "Loosen 1 turn");
    REQUIRE(r.signed_adjustment_minutes().value() == -84);
}

TEST_CASE_METHOD(ConfiguredPrinterFixture, "The whole AD5M/AD5X family declares it",
                 "[calibration][screws_tilt]") {
    // The AD5X was the one that shipped without it. Pin all five so the next
    // entry added to this family cannot quietly omit it either.
    for (const char* name : {"FlashForge Adventurer 5M", "FlashForge Adventurer 5M (ForgeX)",
                             "FlashForge Adventurer 5M Pro",
                             "FlashForge Adventurer 5M Pro (ForgeX)", "FlashForge Adventurer 5X"}) {
        select(name);
        INFO("printer: " << name);
        REQUIRE(PrinterDetector::screws_tilt_direction_override() == "ccw");
    }
}

TEST_CASE_METHOD(ConfiguredPrinterFixture, "A printer without the key is left alone",
                 "[calibration][screws_tilt]") {
    // The flip must be opt-in per printer: applying it by default would invert
    // the advice for every correctly-configured machine.
    SECTION("a printer that declares nothing") {
        select("Voron 2.4");
        REQUIRE(PrinterDetector::screws_tilt_direction_override().empty());
        REQUIRE(parse(CW_LINE).adjustment == "CW 01:24");
    }

    SECTION("no printer selected at all") {
        REQUIRE(PrinterDetector::screws_tilt_direction_override().empty());
        REQUIRE(parse(CW_LINE).adjustment == "CW 01:24");
    }

    SECTION("an unknown printer name") {
        select("Not A Real Printer");
        REQUIRE(PrinterDetector::screws_tilt_direction_override().empty());
        REQUIRE(parse(CW_LINE).adjustment == "CW 01:24");
    }
}
