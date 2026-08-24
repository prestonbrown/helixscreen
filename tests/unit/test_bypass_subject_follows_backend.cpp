// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bypass_subject_follows_backend.cpp
 * @brief The bypass toggle must render the same answer the tap acts on.
 *
 * Measured on a K2 Plus 2026-08-18: bypass was declared (stock CFS stands the
 * box down with BOX_ENABLE_CFS_PRINT ENABLE=0 and latches the declaration in
 * settings), then the external filament was pulled. current_slot fell back to
 * -1 while the declaration stayed, so the two readers disagreed:
 *
 *   ams_bypass_active  = (current_slot == -2)                  -> 0, switch OFF
 *   is_bypass_active() = (current_slot == -2 || declared)      -> 1
 *
 * BypassToggleController branches on the latter, so tapping the visibly-OFF
 * switch took the DISABLE path and toasted "Bypass disabled". The pre-print
 * material gate read the same true value and warned about the external spool
 * for a print the user had mapped to a CFS lane, with no on-screen sign that
 * bypass was on at all.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

namespace {

/// A backend whose declaration is latched independently of the seated slot —
/// the stock-CFS shape. AmsBackendMock alone cannot express this: its
/// is_bypass_active() is exactly current_slot == -2, so the two readers can
/// never disagree and the regression is invisible.
class DeclaredBypassBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    [[nodiscard]] bool is_bypass_active() const override {
        return declared_ || AmsBackendMock::is_bypass_active();
    }

    void set_declared(bool declared) {
        declared_ = declared;
    }

  private:
    bool declared_ = false;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "bypass subject follows the backend, not the seated slot",
                 "[ams][bypass][1289]") {
    auto mock = std::make_unique<DeclaredBypassBackend>(4);
    REQUIRE(mock->start().success());
    auto* backend = mock.get();

    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);

    lv_subject_t* subject = AmsState::instance().get_bypass_active_subject();
    REQUIRE(subject != nullptr);

    SECTION("declared with nothing at the toolhead still reads engaged") {
        // Exactly the K2 state: declaration latched, no external filament, so
        // no backend writes current_slot to the -2 sentinel.
        backend->set_declared(true);
        AmsState::instance().sync_from_backend();

        REQUIRE(backend->get_system_info().current_slot != -2);
        // The switch must not claim OFF while the tap would disable.
        CHECK(lv_subject_get_int(subject) == 1);
        CHECK(backend->is_bypass_active());
    }

    SECTION("not declared reads disengaged") {
        backend->set_declared(false);
        AmsState::instance().sync_from_backend();

        CHECK(lv_subject_get_int(subject) == 0);
        CHECK_FALSE(backend->is_bypass_active());
    }

    SECTION("display and action never disagree across a declaration change") {
        // The invariant, stated directly: whatever the controller would branch
        // on is what the surfaces render.
        for (bool declared : {false, true, false, true}) {
            backend->set_declared(declared);
            AmsState::instance().sync_from_backend();
            CHECK(lv_subject_get_int(subject) == (backend->is_bypass_active() ? 1 : 0));
        }
    }

    AmsState::instance().set_backend(nullptr);
}
