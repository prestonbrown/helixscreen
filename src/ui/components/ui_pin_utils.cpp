// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pin_utils.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ui {

namespace {
// The error line is blank rather than empty, so it keeps its height.
constexpr const char* kBlankError = " ";
} // namespace

int key_digit(lv_event_t* e) {
    const auto* key = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* name = key ? lv_obj_get_name(key) : nullptr;
    const size_t len = name ? strlen(name) : 0;
    if (len == 0 || name[len - 1] < '0' || name[len - 1] > '9') {
        spdlog::warn("[Keypad] Cannot read a digit from key '{}'", name ? name : "");
        return -1;
    }
    return name[len - 1] - '0';
}

PinDisplay::PinDisplay(const char* prefix) : names_(prefix) {
    lv_subject_init_int(&digits_, 0);
    names_.publish(&digits_, "digits_subject", "digits");
    lv_subject_init_string(&error_, error_buf_, nullptr, sizeof(error_buf_), kBlankError);
    names_.publish(&error_, "error_subject", "error");
}

void PinDisplay::set_digit_count(int count) {
    lv_subject_set_int(&digits_, count);
}

void PinDisplay::show_error(const char* text) {
    lv_subject_set_string(&error_, text);
}

void PinDisplay::hide_error() {
    lv_subject_set_string(&error_, kBlankError);
}

} // namespace helix::ui
