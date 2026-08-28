// Copyright (C) 2025-2026 356C LLC
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

TEST_CASE_METHOD(LVGLTestFixture, "an assigned but empty lane still flattens to is_empty",
                 "[ams][ams_state][available_slots]") {
    // A lane the user assigned filament to, whose spool has since been pulled,
    // keeps its identity so ui_ams_slot.cpp can ghost it. It holds no filament
    // though, so no print may be mapped onto it: filament_mapper.cpp skips on
    // is_empty alone (the v0.91 "wrong filament" report), which means identity
    // must never leak into that flag.
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<AmsBackendMock>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    {
        auto slot = mock_ptr->get_slot_info(0);
        slot.material = "ASA-GF";
        slot.brand = "Ambrosia";
        slot.spool_name = "Black ASA-GF";
        mock_ptr->set_slot_info(0, slot);
    }
    mock_ptr->force_slot_status(0, SlotStatus::EMPTY);

    const auto slots = ams.collect_available_slots();
    const AvailableSlot* found = nullptr;
    for (const auto& s : slots) {
        if (s.backend_index == 0 && s.slot_index == 0) {
            found = &s;
            break;
        }
    }
    REQUIRE(found != nullptr);

    CHECK(found->is_empty);             // nothing may be mapped onto it
    CHECK(found->material == "ASA-GF"); // ...but the assignment is still there

    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}
