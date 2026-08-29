// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file mapping_card_render_fixture.h
 * @brief Shared fixture for tests that render FilamentMappingCard for real.
 *
 * The pure-seam mapping tests need no LVGL; the render tests do, so they need
 * LVGLUITestFixture (XML component registration — lv_xml_create must build
 * filament_swatch) plus a started AmsBackendMock so update() passes its
 * availability/editable-mapping gates. Widget wiring mirrors MappingCardFixture
 * (test_filament_mapping_drain_reentrancy.cpp); AMS wiring mirrors
 * test_ams_available_slots.cpp.
 *
 * Lives in its own header because more than one test file renders the card.
 */

#include "ui_filament_mapping_card.h"
#include "ui_update_queue.h"

#include "ams_backend_mock.h"
#include "ams_state.h"
#include "lvgl_ui_test_fixture.h"

#include <memory>

struct MappingCardRenderFixture : LVGLUITestFixture {
    helix::ui::FilamentMappingCard card;
    lv_obj_t* card_widget = nullptr;
    lv_obj_t* rows = nullptr;
    lv_obj_t* warning = nullptr;
    AmsBackendMock* mock = nullptr;

    /// @param slot_count AMS backend slot count. Defaults to 4, matching every
    ///        existing caller, so this stays default-constructible; a derived
    ///        fixture can pass 1 to force the single-slot case the
    ///        multi-tool-vs-single-extruder visibility rule branches on.
    explicit MappingCardRenderFixture(int slot_count = 4) {
        // AmsState subjects must exist before backend events fire; the base
        // fixture already initialized PrinterState's (the init order
        // subject_initializer.cpp enforces in production).
        auto& ams = AmsState::instance();
        ams.init_subjects(false);

        auto owned = std::make_unique<AmsBackendMock>(slot_count);
        mock = owned.get();
        mock->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        mock->start();

        card_widget = lv_obj_create(test_screen());
        rows = lv_obj_create(card_widget);
        warning = lv_obj_create(card_widget);
        card.create(card_widget, rows, warning);
    }

    ~MappingCardRenderFixture() override {
        // Detach while LVGL still runs and before members die (base-class
        // teardown has not happened yet). Drain first so queued backend-event
        // syncs don't leak into the next test.
        helix::ui::UpdateQueue::instance().drain();
        if (mock) {
            mock->stop();
        }
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};
