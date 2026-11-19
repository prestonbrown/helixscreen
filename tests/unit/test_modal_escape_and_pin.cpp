// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_effects.h"
#include "ui_modal.h"
#include "ui_pin_entry_modal.h"
#include "ui_pin_utils.h"
#include "ui_runout_guidance_modal.h"
#include "ui_spaghetti_detection_modal.h"

#include "../lvgl_ui_test_fixture.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void press_escape(lv_obj_t* dialog) {
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(backdrop != nullptr);
    uint32_t key = LV_KEY_ESC;
    lv_obj_send_event(backdrop, LV_EVENT_KEY, &key);
}

/// How many of a PIN pad's six dots show filled.
int filled_dots(lv_obj_t* root, const std::string& prefix) {
    int n = 0;
    for (int i = 0; i < 6; ++i) {
        const std::string name = prefix + std::to_string(i);
        n += lv_obj_get_style_bg_opa(lv_obj_find_by_name(root, name.c_str()), LV_PART_MAIN) ==
             LV_OPA_COVER;
    }
    return n;
}

std::string label_text(lv_obj_t* root, const char* name) {
    return lv_label_get_text(lv_obj_find_by_name(root, name));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ESC closes a print-safety modal without acting on the print",
                 "[modal][escape]") {
    int acted = 0;
    auto closes_on_escape = [this](std::unique_ptr<Modal> owned) {
        REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        press_escape(dialog);
        process_lvgl(50);
        return ModalStack::instance().top_dialog() != dialog;
    };

    auto spaghetti = std::make_unique<SpaghettiDetectionModal>();
    spaghetti->set_on_abort([&] { ++acted; });
    spaghetti->set_on_resume([&] { ++acted; });
    spaghetti->set_detection("detected noodle", nullptr);
    CHECK(closes_on_escape(std::move(spaghetti)));

    auto runout = std::make_unique<RunoutGuidanceModal>();
    runout->set_on_resume([&] { ++acted; });
    runout->set_on_cancel_print([&] { ++acted; });
    CHECK(closes_on_escape(std::move(runout)));
    CHECK(acted == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a PIN pad opened from a PIN callback keeps the navbar covered",
                 "[modal][pin_entry][modal_covers_navbar][from_event]") {
    REQUIRE_FALSE(ui::chrome_covered());
    ui::PinEntryModal::show_pin_entry("Enter PIN", [](const std::string&) {
        ui::PinEntryModal::show_pin_entry("Confirm PIN",
                                          [](const std::string&) { return std::string(); });
        return std::string();
    });
    REQUIRE(ui::chrome_covered());
    lv_obj_t* first_close = lv_obj_find_by_name(lv_screen_active(), "pin_close_btn");
    REQUIRE(first_close != nullptr);

    // Cancelling the first pad opens the second; the closing pad's button then answers nothing.
    lv_obj_send_event(first_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(first_close, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);
    CHECK(ui::chrome_covered());

    ui::PinEntryModal::dismiss();
    process_lvgl(50);
    CHECK_FALSE(ui::chrome_covered());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a stack modal closing over the PIN pad leaves the navbar covered",
                 "[modal][pin_entry][modal_covers_navbar]") {
    ui::PinEntryModal::show_pin_entry("Enter PIN",
                                      [](const std::string&) { return std::string(); });
    REQUIRE(ui::chrome_covered());

    auto owned = std::make_unique<RunoutGuidanceModal>();
    REQUIRE(Modal::show_owned(std::move(owned), test_screen()));
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    Modal::hide(dialog);
    process_lvgl(50);
    CHECK(ui::chrome_covered());

    ui::PinEntryModal::dismiss();
    process_lvgl(50);
    CHECK_FALSE(ui::chrome_covered());
}

TEST_CASE_METHOD(LVGLUITestFixture, "two PIN pads show their own dots and error line",
                 "[modal][pin_entry]") {
    struct Pad {
        const char* component;
        const char* dot_prefix;
        const char* error_label;
    };
    for (const Pad& pad : {Pad{"pin_entry_modal", "pin_dot_", "pin_error_label"},
                           Pad{"lock_screen", "lock_dot_", "lock_error_label"}}) {
        CAPTURE(pad.component);
        ui::PinDisplay first("test_pin");
        ui::PinDisplay second("test_pin");
        auto* a =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), pad.component, first.attrs()));
        auto* b =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), pad.component, second.attrs()));
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        auto filled = [&pad](lv_obj_t* root) { return filled_dots(root, pad.dot_prefix); };
        auto error = [&pad](lv_obj_t* root) { return label_text(root, pad.error_label); };

        first.set_digit_count(3);
        first.show_error("Wrong PIN");
        CHECK(filled(a) == 3);
        CHECK(error(a) == "Wrong PIN");
        CHECK(filled(b) == 0);
        CHECK(error(b) == " ");

        second.set_digit_count(6);
        first.hide_error();
        CHECK(filled(a) == 3);
        CHECK(filled(b) == 6);
        CHECK(error(a) == " ");

        lv_obj_delete(a);
        lv_obj_delete(b);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "the PIN pad's dots and error line follow its keys",
                 "[modal][pin_entry]") {
    std::string entered;
    ui::PinEntryModal::show_pin_entry("Enter PIN", [&entered](const std::string& pin) {
        entered = pin;
        return std::string("Wrong PIN");
    });
    lv_obj_t* screen = lv_screen_active();
    auto press = [screen](const char* name) {
        lv_obj_t* key = lv_obj_find_by_name(screen, name);
        REQUIRE(key != nullptr);
        lv_obj_send_event(key, LV_EVENT_CLICKED, nullptr);
    };
    for (const char* key : {"pin_digit_1", "pin_digit_9", "pin_digit_0", "pin_digit_4"}) {
        press(key);
    }
    CHECK(filled_dots(screen, "pin_dot_") == 4);
    press("pin_confirm");
    CHECK(entered == "1904");
    // A rejected PIN clears the dots and says why.
    CHECK(filled_dots(screen, "pin_dot_") == 0);
    CHECK(label_text(screen, "pin_error_label") == "Wrong PIN");

    ui::PinEntryModal::dismiss();
    process_lvgl(50);
}
