// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

// LVGLUITestFixture registers ALL XML components (via
// helix::register_xml_components()), including spaghetti_detection_modal.xml,
// so the modal can be created from XML inside the test.
#include "../lvgl_ui_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

// NOTE: SpaghettiDetectionModal is a one-shot modal owned by its ModalStack
// entry (Modal::show_owned, #1382): the entry frees it one tick after any
// close. These tests keep a raw view of the instance across show_owned() and
// stop touching it once process_lvgl() has drained the deferred free. A stack-
// allocated modal would outlive its own entry's free, so do NOT switch these
// to stack.

TEST_CASE_METHOD(LVGLUITestFixture, "SpaghettiDetectionModal shows message + invokes callbacks",
                 "[detection][modal][.ui_integration]") {
    int resumed = 0, aborted = 0, tuned = 0;

    // Resume path: the stack frees the modal after the close.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_resume([&] { ++resumed; });
        modal->set_on_abort([&] { ++aborted; });
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_resume_for_test(); // invokes callback then hide(); stack frees after
        REQUIRE(resumed == 1);
        REQUIRE(aborted == 0);
        process_lvgl(50); // drain the entry's deferred free; do not touch modal after
    }

    // Abort path.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_abort([&] { ++aborted; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_abort_for_test();
        REQUIRE(aborted == 1);
        REQUIRE(resumed == 1);
        process_lvgl(50);
    }

    // Tune does NOT hide; it only invokes the callback. The modal stays alive
    // until we hide() it explicitly.
    {
        auto owned = std::make_unique<SpaghettiDetectionModal>();
        auto* modal = owned.get();
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        modal->invoke_tune_for_test();
        REQUIRE(tuned == 1);
        REQUIRE(modal->is_visible()); // still visible: Tune does not hide
        modal->hide();                // the entry frees the instance a tick later
        process_lvgl(50);
    }
}
