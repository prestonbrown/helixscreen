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

#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "temperature_controller.h"
#include "temperature_service.h"
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

/// The TemperatureService the temperature graph's keypad reads its controller
/// through, wired the way SubjectInitializer wires it at app boot.
class TargetEditFixture : public XMLTestFixture {
  public:
    TargetEditFixture() {
        service_.set_controller(controller_.get());
    }

    ~TargetEditFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        service_.set_controller(nullptr);
    }

    TemperatureService service_{state(), &api()};
    std::shared_ptr<helix::TemperatureController> controller_{
        std::make_shared<helix::TemperatureController>(state(), &api())};
};

} // namespace

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
