// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file scoped_animations_enabled.h
 * @brief Drive the "Animations" preference for the length of a scope.
 *
 * HelixTestFixture forces the preference OFF for every test (see the long note
 * in tests/helix_test_fixture.cpp about modal exit timing). Any code path that
 * only runs when animations are ON — a scrolling label, the grid-edit snap
 * animation, the heater icon pulse — is therefore skipped by default, and a
 * test that does not say otherwise asserts against nothing.
 *
 * This drives the very subject the production code observes, and restores
 * whatever the value was on the way out.
 */

#pragma once

#include "ui_animations_pref.h"

#include "lvgl/lvgl.h"

namespace helix::ui {

/// Sets the preference on construction and puts it back on destruction.
class ScopedAnimationsEnabled {
  public:
    explicit ScopedAnimationsEnabled(bool enabled = true) {
        subject_ = helix::ui::animations_pref_subject();
        if (subject_) {
            previous_ = lv_subject_get_int(subject_);
            lv_subject_set_int(subject_, enabled ? 1 : 0);
        }
    }

    ~ScopedAnimationsEnabled() {
        if (subject_) {
            lv_subject_set_int(subject_, previous_);
        }
    }

    ScopedAnimationsEnabled(const ScopedAnimationsEnabled&) = delete;
    ScopedAnimationsEnabled& operator=(const ScopedAnimationsEnabled&) = delete;

    /// Toggle mid-scope, for tests that check the preference reaching live widgets.
    void set(bool enabled) {
        if (subject_) {
            lv_subject_set_int(subject_, enabled ? 1 : 0);
        }
    }

    /// False when the settings subjects were never built, which makes every
    /// set() a no-op — a test that depends on the preference should REQUIRE this.
    bool available() const {
        return subject_ != nullptr;
    }

  private:
    lv_subject_t* subject_ = nullptr;
    int previous_ = 0;
};

} // namespace helix::ui
