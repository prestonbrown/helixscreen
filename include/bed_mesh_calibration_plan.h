// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"
#include "standard_macros.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
namespace bed_mesh {

/// The profile Klipper stores a mesh in when BED_MESH_CALIBRATE names none.
/// Klipper refuses `BED_MESH_PROFILE SAVE=default`, so a mesh only gets there by
/// being probed there.
inline constexpr const char* kDefaultProfile = "default";

/// What to send for a calibration the user named before probing, and where the
/// finished mesh ends up.
struct CalibrationPlan {
    /// The profile the user chose.
    std::string name;

    /// Gcode to send.
    std::string command;

    /// The command heats and homes the printer itself.
    bool self_prepares = false;

    /// The command is a sequence helixscreen ships for this printer.
    bool shipped = false;

    /// The profile the command stores the finished mesh in.
    std::string writes_profile;

    /// Set when the command cannot store under the chosen name: after it completes,
    /// load writes_profile and save it under this name. Never `default`.
    std::string copy_to;

    /// The profile holding the mesh once the plan has run.
    [[nodiscard]] const std::string& final_profile() const {
        return copy_to.empty() ? writes_profile : copy_to;
    }
};

/// @p name as a single gcode parameter value. Klipper splits a command's
/// parameters the way a shell does, with `#` starting a comment, so a name holding
/// whitespace, a quote, a backslash or `#` is quoted. Quoting cannot carry a `;`:
/// Klipper cuts the line there before its parameters are read, so such a name is
/// refused where it is typed (helix::ui::bed_mesh::check_profile_name()).
[[nodiscard]] inline std::string gcode_param_value(const std::string& name) {
    if (name.find_first_of(" \t\"'\\#") == std::string::npos) {
        return name;
    }
    std::string quoted = "\"";
    for (char c : name) {
        if (c == '"' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

/// `BED_MESH_PROFILE <verb>=<name>`, the name as one parameter value.
[[nodiscard]] inline std::string profile_command(const char* verb, const std::string& name) {
    return std::string("BED_MESH_PROFILE ") + verb + "=" + gcode_param_value(name);
}

/// ` PROFILE=<name>` for a named profile, empty for the default one, which is
/// what BED_MESH_CALIBRATE writes when given no profile.
[[nodiscard]] inline std::string profile_arg(const std::string& name) {
    return name == kDefaultProfile ? std::string() : " PROFILE=" + gcode_param_value(name);
}

/// Does Klipper refuse a `BED_MESH_PROFILE SAVE` under @p name?
[[nodiscard]] inline bool save_is_refused_for(const std::string& name) {
    return name == kDefaultProfile;
}

/// Is @p line Klipper declining a `BED_MESH_PROFILE SAVE`? It says so on the
/// console and still answers the request `ok`.
[[nodiscard]] inline bool is_profile_save_refusal(const std::string& line) {
    return line.find("Unable to save to profile") != std::string::npos ||
           line.find("has not been probed") != std::string::npos ||
           line.find("is reserved, please choose another profile name") != std::string::npos;
}

namespace detail {

/// The first word of a one-line @p script, uppercased; empty for several lines.
inline std::string single_command_word(const std::string& script) {
    if (script.find('\n') != std::string::npos) {
        return {};
    }
    const size_t start = script.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = script.find_first_of(" \t", start);
    std::string word = script.substr(start, end == std::string::npos ? end : end - start);
    word = helix::text_io::to_upper(word);
    return word;
}

/// The value of a PROFILE= parameter already on @p command, unquoted, or empty.
inline std::string named_profile(const std::string& command) {
    std::string upper = command;
    upper = helix::text_io::to_upper(upper);
    const size_t at = upper.find(" PROFILE=");
    if (at == std::string::npos) {
        return {};
    }
    // Read as Klipper's shell-style parameter parsing reads it: quotes group, and
    // inside double quotes a backslash escapes a quote or a backslash.
    std::string value;
    char quote = 0;
    for (size_t i = at + 9; i < command.size(); ++i) {
        const char c = command[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else if (quote == '"' && c == '\\' && i + 1 < command.size() &&
                       (command[i + 1] == '"' || command[i + 1] == '\\')) {
                value += command[++i];
            } else {
                value += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ' ' || c == '\t') {
            break;
        } else {
            value += c;
        }
    }
    return value;
}

} // namespace detail

/**
 * @brief Plan a mesh calibration whose result is to be stored as @p name.
 *
 * BED_MESH_CALIBRATE stores the finished mesh in the profile its PROFILE= names,
 * or in `default` when given none. A shipped sequence marks where that argument
 * goes with `{profile_arg}`; a bare BED_MESH_CALIBRATE, detected or configured,
 * takes it appended. Anything else (G29, a user's own macro, a sequence without
 * the placeholder) cannot be told a name, so it is left to store in `default`
 * and the plan copies the mesh from there.
 *
 * @param slot       The bed mesh slot to resolve.
 * @param name       Profile name the user chose, already trimmed and non-empty.
 * @param bed_temp_c Substituted for `{bed_temp}`.
 */
[[nodiscard]] inline CalibrationPlan plan_calibration(const StandardMacroInfo& slot,
                                                      const std::string& name, int bed_temp_c) {
    // accept_fallback=false: the HELIX_* fallback returns without probing when a
    // recent mesh exists, and a calibration the user asked for must probe.
    ResolvedMacroScript resolved =
        resolve_macro_script(slot, {.profile_arg = profile_arg(name), .bed_temp_c = bed_temp_c},
                             /*accept_fallback=*/false);
    if (resolved.script.empty()) {
        resolved.script = "BED_MESH_CALIBRATE";
    }

    CalibrationPlan plan;
    plan.name = name;
    plan.self_prepares = resolved.self_prepares;
    plan.shipped = resolved.shipped;
    plan.command = resolved.script;

    if (resolved.takes_profile_arg) {
        plan.writes_profile = name;
        return plan;
    }

    if (detail::single_command_word(resolved.script) == "BED_MESH_CALIBRATE") {
        const std::string already = detail::named_profile(resolved.script);
        if (already.empty()) {
            plan.command += profile_arg(name);
            plan.writes_profile = name;
            return plan;
        }
        plan.writes_profile = already;
    } else {
        plan.writes_profile = kDefaultProfile;
    }

    if (plan.writes_profile != name && !save_is_refused_for(name)) {
        plan.copy_to = name;
    }
    return plan;
}

/// Each stored profile's points as the bed_mesh object reports them; a profile
/// reported without points maps to null.
using StoredMeshes = std::map<std::string, nlohmann::json>;

/// The stored profiles in a `bed_mesh` status object.
[[nodiscard]] inline StoredMeshes stored_meshes_from_status(const nlohmann::json& bed_mesh) {
    StoredMeshes meshes;
    if (!bed_mesh.is_object() || !bed_mesh.contains("profiles") ||
        !bed_mesh["profiles"].is_object()) {
        return meshes;
    }
    for (const auto& [name, profile] : bed_mesh["profiles"].items()) {
        meshes[name] = profile.is_object() && profile.contains("points") ? profile["points"]
                                                                         : nlohmann::json();
    }
    return meshes;
}

/// Where a finished calibration's mesh turned out to be.
enum class CalibrationOutcome {
    Stored,   ///< In `to`, the profile the user chose. Nothing more to do.
    Copy,     ///< In `from`; load it and save it as `to`.
    NotStored ///< No profile holds a new mesh.
};

struct CalibrationCheck {
    CalibrationOutcome outcome = CalibrationOutcome::NotStored;
    std::string from;
    std::string to;
};

/**
 * @brief Judge a finished calibration by the stored profiles before and after it.
 *
 * A command is trusted to have stored a mesh only where one changed. A macro that
 * drops the PROFILE it was given stores in `default` instead, so a new mesh there
 * is copied to the chosen name; anything else is a calibration that stored nothing,
 * and whatever the profiles already held is never passed off as its result.
 */
[[nodiscard]] inline CalibrationCheck check_calibration(const CalibrationPlan& plan,
                                                        const StoredMeshes& before,
                                                        const StoredMeshes& after) {
    const auto changed = [&before, &after](const std::string& profile) {
        const auto now = after.find(profile);
        if (now == after.end()) {
            return false;
        }
        const auto was = before.find(profile);
        return was == before.end() || was->second != now->second;
    };

    if (changed(plan.writes_profile)) {
        if (plan.copy_to.empty()) {
            return {CalibrationOutcome::Stored, {}, plan.writes_profile};
        }
        return {CalibrationOutcome::Copy, plan.writes_profile, plan.copy_to};
    }
    if (plan.writes_profile != kDefaultProfile && changed(kDefaultProfile)) {
        if (save_is_refused_for(plan.name)) {
            return {CalibrationOutcome::Stored, {}, kDefaultProfile};
        }
        return {CalibrationOutcome::Copy, kDefaultProfile, plan.name};
    }
    return {CalibrationOutcome::NotStored, {}, {}};
}

/**
 * @brief Stored profiles a calibration planned by @p plan replaces, to confirm first.
 *
 * The chosen profile, unless it is `default`, which the dialog opens on; and the
 * profile the command stores in when that is not the chosen one.
 */
[[nodiscard]] inline std::vector<std::string>
profiles_replaced_by(const CalibrationPlan& plan, const std::vector<std::string>& stored) {
    const auto is_stored = [&stored](const std::string& profile) {
        return std::find(stored.begin(), stored.end(), profile) != stored.end();
    };
    std::vector<std::string> replaced;
    if (plan.name != kDefaultProfile && is_stored(plan.name)) {
        replaced.push_back(plan.name);
    }
    if (plan.writes_profile != plan.name && is_stored(plan.writes_profile)) {
        replaced.push_back(plan.writes_profile);
    }
    return replaced;
}

} // namespace bed_mesh
} // namespace helix
