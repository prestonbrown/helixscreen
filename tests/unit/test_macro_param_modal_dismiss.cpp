// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A parameter modal closed by a backdrop tap leaves no live static instance
// for a late Run press to reach.

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "macro_param_modal.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

const char* const PARAMS_TEMPLATE = "M117 T{params.TEMP|default(200)} S{params.SPEED|default(60)}";

class MacroDismissFixture : public LVGLUITestFixture {
  public:
    MacroDismissFixture() {
        // Synchronous hide, so a dismissed modal is gone by the next drain.
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
    }

    ~MacroDismissFixture() override {
        // A failed assertion can leave the modal up with its static instance
        // pointer aimed at this soon-dead object.
        if (modal.dialog()) {
            helix::MacroParamModal::cancel_cb(nullptr);
        }
        process_lvgl(20);
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    void show_run() {
        auto params = helix::parse_macro_params(PARAMS_TEMPLATE);
        REQUIRE(params.size() == 2);
        modal.show_for_macro(lv_screen_active(), "MY_MACRO", params,
                             [this](const auto&) { ++runs; });
        REQUIRE(modal.dialog() != nullptr);
    }

    helix::MacroParamModal modal;
    int runs = 0;

  private:
    bool prev_animations_ = true;
};

} // namespace

TEST_CASE_METHOD(MacroDismissFixture, "A backdrop close leaves no live instance behind",
                 "[macro][macro_params][dismiss]") {
    show_run();
    lv_obj_send_event(modal.backdrop(), LV_EVENT_CLICKED, nullptr);
    process_lvgl(20);
    REQUIRE(modal.dialog() == nullptr);

    // A late Run press from the closed dialog's callbacks reaches nothing.
    helix::MacroParamModal::run_cb(nullptr);
    process_lvgl(20);
    CHECK(runs == 0);
}

TEST_CASE_METHOD(MacroDismissFixture, "A modal destroyed while shown leaves no live instance behind",
                 "[macro][macro_params][dismiss]") {
    alignas(helix::MacroParamModal) unsigned char storage[sizeof(helix::MacroParamModal)];
    auto* doomed = new (storage) helix::MacroParamModal;
    auto params = helix::parse_macro_params(PARAMS_TEMPLATE);
    doomed->show_for_macro(lv_screen_active(), "MY_MACRO", params,
                           [this](const auto&) { ++runs; });
    REQUIRE(doomed->dialog() != nullptr);
    doomed->~MacroParamModal();
    std::memset(storage, 0, sizeof(storage));

    // A pointer left at the freed instance dispatches through its zeroed vtable.
    helix::MacroParamModal::run_cb(nullptr);
    CHECK(runs == 0);
}
