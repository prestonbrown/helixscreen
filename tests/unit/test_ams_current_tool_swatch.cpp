// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_current_tool_swatch.cpp
 * @brief The print status tool indicator paints the loaded filament through the
 *        swatch painter: its colour, its multi-colour split and its finish.
 */

#include "ui_ams_current_tool.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
void drain() {
    ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_current_tool: the swatch paints the loaded slot's colours and finish",
                 "[ams][swatch][ams_current_tool]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);

    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* mock_ptr = mock.get();
    mock_ptr->set_operation_delay(10);
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    SlotInfo multi = mock_ptr->get_slot_info(1);
    multi.color_rgb = 0xFF0000;
    multi.multi_color_hexes = "#FF0000,#00FF00,#0000FF";
    multi.material = "PLA";
    multi.product_name = "Silk Tricolor";
    REQUIRE(test::apply_edit(*mock_ptr, 1, multi).success());
    SlotInfo plain = mock_ptr->get_slot_info(2);
    plain.color_rgb = 0x2266AA;
    plain.multi_color_hexes.clear();
    plain.material = "PETG";
    plain.product_name.clear();
    plain.color_name.clear();
    plain.spool_name.clear();
    REQUIRE(test::apply_edit(*mock_ptr, 2, plain).success());

    lv_obj_t* tool =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_current_tool", nullptr));
    REQUIRE(tool != nullptr);
    ui_ams_current_tool_setup(tool);
    lv_obj_t* swatch = lv_obj_find_by_name(tool, "color_swatch");
    REQUIRE(swatch != nullptr);
    lv_obj_t* disc = lv_obj_find_by_name(swatch, "color_swatch_disc");
    REQUIRE(disc != nullptr);

    mock_ptr->load_filament(1);
    mock_ptr->wait_for_operation_thread();
    REQUIRE(mock_ptr->get_current_slot() == 1);
    ams.sync_from_backend();
    drain();

    REQUIRE(lv_subject_get_int(ams.get_current_color_subject()) == 0xFF0000);
    CHECK(std::string(lv_subject_get_string(ams.get_current_colors_subject())) ==
          multi.multi_color_hexes);
    CHECK(ui::has_swatch_paint(disc));
    CHECK(ui::swatch_paint_colors(disc).size() == 3);
    CHECK(ui::swatch_paint_finish(disc) == ui::SwatchFinish::Silk);

    // A plain spool loaded after it paints its one colour, with no finish.
    mock_ptr->unload_filament(1);
    mock_ptr->wait_for_operation_thread();
    mock_ptr->load_filament(2);
    mock_ptr->wait_for_operation_thread();
    REQUIRE(mock_ptr->get_current_slot() == 2);
    ams.sync_from_backend();
    drain();

    const auto colors = ui::swatch_paint_colors(disc);
    REQUIRE(colors.size() == 1);
    CHECK((lv_color_to_u32(colors[0]) & 0xFFFFFF) == 0x2266AAu);
    CHECK(ui::swatch_paint_finish(disc) == ui::SwatchFinish::Plain);

    lv_obj_delete(tool);
    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}
