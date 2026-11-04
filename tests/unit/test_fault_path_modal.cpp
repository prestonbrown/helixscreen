// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_fault_path_modal.cpp
 * @brief The AFC position graphic actually reaches the modals (#1184).
 *
 * Run with: ./build/bin/helix-tests "[afc][fault][modal]"
 *
 * Three modals can show an AFC lane fault and all are wired:
 *   - RecoveryModalPresenter -> RecoveryFaultModal  (the `!!` / GcodeErrorRouter path)
 *   - AmsPanel -> AmsLoadingErrorModal              (the AmsAction::ERROR path)
 *   - ActionPromptModal                             (Klipper's own action:prompt)
 *
 * The first two hand their text through helix::ui::fault_path_apply(), which
 * publishes the stop point to `fault_path_segment`; ActionPromptModal publishes
 * from its own text before it builds, so any action:prompt shows the strip only for
 * the fault its text names. `<fault_path/>` in each modal's XML binds
 * `hidden` to 0, so the safety property (an unrecognised message renders
 * exactly as it would with no diagram at all) is one assertion away.
 *
 * Mutation checks (each must break the listed test):
 *   - drop the fault_path_apply() call from RecoveryFaultModal::show_fault()
 *     -> "the recovery modal shows the graphic for a recognised AFC fault" fails
 *   - make fault_path_apply() leave the subject alone when unrecognised
 *     -> "an unrecognised fault leaves no graphic behind" fails
 *   - remove <fault_path/> from ams_loading_error_modal.xml
 *     -> "the loading-error modal carries the graphic" fails
 *   - drop a caption's bind_flag_if_not_eq, or point it at the wrong ref_value
 *     -> "the fault position is named in words, not only in colour" fails
 *   - drop publish_fault_path() from ActionPromptModal::show_prompt()
 *     -> "a prompt that names no fault shows no diagram after one that did" fails
 *   - register FaultPath with the default (printer) lifetime
 *     -> "the diagram still binds after a printer switch" fails
 */

#include "ui_ams_loading_error_modal.h"
#include "ui_fault_path.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_modal.h"
#include "ams_types.h"
#include "core/static_subject_registry.h"
#include "error_event.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "recovery_modal_presenter.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// AFC.py:1345, exactly as it arrives — lane prefix, sentence, art, labels.
const std::string kPreGearFault =
    "lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL";

const std::string kPreGearSentence =
    "lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH";

/// Not one of AFC's five diagram-bearing faults.
const std::string kUnknownFault = "!! Move out of range: 300.000 0.000 10.000 [0.000]";

int fault_segment() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "fault_path_segment");
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

/// The graphic is present in the tree but hides itself; "shown" means both.
bool graphic_visible(lv_obj_t* root) {
    lv_obj_t* g = lv_obj_find_by_name(root, "fault_path");
    return g != nullptr && !lv_obj_has_flag(g, LV_OBJ_FLAG_HIDDEN);
}

/// Every caption in fault_path.xml, so "exactly one is visible" is checkable.
const char* const kAllCaptions[] = {
    "fault_path_stop_caption_spool",
    "fault_path_stop_caption_hub",
    "fault_path_stop_caption_output",
    "fault_path_stop_caption_toolhead",
};

helix::ErrorEvent make_event(const std::string& detail) {
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Filament System Error";
    e.detail = detail;
    e.recovery_actions = {
        {"Resume", "RESUME", "afc::resume", "primary", /*needs_hot_nozzle=*/true},
    };
    return e;
}

/// The fault dialog's message text.
std::string fault_body_text(lv_obj_t* screen) {
    lv_obj_t* message = lv_obj_find_by_name(screen, "dialog_message");
    REQUIRE(message != nullptr);
    return lv_label_get_text(message);
}

} // namespace

// ============================================================================
// The subject, which is what the XML binds
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "fault_path_apply publishes and clears the stop point",
                 "[afc][fault][modal]") {
    // OUTPUT (5) — past the hub, short of the toolhead.
    const std::string shown = helix::ui::fault_path_apply(kPreGearFault);
    CHECK(fault_segment() == static_cast<int>(helix::PathSegment::OUTPUT));
    CHECK(shown == kPreGearSentence);

    // An unrecognised message must CLEAR the previous marker, not inherit it,
    // and must come back byte-for-byte.
    const std::string passthrough = helix::ui::fault_path_apply(kUnknownFault);
    CHECK(fault_segment() == 0);
    CHECK(passthrough == kUnknownFault);
}

// ============================================================================
// The caption — the non-colour channel (#1196)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "the fault position is named in words, not only in colour",
                 "[afc][fault][modal][1196]") {
    auto* graphic =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "fault_path", nullptr));
    REQUIRE(graphic != nullptr);

    struct Case {
        const char* message;
        helix::PathSegment segment;
        const char* caption_name;
        const char* caption_text;
    };

    // One per AFC fault position, message text as afc_fault_position() sees it.
    const Case cases[] = {
        {"lane1 FAILED TO LOAD, CHECK FILAMENT AT TRIGGER", helix::PathSegment::SPOOL,
         "fault_path_stop_caption_spool", "Stopped"},
        {"lane1 filament did not trigger hub sensor, CHECK FILAMENT PATH", helix::PathSegment::HUB,
         "fault_path_stop_caption_hub", "Stopped"},
        {"lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH",
         helix::PathSegment::OUTPUT, "fault_path_stop_caption_output", "Stopped"},
        {"lane1 filament failed to trigger post extruder gear toolhead sensor, CHECK FILAMENT PATH",
         helix::PathSegment::TOOLHEAD, "fault_path_stop_caption_toolhead", "Jammed"},
    };

    for (const auto& c : cases) {
        INFO(c.message);
        helix::ui::fault_path_apply(c.message);
        process_lvgl(10);
        REQUIRE(fault_segment() == static_cast<int>(c.segment));
        REQUIRE_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));

        int visible = 0;
        for (const char* name : kAllCaptions) {
            lv_obj_t* caption = lv_obj_find_by_name(graphic, name);
            REQUIRE(caption != nullptr);
            if (lv_obj_has_flag(caption, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            ++visible;
            CHECK(std::string(name) == c.caption_name);
            CHECK(std::string(lv_label_get_text(caption)) == c.caption_text);
        }
        // Two visible captions would contradict each other; none leaves the fault
        // legible in colour alone, which is the whole complaint.
        CHECK(visible == 1);
        // "Jammed" stands in the toolhead's own label slot; every other fault
        // keeps the toolhead named.
        lv_obj_t* toolhead_label = lv_obj_find_by_name(graphic, "fault_path_stop_label_toolhead");
        REQUIRE(toolhead_label != nullptr);
        CHECK(lv_obj_has_flag(toolhead_label, LV_OBJ_FLAG_HIDDEN) ==
              (c.segment == helix::PathSegment::TOOLHEAD));
    }

    // Unrecognised message: the component hides, so no caption speaks for a fault
    // whose position we could not place.
    helix::ui::fault_path_apply(kUnknownFault);
    process_lvgl(10);
    CHECK(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(graphic);
    process_lvgl(10);
}

// ============================================================================
// RecoveryModalPresenter -> RecoveryFaultModal (the `!!` path)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the recovery modal shows the graphic for a recognised AFC fault",
                 "[afc][fault][modal]") {
    helix::ui::RecoveryModalPresenter presenter(api());

    presenter.present(make_event(kPreGearFault));
    process_lvgl(20);
    REQUIRE(presenter.is_visible());

    lv_obj_t* screen = lv_screen_active();
    CHECK(graphic_visible(screen));
    CHECK(fault_segment() == static_cast<int>(helix::PathSegment::OUTPUT));
    // The art rows are gone; the sentence survives.
    CHECK(fault_body_text(screen) == kPreGearSentence);

    presenter.dismiss();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "an unrecognised fault leaves no graphic behind",
                 "[afc][fault][modal]") {
    helix::ui::RecoveryModalPresenter presenter(api());

    // Show a recognised fault first, so "hidden" cannot pass vacuously.
    presenter.present(make_event(kPreGearFault));
    process_lvgl(20);
    REQUIRE(graphic_visible(lv_screen_active()));
    presenter.dismiss();
    process_lvgl(20);

    presenter.present(make_event(kUnknownFault));
    process_lvgl(20);
    REQUIRE(presenter.is_visible());

    lv_obj_t* screen = lv_screen_active();
    CHECK_FALSE(graphic_visible(screen));
    // ...and the message is rendered exactly as it always was.
    CHECK(fault_body_text(screen) == kUnknownFault);

    presenter.dismiss();
    process_lvgl(20);
}

// ============================================================================
// AmsLoadingErrorModal (the AmsAction::ERROR path)
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "the loading-error modal carries the graphic",
                 "[afc][fault][modal]") {
    lv_xml_register_component_from_file("A:ui_xml/ams_loading_error_modal.xml");

    helix::ui::AmsLoadingErrorModal modal;

    // AmsPanel::show_loading_error_modal() runs the backend's operation_detail
    // through fault_path_apply() before handing it to show().
    const std::string shown = helix::ui::fault_path_apply(kPreGearFault);
    REQUIRE(modal.show(lv_screen_active(), shown, []() {}));
    process_lvgl(20);
    REQUIRE(modal.is_visible());

    CHECK(graphic_visible(modal.dialog()));
    CHECK(shown == kPreGearSentence);

    modal.hide();
    process_lvgl(20);

    // Same modal, unrecognised message: the graphic stays down.
    const std::string plain = helix::ui::fault_path_apply(kUnknownFault);
    REQUIRE(modal.show(lv_screen_active(), plain, []() {}));
    process_lvgl(20);
    CHECK_FALSE(graphic_visible(modal.dialog()));

    modal.hide();
    process_lvgl(20);
}

// ============================================================================
// ActionPromptModal owns the diagram's state
// ============================================================================

namespace {

helix::PromptData make_prompt(const std::string& text) {
    helix::PromptData data;
    data.title = "AFC Calibration";
    data.text_lines = {text};
    data.buttons = {{"Turtle_1", "UNIT_CALIBRATION UNIT=Turtle_1", "primary", "", false, -1}};
    return data;
}

int visible_captions(lv_obj_t* root) {
    int visible = 0;
    for (const char* name : kAllCaptions) {
        lv_obj_t* caption = lv_obj_find_by_name(root, name);
        REQUIRE(caption != nullptr);
        visible += lv_obj_has_flag(caption, LV_OBJ_FLAG_HIDDEN) ? 0 : 1;
    }
    return visible;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a prompt that names no fault shows no diagram after one that did",
                 "[afc][fault][modal][action_prompt]") {
    helix::ui::RecoveryModalPresenter presenter(api());
    presenter.present(make_event(kPreGearFault));
    process_lvgl(20);
    REQUIRE(graphic_visible(lv_screen_active()));
    presenter.dismiss();
    process_lvgl(20);

    // AFC_CALIBRATION's picker arrives as a plain action:prompt.
    helix::ui::ActionPromptModal prompt;
    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt("Select a unit to calibrate.")));
    process_lvgl(20);
    CHECK_FALSE(graphic_visible(prompt.dialog()));
    prompt.hide();
    process_lvgl(20);

    // A wire prompt carrying AFC's fault sentence gets the diagram from its text.
    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt(kPreGearSentence)));
    process_lvgl(20);
    CHECK(graphic_visible(prompt.dialog()));
    CHECK(visible_captions(prompt.dialog()) == 1);
    prompt.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "the diagram still binds after a printer switch",
                 "[afc][fault][modal][action_prompt]") {
    // The subject backs a component registered once at boot, and a printer switch
    // never re-runs component registration.
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "FaultPath", StaticSubjectRegistry::Scope::PrinterSwitch));
    REQUIRE(lv_xml_get_subject(nullptr, "fault_path_segment") != nullptr);

    helix::ui::ActionPromptModal prompt;
    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt("Select a unit to calibrate.")));
    process_lvgl(20);
    // Unbound, the diagram would show with every caption at once.
    CHECK_FALSE(graphic_visible(prompt.dialog()));
    prompt.hide();
    process_lvgl(20);

    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt(kPreGearSentence)));
    process_lvgl(20);
    CHECK(graphic_visible(prompt.dialog()));
    CHECK(visible_captions(prompt.dialog()) == 1);
    CHECK(fault_segment() == static_cast<int>(helix::PathSegment::OUTPUT));
    prompt.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a prompt over the loading-error modal leaves its diagram up when it closes",
                 "[afc][fault][modal][action_prompt]") {
    lv_xml_register_component_from_file("A:ui_xml/ams_loading_error_modal.xml");
    helix::ui::AmsLoadingErrorModal error_modal;
    REQUIRE(
        error_modal.show(lv_screen_active(), helix::ui::fault_path_apply(kPreGearFault), []() {}));
    process_lvgl(20);
    REQUIRE(graphic_visible(error_modal.dialog()));

    helix::ui::ActionPromptModal prompt;
    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt("Select a unit to calibrate.")));
    process_lvgl(20);
    CHECK_FALSE(graphic_visible(prompt.dialog()));
    prompt.hide();
    process_lvgl(20);

    CHECK(fault_segment() == static_cast<int>(helix::PathSegment::OUTPUT));
    CHECK(graphic_visible(error_modal.dialog()));
    error_modal.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a prompt closing does not overwrite a fault published while it was open",
                 "[afc][fault][modal][action_prompt]") {
    helix::ui::fault_path_publish(kUnknownFault);
    helix::ui::ActionPromptModal prompt;
    REQUIRE(prompt.show_prompt(lv_screen_active(), make_prompt("Select a unit to calibrate.")));
    process_lvgl(20);

    // A later modal publishes its own fault over the open prompt.
    helix::ui::fault_path_publish(kPreGearFault);
    prompt.hide();
    process_lvgl(20);
    CHECK(fault_segment() == static_cast<int>(helix::PathSegment::OUTPUT));
}
