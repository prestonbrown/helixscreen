// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_factory.h"
#include "core/temperature_units.h"

#include <memory>

/**
 * @file temp_unit_subject.h
 * @brief The display unit as an LVGL subject, so text formatted in C++ can follow a switch.
 *
 * A string formatted from a cached Celsius value keeps the unit it was formatted in until
 * its owner formats it again. This subject is the trigger for that: it changes only when
 * the display unit does, so a switch wakes the formatters and never the printer's
 * temperature subjects, whose observers have side effects. It lives in core and is created
 * on first use, so a state object built before the settings load can observe it.
 * Main thread only.
 */

namespace helix::units {

/// The display unit as an int subject (a TempUnit); nullptr while LVGL is not initialized.
lv_subject_t* temp_unit_subject();

/// Death signal for temp_unit_subject().
SubjectLifetime temp_unit_subject_lifetime();

/// Publish display_temp_unit() to the subject. Whoever sets the display unit calls this.
void publish_temp_unit();

} // namespace helix::units

namespace helix::ui {

/**
 * @brief Run @p on_change(owner) each time the display unit changes.
 *
 * Deferred, and only on a real change, never on registration. Empty guard while LVGL is
 * not initialized.
 *
 * @tparam Owner Owner class type
 * @tparam OnChange Callable: void(Owner*)
 */
template <typename Owner, typename OnChange>
ObserverGuard observe_temp_unit_change(Owner* owner, OnChange&& on_change) {
    lv_subject_t* subject = units::temp_unit_subject();
    if (!subject) {
        return {};
    }
    // Held by pointer so the non-mutable handler can update it.
    auto shown = std::make_shared<int>(lv_subject_get_int(subject));
    return observe<int>(
        subject, owner,
        [shown, on_change = std::forward<OnChange>(on_change)](Owner* o, int unit) {
            if (unit == *shown) {
                return;
            }
            *shown = unit;
            on_change(o);
        },
        units::temp_unit_subject_lifetime());
}

} // namespace helix::ui
