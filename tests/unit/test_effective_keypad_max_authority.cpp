// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_effective_keypad_max_authority.cpp
 * @brief One ceiling authority for every temperature-input surface
 *        (prestonbrown/helixscreen#1619).
 *
 * TemperatureController::effective_keypad_max() is the only composition of
 * ensure_limits + keypad_range; every keypad surface must derive its ceiling
 * from it through the keypad_ceiling()/custom_keypad_max() face, never
 * compose the primitives itself. These cases pin, per surface, that a
 * configured max below the surface's own fallback WINS, so mutating the
 * shared helper reddens every consumer's suite at once.
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "temperature_controller.h"
#include "temperature_service.h"
#include "test_helpers/controls_panel_test_access.h"
#include "test_helpers/temperature_controller_test_access.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

using helix::HeaterType;
using helix::TemperatureControllerTestAccess;

/// A 290C hotend and a 120C bed — both below every fallback the surfaces
/// would hand themselves, so a pass means the controller's answer won.
void set_typical_caps(helix::TemperatureController& c) {
    TemperatureControllerTestAccess::set_max(c, HeaterType::Nozzle, 290);
    TemperatureControllerTestAccess::set_max(c, HeaterType::Bed, 120);
    TemperatureControllerTestAccess::set_max(c, HeaterType::Chamber, 60);
}

/// ControlsPanel plus the TemperatureService it reads its controller through,
/// wired the way SubjectInitializer wires them at app boot. Destruction runs
/// in reverse declaration order, so the panel dies while the service and the
/// controller it points at are still alive.
class TargetEditFixture : public XMLTestFixture {
  public:
    TargetEditFixture() {
        service_.set_controller(controller_.get());
        panel_.set_temp_control_panel(&service_);
    }

    ~TargetEditFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        service_.set_controller(nullptr);
    }

    TemperatureService service_{state(), &api()};
    std::shared_ptr<helix::TemperatureController> controller_{
        std::make_shared<helix::TemperatureController>(state(), &api())};
    ControlsPanel panel_{state(), &api()};
};

} // namespace

TEST_CASE("ControlsPanel target-edit keypads answer the shared ceiling", "[1619][keypad_ceiling]") {
    TargetEditFixture f;
    set_typical_caps(*f.controller_);

    // 500/150/150 are the panel's own members; the configured max must beat
    // every one of them, or the keypad offers a target the printer rejects.
    CHECK(helix::ui::ControlsPanelTestAccess::target_edit_max(f.panel_, HeaterType::Nozzle, 500) ==
          290);
    CHECK(helix::ui::ControlsPanelTestAccess::target_edit_max(f.panel_, HeaterType::Bed, 150) ==
          120);
    CHECK(helix::ui::ControlsPanelTestAccess::target_edit_max(f.panel_, HeaterType::Chamber, 150) ==
          60);
}

TEST_CASE("TemperatureService custom keypad answers the shared ceiling", "[1619][keypad_ceiling]") {
    TargetEditFixture f;
    set_typical_caps(*f.controller_);

    // The production call passes the heater's static config max as the
    // fallback — the same argument shape TempGraphOverlay's keypad forwards.
    const auto nozzle_fb = f.service_.heater(HeaterType::Nozzle).config.keypad_range.max;
    const auto bed_fb = f.service_.heater(HeaterType::Bed).config.keypad_range.max;
    REQUIRE(nozzle_fb > 290.0f);
    REQUIRE(bed_fb > 120.0f);
    CHECK(f.service_.custom_keypad_max(HeaterType::Nozzle, nozzle_fb) == 290.0f);
    CHECK(f.service_.custom_keypad_max(HeaterType::Bed, bed_fb) == 120.0f);
}

TEST_CASE("A surface with no controller keeps its own fallback", "[1619][keypad_ceiling]") {
    XMLTestFixture f;
    TemperatureService service(f.state(), &f.api());

    CHECK(service.custom_keypad_max(HeaterType::Nozzle, 500.0f) == 500.0f);
    CHECK(service.custom_keypad_max(HeaterType::Bed, 150.0f) == 150.0f);
}
