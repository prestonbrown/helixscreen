// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_drying_capability.cpp
 * @brief printer_is_enclosed and printer_can_bed_dry, resolved from the printer
 *        database, the enclosure override, the heated bed and the Z travel
 *        (prestonbrown/helixscreen#1730).
 */

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using bed_drying::EnclosureStyle;

TEST_CASE("the printer database marks factory-enclosed printers", "[bed_drying][1730]") {
    CHECK(PrinterDetector::is_enclosed("Creality K1"));
    CHECK(PrinterDetector::is_enclosed("Snapmaker U1"));
    CHECK(PrinterDetector::is_enclosed("Voron 2.4"));
    CHECK(PrinterDetector::is_enclosed("FLSUN S1 / S1 Pro"));
    CHECK(PrinterDetector::is_enclosed("FLSUN T1 Pro"));
    CHECK_FALSE(PrinterDetector::is_enclosed("FLSUN V400"));
    CHECK_FALSE(PrinterDetector::is_enclosed("Creality Ender 3"));
    CHECK_FALSE(PrinterDetector::is_enclosed("FlashForge Adventurer 5X"));
    CHECK_FALSE(PrinterDetector::is_enclosed(""));
}

namespace {

struct CapabilityFixture : public LVGLTestFixture {
    PrinterState state;

    CapabilityFixture() {
        SettingsManager::instance().init_subjects();
        state.init_subjects(false);
        SettingsManager::instance().set_enclosure_style(EnclosureStyle::AUTO);
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"extruder", "heater_bed"});
        state.set_hardware(hw);
        z_travel(250);
    }

    ~CapabilityFixture() override {
        SettingsManager::instance().set_enclosure_style(EnclosureStyle::AUTO);
        ui::UpdateQueue::instance().drain_until_empty();
    }

    void z_travel(double z_max) {
        state.update_from_status(
            {{"toolhead",
              {{"axis_minimum", {0, 0, 0, 0}}, {"axis_maximum", {250, 250, z_max, 0}}}}});
        ui::UpdateQueue::instance().drain_until_empty();
    }

    int enclosed() {
        return lv_subject_get_int(state.capabilities_state().subject(Capability::IsEnclosed));
    }
    int can_dry() {
        return lv_subject_get_int(state.capabilities_state().subject(Capability::CanBedDry));
    }
};

} // namespace

TEST_CASE_METHOD(CapabilityFixture, "an enclosed printer with a heated bed can dry on the bed",
                 "[bed_drying][1730]") {
    state.set_printer_type_sync("Creality K1");
    CHECK(enclosed() == 1);
    CHECK(can_dry() == 1);

    SECTION("but not with too little Z travel") {
        z_travel(120);
        CHECK(can_dry() == 0);
    }
    SECTION("and the Open override hides it") {
        SettingsManager::instance().set_enclosure_style(EnclosureStyle::OPEN);
        state.refresh_bed_drying_capability(); // the setter refreshes the app's global state
        CHECK(enclosed() == 0);
        CHECK(can_dry() == 0);
    }
}

TEST_CASE_METHOD(CapabilityFixture, "an open printer never offers it unless marked enclosed",
                 "[bed_drying][1730]") {
    state.set_printer_type_sync("Creality Ender 3");
    CHECK(enclosed() == 0);
    CHECK(can_dry() == 0);

    SettingsManager::instance().set_enclosure_style(EnclosureStyle::ENCLOSED);
    state.refresh_bed_drying_capability(); // the setter refreshes the app's global state
    CHECK(enclosed() == 1);
    CHECK(can_dry() == 1);
}
