// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_available_slots.cpp
 * @brief Tests for AmsState::collect_available_slots() field propagation.
 *
 * collect_available_slots() flattens every backend's live SlotInfo into the
 * LVGL-free helix::AvailableSlot abstraction consumed by the filament-mapping
 * surfaces (mapping modal, preflight check, slot picker). This verifies that
 * per-slot color data — including the multi-color hex list used to render
 * diagonal-chunk swatches — survives the SlotInfo -> AvailableSlot conversion.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "filament_mapper.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

TEST_CASE_METHOD(LVGLTestFixture,
                 "collect_available_slots carries multi_color_hexes across the boundary",
                 "[ams][ams_state][available_slots]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<AmsBackendMock>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // Slot 0 gets a two-color spool; the diagonal-chunk swatch renderer needs
    // this comma-separated hex list to arrive intact on the mapping surfaces.
    const std::string MULTI_HEXES = "#202020,#F0F0F0";
    {
        auto slot = mock_ptr->get_slot_info(0);
        slot.color_rgb = 0x202020;
        slot.multi_color_hexes = MULTI_HEXES;
        mock_ptr->set_slot_info(0, slot);
    }

    auto slots = ams.collect_available_slots();
    REQUIRE_FALSE(slots.empty());

    // Locate the AvailableSlot flattened from backend 0 / slot 0.
    const AvailableSlot* found = nullptr;
    for (const auto& s : slots) {
        if (s.backend_index == 0 && s.slot_index == 0) {
            found = &s;
            break;
        }
    }
    REQUIRE(found != nullptr);

    // The load-bearing assertion: the multi-color list survived the
    // SlotInfo -> AvailableSlot conversion in collect_available_slots().
    CHECK(found->multi_color_hexes == MULTI_HEXES);
    CHECK(found->color_rgb == 0x202020u);

    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}
