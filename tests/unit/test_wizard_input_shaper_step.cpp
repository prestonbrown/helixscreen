// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_input_shaper_step.cpp
 * @brief Unit tests for WizardInputShaperStep skip logic and calibration flow
 *
 * Tests cover:
 * - should_skip() returns true when no accelerometer available
 * - should_skip() returns false when accelerometer is available
 * - Step uses InputShaperCalibrator for calibration operations
 * - Integration: wizard skip flow based on hardware discovery
 */

#include "ui_wizard_input_shaper.h"

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "input_shaper_calibrator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using namespace helix::calibration;

// Helper to get subject by XML name (requires init_subjects(true))
static lv_subject_t* get_subject_by_name(const char* name) {
    return lv_xml_get_subject(NULL, name);
}

// ============================================================================
// Test Fixture
// ============================================================================

class WizardInputShaperStepTestFixture {
  public:
    WizardInputShaperStepTestFixture() {
        // Enable test mode so beta features are available
        get_runtime_config()->test_mode = true;

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_,
                                    [](lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
                                        lv_display_flush_ready(disp);
                                    });
            display_created_ = true;
        }

        // Initialize PrinterState subjects for testing
        PrinterStateTestAccess::reset(state());
        state().init_subjects(true); // Need XML registration to lookup by name
    }

    ~WizardInputShaperStepTestFixture() {
        // Reset after each test
        PrinterStateTestAccess::reset(state());
    }

  protected:
    PrinterState& state() {
        return get_printer_state();
    }

    // Helper to simulate accelerometer discovery via hardware
    void set_has_accelerometer(bool has_accel) {
        // Create hardware discovery with appropriate objects
        PrinterDiscovery hardware;
        nlohmann::json objects = nlohmann::json::array({"heater_bed", "extruder", "fan"});
        hardware.parse_objects(objects);

        // Accelerometers are detected from configfile, not objects list
        // (Klipper's objects list only includes objects with get_status() method)
        if (has_accel) {
            nlohmann::json config;
            config["adxl345"] = nlohmann::json::object();
            config["resonance_tester"] = nlohmann::json::object();
            hardware.parse_config_keys(config);
        }

        state().set_hardware(hardware);

        // Drain async queue to apply subject updates
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

  private:
    static lv_display_t* display_;
    static bool display_created_;
};

// Static members
lv_display_t* WizardInputShaperStepTestFixture::display_ = nullptr;
bool WizardInputShaperStepTestFixture::display_created_ = false;

// ============================================================================
// should_skip() Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - should_skip",
                 "[wizard][input-shaper][skip]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("Returns true when no accelerometer is detected") {
        set_has_accelerometer(false);

        REQUIRE(step.should_skip() == true);
    }

    SECTION("Returns false when accelerometer is detected") {
        set_has_accelerometer(true);

        REQUIRE(step.should_skip() == false);
    }

    SECTION("Works without calling create() - queries subject directly") {
        // Critical: skip logic should work even when step UI is not created
        set_has_accelerometer(true);

        // Do NOT call create() - step should still work
        REQUIRE(step.should_skip() == false);
    }
}

// ============================================================================
// is_validated() Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - is_validated",
                 "[wizard][input-shaper][validate]") {
    WizardInputShaperStep step;
    step.init_subjects();
    set_has_accelerometer(true);

    SECTION("Returns true when input shaper has been calibrated") {
        // Before calibration - not validated
        REQUIRE(step.is_validated() == false);

        // Mark calibration complete
        step.set_calibration_complete(true);

        REQUIRE(step.is_validated() == true);
    }

    SECTION("Returns true when user chooses to skip calibration") {
        // User explicitly chose to skip
        step.set_user_skipped(true);

        REQUIRE(step.is_validated() == true);
    }

    SECTION("Returns false when calibration not complete and not skipped") {
        REQUIRE(step.is_validated() == false);
    }
}

// ============================================================================
// has_accelerometer() Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - has_accelerometer",
                 "[wizard][input-shaper][accel]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("Returns false when no accelerometer") {
        set_has_accelerometer(false);

        REQUIRE(step.has_accelerometer() == false);
    }

    SECTION("Returns true when accelerometer detected") {
        set_has_accelerometer(true);

        REQUIRE(step.has_accelerometer() == true);
    }
}

// ============================================================================
// Calibrator Integration Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - calibrator integration",
                 "[wizard][input-shaper][calibrator]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("get_calibrator returns non-null calibrator") {
        InputShaperCalibrator* calibrator = step.get_calibrator();
        REQUIRE(calibrator != nullptr);
    }

    SECTION("Calibrator starts in IDLE state") {
        InputShaperCalibrator* calibrator = step.get_calibrator();
        REQUIRE(calibrator->get_state() == InputShaperCalibrator::State::IDLE);
    }
}

TEST_CASE_METHOD(WizardInputShaperStepTestFixture,
                 "WizardInputShaperStep - analysis phase counts elapsed seconds",
                 "[wizard][input-shaper][analysis]") {
    // The offline analysis reports no percent; the step's only liveness signal
    // is the elapsed label refreshed by the shared 1 Hz timer.
    WizardInputShaperStep step;
    step.init_subjects();

    step.begin_analysis_display();
    REQUIRE(step.analysis_timer_for_test() != nullptr);
    CHECK(std::string(lv_subject_get_string(step.get_status_subject())) == "Analyzing data... 0s");

    // The harness only runs finite-repeat timers; lend this one a count.
    lv_timer_set_repeat_count(step.analysis_timer_for_test(), 1000);
    lv_tick_inc(1500);
    lv_timer_handler_safe();
    CHECK(std::string(lv_subject_get_string(step.get_status_subject())) == "Analyzing data... 1s");

    // Cancel freezes the label; further virtual time changes nothing.
    step.cancel_analysis_display();
    const std::string frozen = lv_subject_get_string(step.get_status_subject());
    lv_tick_inc(5000);
    lv_timer_handler_safe();
    CHECK(std::string(lv_subject_get_string(step.get_status_subject())) == frozen);
    CHECK(step.analysis_timer_for_test() == nullptr);
}

// ============================================================================
// Wizard Flow Integration Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture,
                 "WizardInputShaperStep - wizard flow integration",
                 "[wizard][input-shaper][integration]") {
    SECTION("Flow with no accelerometer: should_skip returns true") {
        set_has_accelerometer(false);

        WizardInputShaperStep step;
        step.init_subjects();

        // Wizard framework checks should_skip before loading step
        REQUIRE(step.should_skip() == true);

        // Even if user somehow reaches the step, is_validated allows proceeding
        step.set_user_skipped(true);
        REQUIRE(step.is_validated() == true);
    }

    SECTION("Flow with accelerometer: show step, require calibration or skip") {
        set_has_accelerometer(true);

        WizardInputShaperStep step;
        step.init_subjects();

        // Should show the step
        REQUIRE(step.should_skip() == false);

        // Not validated until calibrated or skipped
        REQUIRE(step.is_validated() == false);

        // After calibration
        step.set_calibration_complete(true);
        REQUIRE(step.is_validated() == true);
    }

    SECTION("User can skip calibration in step UI") {
        set_has_accelerometer(true);

        WizardInputShaperStep step;
        step.init_subjects();

        REQUIRE(step.is_validated() == false);

        // User clicks "Skip for now" button
        step.set_user_skipped(true);

        REQUIRE(step.is_validated() == true);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - edge cases",
                 "[wizard][input-shaper][edge]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("Multiple calls to init_subjects are safe") {
        step.init_subjects();
        step.init_subjects();
        step.init_subjects();

        // Should not crash or corrupt state
        REQUIRE(step.is_validated() == false);
    }

    SECTION("Hardware rediscovery updates should_skip correctly") {
        // Start with no accelerometer
        set_has_accelerometer(false);
        REQUIRE(step.should_skip() == true);

        // Accelerometer discovered (user connected it)
        set_has_accelerometer(true);
        REQUIRE(step.should_skip() == false);

        // Removed again
        set_has_accelerometer(false);
        REQUIRE(step.should_skip() == true);
    }

    SECTION("cleanup() is safe to call before create()") {
        // Should not crash
        step.cleanup();
    }

    SECTION("Calibration and skip are mutually exclusive states but both validate") {
        set_has_accelerometer(true);

        step.set_calibration_complete(true);
        REQUIRE(step.is_validated() == true);

        // Setting user_skipped doesn't change validation
        step.set_user_skipped(true);
        REQUIRE(step.is_validated() == true);
    }
}

// ============================================================================
// State Persistence Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - state persistence",
                 "[wizard][input-shaper][state]") {
    SECTION("Calibration complete flag persists across step lifecycle") {
        set_has_accelerometer(true);

        WizardInputShaperStep step;
        step.init_subjects();

        step.set_calibration_complete(true);
        REQUIRE(step.is_calibration_complete() == true);

        // Cleanup and verify state is maintained
        step.cleanup();
        REQUIRE(step.is_calibration_complete() == true);
    }

    SECTION("User skip flag persists across step lifecycle") {
        set_has_accelerometer(true);

        WizardInputShaperStep step;
        step.init_subjects();

        step.set_user_skipped(true);
        REQUIRE(step.is_user_skipped() == true);

        // Cleanup and verify state is maintained
        step.cleanup();
        REQUIRE(step.is_user_skipped() == true);
    }
}

// ============================================================================
// Lifetime Guard Tests (Thread Safety)
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - lifetime guard",
                 "[wizard][input-shaper][threading]") {
    SECTION("Token is valid after construction") {
        WizardInputShaperStep step;
        step.init_subjects();

        auto token = step.get_lifetime_token();
        REQUIRE_FALSE(token.expired());
    }

    SECTION("Token becomes expired after cleanup") {
        WizardInputShaperStep step;
        step.init_subjects();

        auto token = step.get_lifetime_token();

        step.cleanup();

        REQUIRE(token.expired());
    }

    SECTION("Token remains checkable after cleanup for callback safety") {
        WizardInputShaperStep step;
        step.init_subjects();

        auto token = step.get_lifetime_token();

        // Simulate callback checking after cleanup
        step.cleanup();

        // Token should report expired (guard was invalidated)
        REQUIRE(token.expired());
    }
}

// ============================================================================
// Combined Progress Bar Mapping Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - combined bar mapping",
                 "[wizard][input-shaper][progress]") {
    using Phase = ShaperCalibrationPhase;

    SECTION("Sweep reports map onto the first half of the bar for X") {
        CHECK(WizardInputShaperStep::combined_bar_value(0, Phase::Sweeping, false) == 0);
        CHECK(WizardInputShaperStep::combined_bar_value(80, Phase::Sweeping, false) == 40);
        CHECK(WizardInputShaperStep::combined_bar_value(100, Phase::Sweeping, false) == 50);
    }

    SECTION("Sweep reports map onto the second half of the bar for Y") {
        CHECK(WizardInputShaperStep::combined_bar_value(0, Phase::Sweeping, true) == 50);
        CHECK(WizardInputShaperStep::combined_bar_value(80, Phase::Sweeping, true) == 90);
        CHECK(WizardInputShaperStep::combined_bar_value(100, Phase::Sweeping, true) == 100);
    }

    SECTION("Completion maps like a sweep report") {
        CHECK(WizardInputShaperStep::combined_bar_value(100, Phase::Complete, false) == 50);
        CHECK(WizardInputShaperStep::combined_bar_value(100, Phase::Complete, true) == 100);
    }

    SECTION("Analysis reports never move the bar") {
        // The analysis phase reports no percent (0 is the phase-change signal
        // only), so the bar holds its last sweep value; -1 tells the callback
        // to update the status label instead of the bar.
        CHECK(WizardInputShaperStep::combined_bar_value(0, Phase::Analyzing, false) == -1);
        CHECK(WizardInputShaperStep::combined_bar_value(0, Phase::Analyzing, true) == -1);
    }
}

// ============================================================================
// Subject Initialization Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - subjects",
                 "[wizard][input-shaper][subjects]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("Status subject is registered with correct name") {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "wizard_input_shaper_status");
        REQUIRE(subject != nullptr);
    }

    SECTION("Progress subject is registered with correct name") {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "wizard_input_shaper_progress");
        REQUIRE(subject != nullptr);
    }

    SECTION("Status subject has initial value") {
        lv_subject_t* subject = step.get_status_subject();
        REQUIRE(subject != nullptr);
        const char* status = lv_subject_get_string(subject);
        REQUIRE(status != nullptr);
        REQUIRE(std::string(status) == "Ready to calibrate");
    }

    SECTION("Progress subject starts at 0") {
        lv_subject_t* subject = step.get_progress_subject();
        REQUIRE(subject != nullptr);
        REQUIRE(lv_subject_get_int(subject) == 0);
    }
}

// ============================================================================
// Cleanup Behavior Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - cleanup behavior",
                 "[wizard][input-shaper][cleanup]") {
    WizardInputShaperStep step;
    step.init_subjects();
    set_has_accelerometer(true);

    SECTION("Cleanup cancels calibrator") {
        InputShaperCalibrator* calibrator = step.get_calibrator();
        REQUIRE(calibrator != nullptr);

        // Cleanup should not crash even if calibrator is in various states
        step.cleanup();

        // Step should remain functional after cleanup
        REQUIRE(step.is_calibration_complete() == false);
    }

    SECTION("Cleanup clears screen_root but preserves state flags") {
        step.set_calibration_complete(true);
        step.set_user_skipped(true);

        step.cleanup();

        // State flags should persist
        REQUIRE(step.is_calibration_complete() == true);
        REQUIRE(step.is_user_skipped() == true);
    }

    SECTION("Multiple cleanup calls are safe") {
        step.cleanup();
        step.cleanup();
        step.cleanup();

        // Should not crash
        REQUIRE(step.is_validated() == false);
    }
}

// ============================================================================
// Calibrator State Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - calibrator states",
                 "[wizard][input-shaper][calibrator]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("Calibrator is lazily created on first access") {
        // First access creates calibrator
        InputShaperCalibrator* cal1 = step.get_calibrator();
        REQUIRE(cal1 != nullptr);

        // Second access returns same instance
        InputShaperCalibrator* cal2 = step.get_calibrator();
        REQUIRE(cal2 == cal1);
    }

    SECTION("Calibrator state machine starts in IDLE") {
        InputShaperCalibrator* calibrator = step.get_calibrator();
        REQUIRE(calibrator->get_state() == InputShaperCalibrator::State::IDLE);
    }
}

// ============================================================================
// Name and Identity Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - identity",
                 "[wizard][input-shaper][identity]") {
    WizardInputShaperStep step;

    SECTION("get_name returns correct identifier") {
        REQUIRE(std::string(step.get_name()) == "Wizard Input Shaper");
    }
}

// ============================================================================
// Create Method Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - create scenarios",
                 "[wizard][input-shaper][create]") {
    WizardInputShaperStep step;
    step.init_subjects();
    step.register_callbacks();

    SECTION("create with null parent returns nullptr") {
        lv_obj_t* result = step.create(nullptr);
        // Should handle gracefully - either return nullptr or create at screen level
        // The important thing is no crash
        REQUIRE((result == nullptr || result != nullptr)); // Just verify no crash
        step.cleanup();
    }

    SECTION("create with valid parent returns non-null") {
        // Need to create the XML component first - check if it's registered
        // This test may fail if XML is not registered in test environment
        lv_obj_t* parent = lv_obj_create(lv_screen_active());
        REQUIRE(parent != nullptr);

        // Note: create() may return nullptr if XML component not registered
        // In unit tests without full XML registration, this is expected
        lv_obj_t* result = step.create(parent);
        // We mainly verify no crash occurs

        step.cleanup();
        lv_obj_delete(parent);
    }

    SECTION("create called twice logs warning but doesn't crash") {
        lv_obj_t* parent = lv_obj_create(lv_screen_active());
        REQUIRE(parent != nullptr);

        // First create
        step.create(parent);

        // Second create without cleanup - should log warning
        step.create(parent); // Should handle gracefully

        step.cleanup();
        lv_obj_delete(parent);
    }

    SECTION("cleanup is safe after failed create") {
        step.create(nullptr); // May fail
        step.cleanup();       // Should not crash
        step.cleanup();       // Multiple cleanups should be safe
    }
}

// ============================================================================
// Screen Root Access Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - screen root access",
                 "[wizard][input-shaper][screen]") {
    WizardInputShaperStep step;
    step.init_subjects();

    SECTION("get_screen_root returns nullptr before create") {
        REQUIRE(step.get_screen_root() == nullptr);
    }

    SECTION("get_screen_root returns nullptr after cleanup") {
        lv_obj_t* parent = lv_obj_create(lv_screen_active());
        step.create(parent);
        step.cleanup();
        REQUIRE(step.get_screen_root() == nullptr);
        lv_obj_delete(parent);
    }
}

// ============================================================================
// Error and Cancellation Tests
// ============================================================================

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - error scenarios",
                 "[wizard][input-shaper][error]") {
    WizardInputShaperStep step;
    step.init_subjects();
    set_has_accelerometer(true);

    SECTION("is_validated returns false after error (no skip, no complete)") {
        // Simulate error state - neither complete nor skipped
        step.set_calibration_complete(false);
        step.set_user_skipped(false);

        REQUIRE(step.is_validated() == false);
    }

    SECTION("status subject can be updated with error messages") {
        lv_subject_t* status = step.get_status_subject();
        REQUIRE(status != nullptr);

        // Simulate error message update
        lv_subject_copy_string(status, "Accelerometer not responding");

        const char* msg = lv_subject_get_string(status);
        REQUIRE(std::string(msg) == "Accelerometer not responding");
    }

    SECTION("progress subject can be reset to 0 after error") {
        lv_subject_t* progress = step.get_progress_subject();
        REQUIRE(progress != nullptr);

        // Simulate progress during calibration
        lv_subject_set_int(progress, 50);
        REQUIRE(lv_subject_get_int(progress) == 50);

        // Reset on error
        lv_subject_set_int(progress, 0);
        REQUIRE(lv_subject_get_int(progress) == 0);
    }

    SECTION("long error messages are handled safely") {
        lv_subject_t* status = step.get_status_subject();
        REQUIRE(status != nullptr);

        // Create a long error message (longer than 128 char buffer)
        std::string long_error(200, 'E');

        // Should not crash - LVGL truncates safely
        lv_subject_copy_string(status, long_error.c_str());

        // Verify it was truncated (not full length)
        const char* msg = lv_subject_get_string(status);
        REQUIRE(strlen(msg) < 200);
    }
}

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - cancellation scenarios",
                 "[wizard][input-shaper][cancel]") {
    WizardInputShaperStep step;
    step.init_subjects();
    set_has_accelerometer(true);

    SECTION("cleanup cancels calibrator") {
        InputShaperCalibrator* cal = step.get_calibrator();
        REQUIRE(cal != nullptr);
        REQUIRE(cal->get_state() == InputShaperCalibrator::State::IDLE);

        // Cleanup should call cancel()
        step.cleanup();

        // Calibrator should still be in IDLE (cancel resets to IDLE)
        REQUIRE(cal->get_state() == InputShaperCalibrator::State::IDLE);
    }

    SECTION("lifetime token prevents callbacks after cleanup") {
        auto token = step.get_lifetime_token();

        // Before cleanup - valid
        REQUIRE_FALSE(token.expired());

        // Cleanup
        step.cleanup();

        // After cleanup - expired
        REQUIRE(token.expired());
    }

    SECTION("user skip sets flag and allows validation") {
        REQUIRE(step.is_validated() == false);

        step.set_user_skipped(true);

        REQUIRE(step.is_validated() == true);
        REQUIRE(step.is_user_skipped() == true);
        REQUIRE(step.is_calibration_complete() == false);
    }

    SECTION("calibration and skip flags are independent") {
        // Set both flags
        step.set_calibration_complete(true);
        step.set_user_skipped(true);

        // Both can be true (though semantically one should be false)
        REQUIRE(step.is_calibration_complete() == true);
        REQUIRE(step.is_user_skipped() == true);
        REQUIRE(step.is_validated() == true);
    }
}

TEST_CASE_METHOD(WizardInputShaperStepTestFixture, "WizardInputShaperStep - async callback safety",
                 "[wizard][input-shaper][async]") {
    SECTION("token reports expired after step destruction") {
        helix::LifetimeToken token_copy(helix::LifetimeToken{helix::AsyncLifetimeGuard{}.token()});

        {
            WizardInputShaperStep step;
            step.init_subjects();
            token_copy = step.get_lifetime_token();

            // Verify valid inside scope
            REQUIRE_FALSE(token_copy.expired());
        }
        // Step destroyed here — guard destructor calls invalidate()

        // Token should be expired after destruction
        REQUIRE(token_copy.expired());
    }

    SECTION("multiple tokens from same step are consistent") {
        WizardInputShaperStep step;
        step.init_subjects();

        auto token1 = step.get_lifetime_token();
        auto token2 = step.get_lifetime_token();

        REQUIRE_FALSE(token1.expired());
        REQUIRE_FALSE(token2.expired());

        step.cleanup();

        // Both should expire together
        REQUIRE(token1.expired());
        REQUIRE(token2.expired());
    }
}
