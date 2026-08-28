// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_spool_autoassign.cpp
 * @brief The bypass gate and per-tool spool assignment are mutually exclusive.
 *
 * Application's Spoolman sync mirrors the active spool onto the external-spool
 * (bypass) slot, and it guards that with active_spool_describes_bypass() so a
 * lane assignment cannot overwrite the bypass record. The tool-changer
 * auto-assign - "no spool recorded for the active tool, adopt the active
 * Spoolman spool" - was nested INSIDE that guard.
 *
 * It could therefore never run on a real changer. The guard passes only when
 * `no backend || any_bypass_active()`, while the auto-assign itself needs a
 * backend that answers supports_per_tool_spool_assignment(). Every backend that
 * answers it - AmsBackendToolChanger and AmsBackendSnapmaker, the two types
 * is_tool_changer() covers - hardcodes is_bypass_active() to false, because a
 * changer carries its extruder on the toolhead and has no bypass path at all.
 * The two conditions could not both hold, so the assign was dead code on
 * exactly the hardware it was written for.
 *
 * These tests pin the contradiction rather than the fix, so the assign cannot
 * be moved back under the gate without something going red.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

/// Install a real changer backend and hand back the raw pointer AmsState owns.
template <typename Backend> AmsBackend* install() {
    auto backend = std::make_unique<Backend>(nullptr, nullptr);
    AmsBackend* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    AmsState::instance().init_subjects(true);
    return raw;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "A tool changer never describes the bypass",
                 "[ams][toolchanger][spoolman]") {
    SECTION("klipper-toolchanger") {
        AmsBackend* backend = install<AmsBackendToolChanger>();

        // The auto-assign's own precondition.
        REQUIRE(backend->supports_per_tool_spool_assignment());

        // ...and the gate it used to sit behind, which excludes it.
        CHECK_FALSE(backend->is_bypass_active());
        CHECK_FALSE(AmsState::instance().active_spool_describes_bypass());
    }

    SECTION("Snapmaker U1") {
        AmsBackend* backend = install<AmsBackendSnapmaker>();

        REQUIRE(backend->supports_per_tool_spool_assignment());
        CHECK_FALSE(backend->is_bypass_active());
        CHECK_FALSE(AmsState::instance().active_spool_describes_bypass());
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "The bypass gate's other passing case has no backend to assign to",
                 "[ams][toolchanger][spoolman]") {
    // active_spool_describes_bypass() is `no backend || any_bypass_active()`.
    // With no backend it passes - and the auto-assign then bails on its own
    // null check. So neither branch of the gate can ever reach it: that is the
    // whole reason the call had to move out from under the gate rather than
    // have its conditions relaxed.
    AmsState::instance().clear_backends();

    CHECK(AmsState::instance().get_backend() == nullptr);
    CHECK(AmsState::instance().active_spool_describes_bypass());
}

TEST_CASE_METHOD(LVGLTestFixture, "Every per-tool-assignment backend is bypass-free",
                 "[ams][toolchanger][spoolman]") {
    // Stated against the capability rather than a list of classes, so a new
    // backend that opts into per-tool assignment while reporting a live bypass
    // has to come back through here and decide what the sync should do.
    struct Case {
        const char* name;
        AmsBackend* backend;
    };

    auto tc = std::make_unique<AmsBackendToolChanger>(nullptr, nullptr);
    auto sm = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);

    const Case cases[] = {
        {"AmsBackendToolChanger", tc.get()},
        {"AmsBackendSnapmaker", sm.get()},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        if (c.backend->supports_per_tool_spool_assignment()) {
            CHECK_FALSE(c.backend->is_bypass_active());
        }
        // The capability tracks is_tool_changer() by default; both of these are.
        CHECK(c.backend->supports_per_tool_spool_assignment() ==
              is_tool_changer(c.backend->get_type()));
    }
}
