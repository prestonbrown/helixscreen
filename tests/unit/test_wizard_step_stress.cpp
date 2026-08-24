// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Wizard step-transition stress harness.
//
// Repeatedly drives ui_wizard_navigate_to_step() between steps 2 (WiFi) and
// 3 (Connection) to surface the chronic heap corruption family that has
// landed five separate fixes (#793, #827, #840, #871, #880, plus tonight's
// SIGABRT in lv_draw_sw_blend_image during step 3->2 back-nav).
//
// Run under AddressSanitizer for actionable UAF reports:
//   make test SANITIZE=address
//   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:fast_unwind_on_malloc=0 \
//       ./build/bin/helix-tests "[stress][wizard]"
//
// Default iteration count is moderate; override with WIZARD_STRESS_ITERATIONS
// in the environment for longer soak runs.
//
// The test is tagged [.ui_integration] (hidden by default) because it needs
// the XML component tree on disk. Run explicitly via the [stress] tag.

#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "wizard_step.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int env_iterations(int default_count) {
    if (const char* v = std::getenv("WIZARD_STRESS_ITERATIONS")) {
        try {
            int n = std::stoi(v);
            if (n > 0)
                return n;
        } catch (...) {
        }
    }
    return default_count;
}

class WizardStressFixture : public LVGLUITestFixture {
  public:
    WizardStressFixture() {
        wizard_ = ui_wizard_create(test_screen());
        if (!wizard_) {
            spdlog::error("[WizardStressFixture] ui_wizard_create returned null");
            return;
        }
        // Without the XML component tree on disk this test can't drive the
        // real navigation paths. Skip rather than spuriously pass.
        ready_ = (lv_obj_find_by_name(wizard_, "wizard_content") != nullptr);
    }

    ~WizardStressFixture() {
        wizard_ = nullptr;
    }

    void require_ready() {
        if (!ready_)
            SKIP("Wizard XML components not available");
    }

    /// The wizard's internal step index, published by ui_wizard_navigate_to_step().
    /// This is the only external evidence that a navigation actually landed;
    /// the wizard_content container itself outlives every step, so its mere
    /// presence proves nothing.
    int current_step() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "current_step");
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    /// Children currently parented under the step container. Steady-state
    /// after a settled navigation; a climbing value means a rebuild leaked.
    uint32_t content_child_count() const {
        lv_obj_t* content = lv_obj_find_by_name(wizard_, "wizard_content");
        REQUIRE(content != nullptr);
        return lv_obj_get_child_count(content);
    }

    /// Navigate and assert the wizard reports the step we asked for.
    void navigate_and_verify(helix::wizard::StepId step, int settle_ms) {
        ui_wizard_navigate_to_step(step);
        process_lvgl(settle_ms);
        INFO("requested step " << static_cast<int>(step));
        REQUIRE(current_step() == static_cast<int>(step));
    }

    lv_obj_t* wizard_ = nullptr;
    bool ready_ = false;
};

} // namespace

TEST_CASE_METHOD(WizardStressFixture, "Wizard stress: bounce step 2 <-> 3",
                 "[wizard][stress][.ui_integration]") {
    require_ready();

    const int iterations = env_iterations(50);
    spdlog::info("[wizard-stress] bouncing 2<->3 for {} iterations", iterations);

    // Settle one full nav before the loop so we start from a known state, and
    // record the settled child count as the steady-state baseline.
    navigate_and_verify(helix::wizard::StepId::Wifi, 120);
    const uint32_t baseline_children = content_child_count();

    for (int i = 0; i < iterations; ++i) {
        // Long enough to let the slide-out animation start, the async delete
        // pipeline drain, and any observer-driven rebuild fire.
        navigate_and_verify(helix::wizard::StepId::Connection, 80);
        navigate_and_verify(helix::wizard::StepId::Wifi, 80);

        // Back at the same step as the baseline with everything settled, so
        // the container must hold the same number of children. A rebuild that
        // forgot to drop the outgoing step shows up here as monotonic growth
        // long before it shows up as a crash.
        INFO("iteration " << i);
        REQUIRE(content_child_count() == baseline_children);

        if ((i + 1) % 10 == 0) {
            spdlog::info("[wizard-stress] iteration {}/{}", i + 1, iterations);
        }
    }

    // Surviving without aborting is the ASAN half of this test; the assertions
    // above are the half that fails on a merely-wrong (non-crashing) wizard.
}

TEST_CASE_METHOD(WizardStressFixture, "Wizard stress: full sweep 1..N",
                 "[wizard][stress][.ui_integration]") {
    require_ready();

    // The sweep range is the wizard's own declared step count, not a probe.
    //
    // The old probe walked forward and broke out when
    // `lv_obj_find_by_name(wizard_, "wizard_content")` came back null. That is
    // the wrong signal twice over: the container is created once by
    // ui_wizard_create() and outlives every step, so it is never null while
    // navigation merely misbehaves - and on the one path where it *could* be
    // null the loop fell out with last_valid == 1, collapsing the whole sweep
    // to a single step that still reported success. Nothing about the probe
    // could fail. STEP_COUNT is the honest bound and it comes from
    // wizard_step.h, not from this test.
    //
    // Step 0 (TouchCalibration) is excluded on purpose: navigate_to_step
    // forwards past it when it is skipped, so it is the one id that legitimately
    // does not land where it was asked to.
    const int last_valid = helix::wizard::STEP_COUNT - 1;
    REQUIRE(last_valid >= 3); // Wifi + Connection at minimum, or the sweep is meaningless
    spdlog::info("[wizard-stress] sweep range: 1..{}", last_valid);

    // Settle on the sweep's start point and take the steady-state baseline.
    navigate_and_verify(static_cast<helix::wizard::StepId>(1), 120);
    const uint32_t baseline_children = content_child_count();

    const int iterations = env_iterations(20);
    for (int i = 0; i < iterations; ++i) {
        for (int s = 1; s <= last_valid; ++s) {
            navigate_and_verify(static_cast<helix::wizard::StepId>(s), 60);
        }
        for (int s = last_valid; s >= 1; --s) {
            navigate_and_verify(static_cast<helix::wizard::StepId>(s), 60);
        }
        // Ends where it started, fully settled - same child count as before.
        INFO("sweep " << i);
        REQUIRE(content_child_count() == baseline_children);
    }
}
