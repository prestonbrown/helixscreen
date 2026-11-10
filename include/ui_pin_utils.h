// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "instance_subjects.h"

#include <lvgl.h>
#include <string>

namespace helix::ui {

/**
 * The digit a keypad key stands for, read from the last character of its name
 * ("btn_7", "pin_digit_7", "lock_digit_7" all give 7); -1 when the clicked
 * object is unnamed or its name does not end in a digit. Keypads share one
 * clicked callback across their ten digit keys this way.
 */
int key_digit(lv_event_t* e);

/**
 * The dot row and error line of a PIN pad, as subjects its component binds.
 *
 * Each pad owns one and passes attrs() to lv_xml_create(), so the component's
 * digits_subject and error_subject props name this instance's subjects and two
 * pads alive at once (one closing while the next opens) never share state.
 * The subjects die with the object.
 */
class PinDisplay {
  public:
    /// @param prefix Names the subjects "<prefix>_<n>_digits" and "<prefix>_<n>_error",
    ///               n unique per process.
    explicit PinDisplay(const char* prefix);

    PinDisplay(const PinDisplay&) = delete;
    PinDisplay& operator=(const PinDisplay&) = delete;

    /// Prop list for lv_xml_create(): digits_subject, error_subject, nullptr.
    const char** attrs() {
        return names_.attrs();
    }

    /// Number of dots shown filled.
    void set_digit_count(int count);
    /// Show @p text on the error line.
    void show_error(const char* text);
    /// Blank the error line; it keeps its height.
    void hide_error();

  private:
    lv_subject_t digits_{};
    lv_subject_t error_{};
    char error_buf_[128] = {};
    helix::InstanceSubjects names_; ///< After the subjects it publishes
};

} // namespace helix::ui
