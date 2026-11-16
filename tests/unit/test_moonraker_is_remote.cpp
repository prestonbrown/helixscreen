// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_moonraker_is_remote.cpp
 * @brief moonraker_is_remote subject lifecycle
 *
 * The subject is the UI-facing mirror of helix::is_moonraker_on_same_host()
 * evaluated against the live websocket endpoint. Default is 0 (local/
 * unknown); it flips on every connected edge, so a mid-session printer
 * switch re-truths every gated affordance.
 */

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

namespace {
struct RemoteSubjectFixture : public LVGLUITestFixture {
    lv_subject_t* subject() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "moonraker_is_remote");
        REQUIRE(s != nullptr);
        return s;
    }

    /// set_moonraker_is_remote() defers through UpdateQueue; drain before
    /// asserting or the write has not happened yet.
    void set_remote_and_drain(bool remote) {
        get_printer_state().set_moonraker_is_remote(remote);
        helix::ui::UpdateQueue::instance().drain();
    }
};
} // namespace

TEST_CASE_METHOD(RemoteSubjectFixture, "moonraker_is_remote defaults to 0", "[subjects][network]") {
    REQUIRE(lv_subject_get_int(subject()) == 0);
}

TEST_CASE_METHOD(RemoteSubjectFixture, "moonraker_is_remote flips per connected edge",
                 "[subjects][network]") {
    set_remote_and_drain(true);
    REQUIRE(lv_subject_get_int(subject()) == 1);
    REQUIRE(get_printer_state().is_moonraker_remote());

    // Printer switch: same session, opposite verdict
    set_remote_and_drain(false);
    REQUIRE(lv_subject_get_int(subject()) == 0);
    REQUIRE_FALSE(get_printer_state().is_moonraker_remote());
}

TEST_CASE_METHOD(RemoteSubjectFixture, "HELIX_MOCK_REMOTE_PRINTER is test-mode gated",
                 "[subjects][network]") {
    // Accessor contract: production runs never force, regardless of env.
    // (Direct env manipulation inside the test process would leak across
    // parallel cases; assert the negative arm only.)
    REQUIRE_FALSE(get_runtime_config()->should_mock_remote_printer());
}
