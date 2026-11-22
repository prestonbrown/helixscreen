// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_live_card.h"

#include "../catch_amalgamated.hpp"

using helix::calibration::StepKind;
using helix::calibration::StepLive;
using helix::ui::LiveKind;

TEST_CASE("the live card picks its view, badge and warning from the step's kind",
          "[calibration_live]") {
    StepLive live;
    const auto heat = helix::ui::live_card_text(StepKind::Heaters, live);
    CHECK(heat.kind == LiveKind::Heat);
    CHECK(heat.badge_tone == 2);
    CHECK(heat.hot);

    const auto mesh = helix::ui::live_card_text(StepKind::BedMesh, live);
    CHECK(mesh.kind == LiveKind::Mesh);
    CHECK(mesh.hot);

    const auto shaper = helix::ui::live_card_text(StepKind::InputShaper, live);
    CHECK(shaper.kind == LiveKind::Shaper);
    CHECK(shaper.badge_tone == 3);
    CHECK_FALSE(shaper.hot);

    const auto belt = helix::ui::live_card_text(StepKind::BeltTension, live);
    CHECK(belt.kind == LiveKind::Belt);
    CHECK(belt.badge_tone == 1);

    const auto other = helix::ui::live_card_text(StepKind::ZOffset, live);
    CHECK(other.kind == LiveKind::None);
    CHECK(other.badge.empty());

    const auto done = helix::ui::live_card_done_text();
    CHECK(done.kind == LiveKind::Done);
    CHECK(done.badge_tone == 4);
}

TEST_CASE("the heater card says which heater and method it is tuning", "[calibration_live]") {
    StepLive live;
    live.heater = "extruder";
    live.mpc = true;
    const auto nozzle_mpc = helix::ui::live_card_text(StepKind::Heaters, live).what;
    live.heater = "heater_bed";
    const auto bed_mpc = helix::ui::live_card_text(StepKind::Heaters, live).what;
    live.mpc = false;
    const auto bed_pid = helix::ui::live_card_text(StepKind::Heaters, live).what;
    CHECK(nozzle_mpc.find("nozzle") != std::string::npos);
    CHECK(nozzle_mpc.find("MPC") != std::string::npos);
    CHECK(bed_mpc.find("bed") != std::string::npos);
    CHECK(bed_pid.find("PID") != std::string::npos);
    CHECK(bed_pid.find("bed") != std::string::npos);
}

TEST_CASE("a belt fills its track up to the target mark", "[calibration_live]") {
    CHECK(helix::ui::belt_fill_percent(0.0f) == 0);
    CHECK(helix::ui::belt_fill_percent(-1.0f) == 0);
    CHECK(helix::ui::belt_fill_percent(0.5f) == helix::ui::kBeltTargetPercent / 2);
    CHECK(helix::ui::belt_fill_percent(1.0f) == helix::ui::kBeltTargetPercent);
    CHECK(helix::ui::belt_fill_percent(1.7f) == helix::ui::kBeltTargetPercent);
}

TEST_CASE("the mesh stays dimmed where the probe has not been", "[calibration_live]") {
    CHECK(helix::ui::mesh_unprobed_percent(0, 49) == 100);
    CHECK(helix::ui::mesh_unprobed_percent(49, 49) == 0);
    CHECK(helix::ui::mesh_unprobed_percent(10, 40) == 75);
    CHECK(helix::ui::mesh_unprobed_percent(60, 49) == 0);
    CHECK(helix::ui::mesh_unprobed_percent(5, 0) == 0);
}
