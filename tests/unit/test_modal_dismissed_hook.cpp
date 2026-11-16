// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_modal_dismissed_hook.cpp
 * @brief Modal::on_dismissed() fires for every close no button answered, and
 *        ModalStack covers the navbar chrome exactly while a covering modal is
 *        visible.
 *
 * Run with: ./build/bin/helix-tests "[modal][dismiss]"
 */

#include "ui_effects.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

/// Counts dismissals; every button hook keeps Modal's default.
class CountingModal : public Modal {
  public:
    const char* get_name() const override {
        return "Counting";
    }
    const char* component_name() const override {
        return "modal_dialog";
    }
    using Modal::on_cancel;
    using Modal::on_ok;
    using Modal::on_quaternary;
    using Modal::on_quinary;
    using Modal::on_senary;
    using Modal::on_tertiary;

    int dismissed = 0;

  protected:
    void on_dismissed() override {
        ++dismissed;
    }
};

void settle(LVGLTestFixture& fx) {
    fx.process_lvgl(400);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a default button hook is an answer, not a dismissal",
                 "[modal][dismiss][on_dismissed]") {
    CountingModal modal;
    REQUIRE(modal.show(test_screen()));

    SECTION("ok") {
        modal.on_ok();
    }
    SECTION("cancel") {
        modal.on_cancel();
    }
    SECTION("tertiary") {
        modal.on_tertiary();
    }
    SECTION("quaternary") {
        modal.on_quaternary();
    }
    SECTION("quinary") {
        modal.on_quinary();
    }
    SECTION("senary") {
        modal.on_senary();
    }
    CHECK_FALSE(modal.is_visible());
    CHECK(modal.dismissed == 0);
    settle(*this);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a close no button answered reports one dismissal",
                 "[modal][dismiss][on_dismissed]") {
    CountingModal modal;
    REQUIRE(modal.show(test_screen()));

    SECTION("backdrop tap") {
        lv_obj_send_event(modal.backdrop(), LV_EVENT_CLICKED, nullptr);
    }
    SECTION("programmatic hide") {
        modal.hide();
    }
    SECTION("hot reload") {
        Modal::rebuild_top();
    }
    CHECK_FALSE(modal.is_visible());
    modal.hide();
    CHECK(modal.dismissed == 1);
    settle(*this);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the navbar chrome is covered exactly while a modal is open",
                 "[modal][dismiss][modal_covers_navbar]") {
    REQUIRE_FALSE(helix::ui::chrome_covered());

    SECTION("uncovered the moment a hide starts its exit animation") {
        CountingModal modal;
        REQUIRE(modal.show(test_screen()));
        REQUIRE(helix::ui::chrome_covered());
        lv_obj_t* backdrop = modal.backdrop();
        modal.hide();
        REQUIRE(ModalStack::instance().is_exiting(backdrop)); // the backdrop is still there
        CHECK_FALSE(helix::ui::chrome_covered());
        settle(*this);
    }

    SECTION("uncovered when a visible modal is destroyed without a hide") {
        auto modal = std::make_unique<CountingModal>();
        REQUIRE(modal->show(test_screen()));
        REQUIRE(helix::ui::chrome_covered());
        modal.reset();
        CHECK_FALSE(helix::ui::chrome_covered());
    }

    SECTION("uncovered when teardown clears the stack") {
        lv_obj_t* dialog =
            helix::ui::modal_confirm("Title", "Message", ModalSeverity::Info, "OK", [] {});
        REQUIRE(dialog != nullptr);
        REQUIRE(helix::ui::chrome_covered());
        ModalStack::instance().clear();
        CHECK_FALSE(helix::ui::chrome_covered());
    }
}
