// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_macro_param_modal_defaults.cpp
 * @brief The parameter modal's save mode edits a macro's saved defaults: same
 *        fields as a run, but titled "Default Parameters", Save in place of
 *        Run, an "Ask for parameters" toggle under the fields, and nothing
 *        executed.
 *
 * Run with: ./build/bin/helix-tests "[macro_param_defaults_modal]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "macro_param_defaults.h"
#include "macro_param_modal.h"

#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using ParamValues = std::map<std::string, std::string>;

namespace {

const char* const PARAMS_TEMPLATE = "M117 T{params.TEMP|default(200)} S{params.SPEED|default(60)}";

} // namespace

class SaveModeFixture : public LVGLUITestFixture {
  public:
    SaveModeFixture() {
        // Synchronous hide, so a dismissed modal is gone by the next drain.
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
    }

    ~SaveModeFixture() override {
        // A failed assertion can leave the modal up with its static instance
        // pointer aimed at this soon-dead object.
        if (modal.dialog()) {
            helix::MacroParamModal::cancel_cb(nullptr);
        }
        process_lvgl(20);
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    void show_defaults(const ParamValues& values, bool ask = true) {
        helix::MacroParamDefaultRecord record;
        record.values = values;
        record.ask_for_params = ask;
        modal.show_for_defaults(lv_screen_active(), "MY_MACRO", params(), record,
                                [this](const helix::MacroParamDefaultRecord& saved) {
                                    ++saves;
                                    saved_record = saved;
                                });
        REQUIRE(modal.dialog() != nullptr);
    }

    void show_run() {
        modal.show_for_macro(lv_screen_active(), "MY_MACRO", params(),
                             [this](const auto&) { ++runs; });
        REQUIRE(modal.dialog() != nullptr);
    }

    static std::vector<helix::MacroParam> params() {
        auto parsed = helix::parse_macro_params(PARAMS_TEMPLATE);
        REQUIRE(parsed.size() == 2);
        return parsed;
    }

    lv_obj_t* widget(const char* name) const {
        lv_obj_t* found = lv_obj_find_by_name(modal.dialog(), name);
        REQUIRE(found != nullptr);
        return found;
    }

    /// Every field's text, in the order the fields are laid out.
    std::vector<std::string> field_texts() const {
        std::vector<std::string> texts;
        lv_obj_t* list = widget("param_list");
        const uint32_t count = lv_obj_get_child_count(list);
        for (uint32_t i = 0; i < count; ++i) {
            lv_obj_t* input =
                lv_obj_find_by_name(lv_obj_get_child(list, static_cast<int32_t>(i)), "field_input");
            REQUIRE(input != nullptr);
            texts.emplace_back(lv_textarea_get_text(input));
        }
        return texts;
    }

    /// What a finger produces: the switch flips its own CHECKED state, then the
    /// handler runs off the resulting value_changed.
    void tap(lv_obj_t* toggle) {
        if (lv_obj_has_state(toggle, LV_STATE_CHECKED)) {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
        process_lvgl(20);
    }

    helix::MacroParamModal modal;
    int saves = 0;
    int runs = 0;
    helix::MacroParamDefaultRecord saved_record;

  private:
    bool prev_animations_ = true;
};

TEST_CASE_METHOD(SaveModeFixture, "Save mode shows the defaults furniture and hides the run's",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_defaults({});

    CHECK(lv_obj_has_flag(widget("modal_title"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(widget("modal_title_save"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(widget("run_button_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(widget("save_button_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(widget("save_extras"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* ask = widget("ask_row");
    CHECK(lv_obj_has_state(lv_obj_find_by_name(ask, "toggle"), LV_STATE_CHECKED));
}

TEST_CASE_METHOD(SaveModeFixture, "Run mode shows none of the save-mode furniture",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_run();

    CHECK_FALSE(lv_obj_has_flag(widget("modal_title"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(widget("modal_title_save"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(widget("run_button_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(widget("save_button_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(widget("save_extras"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(SaveModeFixture, "Saved values prefill the fields and Save hands back the record",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_defaults({{"TEMP", "210"}, {"SPEED", "40"}});

    CHECK(field_texts() == std::vector<std::string>{"210", "40"});

    helix::MacroParamModal::save_cb(nullptr);
    process_lvgl(20);

    CHECK(saves == 1);
    CHECK(runs == 0); // Save runs nothing
    CHECK(saved_record.values == ParamValues{{"TEMP", "210"}, {"SPEED", "40"}});
    CHECK(saved_record.ask_for_params);
}

TEST_CASE_METHOD(SaveModeFixture, "An edited field and a toggled Ask both reach the saved record",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_defaults({{"TEMP", "210"}, {"SPEED", "40"}});

    lv_obj_t* ask = widget("ask_row");
    REQUIRE(lv_obj_has_state(lv_obj_find_by_name(ask, "toggle"), LV_STATE_CHECKED));
    tap(lv_obj_find_by_name(ask, "toggle"));

    lv_textarea_set_text(widget("field_input"), "220");

    helix::MacroParamModal::save_cb(nullptr);
    process_lvgl(20);

    CHECK(saves == 1);
    CHECK(saved_record.values == ParamValues{{"TEMP", "220"}, {"SPEED", "40"}});
    CHECK_FALSE(saved_record.ask_for_params);
}

TEST_CASE_METHOD(SaveModeFixture, "A record saved with Ask off reopens with the toggle off",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_defaults({}, /*ask=*/false);

    lv_obj_t* ask = widget("ask_row");
    CHECK_FALSE(lv_obj_has_state(lv_obj_find_by_name(ask, "toggle"), LV_STATE_CHECKED));
}

TEST_CASE_METHOD(SaveModeFixture, "An emptied field is not saved as a value",
                 "[macro][macro_params][macro_param_defaults_modal]") {
    show_defaults({{"TEMP", "210"}, {"SPEED", "40"}});

    lv_textarea_set_text(widget("field_input"), "");

    helix::MacroParamModal::save_cb(nullptr);
    process_lvgl(20);

    CHECK(saved_record.values == ParamValues{{"SPEED", "40"}});
}
