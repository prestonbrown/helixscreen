// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_material_label_buffer.cpp
 * @brief The "currently loaded" material label must not silently truncate.
 *
 * AmsState publishes that label through an lv_subject_t STRING backed by a
 * fixed char buffer, and lv_subject_set_string() writes it with lv_strlcpy —
 * anything past the buffer is dropped with no warning, no error, and no failing
 * assertion anywhere. The only thing standing between a correct label and a
 * clipped one is the buffer's size.
 *
 * Real Spoolman filament names run long once a brand and a material land on
 * either side of them ("Polymaker PolyTerra Dual Sakura Pink & Cotton White
 * PLA" is 55 characters), well past 48 bytes.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string material_text() {
    return std::string(
        lv_subject_get_string(AmsState::instance().get_current_material_text_subject()));
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState current-material label carries a full Spoolman name",
                 "[ams][ams_state][label]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* backend = mock.get();
    ams.set_backend(std::move(mock));

    // The mock boots with slot 0 loaded, which is the branch that builds the
    // "<name> <material>" label rather than the bypass/empty ones.
    REQUIRE(backend->is_filament_loaded());
    REQUIRE(backend->get_current_slot() == 0);

    SECTION("a long name survives the round trip through the subject") {
        SlotInfo slot = backend->get_slot_info(0);
        // spool_name is where a real Spoolman/AFC filament name lands, and
        // helix::resolve_filament_label() takes it ahead of every other naming
        // layer. brand is cleared so the assertion is about length, not dedup.
        slot.spoolman_id = 42;
        slot.spool_name = "Polymaker PolyTerra Dual Sakura Pink & Cotton White";
        slot.brand.clear();
        slot.color_name.clear();
        slot.material = "PLA";
        helix::test::apply_edit(*backend, 0, slot);

        const std::string expected = slot.spool_name + " " + slot.material;
        // Longer than the old 48-byte buffer could hold (47 chars + NUL).
        REQUIRE(expected.size() > 47);

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == expected);
        CHECK(material_text().size() == expected.size());
    }

    SECTION("a short name is unaffected") {
        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = 7;
        slot.spool_name = "Jet Black";
        slot.brand.clear();
        slot.color_name.clear();
        slot.material = "PLA";
        helix::test::apply_edit(*backend, 0, slot);

        ams.sync_current_loaded_from_backend();

        CHECK(material_text() == "Jet Black PLA");
    }

    ams.clear_backends();
    ams.deinit_subjects();
}
