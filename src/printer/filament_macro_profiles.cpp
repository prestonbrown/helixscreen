// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_macro_profiles.h"

#include "core/text_io.h"
#include "json_utils.h"

#include <array>
#include <string>

namespace helix::filament_macros {
namespace {

/// A load macro that is the first step of a wizard the firmware's own screen
/// finishes. Every script is a template: `{temp}` becomes the temperature the
/// macro ran at.
struct GuidedLoadProfile {
    /// The parameter the macro reads its temperature from, and its default.
    const char* temp_param;
    int default_temp_c;
    /// Object, field and value that report the macro stopped for a hand push.
    const char* waiting_object;
    const char* waiting_field;
    const char* waiting_value;
    const char* feed;
    const char* retry;
    const char* flush;
    const char* release;
    const char* cleanup;
};

/// One stock macro, and what it does for itself.
struct MacroProfile {
    /// Uppercase macro name. Must be a spelling that carries no meaning in
    /// stock Klipper - a generic name proves nothing about the body.
    const char* name;
    /// Which firmware ships it, for the reader deciding whether a new row
    /// belongs beside this one.
    const char* firmware;
    /// Drives the hotend to temperature and waits, so a UI preheat in front of
    /// it is a second wait at a temperature the macro then overrides.
    bool heats_hotend;
    /// Moves the toolhead with no home of its own, so the printer must be homed
    /// before it is sent.
    bool needs_home_first;
    /// What the macro leaves undone that must be sent after it, or "".
    const char* restore_after;
    /// The wizard this macro starts, or nullptr.
    const GuidedLoadProfile* guided_load;
};

// QIDI's screen treats M604 as the first step of a wizard. On firmware
// 01.01.02.x, `M604 T0` heats and parks over the trash chute, sets
// print_stats_manager.sub_status to push_filamenting and stops for the user to
// push filament in; the screen then sends the bite (T1, after its own
// bit_filamenting status write), offers Extrude again (T1) or Done (T2, an 80mm
// flush), and the stop path restores main_status. On 1.1.x, T0 already extrudes
// and T1 is the retry; that firmware has no print_stats_manager object, so the
// waiting check answers false there and the 01.01.02-only commands are never
// sent. T2 leaves the nozzle hot.
constexpr GuidedLoadProfile kQidiM604Wizard{
    /*temp_param=*/"S",
    /*default_temp_c=*/250,
    /*waiting_object=*/"print_stats_manager",
    /*waiting_field=*/"sub_status",
    /*waiting_value=*/"push_filamenting",
    /*feed=*/"SET_PRINT_SUB_STATUS SUB_STATUS=bit_filamenting\nM604 T1 S{temp}",
    /*retry=*/"M604 T1 S{temp}",
    /*flush=*/"M604 T2 S{temp}",
    /*release=*/"SET_PRINT_MAIN_STATUS MAIN_STATUS=ready",
    /*cleanup=*/"M104 S0\nENABLE_ALL_SENSOR",
};

// QIDI's stock screen drives M604 to load and M603 to unload. Both take the
// temperature as `params.S` (default 250) and drive it themselves with
// `M104 S{hotendtemp}` followed by a blocking `M109 S{hotendtemp}`, so the
// hotend is already at temperature by the time either extrudes - a preheat in
// front of one waits for the same heat twice, at whatever temperature we picked
// rather than the one the macro then commands. AmsBackendQidi drives
// `M603 S<temp>` for the same reason.
//
// Neither spelling means anything in Marlin either (M603 there configures a
// filament change and there is no M604), so a Klipper config defining them is
// running QIDI's convention.
//
// Both also open with `_CG28`, QIDI's conditional home
// (`{% if "xyz" not in printer.toolhead.homed_axes %} G28 {% endif %}`), so on
// the macro tier neither needs a home sent in front of it.
//
// That is a claim about these macros run ALONE. AmsBackendQidi composes them
// with CLEAR_NOZZLE, which has no `_CG28` and reaches Z through MOVE_TO_TRASH's
// XY-only guard - which is why the backend still answers
// delegates_homing_to_printer() false and routes through ensure_homed_then().
//
// Both also run DISABLE_ALL_SENSOR and nothing turns the filament sensor back on
// before PRINT_START: M604's wizard ends with ENABLE_ALL_SENSOR in its cleanup,
// and M603 gets it sent after.
constexpr std::array<MacroProfile, 2> kMacroProfiles{{
    {"M604", "QIDI stock", /*heats_hotend=*/true, /*needs_home_first=*/false,
     /*restore_after=*/"", &kQidiM604Wizard},
    {"M603", "QIDI stock", /*heats_hotend=*/true, /*needs_home_first=*/false,
     /*restore_after=*/"ENABLE_ALL_SENSOR", nullptr},
}};

/// The row for @p macro_name, or nullptr when no firmware we know ships it.
const MacroProfile* find_profile(const std::string& macro_name) {
    if (macro_name.empty()) {
        return nullptr;
    }
    const std::string upper = helix::text_io::to_upper(macro_name);
    for (const MacroProfile& p : kMacroProfiles) {
        if (upper == p.name) {
            return &p;
        }
    }
    return nullptr;
}

std::string render(const char* tmpl, int temp_c) {
    std::string out = tmpl;
    const std::string temp = std::to_string(temp_c);
    for (size_t at = out.find("{temp}"); at != std::string::npos;
         at = out.find("{temp}", at + temp.size())) {
        out.replace(at, 6, temp);
    }
    return out;
}

GuidedLoad render_guided_load(const GuidedLoadProfile& w, int temp_c) {
    GuidedLoad g;
    g.waiting_object = w.waiting_object;
    g.waiting_field = w.waiting_field;
    g.waiting_value = w.waiting_value;
    g.feed = render(w.feed, temp_c);
    g.retry = render(w.retry, temp_c);
    g.flush = render(w.flush, temp_c);
    g.release = render(w.release, temp_c);
    g.cleanup = render(w.cleanup, temp_c);
    return g;
}

} // namespace

bool macro_heats_hotend(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p && p->heats_hotend;
}

bool macro_needs_home_first(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p && p->needs_home_first;
}

std::optional<GuidedLoad> guided_load(const std::string& macro_name,
                                      const std::map<std::string, std::string>& sent_params) {
    const MacroProfile* p = find_profile(macro_name);
    if (!p || !p->guided_load) {
        return std::nullopt;
    }
    const GuidedLoadProfile& w = *p->guided_load;
    int temp_c = w.default_temp_c;
    for (const auto& [key, value] : sent_params) {
        if (helix::text_io::to_upper(key) == w.temp_param) {
            const int sent = helix::text_io::parse_leading<int>(value).value_or(0);
            if (sent > 0) {
                temp_c = sent;
            }
        }
    }
    return render_guided_load(w, temp_c);
}

std::optional<GuidedLoad> guided_load_example() {
    for (const MacroProfile& p : kMacroProfiles) {
        if (p.guided_load) {
            return render_guided_load(*p.guided_load, p.guided_load->default_temp_c);
        }
    }
    return std::nullopt;
}

nlohmann::json guided_load_status_query(const GuidedLoad& steps) {
    return {{"objects", {{steps.waiting_object, nlohmann::json::array({steps.waiting_field})}}}};
}

bool guided_load_waits(const nlohmann::json& query_response, const GuidedLoad& steps) {
    const nlohmann::json* result = json_util::find_member(query_response, "result");
    const nlohmann::json* status = result ? json_util::find_member(*result, "status") : nullptr;
    const nlohmann::json* object =
        status ? json_util::find_member(*status, steps.waiting_object.c_str()) : nullptr;
    const nlohmann::json* field =
        object ? json_util::find_member(*object, steps.waiting_field.c_str()) : nullptr;
    return field && field->is_string() && json_util::as_string(*field) == steps.waiting_value;
}

std::string restore_after(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p ? p->restore_after : std::string{};
}

} // namespace helix::filament_macros
