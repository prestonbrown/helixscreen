// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_input_shaper.h"

#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_update_queue.h"
#include "ui_wizard_helpers.h"

#include "app_globals.h"
#include "calibration_types.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "input_shaper_calibrator.h"
#include "lvgl/lvgl.h"
#include "memory_utils.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/fmt/fmt.h>
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

WizardInputShaperStep* get_wizard_input_shaper_step() {
    return &helix::lazy_global<WizardInputShaperStep>("WizardInputShaperStep");
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardInputShaperStep::WizardInputShaperStep()
    : calibrator_(std::make_unique<InputShaperCalibrator>(get_moonraker_api())) {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardInputShaperStep::~WizardInputShaperStep() {
    // Stop the elapsed timer before the subject it refreshes goes away (the
    // helper's own cancel-safe destructor would too; doing it here keeps the
    // ordering explicit).
    cancel_analysis_display();

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
    UI_MANAGED_SUBJECT_STRING(calibration_status_, status_buffer_, status_buffer_,
                              "wizard_input_shaper_status", subjects_);

    // Initialize progress subject
    UI_MANAGED_SUBJECT_INT(calibration_progress_, 0, "wizard_input_shaper_progress", subjects_);

    // Initialize started subject (controls Start button and skip hint visibility)
    UI_MANAGED_SUBJECT_INT(calibration_started_, 0, "wizard_input_shaper_started", subjects_);

    // Initialize active subject (controls Cancel button visibility — 1 only while
    // calibration is in flight; cleared on complete / cancel / error)
    UI_MANAGED_SUBJECT_INT(calibration_active_, 0, "wizard_input_shaper_active", subjects_);

    // Initialize indeterminate subject (1 during the offline analysis phase:
    // hides the bar, shows the spinner)
    UI_MANAGED_SUBJECT_INT(calibration_indeterminate_, 0, "wizard_input_shaper_indeterminate",
                           subjects_);

    // 1 while the live chart holds a curve (shows its host)
    UI_MANAGED_SUBJECT_INT(has_chart_, 0, "wizard_input_shaper_has_chart", subjects_);
    sweep_chart_.set_on_curve_changed(
        [this](bool has_curve) { lv_subject_set_int(&has_chart_, has_curve ? 1 : 0); });

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

// Helper to safely update subjects from async callbacks
// Captures alive flag and queues update to UI thread. The translation lookup
// runs INSIDE the queued lambda: these callbacks fire on the libhv WebSocket
// thread, and lv_tr() touches LVGL state (threading rule 1). Unregistered
// keys fall back to the key itself, so plain untranslated strings pass through
// unchanged.
static void safe_update_status(helix::LifetimeToken token, const std::string& msg_key) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_update_status", [token, msg_key]() {
        if (token.expired()) {
            return; // Step was cleaned up
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            lv_subject_copy_string(step->get_status_subject(), lv_tr(msg_key.c_str()));
        }
    });
}

// Switches the progress area between the determinate bar and the analysis
// treatment (spinner + "Analyzing data... Ns" elapsed label). Queued for the
// same threading reason as safe_update_status.
static void safe_set_analysis_phase(helix::LifetimeToken token, bool analyzing) {
    helix::ui::queue_update(
        "ui_wizard_input_shaper::safe_set_analysis_phase", [token, analyzing]() {
            if (token.expired()) {
                return;
            }
            WizardInputShaperStep* step = get_wizard_input_shaper_step();
            if (step) {
                lv_subject_set_int(step->get_indeterminate_subject(), analyzing ? 1 : 0);
                if (analyzing) {
                    step->begin_analysis_display();
                } else {
                    step->cancel_analysis_display();
                }
            }
        });
}

// The live chart's calls, queued like the rest: progress and results arrive on
// the WebSocket thread, and the chart is LVGL. Queued in callback order, so an
// axis's begin_axis_preview lands ahead of its first progress report.
static void safe_begin_axis_preview(helix::LifetimeToken token, char axis) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_begin_axis_preview", [token, axis]() {
        if (token.expired()) {
            return;
        }
        if (WizardInputShaperStep* step = get_wizard_input_shaper_step()) {
            step->begin_axis_preview(axis);
        }
    });
}

static void safe_axis_progress(helix::LifetimeToken token, ShaperCalibrationPhase phase,
                               float freq_hz) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_axis_progress",
                            [token, phase, freq_hz]() {
                                if (token.expired()) {
                                    return;
                                }
                                if (WizardInputShaperStep* step = get_wizard_input_shaper_step()) {
                                    step->on_axis_progress(phase, freq_hz);
                                }
                            });
}

static void safe_axis_result(helix::LifetimeToken token, const InputShaperResult& result) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_axis_result", [token, result]() {
        if (token.expired()) {
            return;
        }
        if (WizardInputShaperStep* step = get_wizard_input_shaper_step()) {
            step->on_axis_result(result);
        }
    });
}

static void safe_update_progress(helix::LifetimeToken token, int progress) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_update_progress", [token, progress]() {
        if (token.expired()) {
            return; // Step was cleaned up
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            lv_subject_set_int(step->get_progress_subject(), progress);
        }
    });
}

static void safe_set_complete(helix::LifetimeToken token) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_set_complete", [token]() {
        if (token.expired()) {
            return; // Step was cleaned up
        }
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            // Defensive: complete can be reported without a trailing
            // determinate progress tick, so stop any armed elapsed label.
            step->cancel_analysis_display();
            lv_subject_set_int(step->get_indeterminate_subject(), 0);
            lv_subject_copy_string(step->get_status_subject(), lv_tr("Calibration complete!"));
            lv_subject_set_int(step->get_progress_subject(), 100);
            // Calibration finished — hide the Cancel button
            lv_subject_set_int(step->get_active_subject(), 0);
            step->set_calibration_complete(true);

            // Enable wizard Next button (connection_test_passed controls disabled state)
            lv_subject_set_int(&connection_test_passed, 1);
        }
    });
}

static void safe_handle_error(helix::LifetimeToken token) {
    helix::ui::queue_update("ui_wizard_input_shaper::safe_handle_error", [token]() {
        if (token.expired()) {
            return;
        }
        // On error: switch footer back to Skip so user can proceed past the step,
        // and hide the Cancel button (nothing to cancel anymore).
        WizardInputShaperStep* step = get_wizard_input_shaper_step();
        if (step) {
            // An error can land mid-analysis; stop the elapsed label before it
            // overwrites the error message on its next tick, and drop the spinner.
            step->cancel_analysis_display();
            step->stop_preview();
            lv_subject_set_int(step->get_indeterminate_subject(), 0);
            lv_subject_set_int(step->get_active_subject(), 0);
            lv_subject_set_int(step->get_started_subject(), 0);
        }
        lv_subject_set_int(&connection_test_passed, 1);
        lv_subject_set_int(&wizard_show_skip, 1);
    });
}

// Runs the accelerometer noise check + X/Y calibration chain. Split out from
// on_start_calibration_clicked so the low-RAM warning can gate entry (flipping
// the wizard into its "calibrating" visual state) without duplicating the flow.
static void begin_is_calibration_flow(WizardInputShaperStep* step) {
    // Hide Start button and skip hint via subject binding
    lv_subject_set_int(step->get_started_subject(), 1);
    // Mark calibration in-flight — surfaces the Cancel button
    lv_subject_set_int(step->get_active_subject(), 1);

    // Switch footer from Skip to Next (disabled during calibration)
    lv_subject_set_int(&wizard_show_skip, 0);
    lv_subject_set_int(&connection_test_passed, 0);

    // Update status (already on UI thread, so direct call is safe)
    lv_subject_copy_string(step->get_status_subject(), lv_tr("Checking accelerometer..."));
    lv_subject_set_int(step->get_progress_subject(), 0);

    auto token = step->get_lifetime_token();
    step->query_preview_facts();

    // Start calibration via calibrator
    InputShaperCalibrator* calibrator = step->get_calibrator();
    if (calibrator) {
        calibrator->check_accelerometer(
            [token](float noise_level) {
                // Token expired = user backed out of the step. The calibrator's
                // cancel() can't stop Klipper's gcode chain (it only resets local
                // state), so we must short-circuit here to prevent kicking off
                // SHAPER_CALIBRATE X (and the cascading Y test) after cleanup.
                if (token.expired()) {
                    spdlog::info(
                        "[Wizard Input Shaper] Noise check returned after cleanup, aborting chain");
                    return;
                }
                // Noise check passed - continue to X axis calibration
                spdlog::info("[Wizard Input Shaper] Noise check passed: {:.2f}", noise_level);
                safe_update_status(token, "Calibrating X axis...");

                WizardInputShaperStep* step = get_wizard_input_shaper_step();
                if (!step) {
                    return;
                }
                InputShaperCalibrator* cal = step->get_calibrator();
                if (cal) {
                    safe_begin_axis_preview(token, 'X');
                    cal->run_calibration(
                        'X',
                        [token](int percent, ShaperCalibrationPhase phase, float freq_hz) {
                            safe_axis_progress(token, phase, freq_hz);
                            const int bar =
                                WizardInputShaperStep::combined_bar_value(percent, phase, false);
                            if (bar < 0) {
                                // Analysis reports no percent: spinner on, and
                                // the elapsed label keeps the step visibly alive.
                                safe_set_analysis_phase(token, true);
                                return;
                            }
                            safe_set_analysis_phase(token, false);
                            safe_update_progress(token, bar);
                        },
                        [token](const InputShaperResult& result) {
                            if (token.expired()) {
                                spdlog::info("[Wizard Input Shaper] X axis returned after "
                                             "cleanup, aborting chain");
                                return;
                            }
                            spdlog::info("[Wizard Input Shaper] X axis complete");
                            safe_axis_result(token, result);
                            safe_update_status(token, "Calibrating Y axis...");

                            // Run Y axis calibration
                            WizardInputShaperStep* step = get_wizard_input_shaper_step();
                            if (!step) {
                                return;
                            }
                            InputShaperCalibrator* cal2 = step->get_calibrator();
                            if (cal2) {
                                safe_begin_axis_preview(token, 'Y');
                                cal2->run_calibration(
                                    'Y',
                                    [token](int percent, ShaperCalibrationPhase phase,
                                            float freq_hz) {
                                        safe_axis_progress(token, phase, freq_hz);
                                        const int bar = WizardInputShaperStep::combined_bar_value(
                                            percent, phase, true);
                                        if (bar < 0) {
                                            safe_set_analysis_phase(token, true);
                                            return;
                                        }
                                        safe_set_analysis_phase(token, false);
                                        safe_update_progress(token, bar);
                                    },
                                    [token](const InputShaperResult& result) {
                                        spdlog::info("[Wizard Input Shaper] Y axis complete");
                                        safe_axis_result(token, result);
                                        safe_set_complete(token);
                                    },
                                    [token](const std::string& error) {
                                        spdlog::error("[Wizard Input Shaper] Y axis error: {}",
                                                      error);
                                        safe_update_status(token, error);
                                        safe_update_progress(token, 0);
                                        safe_handle_error(token);
                                    });
                            }
                        },
                        [token](const std::string& error) {
                            spdlog::error("[Wizard Input Shaper] X axis error: {}", error);
                            safe_update_status(token, error);
                            safe_update_progress(token, 0);
                            safe_handle_error(token);
                        });
                }
            },
            [token](const std::string& error) {
                spdlog::error("[Wizard Input Shaper] Accelerometer check failed: {}", error);
                safe_update_status(token, error);
                safe_update_progress(token, 0);
                safe_handle_error(token);
            });
    }
}

// Static trampolines for LVGL callbacks
static void on_start_calibration_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Wizard Input Shaper] Start calibration clicked");
    WizardInputShaperStep* step = get_wizard_input_shaper_step();
    if (!step) {
        return;
    }

    // On memory-constrained hosts, warn before entering the calibrating state so
    // the wizard doesn't flip its visuals if the user cancels.
    auto mem = helix::get_system_memory_info();
    const auto verdict = helix::resonance_memory_check(mem);
    if (verdict == helix::ResonanceMemory::REFUSE) {
        helix::ui::show_resonance_memory_refusal(mem.headroom_mb());
        return; // the Start button stays, as after declining the warning
    }
    if (verdict == helix::ResonanceMemory::WARN) {
        // Re-entry guard: a second entry while the warning modal is open is a no-op.
        if (step->low_ram_warn_dialog_)
            return;
        // The helper owns the re-entry scaffold: it clears the step's stored
        // handle on every close path. The token ties all three callbacks to
        // the step so one torn down while the dialog is up is not called back.
        helix::ui::ConfirmOptions opts;
        opts.on_cancel = []() {
            // User backed out - leave the wizard step as-is (Start still visible).
        };
        opts.owner_token = step->get_lifetime_token();
        helix::ui::show_low_ram_resonance_warning(
            mem.total_mb(), &step->low_ram_warn_dialog_,
            [step]() { begin_is_calibration_flow(step); }, opts);
        if (!step->low_ram_warn_dialog_) {
            // Modal failed to build — don't silently block calibration.
            begin_is_calibration_flow(step);
        }
        return;
    }

    begin_is_calibration_flow(step);
}

// Cancel button visible during in-progress calibration. Routes through
// abort_in_progress_calibration() which sends M112 + firmware_restart so
// Klipper actually stops the SHAPER_CALIBRATE macro (cancel() alone only
// resets local state — the macro blocks the gcode queue, so the printer
// keeps sweeping until M112).
static void on_cancel_calibration_clicked(lv_event_t* e) {
    (void)e;
    spdlog::info("[Wizard Input Shaper] Cancel clicked");
    WizardInputShaperStep* step = get_wizard_input_shaper_step();
    if (!step) {
        return;
    }
    step->abort_in_progress_calibration();
}

void WizardInputShaperStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_start_is_calibration", on_start_calibration_clicked);
    lv_xml_register_event_cb(nullptr, "on_cancel_is_calibration", on_cancel_calibration_clicked);
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

void WizardInputShaperStep::begin_analysis_display() {
    analysis_elapsed_.begin(&calibration_status_, [](uint32_t elapsed_seconds) {
        return fmt::format(lv_tr("Analyzing data... {}s"), elapsed_seconds);
    });
}

void WizardInputShaperStep::cancel_analysis_display() {
    analysis_elapsed_.cancel();
}

void WizardInputShaperStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // Dismiss the low-RAM warning modal if still open — its callbacks capture
    // this step and would otherwise re-enter calibration on a torn-down wizard.
    if (low_ram_warn_dialog_) {
        helix::ui::modal_hide(low_ram_warn_dialog_);
        low_ram_warn_dialog_ = nullptr;
    }

    // If calibration is mid-flight on the printer, send M112 + firmware_restart
    // so Klipper actually stops. cancel() alone only resets local state — the
    // macro blocks the gcode queue, leaving the sweep running after Back.
    bool aborted = false;
    if (calibrator_ && calibrator_->is_active()) {
        aborted = abort_in_progress_calibration();
    }

    // Invalidate lifetime to prevent callbacks from updating subjects
    // (abort_in_progress_calibration already invalidated, but invalidate
    //  is idempotent and we still need it on the non-active path).
    lifetime_.invalidate();

    // Stop the analysis elapsed timer and drop the spinner before the
    // subjects it drives are reset below.
    cancel_analysis_display();
    lv_subject_set_int(&calibration_indeterminate_, 0);

    // The chart lives in the screen the wizard framework is about to delete.
    sweep_chart_.destroy();
    if (subjects_initialized_) {
        lv_subject_set_int(&has_chart_, 0);
    }

    // Cancel any in-progress calibration (local state — covers the non-active
    // path as well as defensively redundant when aborted above).
    if (calibrator_ && !aborted) {
        calibrator_->cancel();
    }

    // If calibration didn't complete, reset step-local UI state so a
    // subsequent visit (e.g., back → forward) starts fresh: Start button
    // visible, progress cleared, status reset. Without this,
    // calibration_started_ stays at 1 and the XML bind_flag_if_eq keeps the
    // Start button hidden, leaving the user only able to Skip the step.
    // When calibration has completed we keep the subjects so re-entry shows
    // the completion summary.
    if (subjects_initialized_ && !calibration_complete_) {
        lv_subject_set_int(&calibration_started_, 0);
        lv_subject_set_int(&calibration_progress_, 0);
        lv_subject_copy_string(&calibration_status_, "Ready to calibrate");
    }
    // Always clear active on cleanup (whether complete or not — nothing is running anymore)
    if (subjects_initialized_) {
        lv_subject_set_int(&calibration_active_, 0);
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
// Abort in-progress calibration
// ============================================================================

bool WizardInputShaperStep::abort_in_progress_calibration() {
    if (!calibrator_ || !calibrator_->is_active()) {
        return false;
    }

    spdlog::info("[{}] Aborting in-progress calibration (M112 + firmware_restart)", get_name());

    // Suppress shutdown/disconnect modals — M112 + restart triggers expected reconnect
    EmergencyStopOverlay::instance().suppress_recovery_dialog(helix::recovery_suppression::LONG);
    if (auto* api = get_moonraker_api()) {
        api->suppress_disconnect_modal(15000);
    }

    // Discard any in-flight async callbacks (token expires; chain bails out
    // when the X-axis collector eventually fires)
    lifetime_.invalidate();

    calibrator_->emergency_abort();
    stop_preview();

    // Reset UI back to start-able state. Step is still on-screen; user can
    // Skip via footer or click Start to retry once Klipper is back.
    if (subjects_initialized_) {
        lv_subject_set_int(&calibration_started_, 0);
        lv_subject_set_int(&calibration_active_, 0);
        lv_subject_set_int(&calibration_progress_, 0);
        lv_subject_copy_string(&calibration_status_, lv_tr("Canceled"));
    }
    lv_subject_set_int(&wizard_show_skip, 1);
    lv_subject_set_int(&connection_test_passed, 0);

    return true;
}

// ============================================================================
// Live sweep chart
// ============================================================================

void WizardInputShaperStep::query_preview_facts() {
    helix::IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    auto token = get_lifetime_token();
    helix::calibration::query_live_sweep_facts(
        *api, [this, token](helix::calibration::LiveSweepFacts facts) {
            token.defer(
                "WizardInputShaperStep::preview_facts",
                [this, facts = std::move(facts)]() mutable { preview_facts_ = std::move(facts); });
        });
}

void WizardInputShaperStep::begin_axis_preview(char axis) {
    lv_obj_t* host = screen_root_
                         ? lv_obj_find_by_name(screen_root_, "wizard_input_shaper_chart_host")
                         : nullptr;
    // Where the layout hides the chart, nothing streams: no target arms nothing.
    const bool fits =
        helix::ui::live_chart_layout_fits("wizard_input_shaper", "wizard_input_shaper_chart_fits");
    const auto target = fits ? helix::calibration::live_sweep_target(
                                   preview_facts_, axis == 'X' ? helix::calibration::SweepAxis::X
                                                               : helix::calibration::SweepAxis::Y)
                             : std::nullopt;
    if (!sweep_chart_.begin(host, axis, target)) {
        spdlog::debug("[{}] No live chart for axis {}", get_name(), axis);
    }
}

void WizardInputShaperStep::on_axis_progress(ShaperCalibrationPhase phase, float freq_hz) {
    sweep_chart_.on_progress(phase, freq_hz);
}

void WizardInputShaperStep::on_axis_result(const InputShaperResult& result) {
    sweep_chart_.show_curve(result.freq_response);
}

void WizardInputShaperStep::stop_preview() {
    sweep_chart_.clear();
    if (subjects_initialized_) {
        lv_subject_set_int(&has_chart_, 0);
    }
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

bool WizardInputShaperStep::should_skip(const helix::wizard::StepContext& ctx) const {
    // Preset printers normally skip hardware calibration — but a factory image
    // can force a one-time resonance run by setting the per-printer flag
    // `initial_resonance_compensation_run` to false. When the flag is true or
    // absent (the default), honor the preset skip. When explicitly false, show
    // the step even under a preset so calibration runs once.
    if (ctx.preset.skip_hardware) {
        helix::Config* cfg = ctx.config ? ctx.config : helix::Config::get_instance();
        bool run_resonance =
            cfg ? cfg->get<bool>(cfg->df() + "initial_resonance_compensation_run", true) : true;
        if (run_resonance) {
            spdlog::debug("[{}] Preset skip_hardware and resonance run already done/default — "
                          "skipping step",
                          get_name());
            return true;
        }
        spdlog::info("[{}] Preset skip_hardware but initial_resonance_compensation_run=false — "
                     "forcing calibration step",
                     get_name());
        return false;
    }

    // No preset: fall back to the accelerometer-presence check.
    return should_skip();
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
