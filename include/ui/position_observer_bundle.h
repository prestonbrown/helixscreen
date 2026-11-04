// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file position_observer_bundle.h
 * @brief Bundle for managing position subject observers (X, Y, Z axes)
 *
 * Encapsulates the repetitive pattern of subscribing to 3 position subjects
 * (gcode_position_x, gcode_position_y, gcode_position_z) that appears in
 * multiple panels.
 *
 * Reduces ~9-12 lines of boilerplate per panel to a single setup call.
 *
 * @pattern Observer Bundle - groups related observers for common use cases
 */

#pragma once

#include "ui_observer_guard.h"

#include "core/observer_factory.h"
#include "printer_state.h"

namespace helix::ui {

/**
 * @brief Bundle for position observers (X, Y, Z axes)
 *
 * Use when a panel needs to observe all 3 position subjects from PrinterState.
 * Handlers run deferred on the UI thread, one per subject.
 *
 * @tparam Panel The panel class type (must be pointer-safe)
 *
 * @code{.cpp}
 * PositionObserverBundle<MyPanel> pos_observers_;
 *
 * pos_observers_.setup_sync(
 *     this,
 *     printer_state_,
 *     [](MyPanel* p, int v) { p->format_x(v); p->update_display(); },
 *     [](MyPanel* p, int v) { p->format_y(v); p->update_display(); },
 *     [](MyPanel* p, int v) { p->format_z(v); p->update_display(); }
 * );
 * @endcode
 */
template <typename Panel> class PositionObserverBundle {
  public:
    /**
     * @brief Setup synchronous position observers with individual callbacks
     *
     * Use when handlers run on UI thread and each position update needs
     * its own handler logic. Callbacks receive raw centimillimeter values.
     *
     * @param panel Panel instance (must outlive observers)
     * @param state PrinterState reference for position subjects
     * @param on_x_pos Called when X position changes
     * @param on_y_pos Called when Y position changes
     * @param on_z_pos Called when Z position changes
     */
    template <typename XPosHandler, typename YPosHandler, typename ZPosHandler>
    void setup_sync(Panel* panel, PrinterState& state, XPosHandler&& on_x_pos,
                    YPosHandler&& on_y_pos, ZPosHandler&& on_z_pos) {
        clear();

        // PrinterState-owned subjects: a panel outliving a deinit_subjects()
        // cycle needs the death signal, or clear() calls lv_observer_remove()
        // on observer nodes lv_subject_deinit() already freed.
        const SubjectLifetime lifetime = state.get_subjects_lifetime();

        x_pos_observer_ = observe<int>(state.motion_state().get_gcode_position_x_subject(), panel,
                                       std::forward<XPosHandler>(on_x_pos), lifetime);

        y_pos_observer_ = observe<int>(state.motion_state().get_gcode_position_y_subject(), panel,
                                       std::forward<YPosHandler>(on_y_pos), lifetime);

        z_pos_observer_ = observe<int>(state.motion_state().get_gcode_position_z_subject(), panel,
                                       std::forward<ZPosHandler>(on_z_pos), lifetime);
    }

    /**
     * @brief Clear all observers (automatic on destruction)
     *
     * Safe to call multiple times. Observers are released via RAII.
     */
    void clear() {
        x_pos_observer_ = ObserverGuard();
        y_pos_observer_ = ObserverGuard();
        z_pos_observer_ = ObserverGuard();
    }

    /**
     * @brief Check if bundle has active observers
     * @return true if any observer is set up
     */
    [[nodiscard]] bool is_active() const {
        return x_pos_observer_ || y_pos_observer_ || z_pos_observer_;
    }

  private:
    ObserverGuard x_pos_observer_;
    ObserverGuard y_pos_observer_;
    ObserverGuard z_pos_observer_;
};

} // namespace helix::ui
