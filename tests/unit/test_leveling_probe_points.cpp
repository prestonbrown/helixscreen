// SPDX-License-Identifier: GPL-3.0-or-later

#include "leveling_probe_points.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::LevelingDescentTracker;
using helix::LevelingProbePoint;
using helix::PrintStartPhase;
using json = nlohmann::json;

namespace {

// A Voron 2.4 350's [quad_gantry_level] points.
const std::vector<LevelingProbePoint> kQgl = {{25.0, 25.0, PrintStartPhase::QGL},
                                              {25.0, 275.0, PrintStartPhase::QGL},
                                              {275.0, 275.0, PrintStartPhase::QGL},
                                              {275.0, 25.0, PrintStartPhase::QGL}};

} // namespace

TEST_CASE("Leveling points: probe XY selects the phase that probes it",
          "[print][voron][leveling]") {
    SECTION("an exact point") {
        CHECK(helix::leveling_phase_at(kQgl, 25.0, 275.0) == PrintStartPhase::QGL);
    }
    SECTION("within tolerance") {
        CHECK(helix::leveling_phase_at(kQgl, 276.4, 23.6) == PrintStartPhase::QGL);
    }
    SECTION("just outside tolerance") {
        CHECK_FALSE(helix::leveling_phase_at(kQgl, 25.0, 26.6).has_value());
        // The first point of this printer's mesh, 5mm off the QGL corner.
        CHECK_FALSE(helix::leveling_phase_at(kQgl, 30.0, 5.0).has_value());
    }
    SECTION("Z tilt points name Z_TILT, gantry points QGL") {
        const std::vector<LevelingProbePoint> mixed = {{25.0, 25.0, PrintStartPhase::QGL},
                                                       {150.0, 290.0, PrintStartPhase::Z_TILT}};
        CHECK(helix::leveling_phase_at(mixed, 150.0, 290.0) == PrintStartPhase::Z_TILT);
        CHECK(helix::leveling_phase_at(mixed, 25.0, 25.0) == PrintStartPhase::QGL);
    }
    SECTION("no configured points match nothing") {
        CHECK_FALSE(helix::leveling_phase_at({}, 25.0, 25.0).has_value());
    }
}

TEST_CASE("Leveling points: descents at two points read as leveling", "[print][voron][leveling]") {
    LevelingDescentTracker t;

    SECTION("a probe tour") {
        CHECK_FALSE(t.note_position(kQgl, 25, 25, 10));
        CHECK_FALSE(t.note_position(kQgl, 25, 25, 1)); // one point descended
        CHECK_FALSE(t.note_position(kQgl, 25, 25, 10));
        CHECK_FALSE(t.note_position(kQgl, 25, 275, 10));
        CHECK(t.note_position(kQgl, 25, 275, 1) == PrintStartPhase::QGL);
    }
    SECTION("repeat descents at one corner are a park, not leveling") {
        for (int i = 0; i < 4; ++i) {
            CHECK_FALSE(t.note_position(kQgl, 275, 275, 10));
            CHECK_FALSE(t.note_position(kQgl, 275, 275, 2));
        }
    }
    SECTION("arriving lower at a new point is travel, not a descent there") {
        CHECK_FALSE(t.note_position(kQgl, 25, 25, 10));
        CHECK_FALSE(t.note_position(kQgl, 25, 25, 1));
        CHECK_FALSE(t.note_position(kQgl, 25, 275, 0.5));
    }
    SECTION("no configured points never fire") {
        CHECK_FALSE(t.note_position({}, 25, 25, 10));
        CHECK_FALSE(t.note_position({}, 25, 25, 1));
    }
}

TEST_CASE("PrinterDiscovery: leveling probe points come from QGL and Z tilt config",
          "[printer_discovery][print][voron][leveling]") {
    helix::PrinterDiscovery discovery;

    SECTION("this Voron 2.4's quad_gantry_level points") {
        discovery.parse_leveling_probe_points(json::parse(
            R"({"quad_gantry_level":{"points":[[25.0,25.0],[25.0,275.0],[275.0,275.0],[275.0,25.0]],"speed":200.0}})"));
        const auto& pts = discovery.leveling_probe_points();
        REQUIRE(pts.size() == 4);
        CHECK(pts[2].x == 275.0);
        CHECK(pts[2].y == 275.0);
        CHECK(pts[2].phase == PrintStartPhase::QGL);

        discovery.clear();
        CHECK(discovery.leveling_probe_points().empty());
    }
    SECTION("z_tilt points name Z_TILT") {
        discovery.parse_leveling_probe_points(
            json::parse(R"({"z_tilt":{"points":[[-50,18],[150,348],[350,18]]}})"));
        const auto& pts = discovery.leveling_probe_points();
        REQUIRE(pts.size() == 3);
        CHECK(pts[0].x == -50.0);
        CHECK(pts[0].phase == PrintStartPhase::Z_TILT);
    }
    SECTION("a malformed entry drops the section") {
        discovery.parse_leveling_probe_points(
            json::parse(R"({"z_tilt":{"points":[[50,18],"150,348",[350]]}})"));
        CHECK(discovery.leveling_probe_points().empty());
        discovery.parse_leveling_probe_points(json::parse(R"({"z_tilt":{"points":"50,18"}})"));
        CHECK(discovery.leveling_probe_points().empty());
    }
    SECTION("missing sections give no points") {
        discovery.parse_leveling_probe_points(json::parse(R"({"extruder":{}})"));
        CHECK(discovery.leveling_probe_points().empty());
        discovery.parse_leveling_probe_points(json::array());
        CHECK(discovery.leveling_probe_points().empty());
    }
}
