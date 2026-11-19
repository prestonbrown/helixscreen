// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_change_request.cpp
 * @brief The one decision every tool-change surface asks, and the dispatch
 *        that carries it out.
 */

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "spool_latch_gate.h"
#include "tool_change_request.h"
#include "tool_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::decide_tool_change;
using helix::ui::ToolChangeInputs;
using helix::ui::ToolChangeVerdict;

// ============================================================================
// decide_tool_change(): pure
// ============================================================================

namespace {
ToolChangeInputs idle_inputs() {
    ToolChangeInputs in;
    in.lifecycle = PrintState::Idle;
    in.confirm_enabled = true;
    return in;
}
} // namespace

TEST_CASE("decide_tool_change: idle with the setting on asks first", "[tool_change][decision]") {
    CHECK(decide_tool_change(idle_inputs()).verdict == ToolChangeVerdict::ConfirmSetting);
}

TEST_CASE("decide_tool_change: idle with the setting off changes now", "[tool_change][decision]") {
    auto in = idle_inputs();
    in.confirm_enabled = false;
    const auto d = decide_tool_change(in);
    CHECK(d.verdict == ToolChangeVerdict::Immediate);
    CHECK(d.dispatches());
    CHECK(d.refusal.success());
}

TEST_CASE("decide_tool_change: the mounted tool is never changed to", "[tool_change][decision]") {
    auto in = idle_inputs();
    in.already_active = true;
    // Even with every refusal also true: there is nothing to refuse.
    in.spools_on_bed = true;
    in.lifecycle = PrintState::Printing;
    in.backend_action = AmsAction::LOADING;
    const auto d = decide_tool_change(in);
    CHECK(d.verdict == ToolChangeVerdict::AlreadyActive);
    CHECK_FALSE(d.dispatches());
}

TEST_CASE("decide_tool_change: spools on the bed refuse with the latch copy",
          "[tool_change][decision]") {
    auto in = idle_inputs();
    in.spools_on_bed = true;
    in.confirm_enabled = false;
    const auto d = decide_tool_change(in);
    CHECK(d.verdict == ToolChangeVerdict::RefuseSpoolsOnBed);
    CHECK_FALSE(d.dispatches());
    CHECK(d.refusal.result == AmsResult::WRONG_STATE);
    CHECK(d.refusal.user_msg == lv_tr(spool_latch_message()));
}

TEST_CASE("decide_tool_change: Printing and Preparing refuse whatever the setting",
          "[tool_change][decision]") {
    for (PrintState s : {PrintState::Printing, PrintState::Preparing}) {
        for (bool confirm : {false, true}) {
            for (bool self_homes : {false, true}) {
                CAPTURE(static_cast<int>(s), confirm, self_homes);
                auto in = idle_inputs();
                in.lifecycle = s;
                in.confirm_enabled = confirm;
                in.filament_ops_self_home = self_homes;
                const auto d = decide_tool_change(in);
                CHECK(d.verdict == ToolChangeVerdict::RefusePrint);
                CHECK_FALSE(d.dispatches());
                CHECK_FALSE(d.refusal.success());
            }
        }
    }
}

TEST_CASE("decide_tool_change: Paused confirms even with the setting off",
          "[tool_change][decision][paused]") {
    for (bool confirm : {false, true}) {
        CAPTURE(confirm);
        auto in = idle_inputs();
        in.lifecycle = PrintState::Paused;
        in.confirm_enabled = confirm;
        const auto d = decide_tool_change(in);
        CHECK(d.verdict == ToolChangeVerdict::ConfirmPaused);
        CHECK(d.dispatches());
    }
}

TEST_CASE("decide_tool_change: Paused on a self-homing backend refuses",
          "[tool_change][decision][paused]") {
    auto in = idle_inputs();
    in.lifecycle = PrintState::Paused;
    in.filament_ops_self_home = true;
    const auto d = decide_tool_change(in);
    CHECK(d.verdict == ToolChangeVerdict::RefusePrint);
    CHECK(d.refusal.user_msg == "Can't move filament while the print is paused");
}

TEST_CASE("decide_tool_change: a busy filament system ignores the request",
          "[tool_change][decision]") {
    for (AmsAction a : {AmsAction::LOADING, AmsAction::UNLOADING, AmsAction::SELECTING,
                        AmsAction::CHECKING, AmsAction::HEATING}) {
        for (bool confirm : {false, true}) {
            CAPTURE(static_cast<int>(a), confirm);
            auto in = idle_inputs();
            in.backend_action = a;
            in.confirm_enabled = confirm;
            const auto d = decide_tool_change(in);
            CHECK(d.verdict == ToolChangeVerdict::Busy);
            CHECK_FALSE(d.dispatches());
            CHECK(d.refusal.success());
        }
    }
}

TEST_CASE("decide_tool_change: a filament system paused for attention refuses with a reason",
          "[tool_change][decision]") {
    auto in = idle_inputs();
    in.backend_action = AmsAction::PAUSED;
    const auto d = decide_tool_change(in);
    CHECK(d.verdict == ToolChangeVerdict::RefuseNeedsAttention);
    CHECK_FALSE(d.dispatches());
    CHECK(d.refusal.user_msg == "The filament system needs attention");
}

TEST_CASE("decide_tool_change: idle and error states take the change", "[tool_change][decision]") {
    for (AmsAction a : {AmsAction::IDLE, AmsAction::ERROR}) {
        auto in = idle_inputs();
        in.backend_action = a;
        in.confirm_enabled = false;
        CHECK(decide_tool_change(in).verdict == ToolChangeVerdict::Immediate);
    }
}

TEST_CASE("decide_tool_change: a refusal outranks busy", "[tool_change][decision]") {
    auto in = idle_inputs();
    in.backend_action = AmsAction::LOADING;
    in.lifecycle = PrintState::Printing;
    CHECK(decide_tool_change(in).verdict == ToolChangeVerdict::RefusePrint);
    in.lifecycle = PrintState::Idle;
    in.spools_on_bed = true;
    CHECK(decide_tool_change(in).verdict == ToolChangeVerdict::RefuseSpoolsOnBed);
}

// ============================================================================
// offer_tool_change(): the dispatch every surface shares
// ============================================================================

namespace {

struct OfferFixture : public LVGLUITestFixture {
    OfferFixture() {
        test::set_wire_state(state(), PrintJobState::STANDBY);
    }

    void set_confirm(bool on) {
        SafetySettingsManager::instance().set_tool_change_require_confirmation(on);
    }

    ToolChangeVerdict offer(int tool) {
        ui::ToolChangeHooks hooks;
        hooks.on_success = [this] { ++successes; };
        hooks.on_failed = [this] { ++failures; };
        hooks.on_declined = [this] { ++declines; };
        hooks.resolve_api = [this]() -> IMoonrakerAPI* {
            ++api_lookups;
            return nullptr;
        };
        return ui::offer_tool_change(state(), tool, std::move(hooks));
    }

    ToolChangerRig rig{3};
    int api_lookups = 0;
    int successes = 0;
    int failures = 0;
    int declines = 0;
};

} // namespace

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: setting off changes tool at once",
                 "[tool_change][offer]") {
    set_confirm(false);
    CHECK(offer(1) == ToolChangeVerdict::Immediate);
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 1);
    CHECK(successes == 1);
    CHECK(declines == 0);
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: setting on asks, naming the tool",
                 "[tool_change][offer]") {
    set_confirm(true);
    CHECK(offer(1) == ToolChangeVerdict::ConfirmSetting);
    CHECK(rig.backend->change_tool_calls == 0);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* message = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)).find("Tool 2 (T1)") != std::string::npos);

    SECTION("confirming changes tool") {
        test::answer_top_modal("btn_primary");
        CHECK(rig.backend->change_tool_calls == 1);
        CHECK(rig.backend->last_tool == 1);
        CHECK(successes == 1);
        CHECK(declines == 0);
    }
    SECTION("cancelling changes nothing and reports the decline") {
        test::answer_top_modal("btn_secondary");
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(declines == 1);
    }
    SECTION("a backdrop dismissal reports the decline too") {
        test::dismiss_top_modal();
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(declines == 1);
    }
    SECTION("a close no button or dismissal answers reports the decline too") {
        Modal::hide(ModalStack::instance().top_dialog());
        process_lvgl(500);
        ui::UpdateQueue::instance().drain();
        REQUIRE(ModalStack::instance().stack_empty());
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(declines == 1);
    }
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: the API is looked up when the change is sent",
                 "[tool_change][offer]") {
    set_confirm(true);
    offer(1);
    CHECK(api_lookups == 0);
    test::answer_top_modal("btn_primary");
    CHECK(api_lookups == 1);
    // Answered: closing the dialog afterwards reports no decline.
    process_lvgl(500);
    CHECK(declines == 0);
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: a failed change toasts and reports it",
                 "[tool_change][offer]") {
    set_confirm(false);
    rig.backend->tool_change_should_fail = true;
    offer(2);
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(failures == 1);
    REQUIRE(rig.errors.size() == 1);
    CHECK(rig.errors[0].find("Tool change did not complete") != std::string::npos);
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: a printing refusal toasts without a dialog",
                 "[tool_change][offer]") {
    set_confirm(true);
    test::set_wire_state(state(), PrintJobState::PRINTING);
    CHECK(offer(1) == ToolChangeVerdict::RefusePrint);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(declines == 1);
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("Cannot run filament operation while printing") !=
          std::string::npos);
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: spools on the bed refuse without a dialog",
                 "[tool_change][offer]") {
    set_confirm(true);
    state().print_state().set_spool_latch(true);
    CHECK(offer(1) == ToolChangeVerdict::RefuseSpoolsOnBed);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(rig.warnings.size() == 1);
    state().print_state().set_spool_latch(false);
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: the mounted tool is a silent no-op",
                 "[tool_change][offer]") {
    set_confirm(true);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    CHECK(offer(0) == ToolChangeVerdict::AlreadyActive);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(ModalStack::instance().stack_empty());
    CHECK(rig.warnings.empty());
}

TEST_CASE_METHOD(OfferFixture, "offer_tool_change: paused shows the one paused dialog",
                 "[tool_change][offer][paused]") {
    set_confirm(true);
    test::set_wire_state(state(), PrintJobState::PAUSED);
    CHECK(offer(1) == ToolChangeVerdict::ConfirmPaused);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* message = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)).find("The print is paused") != std::string::npos);
    test::answer_top_modal("btn_primary");
    CHECK(rig.backend->change_tool_calls == 1);
    // Exactly one dialog was raised: nothing is left stacked after the answer.
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(OfferFixture,
                 "offer_tool_change: a print started while the dialog is open refuses",
                 "[tool_change][offer][recheck]") {
    set_confirm(true);
    REQUIRE(offer(1) == ToolChangeVerdict::ConfirmSetting);
    test::set_wire_state(state(), PrintJobState::PRINTING);
    test::answer_top_modal("btn_primary");
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(successes == 0);
    CHECK(declines == 1);
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("Cannot run filament operation while printing") !=
          std::string::npos);
}

TEST_CASE_METHOD(OfferFixture,
                 "offer_tool_change: a filament system paused while the dialog is open refuses",
                 "[tool_change][offer][recheck]") {
    set_confirm(true);
    REQUIRE(offer(1) == ToolChangeVerdict::ConfirmSetting);
    rig.backend->report_action = AmsAction::PAUSED;
    test::answer_top_modal("btn_primary");
    rig.backend->report_action = AmsAction::IDLE;
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(declines == 1);
    REQUIRE(rig.warnings.size() == 1);
    CHECK(rig.warnings[0].find("The filament system needs attention") != std::string::npos);
}

TEST_CASE_METHOD(OfferFixture,
                 "offer_tool_change: a filament system gone busy while the dialog is open is "
                 "silently declined",
                 "[tool_change][offer][recheck]") {
    set_confirm(true);
    REQUIRE(offer(1) == ToolChangeVerdict::ConfirmSetting);
    rig.backend->report_action = AmsAction::LOADING;
    test::answer_top_modal("btn_primary");
    rig.backend->report_action = AmsAction::IDLE;
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(declines == 1);
    CHECK(rig.warnings.empty());
}

TEST_CASE_METHOD(OfferFixture,
                 "offer_tool_change: a print paused while the dialog is open asks again with the "
                 "paused warning",
                 "[tool_change][offer][recheck][paused]") {
    set_confirm(true);
    REQUIRE(offer(1) == ToolChangeVerdict::ConfirmSetting);
    test::set_wire_state(state(), PrintJobState::PAUSED);
    test::answer_top_modal("btn_primary");
    process_lvgl(500);
    ui::UpdateQueue::instance().drain();
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(declines == 0);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* message = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)).find("The print is paused") != std::string::npos);

    SECTION("confirming the paused warning changes tool") {
        test::answer_top_modal("btn_primary");
        CHECK(rig.backend->change_tool_calls == 1);
        CHECK(successes == 1);
        CHECK(declines == 0);
    }
    SECTION("cancelling it reports the decline once") {
        test::answer_top_modal("btn_secondary");
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(declines == 1);
    }
}
