// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file xml_bind_test_utils.h
 * @brief Shared helpers for tests that drive shipped panel XML through subjects
 *
 * Two bindings aimed at one LVGL state or flag do not compose: each observer
 * asserts both polarities on every fire, so the subject that notified last
 * decides alone. Tests pinning that contract all need the same three things —
 * the real subject owner, the subject the registry actually resolved, and a
 * named widget that reports which name vanished when a panel is renamed.
 */

#pragma once

#include "printer_state.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace helix::test {

/// Build a panel purely to publish its subjects, then tear them down.
///
/// Panels own most of the subjects their XML binds to, so XMLTestFixture does
/// not register them. Binding against a stand-in registered under the same name
/// would prove only that lv_xml resolves a name, not that the panel and its XML
/// agree on one. deinit_subjects() on the way out keeps the global XML subject
/// registry from carrying dangling pointers into the next test.
template <typename Owner> class PanelSubjectOwner {
  public:
    explicit PanelSubjectOwner(PrinterState& st) : owner_(st, nullptr) {
        owner_.init_subjects();
    }
    ~PanelSubjectOwner() {
        owner_.deinit_subjects();
    }
    PanelSubjectOwner(const PanelSubjectOwner&) = delete;
    PanelSubjectOwner& operator=(const PanelSubjectOwner&) = delete;

  private:
    Owner owner_;
};

/// Look up a named descendant, naming the widget when it is absent. Panel XML is
/// edited far more often than these tests, so a rename must report WHICH widget
/// vanished rather than failing on a bare nullptr deref.
inline lv_obj_t* require_named(lv_obj_t* root, const char* name) {
    REQUIRE(root != nullptr);
    lv_obj_t* found = lv_obj_find_by_name(root, name);
    INFO("looking for widget named '" << name << "'");
    REQUIRE(found != nullptr);
    return found;
}

/// Fetch a subject from the registry the XML binds against, rather than from a
/// getter that may hand back a different instance.
inline lv_subject_t* xml_subject(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    INFO("looking for subject '" << name << "'");
    REQUIRE(s != nullptr);
    return s;
}

inline void set_xml_subject(const char* name, int value) {
    lv_subject_set_int(xml_subject(name), value);
}

} // namespace helix::test
