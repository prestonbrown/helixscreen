// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

/// Counts the notifications a subject sends after this is constructed: what a
/// bound widget sees. A writer that fills a string subject's own buffer and
/// skips lv_subject_set_string() changes the value a getter reads but never
/// repaints a label, so only a count tells the two apart.
class SubjectNotifications {
  public:
    explicit SubjectNotifications(lv_subject_t* subject)
        : observer_(lv_subject_add_observer(subject, &SubjectNotifications::on_notify, this)) {
        count = 0; // the add itself notifies once
    }
    ~SubjectNotifications() {
        lv_observer_delete(observer_);
    }
    SubjectNotifications(const SubjectNotifications&) = delete;
    SubjectNotifications& operator=(const SubjectNotifications&) = delete;

    int count = 0;

  private:
    static void on_notify(lv_observer_t* observer, lv_subject_t*) {
        ++static_cast<SubjectNotifications*>(lv_observer_get_user_data(observer))->count;
    }
    lv_observer_t* observer_;
};
