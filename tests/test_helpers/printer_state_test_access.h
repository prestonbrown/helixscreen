// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "capability_overrides.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "update_queue_test_access.h"

#include <mutex>

namespace helix {

class PrinterPrintStateTestAccess {
  public:
    static void reset_extra(PrinterPrintState& pps) {
        pps.estimated_print_time_ = 0;
        // File-scoped slice geometry + per-print Z cache used by the Z-height
        // layer derivation. Like estimated_print_time_ these survive
        // reset_for_new_print() in production; the test reset() simulates a fresh
        // session on the shared singleton, so clear them for cross-test isolation.
        pps.layer_height_ = 0.0;
        pps.first_layer_height_ = 0.0;
        pps.last_gcode_z_mm_ = 0.0;
        pps.have_gcode_z_ = false;
        pps.layer_z_derived_ = false;
        pps.has_real_layer_data_ = false;
        // Sticky printer capability — session-scoped in production (survives
        // reset_for_new_print, cleared only on a fresh session). The test
        // reset() simulates a fresh session on the shared PrinterState
        // singleton, so clear it here for cross-test isolation.
        pps.printer_reports_layers_ = false;
        pps.slicer_progress_ = 0.0;
        pps.slicer_progress_active_ = false;
        pps.smoothed_remaining_ = 0.0;
        pps.has_smoothed_remaining_ = false;
        pps.sdcard_active_ = false;
        // The job being prepared is session-scoped: it outlives
        // reset_for_new_print() by design, since it exists precisely for the
        // window before the printer reports the job. Leaving it set leaks a
        // live preparing job into the next test, where it relaxes the
        // phase-update guard and the print_active safety reset.
        pps.preparing_job_ = {};
        pps.cancel_preparing_watchdog();
    }

    /// Fire the preparing-job watchdog as if its timer had elapsed.
    ///
    /// The real bound is half an hour - longer than the slowest legitimate
    /// pre-print - so no test can wait for it. Mirrors
    /// ActivePrintMediaManagerTestAccess::fire_pending_retry().
    ///
    /// Invokes the PRODUCTION callback rather than reproducing what it does. An
    /// earlier version open-coded `cancel(); retire(TimedOut);` here, which made
    /// every watchdog assertion a tautology - the test asserted the exit reason
    /// the helper itself had just supplied, and `preparing_watchdog_cb` was
    /// reachable from no test at all. Changing the real callback's exit reason,
    /// or dropping its `preparing_watchdog_ = nullptr` (a double-free setup,
    /// since LVGL frees the one-shot on return), left the suite green.
    ///
    /// Deletes the timer afterwards because LVGL's one-shot repeat count is what
    /// would normally free it, and nothing here runs lv_timer_handler.
    static bool fire_preparing_watchdog(PrinterPrintState& pps) {
        lv_timer_t* timer = pps.preparing_watchdog_;
        if (!timer) {
            return false;
        }
        PrinterPrintState::preparing_watchdog_cb(timer);
        lv_timer_delete(timer);
        return true;
    }

    static bool has_preparing_watchdog(const PrinterPrintState& pps) {
        return pps.preparing_watchdog_ != nullptr;
    }

    /// Mark the layer counters as coming from real slicer/Moonraker fields
    /// rather than being derived from the progress fraction.
    static void set_has_real_layer_data(PrinterPrintState& pps, bool value) {
        pps.has_real_layer_data_ = value;
    }
};

// ---------------------------------------------------------------------------
// Per-domain data accessors used by PrinterStateTestAccess::clear_data().
//
// Every PrinterXState declares `friend class PrinterXStateTestAccess;` next to
// its private members; these are those friends. They exist ONLY to clear plain
// (non-subject) data — the members that init_subjects()/deinit_subjects() never
// touch, and that therefore live for the whole process on the shared
// get_printer_state() singleton.
//
// Deliberately absent: PrinterFanStateTestAccess and
// PrinterTemperatureStateTestAccess. Both names are already taken by local
// definitions in tests/unit/test_print_status_fan_section.cpp and
// tests/unit/test_printer_temperature_{state,char}.cpp, two of which already
// include this header — defining them here is an immediate ambiguity. Those two
// domains are cleared through their public init_fans({}) / init_extruders({})
// instead, which is also the only correct way to expire their per-item subjects.
// ---------------------------------------------------------------------------

class PrinterExcludedObjectsStateTestAccess {
  public:
    static void clear_data(PrinterExcludedObjectsState& s) {
        s.excluded_objects_.clear();
        s.defined_objects_.clear();
        s.current_object_.clear();
        s.object_geometry_.clear();
    }
};

class PrinterMotionStateTestAccess {
  public:
    static void clear_data(PrinterMotionState& s) {
        s.axis_bounds_ = AxisBounds{};
    }
};

class PrinterLedStateTestAccess {
  public:
    static void clear_data(PrinterLedState& s) {
        s.tracked_led_name_.clear();
    }
};

class PrinterCapabilitiesStateTestAccess {
  public:
    static void clear_data(PrinterCapabilitiesState& s) {
        // Values latched while subjects were down, replayed by the next
        // init_subjects(). Left behind, they re-seed the NEXT test's
        // capability subjects with the previous printer's answers.
        s.pending_capability_values_.clear();
        s.stepper_z_endstop_microns_ = 0;
    }
};

class PrinterCalibrationStateTestAccess {
  public:
    static void clear_data(PrinterCalibrationState& s) {
        s.busy_queue_toast_shown_.store(false, std::memory_order_relaxed);
        // Debounced view of idle_timeout.printing; not a subject, so
        // deinit_subjects() leaves it latched. A test that drove a blocking op
        // otherwise makes every later test's is_blocking_operation_active()
        // read true once its SETTLE window has passed.
        s.idle_timeout_busy_.set_printing(false);
    }
};

class PrinterHardwareValidationStateTestAccess {
  public:
    static void clear_data(PrinterHardwareValidationState& s) {
        s.hardware_validation_result_ = HardwareValidationResult{};
    }
};

class PrinterCompositeVisibilityStateTestAccess {
  public:
    static void clear_data(PrinterCompositeVisibilityState& s) {
        s.last_log_state_initialized_ = false;
        s.last_any_ = -1;
        s.last_plugin_ = false;
    }
};

class PrinterNetworkStateTestAccess {
  public:
    static void clear_data(PrinterNetworkState& s) {
        s.klippy_state_message_.clear();
        s.was_ever_connected_ = false;
    }
};

// PrinterStateTestAccess must be in namespace helix to match friend declaration in PrinterState
class PrinterStateTestAccess {
  public:
    /// Full teardown: clear the data AND tear the subjects down.
    ///
    /// Only for tests that genuinely want the subject tree gone (they are about
    /// to re-init it, or they are asserting on deinit behaviour). Cross-test
    /// data isolation no longer needs this — HelixTestFixture::reset_all() calls
    /// clear_data() on the global PrinterState in its ctor and dtor.
    static void reset(PrinterState& ps) {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        clear_data(ps);
        ps.deinit_subjects();
    }

    /// Clear every plain (non-subject) data member reachable from PrinterState.
    ///
    /// WHY THIS EXISTS, and why it is called from the fixture base rather than
    /// from individual tests:
    ///
    /// PrinterState's thirteen domain components each pair an init_subjects()
    /// with a deinit_subjects(), and those manage the SUBJECTS only. The plain
    /// members alongside them — the excluded/defined object sets, the discovery
    /// result, the capability override map, the cached status JSON — have no
    /// lifetime hook at all. On the shared get_printer_state() singleton that
    /// means they live for the whole process, so a test that writes one poisons
    /// every later test in the same binary. The failure is silent: the next test
    /// reads a plausible value it never set, and the assertion that trips is
    /// usually in some third test that looks unrelated.
    ///
    /// Adding a plain member to any PrinterXState? Add it here too.
    ///
    /// Does NOT touch subjects, so it is safe from a fixture ctor/dtor whether
    /// or not init_subjects() has run, and it does not walk into the
    /// deinit-time observer minefield that reset() does.
    static void clear_data(PrinterState& ps) {
        // --- Domains whose data is owned together with per-item subjects ------
        // These two must go through their public re-init: the collections own
        // heap lv_subject_t's, and re-initing empty is the only path that
        // expires each item's SubjectLifetime before freeing it. init_fans()
        // also carries live readings across a re-init for fans that persist
        // (#1181), so a leaked fans_ leaks speed_percent/ever_ran/rpm forward.
        // Guarded because both bump a version subject on the way out, which
        // needs the subject tree to exist.
        // cached_display_ is checked for the same reason PrinterState::init_subjects()
        // checks it: after an lv_init() cycle the flag still reads true while every
        // subject points at freed memory, and re-initing them would touch it.
        if (ps.subjects_initialized_ && ps.cached_display_ == lv_display_get_default()) {
            ps.fan_state_.init_fans({});
            ps.temperature_state_.init_extruders({});
        }
        // Plain string setters — no subject touched, so they run unguarded. These
        // are the Klipper object names the chamber logic matches against; a leaked
        // one makes a later test's chamber read the previous test's sensor.
        ps.temperature_state_.set_chamber_sensor_name("");
        ps.temperature_state_.set_chamber_heater_name("");
        ps.temperature_state_.set_chamber_cooling_fan_name("");

        // --- Domains with pure data ------------------------------------------
        PrinterExcludedObjectsStateTestAccess::clear_data(*ps.get_excluded_objects_state());
        PrinterMotionStateTestAccess::clear_data(ps.motion_state_);
        PrinterLedStateTestAccess::clear_data(ps.led_state_component_);
        PrinterCapabilitiesStateTestAccess::clear_data(ps.capabilities_state_);
        PrinterCalibrationStateTestAccess::clear_data(ps.calibration_state_);
        PrinterHardwareValidationStateTestAccess::clear_data(ps.hardware_validation_state_);
        PrinterCompositeVisibilityStateTestAccess::clear_data(ps.composite_visibility_state_);
        PrinterNetworkStateTestAccess::clear_data(ps.network_state_);
        PrinterPrintStateTestAccess::reset_extra(ps.print_domain_);

        // --- PrinterState's own members ---------------------------------------
        ps.printer_type_.clear();
        ps.pre_print_option_set_ = PrePrintOptionSet();
        ps.z_offset_calibration_strategy_ = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
        ps.auto_detected_bed_moves_ = false;
        ps.is_paused_ = false;
        ps.last_kinematics_.clear();
        ps.capability_overrides_ = CapabilityOverrides();
        ps.discovery_ = helix::PrinterDiscovery();
        ps.last_unknown_klippy_state_.clear();
        ps.timelapse_default_enabled_ = false;
        {
            std::lock_guard<std::mutex> lock(ps.state_mutex_);
            ps.json_state_ = nlohmann::json::object();
        }
        // Takes state_mutex_ itself, so it must be outside the block above.
        ps.reset_klippy_state_freshness();
    }

    static PrinterFanState& get_fan_state(PrinterState& ps) {
        return ps.fan_state_;
    }

    /// Pin the calibration strategy directly, bypassing the printer DB lookup
    /// that set_printer_type_sync() drives, for tests that need one specific
    /// arm of a strategy branch.
    static void pin_z_offset_strategy(PrinterState& ps, ZOffsetCalibrationStrategy strategy) {
        ps.z_offset_calibration_strategy_ = strategy;
    }

    static PrinterPrintState& get_print_state(PrinterState& ps) {
        return ps.print_domain_;
    }

    /// Inject a synthetic pre-print option set (bypasses the printer DB) so tests
    /// can exercise option configurations that no shipped printer declares yet —
    /// e.g. a bed_mesh option with a custom adaptive_param name.
    static void set_option_set(PrinterState& ps, PrePrintOptionSet set) {
        ps.pre_print_option_set_ = std::move(set);
    }

    /// Pin a capability override without going through settings.json, for tests
    /// that need one specific override arm. The config-load path has its own
    /// coverage in test_capability_overrides.cpp; this is the wiring-level lever.
    static void set_capability_override(PrinterState& ps, const std::string& name,
                                        OverrideState state) {
        ps.capability_overrides_.set_override(name, state);
    }

    /**
     * @brief Make the blocking-op guard see a SUSTAINED idle_timeout "Printing"
     *
     * Setting the subject alone no longer reaches is_blocking_operation_active():
     * it reads IdleTimeoutBusy, which requires the flag to hold for SETTLE before
     * it counts (see include/idle_timeout_busy.h). A test that means "an external
     * blocking op is under way" means one that has already outlasted that window,
     * so back-date the transition rather than sleeping. Use this instead of
     * writing the subject directly; the settle behaviour itself is covered in
     * test_idle_timeout_busy.cpp.
     */
    static void set_sustained_idle_timeout_printing(PrinterState& ps, bool on) {
        lv_subject_set_int(ps.get_idle_timeout_printing_subject(), on ? 1 : 0);
        ps.idle_timeout_busy().set_printing(on, IdleTimeoutBusy::clock::now() -
                                                    IdleTimeoutBusy::SETTLE);
    }
};

} // namespace helix

using namespace helix;
