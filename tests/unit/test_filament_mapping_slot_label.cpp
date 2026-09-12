// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_mapping_slot_label.cpp
 * @brief The remap modal names a position in the printer's own word.
 *
 * Run with: ./build/bin/helix-tests "[filament_mapping][modal][i18n]"
 *
 * Two paths reach a row's slot text. A mapping that resolves to an
 * AvailableSlot is spelled by FilamentMapper::format_slot_label() from that
 * slot's own noun; a mapping that resolves to nothing falls back to the active
 * printer's noun. Both have to agree with what every other surface calls the
 * position, so both are pinned here.
 */

#include "ui_filament_mapping_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_mapping_modal_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_mapper.h"
#include "printer_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::AmsState;
using helix::AvailableSlot;
using helix::ToolMapping;
using helix::ui::FilamentMappingModal;

namespace {

/// AmsBackendMock reports Happy Hare, so active_lane_noun() answers Gate and a
/// hardcoded "Slot" cannot pass either section.
struct MappingLabelFixture : public LVGLUITestFixture {
    MappingLabelFixture() {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
        ams.set_backend(std::make_unique<AmsBackendMock>(4));
    }

    ~MappingLabelFixture() override {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        ams.deinit_subjects();
    }

    static ToolMapping mapping_to(int slot) {
        ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = slot;
        m.mapped_backend = 0;
        return m;
    }
};

} // namespace

TEST_CASE_METHOD(MappingLabelFixture, "the remap row names a position in the printer's own word",
                 "[filament_mapping][modal][i18n]") {
    REQUIRE(AmsState::instance().get_backend()->lane_noun() == helix::ui::LaneNoun::Gate);

    AvailableSlot known{1, 0, 0xE72F1D, "PLA", false, -1};
    known.local_slot_index = 1;
    known.noun = helix::ui::LaneNoun::Gate;

    FilamentMappingModal modal;
    modal.set_available_slots({known});

    SECTION("a mapping that resolves to a known slot is spelled from that slot's noun") {
        const std::string text =
            FilamentMappingModalTestAccess::slot_display_text(modal, mapping_to(1));
        CHECK(text.find("Gate 2") != std::string::npos);
        CHECK(text.find("Slot") == std::string::npos);
    }

    SECTION("a mapping that resolves to nothing falls back to the active printer's noun") {
        // Slot 2 is not among the available slots, so find_mapped_slot() misses
        // and the fallback is what renders. 1-based, like every other surface.
        const std::string text =
            FilamentMappingModalTestAccess::slot_display_text(modal, mapping_to(2));
        CHECK(text == "Gate 3");
    }
}
