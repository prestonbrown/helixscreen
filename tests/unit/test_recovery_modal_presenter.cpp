// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "safety_settings_manager.h"

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
    // Mirrors AmsBackendAfc::build_recovery_actions(): the two filament-moving
    // actions are flagged, the reset is not.
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
        lv_subject_set_int(st.get_print_state_enum_subject(),
                           static_cast<int>(helix::PrintJobState::STANDBY));

        SafetyLimits limits; // min_extrude_temp_celsius = 170 (Klipper default)
        api_.set_safety_limits(limits);

        // No AMS backend and no external spool: the preheat target resolves to
        // DEFAULT_LOAD_PREHEAT_TEMP, so the tests know what they are waiting for.
        AmsState::instance().set_backend(nullptr);
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
        lv_subject_set_int(get_printer_state().get_active_extruder_temp_subject(), celsius * 10);
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
    // or DEFAULT_LOAD_PREHEAT_TEMP).
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
