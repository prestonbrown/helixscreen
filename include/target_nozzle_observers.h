// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_factory.h"
#include "core/observer_guard.h"
#include "printer_temperature_state.h"

#include <string>

namespace helix::ui {

/// A surface's nozzle current/target observers, pointed at one tool's
/// extruder. The mounted tool reads the active-extruder mirror, which every
/// printer publishes; another tool reads its own extruder's subjects. Main
/// thread only.
class TargetNozzleObservers {
  public:
    /// Watch @p extruder's nozzle; @p mounted says the tool is the one on the
    /// carriage (by tool index, not extruder name: after a park Klipper still
    /// names the last tool's extruder active). A no-op when already watching
    /// the same subjects, so a caller may call it on every change without
    /// replaying the current values.
    template <typename Owner>
    void bind(PrinterTemperatureState& temps, const std::string& extruder, bool mounted,
              Owner* owner, void (*on_temp)(Owner*, int), void (*on_target)(Owner*, int)) {
        lv_subject_t* temp_src = nullptr;
        lv_subject_t* target_src = nullptr;
        SubjectLifetime temp_lifetime = temps.get_subjects_lifetime();
        SubjectLifetime target_lifetime = temps.get_subjects_lifetime();
        if (!mounted) {
            temp_src = temps.get_extruder_temp_subject(extruder, temp_lifetime);
            target_src = temps.get_extruder_target_subject(extruder, target_lifetime);
        }
        if (!temp_src || !target_src) {
            temp_src = temps.get_active_extruder_temp_subject();
            target_src = temps.get_active_extruder_target_subject();
            temp_lifetime = temps.get_subjects_lifetime();
            target_lifetime = temps.get_subjects_lifetime();
        }
        if (temp_ && target_ && temp_src == temp_src_ && target_src == target_src_) {
            return;
        }
        temp_src_ = temp_src;
        target_src_ = target_src;
        temp_ = observe<int>(temp_src, owner, on_temp, temp_lifetime, Dispatch::Immediate);
        target_ = observe<int>(target_src, owner, on_target, target_lifetime, Dispatch::Immediate);
    }

    /// The next bind() rebinds even to the same addresses: rebuilt subjects
    /// can reuse an old one.
    void invalidate() {
        temp_src_ = nullptr;
        target_src_ = nullptr;
    }

    void reset() {
        temp_.reset();
        target_.reset();
        invalidate();
    }

  private:
    ObserverGuard temp_;
    ObserverGuard target_;
    lv_subject_t* temp_src_ = nullptr;
    lv_subject_t* target_src_ = nullptr;
};

} // namespace helix::ui
