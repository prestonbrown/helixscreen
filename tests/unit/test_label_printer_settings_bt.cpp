// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "label_printer_settings.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Label printer settings: the bluetooth fields persist", "[label][settings]") {
    LVGLTestFixture fixture;
    auto& settings = LabelPrinterSettingsManager::instance();
    settings.init_subjects();

    settings.set_printer_type("bluetooth");
    CHECK(Config::get_instance()->get<std::string>("/label_printer/type", "") == "bluetooth");
    settings.set_bt_address("AA:BB:CC:DD:EE:FF");
    CHECK(settings.get_bt_address() == "AA:BB:CC:DD:EE:FF");
    for (const char* transport : {"spp", "ble"}) {
        settings.set_bt_transport(transport);
        CHECK(settings.get_bt_transport() == transport);
    }
    for (int channel : {0, 7}) {
        settings.set_bt_channel(channel);
        CHECK(settings.get_bt_channel() == channel);
    }
    settings.set_bt_channel(0); // reset for subsequent tests
}

TEST_CASE("Label printer settings - BT configured check", "[label][settings]") {
    LVGLTestFixture fixture;

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.init_subjects();

    settings.set_printer_type("bluetooth");

    settings.set_bt_address("");
    REQUIRE_FALSE(settings.is_configured());

    settings.set_bt_address("AA:BB:CC:DD:EE:FF");
    REQUIRE(settings.is_configured());
}
