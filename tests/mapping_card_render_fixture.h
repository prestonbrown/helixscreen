// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file mapping_card_render_fixture.h
 * @brief Shared fixture for tests that render FilamentMappingCard for real.
 *
 * The pure-seam mapping tests need no LVGL; the render tests do, so they need
 * LVGLUITestFixture (XML component registration: `rows` holds a real
 * filament_chip_row, which repeats filament_swatch over the card's subjects)
 * plus a started AmsBackendMock so update() passes its availability gate. The
 * AMS is a ScopedMockAms.
 *
 * Lives in its own header because more than one test file renders the card.
 */

#include "ui_filament_mapping_card.h"

#include "ams_backend_mock.h"
#include "core/update_queue.h"
#include "lvgl_ui_test_fixture.h"
#include "test_helpers/scoped_mock_ams.h"

#include <string>

struct MappingCardRenderFixture : LVGLUITestFixture {
    helix::ui::FilamentMappingCard card;
    /// Declared after the card, so the AMS is removed before the card goes.
    helix::test::ScopedMockAms ams;
    helix::AmsBackendMock* mock = nullptr;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;

    /// @param slot_count AMS backend slot count. Defaults to 4, matching every
    ///        existing caller, so this stays default-constructible; a derived
    ///        fixture can pass 1 to force the single-slot case the
    ///        multi-tool-vs-single-extruder visibility rule branches on.
    explicit MappingCardRenderFixture(int slot_count = 4) : ams(slot_count), mock(ams.backend) {
        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        helix::ui::FilamentMappingCard::init_subjects();
        lv_xml_create(rows, "filament_chip_row", nullptr);
        card.create(card_widget, rows, warning);
    }

    /// Chips the card publishes (filament_chip_count).
    static int chip_count() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_chip_count"));
    }

    /// filament_chip_<i><suffix>, e.g. chip_subject(0, "_severity").
    static lv_subject_t* chip_subject(int i, const char* suffix) {
        const std::string name = "filament_chip_" + std::to_string(i) + suffix;
        return lv_xml_get_subject(nullptr, name.c_str());
    }
    static int chip_int(int i, const char* suffix) {
        return lv_subject_get_int(chip_subject(i, suffix));
    }
    static std::string chip_string(int i, const char* suffix) {
        return lv_subject_get_string(chip_subject(i, suffix));
    }

    /// Chip @p i's widget, built by the repeat; nullptr when not drawn.
    lv_obj_t* chip(int i) const {
        const std::string name = "filament_chip_" + std::to_string(i);
        return lv_obj_find_by_name(rows, name.c_str());
    }
};
