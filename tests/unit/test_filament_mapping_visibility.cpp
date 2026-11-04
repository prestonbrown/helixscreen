// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_visibility.cpp
 * @brief Pins the merged FilamentMappingCard::should_show() predicate.
 *
 * The card used to hide itself on backends whose tool mapping is not editable
 * (Snapmaker U1, ACE), because a second surface - the print-detail FILAMENTS
 * card - rendered the same information there. That second surface is gone, so
 * hiding on non-editable backends would show the user nothing at all. The
 * dead-control concern it existed for now lives entirely on the tap affordance
 * (PrintSelectDetailView::current_remap_block).
 *
 * The two gates that MUST survive the merge are pinned here too: the bypass
 * single-lane suppression (a K2 user printed off the bypass spool after reading
 * a chip as a lane claim) and BOTH halves of the multi-tool-printer-aware
 * tool-count rule (any tool on a multi-tool printer, 2+ tools on a single
 * extruder) - the single-extruder half via SingleExtruderMappingCardFixture,
 * which forces a genuine 1-tool ToolState topology rather than relying on
 * whatever the shared 4-slot AmsBackendMock fixture happens to report.
 */

#include "ui_filament_mapping_card.h"

#include "../mapping_card_render_fixture.h"
#include "../test_helpers/mock_bypass.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

namespace {

/// Forces ToolState into a genuine single-extruder topology (tool_count()==1,
/// is_multi_tool()==false), independent of whatever a prior test file left the
/// global ToolState singleton (a Meyers singleton, so its state otherwise
/// survives across the whole binary) holding. Slot count 1 on the AMS side
/// makes the `ams_slots > 1` half of is_multi_tool_printer false too, so both
/// halves of the OR are pinned rather than just one.
///
/// init_tools()/deinit_subjects() sequencing mirrors the established pattern
/// in test_tool_switcher_print_gate.cpp. The object list omits any
/// "toolchanger"/"tool T*" entry and reports exactly one "extruder" heater, so
/// PrinterDiscovery takes ToolState::init_tools()'s "no tool changer" branch
/// and builds exactly one tool.
struct SingleExtruderMappingCardFixture : MappingCardRenderFixture {
    SingleExtruderMappingCardFixture() : MappingCardRenderFixture(/*slot_count=*/1) {
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(false);
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
        helix::ToolState::instance().init_tools(hw);
        REQUIRE(helix::ToolState::instance().tool_count() == 1);
        REQUIRE_FALSE(helix::ToolState::instance().is_multi_tool());
    }

    ~SingleExtruderMappingCardFixture() override {
        helix::ToolState::instance().deinit_subjects();
    }
};

/// A single extruder with no filament system at all: no AMS backend, one tool,
/// and whatever external spool record the test writes.
struct NoFilamentSystemCardFixture : LVGLUITestFixture {
    FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;

    NoFilamentSystemCardFixture() {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);
        ams.clear_backends();
        ams.clear_external_spool_info();
        helix::ToolState::instance().deinit_subjects();
        helix::ToolState::instance().init_subjects(false);
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "gcode_move"}));
        helix::ToolState::instance().init_tools(hw);
        REQUIRE_FALSE(ams.is_available());
        REQUIRE_FALSE(helix::ToolState::instance().has_multiple_toolheads());

        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        FilamentMappingCard::init_subjects();
        lv_xml_create(rows, "filament_chip_row", nullptr);
        card.create(card_widget, rows, lv_obj_create(card_widget));
    }

    ~NoFilamentSystemCardFixture() override {
        AmsState::instance().clear_external_spool_info();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        helix::ToolState::instance().deinit_subjects();
    }

    static void load(const char* material, uint32_t rgb) {
        helix::SlotInfo spool;
        spool.material = material;
        spool.color_rgb = rgb;
        spool.status = helix::SlotStatus::LOADED;
        AmsState::instance().set_external_spool_info_in_memory(spool);
    }
};

using Chip = MappingCardRenderFixture;

} // namespace

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "No filament system: a one-tool file shows one Loaded chip",
                 "[filament_mapping][visibility][loaded]") {
    load("PLA", 0xFF0000);
    card.update({"#FF0000"}, {"PLA"});
    REQUIRE(card.should_show());
    CHECK(card.shows_loaded());
    REQUIRE(Chip::chip_count() == 1);
    CHECK(Chip::chip_string(0, "_material") == "PLA");
    CHECK(Chip::chip_string(0, "_slot").empty());
    CHECK(Chip::chip_int(0, "_severity") == 0);
    CHECK(Chip::chip_int(0, "_slot_state") == 1); // the loaded spool's dot
    CHECK(card.get_mappings().empty());

    // A tap has nothing to open: no lane to pick, no picker.
    lv_obj_t* chip = lv_obj_find_by_name(rows, "filament_chip_0");
    REQUIRE(chip != nullptr);
    lv_obj_send_event(chip, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(card.slot_picker().is_visible());
    process_lvgl(100);
}

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "No filament system: loaded PLA against a PETG file blocks",
                 "[filament_mapping][visibility][loaded]") {
    load("PLA", 0x00FF00);
    card.update({"#00FF00"}, {"PETG"});
    REQUIRE(card.should_show());
    REQUIRE(Chip::chip_count() == 1);
    CHECK(Chip::chip_string(0, "_slot").empty());
    CHECK(Chip::chip_int(0, "_severity") == 2);

    // The chip follows a spool edit.
    load("PETG", 0x00FF00);
    card.refresh_slot_data();
    CHECK(Chip::chip_int(0, "_severity") == 0);
    process_lvgl(100);
}

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "No filament system: unknown loaded filament reads ? and stays grey",
                 "[filament_mapping][visibility][loaded]") {
    card.update({"#00FF00"}, {"PETG"});
    REQUIRE(card.should_show());
    REQUIRE(Chip::chip_count() == 1);
    CHECK(Chip::chip_string(0, "_slot").empty());
    CHECK(Chip::chip_int(0, "_severity") == 0);
    CHECK(Chip::chip_int(0, "_slot_state") == 4); // the question-mark icon
    process_lvgl(100);
}

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "No filament system: a two-tool file reads Loaded then Swap",
                 "[filament_mapping][visibility][loaded]") {
    load("PLA", 0xFF0000);
    card.set_used_tools(std::set<int>{1, 2});
    card.update({"#000000", "#FF0000", "#0000FF"}, {"ABS", "PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(Chip::chip_count() == 2);
    CHECK(Chip::chip_string(0, "_material") == "PLA");
    CHECK(Chip::chip_string(0, "_slot").empty());
    CHECK(Chip::chip_string(1, "_material") == "PETG");
    CHECK(Chip::chip_string(1, "_slot").empty());
    // Nothing is loaded for a swap yet, so PETG is not judged against PLA.
    CHECK(Chip::chip_int(1, "_severity") == 0);
    CHECK(Chip::chip_int(1, "_slot_state") == 3); // the swap icon
    process_lvgl(100);
}

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "No filament system: a file naming no material shows no card",
                 "[filament_mapping][visibility][loaded]") {
    load("PLA", 0xFF0000);
    card.update({"#FF0000"}, {""});
    CHECK_FALSE(card.should_show());
    // Only the used tools count: an unused tool's material is not the file's.
    card.set_used_tools(std::set<int>{0});
    card.update({"#FF0000", "#00FF00"}, {"", "PLA"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}

TEST_CASE_METHOD(NoFilamentSystemCardFixture,
                 "A filament system arriving seeds the card; one leaving clears its mappings",
                 "[filament_mapping][visibility][loaded]") {
    load("PLA", 0xFF0000);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(card.should_show());
    REQUIRE(card.shows_loaded());
    REQUIRE(card.get_mappings().empty());

    auto& ams = AmsState::instance();
    auto owned = std::make_unique<helix::AmsBackendMock>(2);
    helix::AmsBackendMock* const backend = owned.get();
    backend->set_operation_delay(0);
    ams.set_backend(std::move(owned));
    backend->start();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ams.is_available());

    int changed = 0;
    card.set_on_mappings_changed([&changed] { ++changed; });
    card.refresh_slot_data();
    REQUIRE(card.should_show());
    CHECK_FALSE(card.shows_loaded());
    const auto mappings = card.get_mappings();
    REQUIRE(mappings.size() == 2);
    CHECK_FALSE(card.get_available_slots().empty());
    CHECK(changed == 1);
    // Seeded onto the mock's lanes, so neither chip is the red no-lane state.
    REQUIRE(Chip::chip_count() == 2);
    CHECK(Chip::chip_int(0, "_slot_state") != 0);
    CHECK(Chip::chip_int(1, "_slot_state") != 0);
    CHECK_FALSE(Chip::chip_string(0, "_slot").empty());

    helix::ui::UpdateQueue::instance().drain();
    backend->stop();
    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(ams.is_available());
    card.refresh_slot_data();
    REQUIRE(card.should_show());
    CHECK(card.shows_loaded());
    CHECK(card.get_mappings().empty());
    CHECK(card.get_available_slots().empty());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Card shows on a backend whose mapping is not editable",
                 "[filament_mapping][visibility]") {
    // snapmaker_mode_ is the U1 case: RemapStrategy::PrePrintSend, so the
    // pick IS honored (through the pre-print send) while no tool->slot table is
    // written and owns_tool_mapping_table() is false. Before the merge this hid
    // the card and a second surface drew the chips; that surface is gone.
    // should_show() reads neither, which is the point — card visibility is
    // decided by the tools the file uses, not by what the backend can remap.
    mock->set_snapmaker_mode(true);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(chip_count() == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Card hides a single-lane print while bypass is engaged",
                 "[filament_mapping][visibility]") {
    // With bypass engaged a single-tool print takes filament from the external
    // spool, so offering a lane mapping claims something the print will not do.
    REQUIRE(helix::test::unload_for_bypass(*mock));
    REQUIRE(mock->enable_bypass().success());
    REQUIRE(mock->is_bypass_active());
    card.update({"#FF0000"}, {"PLA"});
    CHECK_FALSE(card.should_show());
    // A printer with a filament system never falls back to the loaded chips.
    CHECK_FALSE(card.shows_loaded());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "Card shows a single-tool file on a multi-tool printer",
                 "[filament_mapping][visibility]") {
    // The multi-tool half of the tool-count rule moved from
    // swatches_card_visible_for(): on a multi-tool printer ANY referenced tool
    // is worth showing, because which lane it routes to is the whole question.
    card.set_used_tools(std::set<int>{0});
    card.update({"#FF0000"}, {"PLA"});
    CHECK(card.should_show());
    CHECK(chip_count() == 1);
    CHECK_FALSE(card.shows_loaded());
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture,
                 "An explicit empty used-tools set does not hide the card",
                 "[filament_mapping][visibility]") {
    // Pins the fix for a real regression this task introduced and review
    // caught: the tool-count rule originally read used_tools_->size()
    // directly, so an explicit (non-nullopt) EMPTY set forced tool_count to 0
    // and hid the card even with a non-empty tool_info_ — unlike every other
    // reader of used_tools_ in this class (the bypass gate above,
    // apply_used_tools_filter()'s documented contract), which treats an empty
    // set as "no answer, show all". The fix reuses print_lane_requirement(),
    // which has that same fallback built in, so this must SHOW.
    card.set_used_tools(std::set<int>{});
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(chip_count() == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(SingleExtruderMappingCardFixture,
                 "Card hides a single tool on a single-extruder printer",
                 "[filament_mapping][visibility]") {
    // The single-extruder half of the same rule: an ordinary one-colour print
    // uses exactly one tool, which is not worth a lane-mapping card when there
    // is no lane to route it through.
    card.update({"#FF0000"}, {"PLA"});
    CHECK_FALSE(card.should_show());
    process_lvgl(100);
}

TEST_CASE_METHOD(SingleExtruderMappingCardFixture,
                 "Card shows two tools on a single-extruder printer",
                 "[filament_mapping][visibility]") {
    // 2+ tools on a single extruder is a manual-swap multi-colour file, not an
    // ordinary single-colour print — the KEEP half of the rule this task's
    // brief named as the failure mode of the merge.
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    CHECK(card.should_show());
    CHECK(chip_count() == 2);
    process_lvgl(100);
}

TEST_CASE_METHOD(MappingCardRenderFixture, "A chip centres each label on its band",
                 "[filament_mapping][chip]") {
    // The chip row's height is the chip's: at 40px the bands are shorter than
    // the lane number's line, which then hangs from the band's top unless the
    // band centres it.
    lv_obj_set_size(rows, 300, 40);
    card.update({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(chip_count() >= 1);
    lv_obj_t* chip = this->chip(0);
    REQUIRE(chip != nullptr);
    lv_obj_update_layout(chip);

    const auto centre_y = [](lv_obj_t* obj) {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        return (a.y1 + a.y2) / 2.0;
    };
    for (const auto& [band_name, label_name] :
         {std::pair{"chip_body", "chip_slot"}, std::pair{"chip_header", "chip_material"}}) {
        INFO(label_name);
        lv_obj_t* band = lv_obj_find_by_name(chip, band_name);
        lv_obj_t* label = lv_obj_find_by_name(chip, label_name);
        REQUIRE(band != nullptr);
        REQUIRE(label != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
        REQUIRE(std::string(lv_label_get_text(label)).size() > 0);
        CAPTURE(lv_obj_get_height(band), lv_obj_get_height(label));
        CHECK(std::abs(centre_y(label) - centre_y(band)) <= 1.0);
    }
    process_lvgl(100);
}
