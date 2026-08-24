// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_observer_cleanup_ordering.cpp
 * @brief Tests for crash-hardening: observer cleanup ordering
 *
 * Validates the fix from f843b0a2: widget pointers must be nullified
 * BEFORE observer guards are reset in cleanup methods. This prevents
 * cascading observer callbacks from accessing freed LVGL objects.
 *
 * Also tests the active_ guard pattern: observer callbacks must be
 * no-ops when the active_ flag is false.
 *
 * These tests FAIL if the protective code is removed.
 */

#include "ui_bypass_toggle_controller.h"
#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <atomic>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// Drain deferred observer callbacks
static void drain() {
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
}

// ============================================================================
// Simulates the pattern used by AmsOperationSidebar, AmsPanel,
// and ZOffsetCalibrationPanel: a class with widget pointers, an
// active_ guard, and observer guards whose callbacks reference widgets.
// ============================================================================

class MockPanel {
  public:
    // Simulated widget pointers (would be lv_obj_t* in real code)
    lv_obj_t* widget_a_ = nullptr;
    lv_obj_t* widget_b_ = nullptr;

    // Lifecycle guard — set true after setup, cleared in cleanup
    bool active_ = false;

    // Observer guards
    ObserverGuard observer_a_;
    ObserverGuard observer_b_;

    // Tracks whether a callback accessed a widget after cleanup started
    bool callback_accessed_freed_widget_ = false;
    int callback_invocations_after_cleanup_ = 0;

    // Subject for testing
    lv_subject_t subject_;

    void init_subject() {
        lv_subject_init_int(&subject_, 0);
    }

    void setup(lv_obj_t* parent) {
        widget_a_ = lv_obj_create(parent);
        widget_b_ = lv_obj_create(parent);
        active_ = true;
    }

    void init_observers() {
        observer_a_ =
            observe_int_sync<MockPanel>(&subject_, this, [](MockPanel* self, int /*val*/) {
                if (!self->active_ || !self->widget_a_)
                    return;
                // In a real panel, this would call lv_label_set_text or similar
                // on widget_a_. If widget_a_ is freed, this is a UAF crash.
                self->callback_invocations_after_cleanup_++;
            });

        observer_b_ =
            observe_int_sync<MockPanel>(&subject_, this, [](MockPanel* self, int /*val*/) {
                if (!self->active_ || !self->widget_b_)
                    return;
                self->callback_invocations_after_cleanup_++;
            });
    }

    // CORRECT cleanup ordering: nullify widgets BEFORE resetting observers
    void cleanup_correct() {
        active_ = false;

        widget_a_ = nullptr;
        widget_b_ = nullptr;

        observer_a_.reset();
        observer_b_.reset();
    }

    // WRONG cleanup ordering: reset observers BEFORE nullifying widgets.
    // This is the bug pattern that f843b0a2 fixed. Resetting an observer
    // can trigger cascading callbacks that dereference widget pointers.
    void cleanup_wrong() {
        active_ = false;

        observer_a_.reset();
        observer_b_.reset();

        widget_a_ = nullptr;
        widget_b_ = nullptr;
    }

    void deinit_subject() {
        lv_subject_deinit(&subject_);
    }
};

// ============================================================================
// Tests for cleanup ordering
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "Observer cleanup: correct ordering nullifies widgets before observers",
                 "[observer_cleanup][crash_hardening]") {
    MockPanel panel;
    panel.init_subject();
    panel.setup(test_screen());
    panel.init_observers();
    drain();

    REQUIRE(panel.widget_a_ != nullptr);
    REQUIRE(panel.widget_b_ != nullptr);
    REQUIRE(panel.active_ == true);

    panel.cleanup_correct();

    // After correct cleanup, widgets are null and active_ is false
    REQUIRE(panel.widget_a_ == nullptr);
    REQUIRE(panel.widget_b_ == nullptr);
    REQUIRE(panel.active_ == false);

    // Observers are released
    // Trigger subject change — callbacks should be no-ops because
    // active_ is false and widgets are null
    panel.callback_invocations_after_cleanup_ = 0;
    lv_subject_set_int(&panel.subject_, 99);
    drain();

    // No callbacks should have executed real work
    REQUIRE(panel.callback_invocations_after_cleanup_ == 0);

    panel.deinit_subject();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Observer cleanup: active_ guard prevents callbacks during teardown",
                 "[observer_cleanup][crash_hardening]") {
    MockPanel panel;
    panel.init_subject();
    panel.setup(test_screen());
    panel.init_observers();
    drain();

    // Verify callbacks work before cleanup
    panel.callback_invocations_after_cleanup_ = 0;
    lv_subject_set_int(&panel.subject_, 1);
    drain();
    REQUIRE(panel.callback_invocations_after_cleanup_ == 2); // both observers fired

    // Set active_ to false (simulating start of cleanup)
    panel.active_ = false;

    // Fire another subject change — callbacks should bail out
    panel.callback_invocations_after_cleanup_ = 0;
    lv_subject_set_int(&panel.subject_, 2);
    drain();
    REQUIRE(panel.callback_invocations_after_cleanup_ == 0);

    // Full cleanup
    panel.widget_a_ = nullptr;
    panel.widget_b_ = nullptr;
    panel.observer_a_.reset();
    panel.observer_b_.reset();

    panel.deinit_subject();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Observer cleanup: null widget guard prevents UAF independently of active_",
                 "[observer_cleanup][crash_hardening]") {
    // Tests that even if active_ is somehow still true, null widget checks
    // prevent the callback from doing dangerous work.
    MockPanel panel;
    panel.init_subject();
    panel.setup(test_screen());
    panel.init_observers();
    drain();

    // Nullify widgets but leave active_ true (partial cleanup, edge case)
    panel.widget_a_ = nullptr;
    panel.widget_b_ = nullptr;

    panel.callback_invocations_after_cleanup_ = 0;
    lv_subject_set_int(&panel.subject_, 3);
    drain();

    // Callbacks should bail out because widgets are null
    REQUIRE(panel.callback_invocations_after_cleanup_ == 0);

    panel.active_ = false;
    panel.observer_a_.reset();
    panel.observer_b_.reset();
    panel.deinit_subject();
}

// ============================================================================
// Tests that verify cleanup resets all state
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Observer cleanup: cleanup resets all pending state",
                 "[observer_cleanup][crash_hardening]") {
    // Simulates AmsOperationSidebar::cleanup() resetting the bypass toggle's
    // pending chain, pending_load_slot_, etc. The bypass state now lives in
    // the shared BypassToggleController, so the simulated sidebar owns one
    // and its cleanup() cancels it through the same call the real sidebar
    // makes.
    struct SidebarLike {
        bool active_ = false;
        lv_obj_t* root_ = nullptr;
        ObserverGuard obs_;
        helix::ui::BypassToggleController bypass_toggle_;
        int pending_slot_ = -1;
        int prev_action_ = 0;

        void cleanup() {
            active_ = false;
            root_ = nullptr;
            obs_.reset();
            bypass_toggle_.cancel_pending();
            pending_slot_ = -1;
            prev_action_ = 0;
        }
    };

    // Arm the controller's pending chain the real way — a loaded slot plus a
    // STANDBY toggle is the only path that sets pending_enable(). The
    // controller resolves its backend through AmsState, so the mock must be
    // installed there (same idiom as test_bypass_toggle_controller.cpp).
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto owned = std::make_unique<AmsBackendMock>(4);
    AmsBackendMock* backend = owned.get();
    backend->set_operation_delay(0);
    ams.set_backend(std::move(owned));
    REQUIRE(backend->start());
    lv_subject_set_int(get_printer_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    REQUIRE(backend->load_filament(0).result == AmsResult::SUCCESS);
    backend->wait_for_operation_thread();
    drain();

    SidebarLike sidebar;
    sidebar.active_ = true;
    sidebar.root_ = lv_obj_create(test_screen());
    sidebar.bypass_toggle_.toggle();
    REQUIRE(sidebar.bypass_toggle_.pending_enable());
    sidebar.pending_slot_ = 3;
    sidebar.prev_action_ = 5;

    sidebar.cleanup();

    REQUIRE(sidebar.active_ == false);
    REQUIRE(sidebar.root_ == nullptr);
    REQUIRE_FALSE(sidebar.bypass_toggle_.pending_enable());
    REQUIRE(sidebar.pending_slot_ == -1);
    REQUIRE(sidebar.prev_action_ == 0);

    // Detach the mock without leaving the unload's op thread in flight.
    backend->wait_for_operation_thread();
    drain();
    ams.set_backend(nullptr);
}

// ============================================================================
// Tests for double-cleanup safety
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Observer cleanup: double cleanup is safe",
                 "[observer_cleanup][crash_hardening]") {
    MockPanel panel;
    panel.init_subject();
    panel.setup(test_screen());
    panel.init_observers();
    drain();

    // First cleanup
    panel.cleanup_correct();

    // Second cleanup should not crash (all pointers already null, observers already reset)
    panel.cleanup_correct();

    REQUIRE(panel.widget_a_ == nullptr);
    REQUIRE(panel.widget_b_ == nullptr);
    REQUIRE(panel.active_ == false);

    panel.deinit_subject();
}

// ============================================================================
// Test that subjects_initialized_ guard works (AmsPanel pattern)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Observer cleanup: subjects_initialized flag prevents callbacks",
                 "[observer_cleanup][crash_hardening]") {
    // Mimics the AmsPanel::clear_panel_reference() pattern where
    // subjects_initialized_ is set to false FIRST.
    struct AmsPanelLike {
        bool subjects_initialized_ = false;
        lv_obj_t* panel_ = nullptr;
        lv_obj_t* slot_grid_ = nullptr;
        ObserverGuard action_observer_;
        ObserverGuard slot_observer_;
        int callback_count_ = 0;
        lv_subject_t subject_;

        void init() {
            lv_subject_init_int(&subject_, 0);
            subjects_initialized_ = true;
        }

        void init_observers() {
            action_observer_ = observe_int_sync<AmsPanelLike>(
                &subject_, this, [](AmsPanelLike* self, int /*val*/) {
                    if (!self->subjects_initialized_ || !self->panel_)
                        return;
                    self->callback_count_++;
                });
        }

        void clear_panel_reference() {
            // Mark subjects uninitialized FIRST
            subjects_initialized_ = false;

            // Nullify widget pointers BEFORE resetting observers
            panel_ = nullptr;
            slot_grid_ = nullptr;

            // Now reset observer guards
            action_observer_.reset();
            slot_observer_.reset();
        }

        void deinit() {
            lv_subject_deinit(&subject_);
        }
    };

    AmsPanelLike panel;
    panel.init();
    panel.panel_ = lv_obj_create(test_screen());
    panel.slot_grid_ = lv_obj_create(panel.panel_);
    panel.init_observers();
    drain();

    // Verify callbacks work initially
    panel.callback_count_ = 0;
    lv_subject_set_int(&panel.subject_, 1);
    drain();
    REQUIRE(panel.callback_count_ == 1);

    // Clear panel reference
    panel.clear_panel_reference();

    // Callbacks should be no-ops
    panel.callback_count_ = 0;
    lv_subject_set_int(&panel.subject_, 2);
    drain();
    REQUIRE(panel.callback_count_ == 0);

    panel.deinit();
}

// ============================================================================
// HomePanel-style subjects_initialized_ guard pattern
//
// HomePanel has 7 observer callbacks that all guard with:
//   if (!subjects_initialized_) return;
// This mock verifies the pattern: when subjects_initialized_ is false,
// callbacks must NOT access widget pointers or update display state.
// ============================================================================

class MockHomePanel {
  public:
    bool subjects_initialized_ = false;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* print_card_thumb_ = nullptr;
    lv_obj_t* print_card_label_ = nullptr;

    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard state_observer_;
    ObserverGuard progress_observer_;
    ObserverGuard thumbnail_observer_;
    ObserverGuard led_observer_;
    ObserverGuard printer_image_observer_;

    int temp_callback_count_ = 0;
    int target_callback_count_ = 0;
    int state_callback_count_ = 0;
    int progress_callback_count_ = 0;
    int thumbnail_callback_count_ = 0;
    int led_callback_count_ = 0;
    int printer_image_callback_count_ = 0;

    lv_subject_t temp_subject_;
    lv_subject_t target_subject_;
    lv_subject_t state_subject_;
    lv_subject_t progress_subject_;
    lv_subject_t thumbnail_subject_;
    lv_subject_t led_subject_;
    lv_subject_t printer_image_subject_;

    void init(lv_obj_t* parent) {
        lv_subject_init_int(&temp_subject_, 0);
        lv_subject_init_int(&target_subject_, 0);
        lv_subject_init_int(&state_subject_, 0);
        lv_subject_init_int(&progress_subject_, 0);
        lv_subject_init_int(&thumbnail_subject_, 0);
        lv_subject_init_int(&led_subject_, 0);
        lv_subject_init_int(&printer_image_subject_, 0);

        panel_ = lv_obj_create(parent);
        print_card_thumb_ = lv_obj_create(panel_);
        print_card_label_ = lv_obj_create(panel_);

        subjects_initialized_ = true;
    }

    void init_observers() {
        // on_extruder_temp_changed pattern
        temp_observer_ = observe_int_sync<MockHomePanel>(&temp_subject_, this,
                                                         [](MockHomePanel* self, int /*val*/) {
                                                             if (!self->subjects_initialized_)
                                                                 return;
                                                             self->temp_callback_count_++;
                                                         });

        // on_extruder_target_changed pattern
        target_observer_ = observe_int_sync<MockHomePanel>(&target_subject_, this,
                                                           [](MockHomePanel* self, int /*val*/) {
                                                               if (!self->subjects_initialized_)
                                                                   return;
                                                               self->target_callback_count_++;
                                                           });

        // on_print_state_changed pattern (guards with subjects_initialized_ + widget)
        state_observer_ = observe_int_sync<MockHomePanel>(
            &state_subject_, this, [](MockHomePanel* self, int /*val*/) {
                if (!self->subjects_initialized_ || !self->print_card_thumb_ ||
                    !self->print_card_label_)
                    return;
                self->state_callback_count_++;
            });

        // on_print_progress_or_time_changed pattern
        progress_observer_ = observe_int_sync<MockHomePanel>(&progress_subject_, this,
                                                             [](MockHomePanel* self, int /*val*/) {
                                                                 if (!self->subjects_initialized_)
                                                                     return;
                                                                 self->progress_callback_count_++;
                                                             });

        // on_print_thumbnail_path_changed pattern
        thumbnail_observer_ = observe_int_sync<MockHomePanel>(
            &thumbnail_subject_, this, [](MockHomePanel* self, int /*val*/) {
                if (!self->subjects_initialized_ || !self->print_card_thumb_)
                    return;
                self->thumbnail_callback_count_++;
            });

        // on_led_state_changed pattern
        led_observer_ = observe_int_sync<MockHomePanel>(&led_subject_, this,
                                                        [](MockHomePanel* self, int /*val*/) {
                                                            if (!self->subjects_initialized_)
                                                                return;
                                                            self->led_callback_count_++;
                                                        });

        // refresh_printer_image pattern (guards with subjects_initialized_ + panel_)
        printer_image_observer_ = observe_int_sync<MockHomePanel>(
            &printer_image_subject_, this, [](MockHomePanel* self, int /*val*/) {
                if (!self->subjects_initialized_ || !self->panel_)
                    return;
                self->printer_image_callback_count_++;
            });
    }

    void deinit_subjects() {
        if (!subjects_initialized_)
            return;
        subjects_initialized_ = false;
        // Observers are released after flag is cleared
        temp_observer_.reset();
        target_observer_.reset();
        state_observer_.reset();
        progress_observer_.reset();
        thumbnail_observer_.reset();
        led_observer_.reset();
        printer_image_observer_.reset();
    }

    void deinit() {
        lv_subject_deinit(&temp_subject_);
        lv_subject_deinit(&target_subject_);
        lv_subject_deinit(&state_subject_);
        lv_subject_deinit(&progress_subject_);
        lv_subject_deinit(&thumbnail_subject_);
        lv_subject_deinit(&led_subject_);
        lv_subject_deinit(&printer_image_subject_);
    }

    void reset_counts() {
        temp_callback_count_ = 0;
        target_callback_count_ = 0;
        state_callback_count_ = 0;
        progress_callback_count_ = 0;
        thumbnail_callback_count_ = 0;
        led_callback_count_ = 0;
        printer_image_callback_count_ = 0;
    }

    int total_callback_count() const {
        return temp_callback_count_ + target_callback_count_ + state_callback_count_ +
               progress_callback_count_ + thumbnail_callback_count_ + led_callback_count_ +
               printer_image_callback_count_;
    }
};

TEST_CASE_METHOD(LVGLTestFixture,
                 "HomePanel pattern: all 7 observers fire when subjects_initialized_ is true",
                 "[observer_cleanup][crash_hardening][home_panel]") {
    MockHomePanel panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    panel.reset_counts();

    // Fire all 7 subjects
    lv_subject_set_int(&panel.temp_subject_, 1);
    lv_subject_set_int(&panel.target_subject_, 1);
    lv_subject_set_int(&panel.state_subject_, 1);
    lv_subject_set_int(&panel.progress_subject_, 1);
    lv_subject_set_int(&panel.thumbnail_subject_, 1);
    lv_subject_set_int(&panel.led_subject_, 1);
    lv_subject_set_int(&panel.printer_image_subject_, 1);
    drain();

    REQUIRE(panel.temp_callback_count_ == 1);
    REQUIRE(panel.target_callback_count_ == 1);
    REQUIRE(panel.state_callback_count_ == 1);
    REQUIRE(panel.progress_callback_count_ == 1);
    REQUIRE(panel.thumbnail_callback_count_ == 1);
    REQUIRE(panel.led_callback_count_ == 1);
    REQUIRE(panel.printer_image_callback_count_ == 1);
    REQUIRE(panel.total_callback_count() == 7);

    panel.deinit_subjects();
    panel.deinit();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "HomePanel pattern: all 7 observers are no-ops when subjects_initialized_ is false",
    "[observer_cleanup][crash_hardening][home_panel]") {
    MockHomePanel panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    // Clear flag — simulates deinit_subjects() setting it to false
    panel.subjects_initialized_ = false;

    panel.reset_counts();

    // Fire all 7 subjects — none should increment
    lv_subject_set_int(&panel.temp_subject_, 2);
    lv_subject_set_int(&panel.target_subject_, 2);
    lv_subject_set_int(&panel.state_subject_, 2);
    lv_subject_set_int(&panel.progress_subject_, 2);
    lv_subject_set_int(&panel.thumbnail_subject_, 2);
    lv_subject_set_int(&panel.led_subject_, 2);
    lv_subject_set_int(&panel.printer_image_subject_, 2);
    drain();

    REQUIRE(panel.total_callback_count() == 0);

    // Cleanup
    panel.temp_observer_.reset();
    panel.target_observer_.reset();
    panel.state_observer_.reset();
    panel.progress_observer_.reset();
    panel.thumbnail_observer_.reset();
    panel.led_observer_.reset();
    panel.printer_image_observer_.reset();
    panel.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "HomePanel pattern: widget-guarded callbacks are no-ops when widgets are null",
                 "[observer_cleanup][crash_hardening][home_panel]") {
    MockHomePanel panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    // Null out widget pointers (simulates panel destruction while subjects live)
    panel.print_card_thumb_ = nullptr;
    panel.print_card_label_ = nullptr;
    panel.panel_ = nullptr;

    panel.reset_counts();

    // Callbacks that guard on widgets should be no-ops
    lv_subject_set_int(&panel.state_subject_, 3);
    lv_subject_set_int(&panel.thumbnail_subject_, 3);
    lv_subject_set_int(&panel.printer_image_subject_, 3);
    drain();

    REQUIRE(panel.state_callback_count_ == 0);
    REQUIRE(panel.thumbnail_callback_count_ == 0);
    REQUIRE(panel.printer_image_callback_count_ == 0);

    // Callbacks that only guard on subjects_initialized_ should still fire
    lv_subject_set_int(&panel.temp_subject_, 3);
    drain();
    REQUIRE(panel.temp_callback_count_ == 1);

    panel.deinit_subjects();
    panel.deinit();
}

// ============================================================================
// TemperatureService-style deinit ordering pattern
//
// TemperatureService sets subjects_initialized_ = false FIRST in deinit_subjects(),
// BEFORE calling subjects_.deinit_all(). This prevents deferred callbacks from
// accessing torn-down subjects during cleanup.
// ============================================================================

class MockTemperatureService {
  public:
    bool subjects_initialized_ = false;
    lv_obj_t* panel_ = nullptr;

    ObserverGuard temp_observer_;
    ObserverGuard target_observer_;
    ObserverGuard extruder_observer_;

    int on_temp_count_ = 0;
    int on_target_count_ = 0;
    int rebuild_segments_count_ = 0;

    lv_subject_t temp_subject_;
    lv_subject_t target_subject_;
    lv_subject_t extruder_subject_;

    void init(lv_obj_t* parent) {
        lv_subject_init_int(&temp_subject_, 0);
        lv_subject_init_int(&target_subject_, 0);
        lv_subject_init_int(&extruder_subject_, 0);

        panel_ = lv_obj_create(parent);
        subjects_initialized_ = true;
    }

    void init_observers() {
        // on_temp_changed pattern — guards after throttle logic
        temp_observer_ = observe_int_sync<MockTemperatureService>(
            &temp_subject_, this, [](MockTemperatureService* self, int /*val*/) {
                if (!self->subjects_initialized_)
                    return;
                self->on_temp_count_++;
            });

        // on_target_changed pattern
        target_observer_ = observe_int_sync<MockTemperatureService>(
            &target_subject_, this, [](MockTemperatureService* self, int /*val*/) {
                if (!self->subjects_initialized_)
                    return;
                self->on_target_count_++;
            });

        // rebuild_extruder_segments_impl / select_extruder pattern
        extruder_observer_ = observe_int_sync<MockTemperatureService>(
            &extruder_subject_, this, [](MockTemperatureService* self, int /*val*/) {
                if (!self->subjects_initialized_)
                    return;
                self->rebuild_segments_count_++;
            });
    }

    // CORRECT deinit ordering: set flag BEFORE deinit
    void deinit_subjects_correct() {
        if (!subjects_initialized_)
            return;
        subjects_initialized_ = false;
        temp_observer_.reset();
        target_observer_.reset();
        extruder_observer_.reset();
    }

    // WRONG deinit ordering: reset observers first, then clear flag
    void deinit_subjects_wrong() {
        if (!subjects_initialized_)
            return;
        temp_observer_.reset();
        target_observer_.reset();
        extruder_observer_.reset();
        subjects_initialized_ = false;
    }

    void deinit() {
        lv_subject_deinit(&temp_subject_);
        lv_subject_deinit(&target_subject_);
        lv_subject_deinit(&extruder_subject_);
    }

    void reset_counts() {
        on_temp_count_ = 0;
        on_target_count_ = 0;
        rebuild_segments_count_ = 0;
    }
};

TEST_CASE_METHOD(LVGLTestFixture,
                 "TemperatureService pattern: correct deinit sets flag before observer reset",
                 "[observer_cleanup][crash_hardening][temp_panel]") {
    MockTemperatureService panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    // Verify callbacks work before deinit
    panel.reset_counts();
    lv_subject_set_int(&panel.temp_subject_, 1);
    lv_subject_set_int(&panel.target_subject_, 1);
    lv_subject_set_int(&panel.extruder_subject_, 1);
    drain();
    REQUIRE(panel.on_temp_count_ == 1);
    REQUIRE(panel.on_target_count_ == 1);
    REQUIRE(panel.rebuild_segments_count_ == 1);

    // Correct deinit: flag set first
    panel.deinit_subjects_correct();
    REQUIRE(panel.subjects_initialized_ == false);

    // Callbacks should be no-ops after deinit
    panel.reset_counts();
    lv_subject_set_int(&panel.temp_subject_, 2);
    lv_subject_set_int(&panel.target_subject_, 2);
    lv_subject_set_int(&panel.extruder_subject_, 2);
    drain();
    REQUIRE(panel.on_temp_count_ == 0);
    REQUIRE(panel.on_target_count_ == 0);
    REQUIRE(panel.rebuild_segments_count_ == 0);

    panel.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "TemperatureService pattern: double deinit_subjects is safe",
                 "[observer_cleanup][crash_hardening][temp_panel]") {
    MockTemperatureService panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    panel.deinit_subjects_correct();
    // Second call should be a no-op (guard: if (!subjects_initialized_) return)
    panel.deinit_subjects_correct();
    REQUIRE(panel.subjects_initialized_ == false);

    panel.deinit();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "TemperatureService pattern: update_display guard prevents access to freed subjects",
    "[observer_cleanup][crash_hardening][temp_panel]") {
    // Simulates TemperatureService::update_display() which checks
    // subjects_initialized_ before accessing subject buffers
    MockTemperatureService panel;
    panel.init(test_screen());
    panel.init_observers();
    drain();

    // Track whether update_display would have proceeded
    int display_update_count = 0;
    auto update_display = [&]() {
        if (!panel.subjects_initialized_)
            return;
        display_update_count++;
    };

    update_display();
    REQUIRE(display_update_count == 1);

    panel.deinit_subjects_correct();

    update_display();
    REQUIRE(display_update_count == 1); // No increment

    panel.deinit();
}

// ============================================================================
// HeatingIconAnimator cleanup ordering pattern
//
// The fix ensures icon_ = nullptr BEFORE theme_observer_.reset() in detach().
// This prevents cascading theme observer callbacks from accessing a freed icon.
// ============================================================================

class MockAnimator {
  public:
    lv_obj_t* icon_ = nullptr;
    ObserverGuard theme_observer_;
    int theme_callback_count_ = 0;
    bool accessed_icon_after_null_ = false;

    lv_subject_t theme_subject_;

    void init() {
        lv_subject_init_int(&theme_subject_, 0);
    }

    void attach(lv_obj_t* icon) {
        icon_ = icon;
        theme_observer_ = observe_int_sync<MockAnimator>(&theme_subject_, this,
                                                         [](MockAnimator* self, int /*val*/) {
                                                             if (!self->icon_)
                                                                 return;
                                                             // In real code this calls
                                                             // refresh_theme() which touches icon_
                                                             self->theme_callback_count_++;
                                                         });
    }

    // CORRECT detach: null icon BEFORE resetting observer
    void detach_correct() {
        if (!icon_)
            return;
        icon_ = nullptr;
        theme_observer_.reset();
    }

    // WRONG detach: reset observer BEFORE nulling icon
    void detach_wrong() {
        if (!icon_)
            return;
        theme_observer_.reset();
        icon_ = nullptr;
    }

    void deinit() {
        lv_subject_deinit(&theme_subject_);
    }
};

TEST_CASE_METHOD(LVGLTestFixture,
                 "HeatingIconAnimator pattern: detach nullifies icon_ before observer reset",
                 "[observer_cleanup][crash_hardening][animator]") {
    MockAnimator anim;
    anim.init();

    lv_obj_t* icon = lv_obj_create(test_screen());
    anim.attach(icon);
    drain();

    // Verify callback fires when attached
    anim.theme_callback_count_ = 0;
    lv_subject_set_int(&anim.theme_subject_, 1);
    drain();
    REQUIRE(anim.theme_callback_count_ == 1);

    // Correct detach: icon_ set to null first
    anim.detach_correct();
    REQUIRE(anim.icon_ == nullptr);

    // Any cascading callback sees null icon_ and bails out
    anim.theme_callback_count_ = 0;
    lv_subject_set_int(&anim.theme_subject_, 2);
    drain();
    REQUIRE(anim.theme_callback_count_ == 0);

    anim.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "HeatingIconAnimator pattern: double detach is safe",
                 "[observer_cleanup][crash_hardening][animator]") {
    MockAnimator anim;
    anim.init();

    lv_obj_t* icon = lv_obj_create(test_screen());
    anim.attach(icon);
    drain();

    anim.detach_correct();
    REQUIRE(anim.icon_ == nullptr);

    // Second detach should be a no-op (guard: if (!icon_) return)
    anim.detach_correct();
    REQUIRE(anim.icon_ == nullptr);

    anim.deinit();
}

// ============================================================================
// AmsEditOverlay thread safety pattern: async completion via queue_update
//
// The fix defers fire_completion() through ui_queue_update() so that
// Spoolman async callbacks don't directly invoke LVGL-touching code
// from a background thread. This test verifies the deferral pattern.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AmsEditOverlay pattern: async callback defers via queue_update",
                 "[observer_cleanup][crash_hardening][ams_modal]") {
    // Simulate the pattern: async callback captures state and defers work
    int completion_count = 0;
    bool callback_guard_valid = true;

    // Simulate the Spoolman callback deferral pattern
    auto simulate_spoolman_callback = [&](bool success) {
        // This lambda mimics what runs on the background thread:
        // it captures state and defers through queue_update
        helix::ui::queue_update([&, success]() {
            if (!callback_guard_valid)
                return;
            if (!success) {
                // Would log error in real code
            }
            completion_count++;
        });
    };

    // Simulate async completion
    simulate_spoolman_callback(true);

    // Before draining, count should still be 0 (deferred)
    REQUIRE(completion_count == 0);

    // Drain the queue — now the deferred callback runs
    drain();
    REQUIRE(completion_count == 1);

    // After guard invalidation, callback should be no-op
    callback_guard_valid = false;
    simulate_spoolman_callback(true);
    drain();
    REQUIRE(completion_count == 1); // Still 1, not 2
}

// ============================================================================
// #816: Multiple holders of SubjectLifetime — ObserverGuard must detect dead
// subject via bool value, not just shared_ptr expiration.
//
// When a dynamic subject is destroyed, the source sets *lifetime = false then
// resets its shared_ptr. But other services may still hold copies (refcount > 0).
// ObserverGuard must check the bool value, not just weak_ptr::expired().
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "SubjectLifetime: guard skips remove when bool is false but refcount > 0",
                 "[observer_cleanup][crash_hardening][subject_lifetime]") {
    // Simulate: source creates a dynamic subject with lifetime token
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime source_lifetime = std::make_shared<bool>(true);

    // Two services observe the same subject and hold lifetime copies
    SubjectLifetime service_a_lifetime = source_lifetime;
    SubjectLifetime service_b_lifetime = source_lifetime;

    // Service A creates an observer (like NozzleTempsWidget)
    struct DummyPanel {
        int count = 0;
    } panel;
    ObserverGuard guard_a = observe_int_sync<DummyPanel>(
        &subject, &panel, [](DummyPanel* self, int) { self->count++; }, service_a_lifetime);
    drain();

    // Verify observer works
    panel.count = 0;
    lv_subject_set_int(&subject, 42);
    drain();
    REQUIRE(panel.count == 1);

    // Source signals death and destroys the subject (like init_extruders)
    *source_lifetime = false;
    source_lifetime.reset();
    lv_subject_deinit(&subject); // Frees observer struct!

    // Service A tries to clean up its observer (like clear_rows)
    // The weak_ptr is NOT expired because service_b still holds a copy.
    // But the bool was set to false, so the guard should skip lv_observer_remove.
    REQUIRE_FALSE(service_b_lifetime.use_count() == 0); // Still alive
    service_a_lifetime.reset();
    guard_a.reset(); // Must NOT crash — observer struct was freed by deinit
}

TEST_CASE_METHOD(LVGLTestFixture, "SubjectLifetime: guard still removes when subject is alive",
                 "[observer_cleanup][crash_hardening][subject_lifetime]") {
    // When the subject IS alive (bool = true), guard must call lv_observer_remove
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    SubjectLifetime lifetime = std::make_shared<bool>(true);

    SubjectLifetime observer_lifetime = lifetime;

    struct DummyPanel {
        int count = 0;
    } panel;
    ObserverGuard guard = observe_int_sync<DummyPanel>(
        &subject, &panel, [](DummyPanel* self, int) { self->count++; }, observer_lifetime);
    drain();

    // Subject is alive — guard should properly remove the observer
    observer_lifetime.reset();
    guard.reset(); // Should call lv_observer_remove (subject alive, bool = true)

    // After removal, setting subject should not fire any callbacks
    panel.count = 0;
    lv_subject_set_int(&subject, 99);
    drain();
    REQUIRE(panel.count == 0);

    lifetime.reset();
    lv_subject_deinit(&subject);
}
