// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h"
#include "ui_heating_animator.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "printer_temperature_state.h" // helix::ChamberMode

namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief Binds a HeatingIconAnimator to a heater icon and keeps it fed.
 *
 * Before this existed, every call site repeated the same four moves: find the
 * icon by name, attach an animator, seed it from the current subject values,
 * then feed update() from its own temperature observers. The last move meant
 * panels grew observers whose only purpose was tinting an icon.
 *
 * This owns the animator AND its observers, so a call site is one line:
 *
 *     bed_icon_.bind(root, printer_state, HeaterType::Bed);
 *
 * Declare it as a plain by-value member of the panel or widget. It must NOT
 * live on a shared/refcounted object: during a rebuild the panel manager does
 * attach A -> detach A -> attach B, and A's deferred LV_EVENT_DELETE fires
 * after B has taken over. Per-instance ownership is what keeps that safe, since
 * each animator's delete callback carries its own `this`.
 */
class HeaterIconBinder {
  public:
    HeaterIconBinder() = default;
    ~HeaterIconBinder();

    HeaterIconBinder(const HeaterIconBinder&) = delete;
    HeaterIconBinder& operator=(const HeaterIconBinder&) = delete;

    /// Conventional glyph name for a heater, as used by the XML components.
    static const char* default_icon_name(HeaterType heater);

    /**
     * @brief Bind the conventional icon under `root` to `heater`'s subjects.
     *
     * Nozzle reads the active-extruder subjects; bed reads the bed subjects;
     * chamber reads chamber current + *effective* target (the same subjects the
     * chamber temp_display binds to) plus the chamber_mode subject, so icon and
     * number track one setpoint AND agree on Maintaining-mode semantics (see
     * classify_heat_state_with_mode()). Nozzle/bed have no mode concept.
     *
     * @p icon_name overrides the conventional glyph name, for a layout that
     * builds a second glyph for the same heater (one binder per glyph).
     *
     * @return true if the icon was found and bound.
     */
    bool bind(lv_obj_t* root, PrinterState& state, HeaterType heater,
              const char* icon_name = nullptr);

    /**
     * @brief Bind an explicitly named icon to explicitly chosen subjects.
     *
     * For callers whose subjects are not the PrinterState defaults — notably the
     * print-status nozzle, which reads tool-pin-aware proxy subjects rather than
     * the raw active-extruder ones.
     *
     * Only valid for subjects with singleton lifetime; dynamic subjects must go
     * through bind() so the lifetime tokens are wired.
     *
     * @return true if the icon was found and bound.
     */
    bool bind_subjects(lv_obj_t* root, const char* icon_name, lv_subject_t* current_subject,
                       lv_subject_t* target_subject);

    /// Detach the animator and drop the observers. Safe to call when unbound.
    void unbind();

    bool is_bound() const {
        return animator_.is_attached();
    }

  private:
    /// Attach to `icon` and subscribe to whatever is in current_/target_subject_.
    bool attach_and_observe(lv_obj_t* icon);
    void refresh();

    HeatingIconAnimator animator_;

    lv_subject_t* current_subject_ = nullptr;
    lv_subject_t* target_subject_ = nullptr;
    // Chamber only — stays nullptr for Nozzle/Bed and for bind_subjects() callers.
    lv_subject_t* mode_subject_ = nullptr;
    int cached_current_ = 250;
    int cached_target_ = 0;
    // Defaults to Heating: HeatingIconAnimator::update()'s mode-unaware passthrough,
    // so a never-bound mode_subject_ (Nozzle/Bed) behaves exactly like before mode
    // support existed.
    int cached_mode_ = helix::ChamberMode::Heating;

    ObserverGuard current_observer_;
    ObserverGuard target_observer_;
    ObserverGuard mode_observer_;
    SubjectLifetime current_lifetime_;
    SubjectLifetime target_lifetime_;
    SubjectLifetime mode_lifetime_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Same rationale as heater_temp_widget.h.
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::ui
