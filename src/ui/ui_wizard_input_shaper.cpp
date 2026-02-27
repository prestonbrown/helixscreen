// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_input_shaper.h"

#include "ui_update_queue.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "calibration_types.h"
#include "input_shaper_calibrator.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>
#include <string>

using helix::calibration::InputShaperCalibrator;

// External wizard subjects (defined in ui_wizard.cpp)
extern lv_subject_t wizard_show_skip;
extern lv_subject_t connection_test_passed;

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardInputShaperStep> g_wizard_input_shaper_step;

WizardInputShaperStep* get_wizard_input_shaper_step() {
    if (!g_wizard_input_shaper_step) {
        g_wizard_input_shaper_step = std::make_unique<WizardInputShaperStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardInputShaperStep", []() { g_wizard_input_shaper_step.reset(); });
    }
    return g_wizard_input_shaper_step.get();
}

void destroy_wizard_input_shaper_step() {
    g_wizard_input_shaper_step.reset();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardInputShaperStep::WizardInputShaperStep()
    : calibrator_(std::make_unique<InputShaperCalibrator>(get_moonraker_api())) {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardInputShaperStep::~WizardInputShaperStep() {
    // Mark as dead to prevent any pending callbacks from executing
    if (alive_) {
        alive_->store(false, std::memory_order_release);
    }

    // Deinitialize subjects to disconnect observers before destruction
    // NOTE: lv_subject_deinit() is safe to call even during shutdown
    if (subjects_initialized_) {
        lv_subject_deinit(&calibration_status_);
        lv_subject_deinit(&calibration_progress_);
        lv_subject_deinit(&calibration_started_);
        subjects_initialized_ = false;
    }

    // NOTE: Do NOT log here - spdlog may be destroyed first
    screen_root_ = nullptr;
}

// ============================================================================
// ============================================================================
// Subject Initialization
// ============================================================================

void WizardInputShaperStep::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize status subject with string buffer
    strncpy(status_buffer_, "Ready to calibrate", sizeof(status_buffer_) - 1);
    status_buffer_[sizeof(status_buffer_) - 1] = '\0';
    lv_subject_init_string(&calibration_status_, status_buffer_, nullptr, sizeof(status_buffer_),
                           status_buffer_);
    lv_xml_register_subject(nullptr, "wizard_input_shaper_status", &calibration_status_);

    // Initialize progress subject
    helix::ui::wizard::init_int_subject(&calibration_progress_, 0, "wizard_input_shaper_progress");

    // Initialize started subject (controls Start button and skip hint visibility)
    helix::ui::wizard::init_int_subject(&calibration_started_, 0, "wizard_input_shaper_started");

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

// Helper to safely update subjects from async callbacks
// Captures alive flag and queues update to UI thread
static void safe_update_status(std::weak_ptr<std::atomic<bool>> alive_weak,
                               const std::string& msg) {
    helix::ui::queue_update([alive_weak, msg]() {
        if (auto alive = alive_weak.lock()) {
            if (!alive->load(std::memory_order_acquire)) {
                return; // Step was cleaned up
            }
        } else {
            return; // Alive flag destroyed
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            lv_subject_copy_string(step->get_status_subject(), msg.c_str());
        }
    });
}

static void safe_update_progress(std::weak_ptr<std::atomic<bool>> alive_weak, int progress) {
    helix::ui::queue_update([alive_weak, progress]() {
        if (auto alive = alive_weak.lock()) {
            if (!alive->load(std::memory_order_acquire)) {
                return; // Step was cleaned up
            }
        } else {
            return; // Alive flag destroyed
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            lv_subject_set_int(step->get_progress_subject(), progress);
        }
    });
}

static void safe_set_complete(std::weak_ptr<std::atomic<bool>> alive_weak) {
    helix::ui::queue_update([alive_weak]() {
        if (auto alive = alive_weak.lock()) {
            if (!alive->load(std::memory_order_acquire)) {
                return; // Step was cleaned up
            }
        } else {
            return; // Alive flag destroyed
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            lv_subject_copy_string(step->get_status_subject(), lv_tr("Calibration complete!"));
            lv_subject_set_int(step->get_progress_subject(), 100);
            step->set_calibration_complete(true);

            // Enable wizard Next button (connection_test_passed controls disabled state)
            lv_subject_set_int(&connection_test_passed, 1);
        }
    });
}

static void safe_handle_error(std::weak_ptr<std::atomic<bool>> alive_weak) {
    helix::ui::queue_update([alive_weak]() {
        if (auto alive = alive_weak.lock()) {
            if (!alive->load(std::memory_order_acquire)) {
                return;
            }
        } else {
            return;
        }
        // On error: switch footer back to Skip so user can proceed past the step
        lv_subject_set_int(&connection_test_passed, 1);
        lv_subject_set_int(&wizard_show_skip, 1);
    });
}

// Static trampolines for LVGL callbacks
static void on_start_calibration_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Wizard Input Shaper] Start calibration clicked");
    WizardInputShaperStep* step = get_wizard_input_shaper_step();
    if (!step) {
        return;
    }

    // Hide Start button and skip hint via subject binding
    lv_subject_set_int(step->get_started_subject(), 1);

    // Switch footer from Skip to Next (disabled during calibration)
    lv_subject_set_int(&wizard_show_skip, 0);
    lv_subject_set_int(&connection_test_passed, 0);

    // Update status (already on UI thread, so direct call is safe)
    lv_subject_copy_string(step->get_status_subject(), lv_tr("Checking accelerometer..."));
    lv_subject_set_int(step->get_progress_subject(), 0);

    // Capture alive flag as weak_ptr for async callbacks
    std::weak_ptr<std::atomic<bool>> alive_weak = step->get_alive_flag();

    // Start calibration via calibrator
    InputShaperCalibrator* calibrator = step->get_calibrator();
    if (calibrator) {
        calibrator->check_accelerometer(
            [alive_weak](float noise_level) {
                // Noise check passed - continue to X axis calibration
                spdlog::info("[Wizard Input Shaper] Noise check passed: {:.2f}", noise_level);
                safe_update_status(alive_weak, "Calibrating X axis...");

                WizardInputShaperStep* step = get_wizard_input_shaper_step();
                if (!step) {
                    return;
                }
                InputShaperCalibrator* cal = step->get_calibrator();
                if (cal) {
                    cal->run_calibration(
                        'X',
                        [alive_weak](int percent) {
                            safe_update_progress(alive_weak, percent / 2);
                        },
                        [alive_weak](const InputShaperResult& result) {
                            (void)result;
                            spdlog::info("[Wizard Input Shaper] X axis complete");
                            safe_update_status(alive_weak, "Calibrating Y axis...");

                            // Run Y axis calibration
                            WizardInputShaperStep* step = get_wizard_input_shaper_step();
                            if (!step) {
                                return;
                            }
                            InputShaperCalibrator* cal2 = step->get_calibrator();
                            if (cal2) {
                                cal2->run_calibration(
                                    'Y',
                                    [alive_weak](int percent) {
                                        safe_update_progress(alive_weak, 50 + percent / 2);
                                    },
                                    [alive_weak](const InputShaperResult& result) {
                                        (void)result;
                                        spdlog::info("[Wizard Input Shaper] Y axis complete");
                                        safe_set_complete(alive_weak);
                                    },
                                    [alive_weak](const std::string& error) {
                                        spdlog::error("[Wizard Input Shaper] Y axis error: {}",
                                                      error);
                                        safe_update_status(alive_weak, error);
                                        safe_update_progress(alive_weak, 0);
                                        safe_handle_error(alive_weak);
                                    });
                            }
                        },
                        [alive_weak](const std::string& error) {
                            spdlog::error("[Wizard Input Shaper] X axis error: {}", error);
                            safe_update_status(alive_weak, error);
                            safe_update_progress(alive_weak, 0);
                            safe_handle_error(alive_weak);
                        });
                }
            },
            [alive_weak](const std::string& error) {
                spdlog::error("[Wizard Input Shaper] Accelerometer check failed: {}", error);
                safe_update_status(alive_weak, error);
                safe_update_progress(alive_weak, 0);
                safe_handle_error(alive_weak);
            });
    }
}

void WizardInputShaperStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_start_is_calibration", on_start_calibration_clicked);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardInputShaperStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating input shaper screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Create screen from XML
    screen_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_input_shaper", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    // Show "Skip" in footer (user can skip calibration)
    lv_subject_set_int(&wizard_show_skip, 1);

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardInputShaperStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // Mark as dead FIRST to prevent callbacks from updating subjects
    if (alive_) {
        alive_->store(false, std::memory_order_release);
    }

    // Cancel any in-progress calibration
    if (calibrator_) {
        calibrator_->cancel();
    }

    // Reset footer subjects for next step
    lv_subject_set_int(&wizard_show_skip, 0);
    lv_subject_set_int(&connection_test_passed, 1);

    // Reset UI references
    // Note: Do NOT call lv_obj_del() here - the wizard framework handles
    // object deletion when clearing wizard_content container
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardInputShaperStep::is_validated() const {
    // Validated if calibration completed OR user explicitly skipped
    return calibration_complete_ || user_skipped_;
}

// ============================================================================
// Skip Logic
// ============================================================================

bool WizardInputShaperStep::should_skip() const {
    bool has_accel = has_accelerometer();

    if (!has_accel) {
        spdlog::info("[{}] No accelerometer detected, skipping step", get_name());
    } else {
        spdlog::debug("[{}] Accelerometer detected, showing step", get_name());
    }

    return !has_accel;
}

// ============================================================================
// Accelerometer Detection
// ============================================================================

bool WizardInputShaperStep::has_accelerometer() const {
    // Query the printer_has_accelerometer subject
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "printer_has_accelerometer");
    if (!subject) {
        spdlog::debug("[{}] printer_has_accelerometer subject not found", get_name());
        return false;
    }

    return lv_subject_get_int(subject) != 0;
}

// ============================================================================
// Calibrator Access
// ============================================================================

InputShaperCalibrator* WizardInputShaperStep::get_calibrator() {
    if (!calibrator_) {
        calibrator_ = std::make_unique<InputShaperCalibrator>(get_moonraker_api());
    }
    return calibrator_.get();
}
