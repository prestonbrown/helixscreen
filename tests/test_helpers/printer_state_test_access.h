// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "capability_overrides.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <mutex>
#include <vector>

namespace helix {

class PrinterPrintStateTestAccess {
  public:
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

class PrinterProfileStateTestAccess {
  public:
    static void pin_strategy(PrinterProfileState& s, ZOffsetCalibrationStrategy strategy) {
        s.z_offset_calibration_strategy_ = strategy;
        s.z_offset_save_strategy_ = strategy;
    }

    static void set_option_set(PrinterProfileState& s, PrePrintOptionSet set) {
        s.pre_print_option_set_ = std::move(set);
    }
};

// PrinterStateTestAccess must be in namespace helix to match friend declaration in PrinterState
class PrinterStateTestAccess {
  public:
    /// printer_has_chamber_sensor: 1 while the resolved chamber sensor name is set.
    static lv_subject_t* has_chamber_sensor_subject(PrinterState& ps) {
        return ps.capabilities_state_.subject(Capability::HasChamberSensor);
    }

    /// Full teardown: clear the data AND tear the subjects down.
    ///
    /// Only for tests that genuinely want the subject tree gone (they are about
    /// to re-init it, or they are asserting on deinit behaviour). Cross-test
    /// data isolation no longer needs this — HelixTestFixture::reset_all() calls
    /// clear_data() on the global PrinterState in its ctor and dtor.
    static void reset(PrinterState& ps) {
        helix::ui::UpdateQueue::instance().drain();
        clear_data(ps);
        ps.deinit_subjects();
    }

    /// Clear every plain (non-subject) data member reachable from PrinterState.
    ///
    /// WHY THIS EXISTS, and why it is called from the fixture base rather than
    /// from individual tests:
    ///
    /// PrinterState's twelve domain components each pair an init_subjects()
    /// with a deinit_subjects(), and those manage the SUBJECTS only. The plain
    /// members alongside them — the excluded/defined object sets, the discovery
    /// result, the capability override map, the cached status JSON — have no
    /// lifetime hook at all. On the shared get_printer_state() singleton that
    /// means they live for the whole process, so a test that writes one poisons
    /// every later test in the same binary. The failure is silent: the next test
    /// reads a plausible value it never set, and the assertion that trips is
    /// usually in some third test that looks unrelated.
    ///
    /// Adding a plain member to any PrinterXState? Clear it in that state's
    /// clear_data(), which PrinterState::clear_printer_data() runs.
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
        // Everything a printer switch forgets, through the same routine
        // production teardown runs, so the two cannot drift apart.
        ps.clear_printer_data();
        // Production reloads the overrides for the next printer in its cache
        // invalidator; a test starts from none.
        ps.capability_overrides_ = CapabilityOverrides();
    }

    /// Every domain member, in declaration order.
    static std::vector<const void*> domain_members(PrinterState& ps) {
        return {&ps.temperature_state_,
                &ps.motion_state_,
                &ps.fan_state_,
                &ps.print_domain_,
                &ps.capabilities_state_,
                &ps.plugin_status_state_,
                &ps.calibration_state_,
                &ps.hardware_validation_state_,
                &ps.composite_visibility_state_,
                &ps.network_state_,
                &ps.versions_state_,
                &ps.excluded_objects_state_,
                &ps.profile_state_};
    }

    static PrinterFanState& get_fan_state(PrinterState& ps) {
        return ps.fan_state_;
    }

    /// Pin the calibration strategy directly, bypassing the printer DB lookup
    /// that set_printer_type_sync() drives, for tests that need one specific
    /// arm of a strategy branch.
    static void pin_z_offset_strategy(PrinterState& ps, ZOffsetCalibrationStrategy strategy) {
        PrinterProfileStateTestAccess::pin_strategy(ps.profile_state_, strategy);
    }

    static PrinterPrintState& get_print_state(PrinterState& ps) {
        return ps.print_domain_;
    }

    /// Non-const temperature state (temperature_state() returns a const ref;
    /// subject getters are non-const)
    static PrinterTemperatureState& get_temperature_state(PrinterState& ps) {
        return ps.temperature_state_;
    }

    static PrinterCapabilitiesState& get_capabilities_state(PrinterState& ps) {
        return ps.capabilities_state_;
    }

    /// Inject a synthetic pre-print option set (bypasses the printer DB) so tests
    /// can exercise option configurations that no shipped printer declares yet —
    /// e.g. a bed_mesh option with a custom adaptive_param name.
    static void set_option_set(PrinterState& ps, PrePrintOptionSet set) {
        PrinterProfileStateTestAccess::set_option_set(ps.profile_state_, std::move(set));
    }

    /// The settings a self-storing firmware reports, as a status frame would
    /// deliver them. Applied on the next UpdateQueue drain.
    static void merge_firmware_option_defaults(PrinterState& ps,
                                               std::map<std::string, bool> defaults) {
        ps.merge_firmware_option_defaults(std::move(defaults));
    }

    /// Recompute has_any_preprint_options after a set_option_set(); the
    /// aggregate follows the option count, which the injection above skips.
    static void refresh_option_visibility(PrinterState& ps) {
        ps.update_gcode_modification_visibility();
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
     * it reads IdleTimeoutBusy, which requires the flag to hold for kSettle before
     * it counts (see include/idle_timeout_busy.h). A test that means "an external
     * blocking op is under way" means one that has already outlasted that window,
     * so back-date the transition rather than sleeping. Use this instead of
     * writing the subject directly; the settle behaviour itself is covered in
     * test_idle_timeout_busy.cpp.
     */
    static void set_sustained_idle_timeout_printing(PrinterState& ps, bool on) {
        lv_subject_set_int(ps.calibration_state().get_idle_timeout_printing_subject(), on ? 1 : 0);
        ps.calibration_state().idle_timeout_busy().set_printing(on, IdleTimeoutBusy::clock::now() -
                                                                        IdleTimeoutBusy::kSettle);
    }
};

} // namespace helix

using namespace helix;
