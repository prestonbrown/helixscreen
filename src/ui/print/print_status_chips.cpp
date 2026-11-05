// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_chips.h"

#include "display_numbering.h"
#include "i18n_plural.h"
#include "lvgl.h"
#include "src/ui/panel_widgets/callout_chip.h"
#include "tool_state.h"
#include "tune_controller.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix {

namespace {
/// The reading as every heater display prints it, without the unit letter:
/// "184 / 220°" with a target, "220°" without.
std::string heater_text(int current_deci, int target_deci) {
    return ui::compact_callout_text(
        ui::temperature::heater_display(current_deci, target_deci).temp);
}

/// Whole percent of the way from `start_deci` to `target_deci`, rounded down.
int heat_progress(int current_deci, int target_deci, int start_deci) {
    const int span = target_deci - start_deci;
    if (span <= 0)
        return 0;
    return std::clamp((current_deci - start_deci) * 100 / span, 0, 100);
}

bool climbing(const OtherToolReading& t) {
    using ui::temperature::HeaterStatusState;
    return ui::temperature::classify_heater_status(t.current_deci, t.target_deci, 0).state ==
           HeaterStatusState::Heating;
}

bool off(const OtherToolReading& t) {
    return t.target_deci <= 0 && !ui::temperature::is_residual_hot(t.current_deci);
}

/// "2 184 / 220°" climbing or cooling, "2 150°" holding or cooling down with
/// its heater off, "2 off" off and cold.
std::string other_tool_text(const OtherToolReading& t, bool by_number) {
    using ui::temperature::HeaterStatusState;
    std::string text = (by_number && !t.number.empty() ? t.number : t.badge) + " ";
    if (off(t))
        return text + lv_tr("off");
    const auto state =
        ui::temperature::classify_heater_status(t.current_deci, t.target_deci, 0).state;
    const bool off_target =
        state == HeaterStatusState::Heating || state == HeaterStatusState::Cooling;
    return text + heater_text(t.current_deci, off_target ? t.target_deci : 0);
}

/// The chip's entries, a middle dot between each.
std::string dotted(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out += " · ";
        out += p;
    }
    return out;
}

} // namespace

std::vector<OtherHead> other_heads(const std::vector<PhysicalHead>& heads, int active_tool,
                                   bool heads_share_heater) {
    std::vector<OtherHead> out;
    // With nothing mounted a lone head is not "another" head either.
    if (heads_share_heater || heads.size() < 2)
        return out;
    for (const PhysicalHead& head : heads) {
        if (!head.extruder_name || head.holds_tool(active_tool))
            continue;
        out.push_back({head.representative_tool(active_tool), *head.extruder_name});
    }
    return out;
}

std::string other_tool_badge(std::string_view name, int index) {
    return ui::is_generated_tool_name(name) ? ui::lane_number_text(index) : std::string(name);
}

ChipContent other_tools_chip(const std::vector<OtherToolReading>& tools,
                             const std::function<bool(const std::string&)>& fits) {
    ChipContent c;
    if (tools.empty())
        return c;
    c.shown = true;

    const OtherToolReading* first_climbing = nullptr;
    bool all_off = true;
    bool any_named = false;
    for (const auto& t : tools) {
        all_off = all_off && off(t);
        any_named = any_named || !t.number.empty();
        if (!first_climbing && climbing(t))
            first_climbing = &t;
    }

    // The full list, else as many heads as fit, in order, and a count of the
    // rest. A climbing head always stays, even past the room: a pre-heat is
    // never hidden. Returns whether the text fits.
    const auto ladder = [&](bool by_number) {
        std::vector<std::string> all;
        for (const bool want_climbing : {true, false})
            for (const auto& t : tools)
                if (climbing(t) == want_climbing)
                    all.push_back(other_tool_text(t, by_number));
        c.text = dotted(all);
        if (fits(c.text))
            return true;
        const int n = static_cast<int>(all.size());
        for (int k = n - 1; k >= 1; --k) {
            std::vector<std::string> parts(all.begin(), all.begin() + k);
            parts.push_back(fmt::format("+{}", n - k));
            c.text = dotted(parts);
            if (fits(c.text))
                return true;
        }
        if (first_climbing) {
            // A lone climbing head, or the first one, with the rest counted.
            c.text = n > 1 ? dotted({all.front(), fmt::format("+{}", n - 1)}) : all.front();
        } else {
            c.text = fmt::format(fmt::runtime(ui::tr_plural("+1 tool", "+{} tools", n)), n);
        }
        return false;
    };
    if (!ladder(false) && any_named)
        ladder(true);

    if (first_climbing) {
        c.tone = ChipTone::Active;
        c.progress_pct = heat_progress(first_climbing->current_deci, first_climbing->target_deci,
                                       first_climbing->start_deci);
    } else {
        c.expendable = true;
        if (all_off)
            c.tone = ChipTone::Dim;
    }
    return c;
}

std::string other_tools_budget_text(const std::vector<OtherToolReading>& tools) {
    if (tools.empty())
        return {};
    std::string_view badge;
    for (const auto& t : tools) {
        const std::string& b = t.number.empty() ? t.badge : t.number;
        if (b.size() > badge.size())
            badge = b;
    }
    std::string text = heater_budget_text(badge);
    if (tools.size() > 1)
        text += fmt::format(" · +{}", tools.size() - 1);
    return text;
}

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

    if (heating)
        c.progress_pct = heat_progress(current_deci, target_deci, start_deci);
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
    // A head may start pre-heating at any moment, and that must never be shed.
    case CalloutKind::OtherTools:
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
