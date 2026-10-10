// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_heater_icon_binder.h"

#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

HeaterIconBinder::~HeaterIconBinder() {
    unbind();
}

const char* HeaterIconBinder::default_icon_name(HeaterType heater) {
    switch (heater) {
    case HeaterType::Bed:
        return "bed_icon_glyph";
    case HeaterType::Chamber:
        return "chamber_icon_glyph";
    case HeaterType::Nozzle:
        break;
    }
    return "nozzle_icon_glyph";
}

bool HeaterIconBinder::bind(lv_obj_t* root, PrinterState& state, HeaterType heater,
                            const char* icon_name) {
    unbind();
    if (!root) {
        return false;
    }

    const char* name = icon_name ? icon_name : default_icon_name(heater);
    lv_obj_t* icon = lv_obj_find_by_name(root, name);
    if (!icon) {
        spdlog::debug("[HeaterIconBinder] Icon '{}' not found under root", name);
        return false;
    }

    switch (heater) {
    case HeaterType::Nozzle:
        // Active-extruder subjects are singleton-lifetime; no tokens needed.
        current_subject_ = state.temperature_state().get_active_extruder_temp_subject();
        target_subject_ = state.temperature_state().get_active_extruder_target_subject();
        break;
    case HeaterType::Bed:
        current_subject_ = state.temperature_state().get_bed_temp_subject(current_lifetime_);
        target_subject_ = state.temperature_state().get_bed_target_subject(target_lifetime_);
        break;
    case HeaterType::Chamber:
        // Effective target, not the raw heater target: in Maintaining mode the
        // real setpoint lives on the cooling fan and the heater target is 0.
        // Mode subject makes Maintaining resolve to Cooling/Neutral instead of
        // the plain classifier's Off/Heating/AtTemp — same as the temp_display
        // label chamber binds (see classify_heat_state_with_mode()).
        current_subject_ = state.temperature_state().get_chamber_temp_subject(current_lifetime_);
        target_subject_ =
            state.temperature_state().get_chamber_effective_target_subject(target_lifetime_);
        mode_subject_ = state.temperature_state().get_chamber_mode_subject(mode_lifetime_);
        break;
    }

    return attach_and_observe(icon);
}

bool HeaterIconBinder::bind_subjects(lv_obj_t* root, const char* icon_name,
                                     lv_subject_t* current_subject, lv_subject_t* target_subject) {
    unbind();
    if (!root || !icon_name) {
        return false;
    }

    lv_obj_t* icon = lv_obj_find_by_name(root, icon_name);
    if (!icon) {
        spdlog::debug("[HeaterIconBinder] Icon '{}' not found under root", icon_name);
        return false;
    }

    if (!current_subject && !target_subject) {
        // Both null means classify(250, 0) forever — the animator never moves
        // off its Off-state default and nothing downstream will ever say why.
        spdlog::warn("[HeaterIconBinder] Icon '{}' found but both current and target subjects "
                     "are null — refusing to bind (icon would freeze at the off-state color)",
                     icon_name);
        return false;
    }

    current_subject_ = current_subject;
    target_subject_ = target_subject;
    return attach_and_observe(icon);
}

bool HeaterIconBinder::attach_and_observe(lv_obj_t* icon) {
    animator_.attach(icon);

    if (current_subject_) {
        cached_current_ = lv_subject_get_int(current_subject_);
    }
    if (target_subject_) {
        cached_target_ = lv_subject_get_int(target_subject_);
    }
    if (mode_subject_) {
        cached_mode_ = lv_subject_get_int(mode_subject_);
    }
    animator_.update(cached_current_, cached_target_,
                     static_cast<helix::ChamberMode>(cached_mode_));

    auto token = lifetime_.token();

    if (current_subject_) {
        current_observer_ = observe<int>(
            current_subject_, this,
            [token](HeaterIconBinder* self, int value) {
                if (token.expired())
                    return;
                self->cached_current_ = value;
                self->refresh();
            },
            current_lifetime_);
    }
    if (target_subject_) {
        target_observer_ = observe<int>(
            target_subject_, this,
            [token](HeaterIconBinder* self, int value) {
                if (token.expired())
                    return;
                self->cached_target_ = value;
                self->refresh();
            },
            target_lifetime_);
    }
    if (mode_subject_) {
        mode_observer_ = observe<int>(
            mode_subject_, this,
            [token](HeaterIconBinder* self, int value) {
                if (token.expired())
                    return;
                self->cached_mode_ = value;
                self->refresh();
            },
            mode_lifetime_);
    }

    return true;
}

void HeaterIconBinder::refresh() {
    animator_.update(cached_current_, cached_target_,
                     static_cast<helix::ChamberMode>(cached_mode_));
}

void HeaterIconBinder::unbind() {
    lifetime_.invalidate();
    animator_.detach();
    current_observer_.reset();
    target_observer_.reset();
    mode_observer_.reset();
    current_lifetime_.reset();
    target_lifetime_.reset();
    mode_lifetime_.reset();
    current_subject_ = nullptr;
    target_subject_ = nullptr;
    mode_subject_ = nullptr;
    cached_current_ = 250;
    cached_target_ = 0;
    cached_mode_ = helix::ChamberMode::Heating;
}

} // namespace helix::ui
