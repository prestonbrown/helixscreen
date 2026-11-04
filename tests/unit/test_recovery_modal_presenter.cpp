// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/modal_answer.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "gcode_response_routing.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "safety_settings_manager.h"
#include "theme_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

using TA = RecoveryModalPresenterTestAccess;

helix::ErrorEvent make_afc_jam() {
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "tool_end jam detected";
    // Mirrors AmsBackendAfc::build_recovery_actions()'s toolhead-loaded branch:
    // Resume + Unload need a hot nozzle, AFC_RESET does not. Re-verified against
    // src/printer/ams_backend_afc.cpp:659 — verbatim match on all three actions.
    // TEST_MIRROR_OK: this is the presenter's INPUT, not its logic. The unit
    //                 under test is RecoveryModalPresenter; build_recovery_actions()
    //                 is private, needs mutex_ held plus tool_start_sensor_ /
    //                 current_lane_name_ state, and calling it would pull the
    //                 whole AFC backend into a modal test.
    e.recovery_actions = {
        {"Resume", "RESUME", "afc::resume", "primary", /*needs_hot_nozzle=*/true},
        {"Unload", "TOOL_UNLOAD", "afc::tool_unload", "", /*needs_hot_nozzle=*/true},
        {"Recover", "AFC_RESET", "afc::reset", "danger", /*needs_hot_nozzle=*/false},
    };
    return e;
}

/// Counts how many times @p needle appears across the recorded gcode scripts.
/// A substring match, because an action's gcode may be one line of a larger
/// script; the send layer itself adds nothing (test_gcode_verbatim.cpp).
int gcode_count(const MoonrakerClientMock& c, const std::string& needle) {
    int n = 0;
    for (const auto& s : c.gcode_script_history()) {
        if (s.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

/// Presenter wired to a recording client so tests assert on the G-code that
/// actually reached the send chokepoint, not on internal flags.
class RecoveryPreheatFixture : public LVGLUITestFixture {
  public:
    RecoveryPreheatFixture()
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24),
          api_(mock_client_, get_printer_state()) {
        auto& st = get_printer_state();
        // execute_gcode() refuses everything while Klipper reads SHUTDOWN, which
        // is where the subjects initialize.
        st.set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(st.print_state().get_print_state_enum_subject(),
                           static_cast<int>(helix::PrintJobState::STANDBY));

        SafetyLimits limits; // min_extrude_temp_celsius = 170 (Klipper default)
        api_.set_safety_limits(limits);

        // No AMS backend and no external spool: the preheat target resolves to
        // kDefaultLoadPreheatTemp, so the tests know what they are waiting for.
        helix::AmsState::instance().set_backend(nullptr);
        prev_cold_extrude_ = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);

        set_nozzle_c(25);
        presenter_ = std::make_unique<helix::ui::RecoveryModalPresenter>(&api_);
    }

    ~RecoveryPreheatFixture() override {
        presenter_.reset();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(prev_cold_extrude_);
    }

    void set_nozzle_c(int celsius) {
        lv_subject_set_int(
            get_printer_state().temperature_state().get_active_extruder_temp_subject(),
            celsius * 10);
    }

    helix::ui::RecoveryModalPresenter& presenter() {
        return *presenter_;
    }

    MoonrakerClientMock mock_client_;
    MoonrakerAPI api_;
    std::unique_ptr<helix::ui::RecoveryModalPresenter> presenter_;
    bool prev_cold_extrude_ = false;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "RecoveryModalPresenter shows and dismisses",
                 "[error-center][recovery-presenter]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "tool_end jam detected";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    // Presenting the same detail again must not stack a second modal.
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    presenter.dismiss();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());
}

TEST_CASE_METHOD(LVGLUITestFixture, "RecoveryModalPresenter dedup allows re-show after dismiss",
                 "[error-center][recovery-presenter]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "jam detail";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};

    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());

    presenter.dismiss();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());

    // After dismiss, re-presenting the same detail must show again.
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "RecoveryModalPresenter replaces a poorer action set with the same detail",
                 "[error-center][recovery-presenter][1171]") {
    // One AFC fault reaches the presenter twice with byte-identical text:
    // AFC_logger.error() sends "!! {msg}" and appends that same msg to the
    // message queue, so the `!!` line-arrival event and the later status-driven
    // current_error() event share a detail string. Only the second carries
    // AFC's recovery set, and the `!!` always lands first — AFC emits it before
    // pausing. Deduping on detail alone would pin the poorer set forever.
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    const std::string shared_detail = "Lane 1 jam detected";

    helix::ErrorEvent generic;
    generic.severity = helix::ErrorSeverity::CRITICAL;
    generic.detail = shared_detail;
    // Empty gcode is the dismiss spelling — matches what error_classify.cpp
    // now emits, rather than the Klipper-comment workaround it used to need
    // (#1172).
    generic.recovery_actions = {{"Resume", "RESUME", "error_classify::resume", ""},
                                {"OK", "", "error_classify::dismiss", ""}};

    presenter.present(generic);
    process_lvgl(20);
    REQUIRE(presenter.is_visible());
    REQUIRE(TA::active_actions(presenter).size() == 2);

    helix::ErrorEvent afc = make_afc_jam();
    afc.detail = shared_detail; // identical text, richer affordances

    presenter.present(afc);
    process_lvgl(20);
    CHECK(presenter.is_visible());

    const auto& active = TA::active_actions(presenter);
    REQUIRE(active.size() == 3);
    CHECK(active[1].gcode == "TOOL_UNLOAD");
    CHECK(active[2].gcode == "AFC_RESET");

    SECTION("but a genuine repeat of the same set still dedups") {
        presenter.present(afc);
        process_lvgl(20);
        CHECK(presenter.is_visible());
        CHECK(TA::active_actions(presenter).size() == 3);
    }
}

// ============================================================================
// Cold-nozzle gate on filament-moving recovery actions
//
// A recovery tapped into a cold nozzle fails the same way the operation that
// raised the error did: three things can zero the heater between the fault and
// the tap (post-op cooldown, TURN_OFF_HEATERS on print ERROR, idle_timeout).
// ============================================================================

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A needs_hot_nozzle recovery preheats instead of firing into a cold nozzle",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    set_nozzle_c(25); // well under min_extrude_temp
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);

    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 0);
    CHECK(TA::preheating(presenter()));
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A needs_hot_nozzle recovery dispatches immediately when the nozzle is hot",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    set_nozzle_c(230); // above min_extrude_temp
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);

    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 1);
    CHECK_FALSE(TA::preheating(presenter()));
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A nozzle one degree under a fractional extrusion minimum is still cold",
                 "[error-center][recovery-presenter][preheat]") {
    SafetyLimits limits;
    limits.min_extrude_temp_celsius = 180.5;
    api_.set_safety_limits(limits);
    presenter().present(make_afc_jam());
    process_lvgl(20);

    // Klipper refuses to extrude at 180 against a 180.5 minimum.
    set_nozzle_c(180);
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);

    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 0);
    CHECK(TA::preheating(presenter()));
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A non-needs_hot_nozzle recovery dispatches immediately even when cold",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    set_nozzle_c(25);
    TA::tap(presenter(), "AFC_RESET"); // lane re-prep; nothing goes through the nozzle
    process_lvgl(20);

    CHECK(gcode_count(mock_client_, "AFC_RESET") == 1);
    CHECK_FALSE(TA::preheating(presenter()));
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "Reaching target dispatches the deferred recovery once",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    set_nozzle_c(25);
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);
    REQUIRE(gcode_count(mock_client_, "TOOL_UNLOAD") == 0);
    REQUIRE(TA::preheating(presenter()));

    // Comfortably past any target the resolver can pick (latch, material temp,
    // or kDefaultLoadPreheatTemp).
    set_nozzle_c(300);
    process_lvgl(600); // > one poll period

    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 1);
    CHECK_FALSE(TA::preheating(presenter()));

    // The poll timer must be gone, not merely idle: keep ticking and the count
    // must not climb.
    process_lvgl(1200);
    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 1);
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A preheat that never reaches target gives up without dispatching",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    // The give-up must be visible to the user, not just to the log.
    std::vector<std::string> errors;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& m) { errors.push_back(m); });

    set_nozzle_c(25);
    TA::set_preheat_budget_ms(presenter(), 300);
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);
    REQUIRE(TA::preheating(presenter()));

    // Nozzle stays cold (no heater in a unit test) past the budget.
    process_lvgl(1500);

    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 0);
    CHECK_FALSE(TA::preheating(presenter())); // bounded, not an indefinite wait
    CHECK(errors.size() == 1);

    helix::ui::set_test_notification_error_hook(nullptr);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A second tap replaces the pending recovery",
                 "[error-center][recovery-presenter][preheat]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    set_nozzle_c(25);
    TA::tap(presenter(), "TOOL_UNLOAD");
    process_lvgl(20);
    presenter().present(make_afc_jam());
    TA::tap(presenter(), "RESUME");
    process_lvgl(20);

    set_nozzle_c(300);
    process_lvgl(600);

    CHECK(gcode_count(mock_client_, "RESUME") == 1);
    CHECK(gcode_count(mock_client_, "TOOL_UNLOAD") == 0);
}

// ============================================================================
// Recovery actions that ask before sending
// ============================================================================

namespace {

helix::ErrorEvent make_seat_fault() {
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::GENERIC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Tool half seated";
    e.detail = "A tool may be half seated";
    helix::RecoveryAction remove{"Remove tool", "MANUAL_TOOL_REMOVE", "t::remove", "danger"};
    remove.confirm_message = "Hold it";
    helix::RecoveryAction seat{"Tool is seated", "MANUAL_TOOL_SEAT TOOL=2", "t::seat", "primary"};
    e.recovery_actions = {seat, remove};
    return e;
}

} // namespace

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A recovery with confirm_message sends only after the confirm",
                 "[error-center][recovery-presenter]") {
    presenter().present(make_seat_fault());
    process_lvgl(20);

    TA::tap(presenter(), "MANUAL_TOOL_REMOVE");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 0);
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    CHECK_THAT(helix::test::top_modal_text(), Catch::Matchers::ContainsSubstring("Hold it"));

    helix::test::answer_top_modal("btn_primary");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 1);
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "Cancelling a recovery confirm sends nothing and does not re-present",
                 "[error-center][recovery-presenter]") {
    const auto fault = make_seat_fault();
    presenter().present(fault);
    process_lvgl(20);

    TA::tap(presenter(), "MANUAL_TOOL_REMOVE");
    process_lvgl(20);
    helix::test::answer_top_modal("btn_secondary");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 0);

    TA::tap(presenter(), "MANUAL_TOOL_REMOVE");
    process_lvgl(20);
    helix::test::dismiss_top_modal();
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 0);

    CHECK(TA::handled_detail(presenter()) == fault.detail);
    TA::user_dismiss(presenter());
    process_lvgl(20);
    presenter().present(fault);
    process_lvgl(20);
    CHECK_FALSE(presenter().is_visible());
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A recovery without confirm_message still sends on the tap",
                 "[error-center][recovery-presenter]") {
    presenter().present(make_seat_fault());
    process_lvgl(20);
    const auto* before = ModalStack::instance().top_dialog();

    TA::tap(presenter(), "MANUAL_TOOL_SEAT TOOL=2");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_SEAT TOOL=2") == 1);
    const auto* after = ModalStack::instance().top_dialog();
    CHECK((after == nullptr || after == before));
}

namespace {

/// The background colour @p role paints, for telling which variant a button took.
lv_color_t style_bg(StyleRole role) {
    lv_style_value_t v{};
    REQUIRE(lv_style_get_prop(ThemeManager::instance().get_style(role), LV_STYLE_BG_COLOR, &v) ==
            LV_STYLE_RES_FOUND);
    return v.color;
}

} // namespace

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A danger recovery's confirm keeps the danger style on its button",
                 "[error-center][recovery-presenter]") {
    auto fault = make_seat_fault();
    fault.recovery_actions[0].confirm_message = "Hold it on";
    presenter().present(fault);
    process_lvgl(20);

    TA::tap(presenter(), "MANUAL_TOOL_REMOVE");
    process_lvgl(20);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(btn != nullptr);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(btn, LV_PART_MAIN),
                      style_bg(StyleRole::ButtonDanger)));
    helix::test::answer_top_modal("btn_secondary");
    process_lvgl(20);

    TA::tap(presenter(), "MANUAL_TOOL_SEAT TOOL=2");
    process_lvgl(20);
    dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    btn = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(btn != nullptr);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(btn, LV_PART_MAIN),
                      style_bg(StyleRole::ButtonPrimary)));
    helix::test::answer_top_modal("btn_secondary");
    process_lvgl(20);
}

// ============================================================================
// The fault renders as the standard dialog with a modal_button_row
// ============================================================================

namespace {

lv_obj_t* fault_dialog() {
    return ModalStack::instance().dialog_for_component("recovery_fault_modal");
}

/// The button named @p name on the fault dialog, which must exist.
lv_obj_t* fault_button(const char* name) {
    lv_obj_t* dialog = fault_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
    REQUIRE(btn != nullptr);
    return btn;
}

bool shown(lv_obj_t* obj) {
    return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

bool painted(lv_obj_t* btn, StyleRole role) {
    return lv_color_eq(lv_obj_get_style_bg_color(btn, LV_PART_MAIN), style_bg(role));
}

/// Tap @p name on the fault dialog the way a finger does.
void press(const char* name) {
    lv_obj_send_event(fault_button(name), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A filament fault shows the standard dialog, one styled button per action",
                 "[error-center][recovery-presenter][1838]") {
    presenter().present(make_afc_jam());
    process_lvgl(20);

    REQUIRE(fault_dialog() != nullptr);
    CHECK(ModalStack::instance().dialog_for_component("action_prompt_modal") == nullptr);

    lv_obj_t* title = lv_obj_find_by_name(fault_dialog(), helix::ui::kModalTitleWidgetName);
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "Toolhead jam");
    CHECK_THAT(helix::test::top_modal_text(),
               Catch::Matchers::ContainsSubstring("tool_end jam detected"));

    // First action at the right in the primary slot, the rest leftward.
    lv_obj_t* resume = fault_button("btn_primary");
    lv_obj_t* unload = fault_button("btn_secondary");
    lv_obj_t* recover = fault_button("btn_tertiary");
    CHECK(helix::test::subtree_text(resume).find("Resume") != std::string::npos);
    CHECK(helix::test::subtree_text(unload).find("Unload") != std::string::npos);
    CHECK(helix::test::subtree_text(recover).find("Recover") != std::string::npos);
    CHECK(shown(resume));
    CHECK(shown(unload));
    CHECK(shown(recover));
    CHECK_FALSE(shown(fault_button("btn_quaternary")));

    CHECK(painted(resume, StyleRole::ButtonPrimary));
    CHECK(painted(unload, StyleRole::ButtonSecondary));
    CHECK(painted(recover, StyleRole::ButtonDanger));

    presenter().dismiss();
    process_lvgl(20);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A fault button sends its own gcode and closes",
                 "[error-center][recovery-presenter][1838]") {
    auto fault = make_afc_jam();
    presenter().present(fault);
    process_lvgl(20);

    press("btn_tertiary");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "AFC_RESET") == 1);
    CHECK(gcode_count(mock_client_, "RESUME") == 0);
    CHECK_FALSE(presenter().is_visible());
    // Answered: the same fault re-notified stays down.
    presenter().present(fault);
    process_lvgl(20);
    CHECK_FALSE(presenter().is_visible());
}

TEST_CASE_METHOD(RecoveryPreheatFixture,
                 "A requires_homed recovery sends only while the toolhead is still homed",
                 "[error-center][recovery-presenter][ad5x]") {
    helix::ErrorEvent fault = make_afc_jam();
    helix::RecoveryAction load{"Load", "INSERT_PRUTOK_IFS PRUTOK=1", "ifs::load", "primary"};
    load.requires_homed = true;
    fault.recovery_actions = {load, {"Resume", "RESUME", "t::resume", ""}};

    SECTION("homed: sent") {
        get_printer_state().update_from_status({{"toolhead", {{"homed_axes", "xyz"}}}});
        presenter().present(fault);
        process_lvgl(20);
        TA::tap(presenter(), "INSERT_PRUTOK_IFS PRUTOK=1");
        process_lvgl(20);
        CHECK(gcode_count(mock_client_, "INSERT_PRUTOK_IFS") == 1);
    }
    SECTION("homing lost since the dialog was built: refused") {
        get_printer_state().update_from_status({{"toolhead", {{"homed_axes", ""}}}});
        presenter().present(fault);
        process_lvgl(20);
        TA::tap(presenter(), "INSERT_PRUTOK_IFS PRUTOK=1");
        process_lvgl(20);
        CHECK(gcode_count(mock_client_, "INSERT_PRUTOK_IFS") == 0);
    }
    get_printer_state().update_from_status({{"toolhead", {{"homed_axes", ""}}}});
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A dismiss-only fault button closes and sends nothing",
                 "[error-center][recovery-presenter][1838]") {
    helix::ErrorEvent fault = make_afc_jam();
    fault.recovery_actions = {{"OK", "", "t::dismiss", ""}};
    presenter().present(fault);
    process_lvgl(20);
    CHECK_FALSE(shown(fault_button("btn_secondary")));

    press("btn_primary");
    process_lvgl(20);
    CHECK(mock_client_.gcode_script_history().empty());
    CHECK_FALSE(presenter().is_visible());
    CHECK(TA::handled_detail(presenter()) == fault.detail);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "ESC closes the fault without running an action",
                 "[error-center][recovery-presenter][1838]") {
    // The secondary slot holds an action that would send at once if ESC ran it.
    auto fault = make_afc_jam();
    fault.recovery_actions = {fault.recovery_actions[0], fault.recovery_actions[2]};
    presenter().present(fault);
    process_lvgl(20);

    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(fault_dialog());
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
    process_lvgl(20);

    CHECK(mock_client_.gcode_script_history().empty());
    CHECK_FALSE(TA::preheating(presenter()));
    CHECK_FALSE(presenter().is_visible());
    CHECK(TA::handled_detail(presenter()) == fault.detail);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A fault button with confirm_message asks before it sends",
                 "[error-center][recovery-presenter][1838]") {
    presenter().present(make_seat_fault());
    process_lvgl(20);

    // make_seat_fault(): Tool is seated (primary), Remove tool (danger, confirms).
    CHECK(painted(fault_button("btn_secondary"), StyleRole::ButtonDanger));
    press("btn_secondary");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 0);
    CHECK_THAT(helix::test::top_modal_text(), Catch::Matchers::ContainsSubstring("Hold it"));

    helix::test::answer_top_modal("btn_primary");
    process_lvgl(20);
    CHECK(gcode_count(mock_client_, "MANUAL_TOOL_REMOVE") == 1);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A Klipper action:prompt macro keeps the prompt renderer",
                 "[error-center][recovery-presenter][1838]") {
    helix::GcodeResponseRouting routing;
    routing.attach(&mock_client_, &api_);
    helix::ActionPromptManager* prompts = routing.action_prompt_manager();
    REQUIRE(prompts != nullptr);

    prompts->process_line("// action:prompt_begin Load filament");
    prompts->process_line("// action:prompt_text Pick a lane");
    prompts->process_line("// action:prompt_button Lane 1|LOAD LANE=1|primary");
    prompts->process_line("// action:prompt_show");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);

    CHECK(ModalStack::instance().dialog_for_component("action_prompt_modal") != nullptr);
    CHECK(fault_dialog() == nullptr);

    routing.detach_handlers(&mock_client_);
    routing.release_routers();
    helix::ActionPromptManager::set_instance(nullptr);
    process_lvgl(20);
}

TEST_CASE_METHOD(RecoveryPreheatFixture, "A fault with no actions offers OK to close it",
                 "[error-center][recovery-presenter][1838]") {
    helix::ErrorEvent fault = make_afc_jam();
    fault.recovery_actions.clear();
    presenter().present(fault);
    process_lvgl(20);
    REQUIRE(presenter().is_visible());

    lv_obj_t* ok = fault_button("btn_primary");
    CHECK(helix::test::subtree_text(ok).find(lv_tr("OK")) != std::string::npos);
    CHECK(shown(ok));
    CHECK_FALSE(shown(fault_button("btn_secondary")));

    press("btn_primary");
    process_lvgl(20);
    CHECK(mock_client_.gcode_script_history().empty());
    CHECK_FALSE(presenter().is_visible());
    CHECK(TA::handled_detail(presenter()) == fault.detail);
}
