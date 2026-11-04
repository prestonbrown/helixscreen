// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file temperature_observer_bundle.h
 * @brief Bundle for managing common temperature subject observers (nozzle + bed)
 *
 * Encapsulates the repetitive pattern of subscribing to 4 temperature subjects
 * (active extruder temp/target, bed temp/target) that appears in 5+ panels.
 *
 * Reduces ~12-15 lines of boilerplate per panel to a single setup call.
 *
 * @pattern Observer Bundle - groups related observers for common use cases
 */

#pragma once

#include "ui_observer_guard.h"

#include "core/observer_factory.h"
#include "printer_state.h"

namespace helix::ui {

/**
 * @brief Bundle for temperature observers (nozzle + bed, current + target)
 *
 * Use when a panel needs to observe all 4 standard temperature subjects from PrinterState.
 * Handlers run on the UI thread, one per subject: deferred by default, or inside the notify with
 * Dispatch::Immediate.
 *
 * @tparam Panel The panel class type (must be pointer-safe)
 *
 * @code{.cpp}
 * TemperatureObserverBundle<MyPanel> temp_observers_;
 *
 * temp_observers_.setup_sync(
 *     this,
 *     printer_state_,
 *     [](MyPanel* p, int v) { p->nozzle_current_ = helix::ui::temperature::deci_to_degrees(v);
 * p->update_display(); },
 *     [](MyPanel* p, int v) { p->nozzle_target_ = helix::ui::temperature::deci_to_degrees(v);
 * p->update_display(); },
 *     [](MyPanel* p, int v) { p->bed_current_ = helix::ui::temperature::deci_to_degrees(v);
 * p->update_display(); },
 *     [](MyPanel* p, int v) { p->bed_target_ = helix::ui::temperature::deci_to_degrees(v);
 * p->update_display(); }
 * );
 * @endcode
 */
template <typename Panel> class TemperatureObserverBundle {
  public:
    /**
     * @brief Setup synchronous temperature observers with individual callbacks
     *
     * Use when handlers run on UI thread and each temperature update needs
     * its own handler logic. Callbacks receive raw decidegree values.
     *
     * @param panel Panel instance (must outlive observers)
     * @param state PrinterState reference for temperature subjects
     * @param on_nozzle_temp Called when nozzle current temperature changes
     * @param on_nozzle_target Called when nozzle target temperature changes
     * @param on_bed_temp Called when bed current temperature changes
     * @param on_bed_target Called when bed target temperature changes
     * @param dispatch Dispatch::Immediate for a handler that caches the value inline and
     *        defers its own UI work
     */
    template <typename NozzleTempHandler, typename NozzleTargetHandler, typename BedTempHandler,
              typename BedTargetHandler>
    void setup_sync(Panel* panel, PrinterState& state, NozzleTempHandler&& on_nozzle_temp,
                    NozzleTargetHandler&& on_nozzle_target, BedTempHandler&& on_bed_temp,
                    BedTargetHandler&& on_bed_target, Dispatch dispatch = Dispatch::Deferred) {
        clear();

        // These four are PrinterState's own static subjects, and panels holding
        // this bundle routinely outlive a PrinterState::deinit_subjects() cycle
        // (printer switch in production, per-fixture teardown in tests). Without
        // the token, deinit frees the observer nodes and the next clear() calls
        // lv_observer_remove() on freed memory.
        const SubjectLifetime lifetime = state.get_subjects_lifetime();

        nozzle_temp_observer_ =
            observe<int>(state.temperature_state().get_active_extruder_temp_subject(), panel,
                         std::forward<NozzleTempHandler>(on_nozzle_temp), lifetime, dispatch);

        nozzle_target_observer_ =
            observe<int>(state.temperature_state().get_active_extruder_target_subject(), panel,
                         std::forward<NozzleTargetHandler>(on_nozzle_target), lifetime, dispatch);

        bed_temp_observer_ =
            observe<int>(state.temperature_state().get_bed_temp_subject(), panel,
                         std::forward<BedTempHandler>(on_bed_temp), lifetime, dispatch);

        bed_target_observer_ =
            observe<int>(state.temperature_state().get_bed_target_subject(), panel,
                         std::forward<BedTargetHandler>(on_bed_target), lifetime, dispatch);
    }

    /**
     * @brief Setup observers for a specific extruder (by Klipper name)
     *
     * Binds only nozzle temp/target observers to named extruder subjects.
     * Does not touch bed observers. Returns silently if subjects not found.
     *
     * @param panel Panel instance (must outlive observers)
     * @param state PrinterState reference for temperature subjects
     * @param extruder_name Klipper heater name (e.g. "extruder", "extruder1")
     * @param on_nozzle_temp Called when nozzle current temperature changes
     * @param on_nozzle_target Called when nozzle target temperature changes
     */
    template <typename NozzleTempHandler, typename NozzleTargetHandler>
    void setup_for_extruder(Panel* panel, PrinterState& state, const std::string& extruder_name,
                            NozzleTempHandler&& on_nozzle_temp,
                            NozzleTargetHandler&& on_nozzle_target) {
        clear(); // Release any existing observers before rebinding

        // Per-extruder subjects are dynamic — require SubjectLifetime tokens
        // to prevent use-after-free when subjects are reinitialized on reconnect.
        auto* temp_subj = state.temperature_state().get_extruder_temp_subject(
            extruder_name, nozzle_temp_lifetime_);
        auto* target_subj = state.temperature_state().get_extruder_target_subject(
            extruder_name, nozzle_target_lifetime_);

        if (temp_subj) {
            nozzle_temp_observer_ =
                observe<int>(temp_subj, panel, std::forward<NozzleTempHandler>(on_nozzle_temp),
                             nozzle_temp_lifetime_);
        }
        if (target_subj) {
            nozzle_target_observer_ = observe<int>(
                target_subj, panel, std::forward<NozzleTargetHandler>(on_nozzle_target),
                nozzle_target_lifetime_);
        }
    }

    /**
     * @brief Clear all observers (automatic on destruction)
     *
     * Safe to call multiple times. Observers are released via RAII.
     */
    void clear() {
        // Reset order between token and observer is not load-bearing here: these
        // tokens are copies of a shared_ptr PrinterTemperatureState keeps, so this
        // reset() does not expire the guard's weak_ptr. The guard still sees the
        // live subject and removes itself normally.
        nozzle_temp_lifetime_.reset();
        nozzle_target_lifetime_.reset();
        nozzle_temp_observer_ = ObserverGuard();
        nozzle_target_observer_ = ObserverGuard();
        bed_temp_observer_ = ObserverGuard();
        bed_target_observer_ = ObserverGuard();
    }

    /**
     * @brief Check if bundle has active observers
     * @return true if any observer is set up
     */
    [[nodiscard]] bool is_active() const {
        return nozzle_temp_observer_ || nozzle_target_observer_ || bed_temp_observer_ ||
               bed_target_observer_;
    }

  private:
    // Lifetimes declared before observers: C++ destroys members in reverse
    // declaration order, so the observers are destroyed first (removing themselves
    // from the subject's list) while the tokens are still alive.
    //
    // With the tokens these accessors hand out, order is not load-bearing — they are
    // copies of a shared_ptr the owner (PrinterTemperatureState) keeps, so dropping
    // this copy neither expires the guard's weak_ptr nor changes what reset() does.
    // Subject death is signalled by the owner writing *token = false, independent of
    // refcount. This order is the one that also stays correct if a token ever becomes
    // exclusively owned here; see docs/devel/THREADING.md § 5.
    SubjectLifetime nozzle_temp_lifetime_;
    SubjectLifetime nozzle_target_lifetime_;
    ObserverGuard nozzle_temp_observer_;
    ObserverGuard nozzle_target_observer_;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
};

} // namespace helix::ui
