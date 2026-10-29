// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widgets/callout_chip.h"
#include "tune_controller.h"

#include <algorithm>

namespace helix {

namespace {
/// The reading as every heater display prints it, without the unit letter:
/// "184 / 220°" with a target, "220°" without.
std::string heater_text(int current_deci, int target_deci) {
    return ui::compact_callout_text(
        ui::temperature::heater_display(current_deci, target_deci).temp);
}
} // namespace

ChipContent heater_chip(bool present, int current_deci, int target_deci,
                        const ui::temperature::HeaterStatus& status, bool printing, bool show_power,
                        std::string_view badge, int start_deci, bool reading_alone_shows) {
    using ui::temperature::HeaterStatusState;
    ChipContent c;
    if (!present || (target_deci <= 0 && !reading_alone_shows &&
                     !ui::temperature::is_residual_hot(current_deci)))
        return c;
    c.shown = true;
    c.glow = target_deci > 0;
    c.status_state = static_cast<int>(status.state);

    const bool heating = status.state == HeaterStatusState::Heating;
    const bool cooling = status.state == HeaterStatusState::Cooling;

    std::string text;
    if (!badge.empty()) {
        text.append(badge);
        text += ' ';
    }
    // The target shows only while the reading is off it; holding, the
    // reading alone says the heater is there.
    text += heater_text(current_deci, heating || cooling ? target_deci : 0);
    if (show_power && !status.duty.empty())
        text += " · " + status.duty;
    c.text = std::move(text);

    if (printing) {
        if (heating || cooling)
            c.tone = ChipTone::Warn;
    } else if (heating) {
        c.tone = ChipTone::Active;
    }

    if (heating) {
        const int span = target_deci - start_deci;
        if (span > 0) {
            // Whole percent, rounded down.
            c.progress_pct = std::clamp((current_deci - start_deci) * 100 / span, 0, 100);
        } else {
            c.progress_pct = 0;
        }
    }
    return c;
}

ChipContent fan_chip(CalloutKind kind, std::string_view label, bool present, int pct,
                     bool automatic, bool labeled) {
    ChipContent c;
    if (!present)
        return c;
    c.shown = true;
    std::string text;
    if (labeled && !label.empty()) {
        text = std::string(label) + " ";
    }
    c.text = text + std::to_string(pct) + "%";
    if (pct <= 0) {
        c.tone = ChipTone::Dim;
        c.expendable = true;
    } else if (kind == CalloutKind::Fan) {
        c.tone = ChipTone::Active;
        c.glow = true;
    }
    if (kind == CalloutKind::HotendFan && automatic)
        c.expendable = true;
    return c;
}

std::string heater_budget_text(std::string_view badge) {
    std::string text(badge);
    if (!text.empty()) {
        text += ' ';
    }
    // The widest shape at the widest reading the home callout measures too.
    return text + heater_text(9990, 9990);
}

ChipContent speed_flow_chip(int speed_pct, int flow_pct, const std::string& speed_text) {
    ChipContent c;
    c.shown = true;
    c.text = speed_text;
    if (speed_pct != 100 || flow_pct != 100) {
        c.tone = ChipTone::Warn;
    } else {
        c.expendable = true;
    }
    return c;
}

ChipContent speed_flow_budget_chip() {
    return speed_flow_chip(100, 100, tune::live_rates_text(999, 999.9));
}

ChipContent light_chip(bool controllable, bool on) {
    ChipContent c;
    if (!controllable)
        return c;
    c.shown = true;
    c.text = "";
    if (on) {
        c.tone = ChipTone::Active;
        c.glow = true;
    } else {
        c.tone = ChipTone::Dim;
        c.expendable = true;
    }
    return c;
}

bool phase_budget_expendable(CalloutKind kind, bool printing) {
    switch (kind) {
    case CalloutKind::Nozzle:
    case CalloutKind::Bed:
    case CalloutKind::Chamber:
    case CalloutKind::Toolhead:
        return false;
    case CalloutKind::Fan:
        return !printing;
    case CalloutKind::HotendFan:
    case CalloutKind::AuxFan:
    case CalloutKind::Light:
    case CalloutKind::SpeedFlow:
        return true;
    }
    return false;
}

bool heater_power_fits(CalloutMode mode, int w_with_power, int column_w) {
    if (mode != CalloutMode::BothSides && mode != CalloutMode::OneSide)
        return false;
    return w_with_power > 0 && w_with_power <= column_w;
}

} // namespace helix
