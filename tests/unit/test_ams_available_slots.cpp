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

TEST_CASE_METHOD(LVGLTestFixture,
                 "clear_backends closes the home widget gates the departed backend opened",
                 "[ams][ams_state][multi-printer]") {
    // A live printer switch clears the backends and keeps the home grid; a slot count left
    // behind keeps the Multi-Filament card on a printer that has no filament system.
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_backend(std::make_unique<AmsBackendMock>(4));
    ams.sync_from_backend();
    REQUIRE(lv_subject_get_int(ams.get_slot_count_subject()) == 4);

    ams.clear_backends();

    CHECK(lv_subject_get_int(ams.get_slot_count_subject()) == 0);
    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "clear_backends returns every backend subject to its default",
                 "[ams][ams_state][multi-printer]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(0);
    ams.set_backend(std::move(mock));
    mock_ptr->start();
    ams.sync_from_backend();
    REQUIRE(lv_subject_get_int(ams.get_ams_type_subject()) != static_cast<int>(AmsType::NONE));
    REQUIRE(lv_subject_get_int(ams.get_slot_count_subject()) == 4);
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(0)) !=
            static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
    REQUIRE(std::string(lv_subject_get_string(ams.get_ams_system_name_subject())) != "");
    const int slots_version = lv_subject_get_int(ams.get_slots_version_subject());

    mock_ptr->stop();
    ams.clear_backends();

    CHECK(lv_subject_get_int(ams.get_ams_type_subject()) == static_cast<int>(AmsType::NONE));
    CHECK(lv_subject_get_int(ams.get_is_filament_system_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_is_tool_changer_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_backend_count_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_slot_count_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_supports_bypass_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_bypass_active_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_current_slot_subject()) == -1);
    CHECK(lv_subject_get_int(ams.get_pending_target_slot_subject()) == -1);
    CHECK(lv_subject_get_int(ams.get_current_tool_subject()) == -1);
    CHECK(lv_subject_get_int(ams.get_ams_action_subject()) == static_cast<int>(AmsAction::IDLE));
    CHECK(lv_subject_get_int(ams.get_filament_loaded_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_toolchange_visible_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_dryer_supported_subject()) == 0);
    CHECK(lv_subject_get_int(ams.get_path_filament_segment_subject()) ==
          static_cast<int>(PathSegment::NONE));
    CHECK(std::string(lv_subject_get_string(ams.get_ams_system_name_subject())).empty());
    CHECK(lv_subject_get_pointer(ams.get_ams_system_logo_subject()) == nullptr);
    CHECK(std::string(lv_subject_get_string(ams.get_current_material_text_subject())) == "---");
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(lv_subject_get_int(ams.get_slot_color_subject(i)) ==
              static_cast<int>(AMS_DEFAULT_SLOT_COLOR));
        CHECK(lv_subject_get_int(ams.get_slot_status_subject(i)) ==
              static_cast<int>(SlotStatus::UNKNOWN));
        CHECK(std::string(lv_subject_get_string(ams.get_slot_material_subject(i))).empty());
    }
    // Lane widgets redraw from the version, so the clear must announce itself.
    CHECK(lv_subject_get_int(ams.get_slots_version_subject()) != slots_version);
    ams.deinit_subjects();
}
