// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"

#include <string>

extern "C" void lv_xml_component_init(void);

namespace helix::test {

/// Pins one integer subject in the global XML scope for the guard's life. An
/// already-registered subject gets its value set and put back; an absent one is
/// registered here and unregistered again, so a later test still finds it
/// absent. For capability subjects a test needs at a known value whatever
/// earlier cases in the binary registered or left behind.
class ScopedIntSubject {
  public:
    ScopedIntSubject(const char* name, int value) : name_(name) {
        if (!lv_xml_component_get_scope("globals")) {
            lv_xml_component_init();
        }
        existing_ = lv_xml_get_subject(nullptr, name);
        if (existing_) {
            saved_ = lv_subject_get_int(existing_);
            lv_subject_set_int(existing_, value);
        } else {
            lv_subject_init_int(&own_, value);
            lv_xml_register_subject(nullptr, name, &own_);
        }
    }

    ~ScopedIntSubject() {
        if (existing_) {
            lv_subject_set_int(existing_, saved_);
        } else {
            lv_xml_unregister_subject(nullptr, name_.c_str());
            lv_subject_deinit(&own_);
        }
        helix::ui::UpdateQueue::instance().drain();
    }

    ScopedIntSubject(const ScopedIntSubject&) = delete;
    ScopedIntSubject& operator=(const ScopedIntSubject&) = delete;

  private:
    std::string name_;
    lv_subject_t* existing_ = nullptr;
    lv_subject_t own_{};
    int saved_ = 0;
};

} // namespace helix::test
