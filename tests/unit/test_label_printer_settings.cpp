// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "label_printer_settings.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// LabelPrinterSettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "LabelPrinterSettingsManager default values after init",
                 "[label]") {
    Config::get_instance();
    LabelPrinterSettingsManager::instance().init_subjects();

    SECTION("printer_address defaults to empty") {
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_address().empty());
    }

    SECTION("printer_port defaults to 9100") {
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_port() == 9100);
    }

    SECTION("label_size_index defaults to 0") {
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_size_index() == 0);
    }

    SECTION("label_preset defaults to 0 (Standard)") {
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_preset() == 0);
    }

    SECTION("is_configured returns false with empty address") {
        REQUIRE(LabelPrinterSettingsManager::instance().is_configured() == false);
    }

    SECTION("printer_configured subject is 0 with empty address") {
        REQUIRE(lv_subject_get_int(
                    LabelPrinterSettingsManager::instance().subject_printer_configured()) == 0);
    }

    LabelPrinterSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "LabelPrinterSettingsManager set/get round trips", "[label]") {
    Config::get_instance();
    LabelPrinterSettingsManager::instance().init_subjects();

    SECTION("printer_address set/get") {
        LabelPrinterSettingsManager::instance().set_printer_address("192.168.1.50");
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_address() == "192.168.1.50");

        LabelPrinterSettingsManager::instance().set_printer_address("");
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_address().empty());
    }

    SECTION("printer_port set/get") {
        LabelPrinterSettingsManager::instance().set_printer_port(9200);
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_port() == 9200);

        LabelPrinterSettingsManager::instance().set_printer_port(9100);
        REQUIRE(LabelPrinterSettingsManager::instance().get_printer_port() == 9100);
    }

    SECTION("label_size_index set/get") {
        LabelPrinterSettingsManager::instance().set_label_size_index(2);
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_size_index() == 2);

        LabelPrinterSettingsManager::instance().set_label_size_index(0);
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_size_index() == 0);
    }

    SECTION("label_preset set/get") {
        LabelPrinterSettingsManager::instance().set_label_preset(1);
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_preset() == 1);

        LabelPrinterSettingsManager::instance().set_label_preset(2);
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_preset() == 2);

        LabelPrinterSettingsManager::instance().set_label_preset(0);
        REQUIRE(LabelPrinterSettingsManager::instance().get_label_preset() == 0);
    }

    LabelPrinterSettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "LabelPrinterSettingsManager configured subject tracks address",
                 "[label]") {
    Config::get_instance();
    LabelPrinterSettingsManager::instance().init_subjects();

    SECTION("setting address updates configured subject to 1") {
        LabelPrinterSettingsManager::instance().set_printer_address("192.168.1.50");
        REQUIRE(lv_subject_get_int(
                    LabelPrinterSettingsManager::instance().subject_printer_configured()) == 1);
        REQUIRE(LabelPrinterSettingsManager::instance().is_configured() == true);
    }

    SECTION("clearing address updates configured subject to 0") {
        LabelPrinterSettingsManager::instance().set_printer_address("192.168.1.50");
        LabelPrinterSettingsManager::instance().set_printer_address("");
        REQUIRE(lv_subject_get_int(
                    LabelPrinterSettingsManager::instance().subject_printer_configured()) == 0);
        REQUIRE(LabelPrinterSettingsManager::instance().is_configured() == false);
    }

    LabelPrinterSettingsManager::instance().deinit_subjects();
}
