// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pin_utils.h"

namespace helix::ui {

namespace {
// The error line is blank rather than empty, so it keeps its height.
constexpr const char* kBlankError = " ";
} // namespace

PinDisplay::PinDisplay(const char* prefix) {
    static unsigned next_id = 0;
    const std::string base = std::string(prefix) + "_" + std::to_string(next_id++);
    digits_name_ = base + "_digits";
    error_name_ = base + "_error";

    lv_subject_init_int(&digits_, 0);
    subjects_.publish(digits_name_.c_str(), &digits_);
    lv_subject_init_string(&error_, error_buf_, nullptr, sizeof(error_buf_), kBlankError);
    subjects_.publish(error_name_.c_str(), &error_);

    attrs_[0] = "digits_subject";
    attrs_[1] = digits_name_.c_str();
    attrs_[2] = "error_subject";
    attrs_[3] = error_name_.c_str();
    attrs_[4] = nullptr;
}

PinDisplay::~PinDisplay() {
    // Before the members holding the subjects go.
    subjects_.deinit_all();
}

void PinDisplay::set_digit_count(int count) {
    lv_subject_set_int(&digits_, count);
}

void PinDisplay::show_error(const char* text) {
    lv_subject_copy_string(&error_, text);
}

void PinDisplay::hide_error() {
    lv_subject_copy_string(&error_, kBlankError);
}

} // namespace helix::ui
