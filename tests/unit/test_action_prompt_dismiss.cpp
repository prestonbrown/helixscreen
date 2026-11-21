// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// How an ActionPromptModal closes: an empty-gcode button is a dismiss that sends
// nothing (#1172), and every close the owner did not ask for reaches the dismiss
// callback, which is how a firmware prompt closed on screen gets ended.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

class DismissFixture : public LVGLUITestFixture {
  public:
    DismissFixture() {
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        modal_.set_gcode_callback([this](const std::string& g) { sent_.push_back(g); });
        modal_.set_dismiss_callback(
            [this](helix::PromptCloseKind kind) { dismissals_.push_back(kind); });
    }
    ~DismissFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    /// Depth-first search for the nth shown lv_button in the dialog tree. The
    /// modal's buttons are nameless, so type + order is the only handle; the
    /// frame's hidden header close button is skipped.
    lv_obj_t* nth_button(size_t n) {
        std::vector<lv_obj_t*> found;
        std::function<void(lv_obj_t*)> walk = [&](lv_obj_t* node) {
            if (!node || lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN))
                return;
            if (lv_obj_check_type(node, &lv_button_class))
                found.push_back(node);
            uint32_t count = lv_obj_get_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
                walk(lv_obj_get_child(node, i));
        };
        walk(modal_.dialog());
        return n < found.size() ? found[n] : nullptr;
    }

    helix::ui::ActionPromptModal modal_;
    std::vector<std::string> sent_;
    std::vector<helix::PromptCloseKind> dismissals_; ///< One entry per dismiss callback
    bool prev_animations_ = true;
};

helix::PromptData two_button_prompt() {
    helix::PromptData data;
    data.title = "Printer Error";
    data.text_lines.push_back("Unattributed fault");

    helix::PromptButton resume;
    resume.label = "Resume";
    resume.gcode = "RESUME";
    data.buttons.push_back(std::move(resume));

    // The dismiss affordance: a label, deliberately no gcode.
    helix::PromptButton dismiss;
    dismiss.label = "OK";
    data.buttons.push_back(std::move(dismiss));

    return data;
}

} // namespace

TEST_CASE_METHOD(DismissFixture, "a button with no gcode sends nothing and closes",
                 "[action_prompt][1172][ui_integration]") {
    REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));

    lv_obj_t* dismiss = nth_button(1);
    REQUIRE(dismiss != nullptr);

    lv_obj_send_event(dismiss, LV_EVENT_CLICKED, nullptr);
    process_lvgl(40);

    // The label must NOT have been sent as a command — `OK` is not gcode.
    CHECK(sent_.empty());
    CHECK_FALSE(modal_.is_visible());
}

TEST_CASE_METHOD(DismissFixture, "a button with a gcode still sends it",
                 "[action_prompt][1172][ui_integration]") {
    // The other side of the branch: dropping the fallback must not make real
    // actions inert.
    REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));

    lv_obj_t* resume = nth_button(0);
    REQUIRE(resume != nullptr);

    lv_obj_send_event(resume, LV_EVENT_CLICKED, nullptr);
    process_lvgl(40);

    REQUIRE(sent_.size() == 1);
    CHECK(sent_[0] == "RESUME");
    CHECK_FALSE(modal_.is_visible());
}

TEST_CASE_METHOD(DismissFixture, "closes the owner did not ask for reach the dismiss callback",
                 "[action_prompt][dismiss][ui_integration]") {
    using helix::PromptCloseKind;

    SECTION("a button with a gcode reports that it sent one") {
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        lv_obj_send_event(nth_button(0), LV_EVENT_CLICKED, nullptr);
        process_lvgl(40);
        REQUIRE(dismissals_.size() == 1);
        CHECK(dismissals_[0] == PromptCloseKind::ButtonWithGcode);
    }

    SECTION("a button without a gcode reports that it sent none") {
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        lv_obj_send_event(nth_button(1), LV_EVENT_CLICKED, nullptr);
        process_lvgl(40);
        REQUIRE(dismissals_.size() == 1);
        CHECK(dismissals_[0] == PromptCloseKind::ButtonWithoutGcode);
    }

    SECTION("each other close reason maps to its kind") {
        const std::pair<ModalCloseReason, PromptCloseKind> cases[] = {
            {ModalCloseReason::BackdropTap, PromptCloseKind::UserDismiss},
            {ModalCloseReason::EscKey, PromptCloseKind::UserDismiss},
            {ModalCloseReason::HotReload, PromptCloseKind::HotReload},
            {ModalCloseReason::External, PromptCloseKind::External},
        };
        for (const auto& [reason, kind] : cases) {
            dismissals_.clear();
            REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
            modal_.hide(reason);
            process_lvgl(40);
            REQUIRE(dismissals_.size() == 1);
            CHECK(dismissals_[0] == kind);
        }
    }

    SECTION("the owner's own hide is not a dismissal") {
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        modal_.hide();
        process_lvgl(40);
        // Replacing the content hides the old dialog too.
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        modal_.hide();
        process_lvgl(40);
        CHECK(dismissals_.empty());
    }

    SECTION("a gcode sent before a reshow does not leak into the next close") {
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        lv_obj_send_event(nth_button(0), LV_EVENT_CLICKED, nullptr);
        process_lvgl(40);
        REQUIRE(modal_.show_prompt(test_screen(), two_button_prompt()));
        lv_obj_send_event(nth_button(1), LV_EVENT_CLICKED, nullptr);
        process_lvgl(40);
        REQUIRE(dismissals_.size() == 2);
        CHECK(dismissals_[1] == PromptCloseKind::ButtonWithoutGcode);
    }
}
