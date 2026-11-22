// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

// Targeted (subset) wizard session tests.
//
// Verifies that ui_wizard_create_targeted() launches the wizard running only a
// requested subset of steps (for hardware reconfiguration) and that finishing
// the subset goes through ui_wizard_complete_targeted() — which fires the
// on_complete callback and clears the subset WITHOUT setting wizard_completed.
//
// Uses LVGLUITestFixture for subject/widget/XML setup. The core contract
// (subset storage + completion wiring) is exercised without depending on the
// XML component tree being present on disk: ui_wizard_create_targeted() records
// the subset before creating the container, and the completion path is safe
// even when the container failed to materialize.

#include "ui_wizard.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using helix::wizard::StepId;

TEST_CASE_METHOD(LVGLUITestFixture, "targeted wizard records the requested step subset",
                 "[wizard][targeted]") {
    // Not in targeted mode before any targeted session is created.
    REQUIRE(ui_wizard_active_step_subset().empty());

    bool completed = false;
    lv_obj_t* root =
        ui_wizard_create_targeted(test_screen(), {StepId::FanSelect}, [&]() { completed = true; });

    // The container should be created when the XML tree is available. If it is
    // not (bare environment), the subset contract below is still the meaningful
    // assertion, so don't hard-fail on a missing widget tree.
    if (root == nullptr) {
        spdlog::warn("[test] wizard container not created (XML infra unavailable)");
    }

    // Core contract: the requested subset is active and exactly what was asked.
    const auto& subset = ui_wizard_active_step_subset();
    REQUIRE(subset.size() == 1);
    REQUIRE(subset[0] == StepId::FanSelect);
    REQUIRE(is_wizard_active());
    REQUIRE_FALSE(completed);

    // Finishing the targeted session fires the callback and clears subset mode
    // (and must NOT mark the wizard active anymore).
    ui_wizard_complete_targeted();
    REQUIRE(completed);
    REQUIRE(ui_wizard_active_step_subset().empty());
    REQUIRE_FALSE(is_wizard_active());
}

TEST_CASE_METHOD(LVGLUITestFixture, "targeted wizard preserves multi-step order",
                 "[wizard][targeted]") {
    bool completed = false;
    ui_wizard_create_targeted(test_screen(),
                              {StepId::HeaterSelect, StepId::FanSelect, StepId::LedSelect},
                              [&]() { completed = true; });

    const auto& subset = ui_wizard_active_step_subset();
    REQUIRE(subset.size() == 3);
    REQUIRE(subset[0] == StepId::HeaterSelect);
    REQUIRE(subset[1] == StepId::FanSelect);
    REQUIRE(subset[2] == StepId::LedSelect);

    // Tear down so the fixture (and any later test) starts clean.
    ui_wizard_complete_targeted();
    REQUIRE(completed);
    REQUIRE(ui_wizard_active_step_subset().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "targeted wizard navigate_to_step updates subset-scoped step subjects",
                 "[wizard][targeted]") {
    // Two-step subset so the first and last steps are distinct: HeaterSelect at
    // index 0 must show "not final", FanSelect at index 1 must show "final".
    // ui_wizard_create_targeted() navigates to the first step automatically.
    bool completed = false;
    lv_obj_t* root = ui_wizard_create_targeted(
        test_screen(), {StepId::HeaterSelect, StepId::FanSelect}, [&]() { completed = true; });

    if (root == nullptr) {
        spdlog::warn("[test] wizard container not created (XML infra unavailable)");
    }

    // Subjects are registered by name in ui_wizard_init_subjects() and are
    // accessible globally via lv_xml_get_subject(nullptr, "<name>").
    // wizard_step_text is the header's counter. A targeted session always shows
    // "of N": the subset IS the settled denominator.
    // wizard_is_final_step is an INT subject (0 = not final, 1 = final).
    // wizard_back_visible is an INT subject (0 = hidden, 1 = visible).
    // wizard_next_text / wizard_skip_text caption the footer's primary and Skip.
    lv_subject_t* step_text = lv_xml_get_subject(nullptr, "wizard_step_text");
    lv_subject_t* is_final = lv_xml_get_subject(nullptr, "wizard_is_final_step");
    lv_subject_t* back_vis = lv_xml_get_subject(nullptr, "wizard_back_visible");
    lv_subject_t* next_text = lv_xml_get_subject(nullptr, "wizard_next_text");
    lv_subject_t* skip_text = lv_xml_get_subject(nullptr, "wizard_skip_text");

    REQUIRE(step_text != nullptr);
    REQUIRE(is_final != nullptr);
    REQUIRE(back_vis != nullptr);
    REQUIRE(next_text != nullptr);
    REQUIRE(skip_text != nullptr);

    // At HeaterSelect (subset index 0): step 1 of 2, NOT final.
    // Back button hidden: first subset step with no cancel callback registered.
    REQUIRE(std::string(lv_subject_get_string(step_text)) == "Step 1 of 2");
    REQUIRE(lv_subject_get_int(is_final) == 0);
    REQUIRE(lv_subject_get_int(back_vis) == 0);
    REQUIRE(std::string(lv_subject_get_string(next_text)) == "Next");
    REQUIRE(std::string(lv_subject_get_string(skip_text)) == "Skip");

    // Navigate to the second (and last) subset step — exercises the
    // !g_step_subset.empty() branch of ui_wizard_navigate_to_step directly.
    ui_wizard_navigate_to_step(StepId::FanSelect);

    // At FanSelect (subset index 1): step 2 of 2, IS final.
    REQUIRE(std::string(lv_subject_get_string(step_text)) == "Step 2 of 2");
    REQUIRE(lv_subject_get_int(is_final) == 1);
    REQUIRE(std::string(lv_subject_get_string(next_text)) == "Finish");
    REQUIRE(std::string(lv_subject_get_string(skip_text)) == "Skip for now");

    // Tear down.
    ui_wizard_complete_targeted();
    REQUIRE(completed);
    REQUIRE(ui_wizard_active_step_subset().empty());
}

// Full-flow (no subset) navigation: the "of N" pair must hide through the
// connection step - where the denominator is still an estimate - and appear
// once the wizard is past it. The lurch this prevents: detection applies a
// preset between steps, and "Step 2 of 6" -> "Step 3 of 3" reads as the wizard
// skipping something (or "1 of 7" -> "2 of 11" growing) while both were honest
// recomputations.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "full-flow wizard hides the step total until the connection settles it",
                 "[1550][wizard]") {
    ui_wizard_init_subjects();

    lv_subject_t* step_text = lv_xml_get_subject(nullptr, "wizard_step_text");
    REQUIRE(step_text != nullptr);
    auto shows_total = [step_text]() {
        return std::string(lv_subject_get_string(step_text)).find(" of ") != std::string::npos;
    };

    // "The connection settles it" means Klipper actually reported hardware —
    // wizard_total_is_settled() requires that signal past Connection, not just
    // position. Seed a heater so PrinterIdentify sees a printer discovery
    // actually completed.
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};
    api.hardware().parse_objects(nlohmann::json::array({"extruder", "heater_bed"}));
    set_moonraker_api(&api);
    // RAII rather than a trailing reset: a failed REQUIRE below would throw and
    // skip it, leaving this process-global pointed at a stack object other
    // wizard tests in the shard then dereference.
    struct ApiGuard {
        ~ApiGuard() {
            set_moonraker_api(nullptr);
        }
    } api_guard;

    // Language and Wifi sit before Connection: total hidden.
    ui_wizard_navigate_to_step(StepId::Language);
    REQUIRE_FALSE(shows_total());
    ui_wizard_navigate_to_step(StepId::Wifi);
    REQUIRE_FALSE(shows_total());

    // Past the connection the printer is discovered and the denominator holds.
    ui_wizard_navigate_to_step(StepId::PrinterIdentify);
    REQUIRE(shows_total());

    // No deinit: other wizard tests in this shard reuse these process-wide
    // subjects, and init_subjects() is idempotent for the next one.
}

namespace {

/// The text of the first label under @p obj, or empty.
std::string first_label_text(lv_obj_t* obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        return lv_label_get_text(obj);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        const std::string text = first_label_text(lv_obj_get_child(obj, static_cast<int32_t>(i)));
        if (!text.empty()) {
            return text;
        }
    }
    return {};
}

} // namespace

// The wizard's footer is the calibration frame's: Back, Skip and the primary
// (Next / Finish), each driven by the wizard's subjects.
TEST_CASE_METHOD(LVGLUITestFixture, "setup wizard footer follows the step's subjects",
                 "[wizard][wizard_frame]") {
    lv_obj_t* root = ui_wizard_create_targeted(test_screen(),
                                               {StepId::HeaterSelect, StepId::FanSelect}, []() {});
    REQUIRE(root != nullptr);

    lv_obj_t* back = lv_obj_find_by_name(root, "btn_back");
    lv_obj_t* skip = lv_obj_find_by_name(root, "btn_skip");
    lv_obj_t* next = lv_obj_find_by_name(root, "btn_next");
    REQUIRE(back != nullptr);
    REQUIRE(skip != nullptr);
    REQUIRE(next != nullptr);
    lv_subject_t* passed = lv_xml_get_subject(nullptr, "connection_test_passed");
    lv_subject_t* show_skip = lv_xml_get_subject(nullptr, "wizard_show_skip");
    lv_subject_t* nav = lv_xml_get_subject(nullptr, "nav_buttons_enabled");
    REQUIRE(passed != nullptr);
    REQUIRE(show_skip != nullptr);
    REQUIRE(nav != nullptr);

    // First step, no cancel callback: no Back; Next, not Skip.
    lv_subject_set_int(passed, 1);
    lv_subject_set_int(show_skip, 0);
    process_lvgl(10);
    CHECK(lv_obj_is_hidden(back));
    CHECK(lv_obj_is_hidden(skip));
    CHECK_FALSE(lv_obj_is_hidden(next));
    CHECK(first_label_text(next) == "Next");
    CHECK_FALSE(lv_obj_has_state(next, LV_STATE_DISABLED));

    // The step's verdict gates Next.
    lv_subject_set_int(passed, 0);
    process_lvgl(10);
    CHECK(lv_obj_has_state(next, LV_STATE_DISABLED));
    lv_subject_set_int(passed, 1);

    // The wizard runs before Moonraker is up: an unreachable printer does not.
    const int nav_was = lv_subject_get_int(nav);
    lv_subject_set_int(nav, 0);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_has_state(next, LV_STATE_DISABLED));
    lv_subject_set_int(nav, nav_was);

    // A step offering its own action shows Skip in Next's place.
    lv_subject_set_int(show_skip, 1);
    process_lvgl(10);
    CHECK(lv_obj_is_hidden(next));
    CHECK_FALSE(lv_obj_is_hidden(skip));
    CHECK(first_label_text(skip) == "Skip");
    lv_subject_set_int(show_skip, 0);

    // Last step: Back, and Finish / Skip for now.
    ui_wizard_navigate_to_step(StepId::FanSelect);
    lv_subject_set_int(passed, 1);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_is_hidden(back));
    CHECK(first_label_text(back) == "Back");
    CHECK(first_label_text(next) == "Finish");
    lv_subject_set_int(show_skip, 1);
    process_lvgl(10);
    CHECK(first_label_text(skip) == "Skip for now");
    lv_subject_set_int(show_skip, 0);

    ui_wizard_complete_targeted();
}

// equal_width gives the footer's buttons one width; a calibration footer
// without it keeps the canvas look, Back sized to its caption.
TEST_CASE_METHOD(LVGLUITestFixture, "wizard frame footer equal_width gives its buttons one width",
                 "[wizard][wizard_frame]") {
    auto make_footer = [this](const char* equal) {
        const char* attrs[] = {"back_callback",
                               "on_back_clicked",
                               "back_name",
                               "eq_back",
                               "skip_callback",
                               "on_next_clicked",
                               "skip_name",
                               "eq_skip",
                               "primary_callback",
                               "on_next_clicked",
                               "primary_name",
                               "eq_next",
                               "primary_text",
                               "Next",
                               "equal_width",
                               equal,
                               nullptr};
        auto* footer =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "wizard_frame_footer", attrs));
        REQUIRE(footer != nullptr);
        lv_obj_update_layout(footer);
        return footer;
    };
    auto width_of = [](lv_obj_t* footer, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(footer, name);
        REQUIRE(btn != nullptr);
        REQUIRE_FALSE(lv_obj_is_hidden(btn));
        return lv_obj_get_width(btn);
    };

    lv_obj_t* equal = make_footer("1");
    const int32_t back_w = width_of(equal, "eq_back");
    CHECK(back_w > 0);
    CHECK(width_of(equal, "eq_skip") == back_w);
    CHECK(width_of(equal, "eq_next") == back_w);
    lv_obj_delete(equal);

    lv_obj_t* canvas = make_footer("0");
    CHECK(width_of(canvas, "eq_back") < width_of(canvas, "eq_next"));
    lv_obj_delete(canvas);
}
