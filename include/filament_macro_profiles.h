// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_macro_profiles.h
 * @brief What a printer's stock filament macros do for themselves.
 *
 * Most load/unload macros expect the hotend to already be at temperature, so
 * every dispatch surface heats first and waits. Some firmwares ship macros that
 * drive the hotend themselves, and preheating in front of one of those makes the
 * user wait twice and can settle on a different temperature than the macro then
 * commands.
 *
 * This module is the ONLY place that names which firmware's macros behave that
 * way. Generic code (the filament panel, the AMS sidebar, the shared dispatch
 * ladder) asks the capability question and never names a firmware.
 *
 * Adding a firmware means adding one row to the table in
 * filament_macro_profiles.cpp - no call site changes.
 *
 * The backend tier has its own answer to the same question,
 * AmsBackend::supports_auto_heat_on_load(). Both feed
 * helix::ui::preheat_skip_reason(), which is where a caller should ask.
 */

#pragma once

#include "json_fwd.h"

#include <map>
#include <optional>
#include <string>

namespace helix::filament_macros {

/**
 * @brief Does @p macro_name bring the hotend to temperature on its own?
 *
 * Answered from the macro NAME alone, which is narrower than it looks: the
 * names in the table are firmware-specific spellings that carry no meaning in
 * stock Klipper, so a printer that defines one is running that firmware. A
 * generic name a user could plausibly write themselves (LOAD_FILAMENT,
 * UNLOAD_FILAMENT, M701, M702) is never in the table, because there the name
 * proves nothing about the body - those users are served by the
 * "Allow cold load/unload" setting instead.
 *
 * Klipper macro names are case-insensitive; @p macro_name may be in any case.
 *
 * @param macro_name Resolved macro name, e.g. StandardMacroInfo::get_macro()
 * @return true when a UI preheat in front of this macro would be redundant
 */
[[nodiscard]] bool macro_heats_hotend(const std::string& macro_name);

/**
 * @brief Must the printer be homed before @p macro_name runs?
 *
 * True only for a known macro whose body moves the toolhead with no home of its
 * own. Any macro not in the table answers false and runs as it is: a generic
 * LOAD_FILAMENT may do nothing but extrude, and a G28 in front of it parks the
 * head wherever homing leaves it (#1819). A known macro that carries its own
 * conditional home (QIDI's M604/M603 open with `_CG28`) answers false too.
 *
 * Answered from the name for the same reason macro_heats_hotend() is: only
 * firmware-specific spellings whose bodies are known belong in the table.
 *
 * The backend tier has its own answer, AmsBackend::delegates_homing_to_printer().
 * Both feed helix::ui::needs_prerequisite_home(), which is where a caller should ask.
 *
 * @param macro_name Resolved macro name, e.g. StandardMacroInfo::get_macro()
 * @return true when a caller must home before sending the macro
 */
[[nodiscard]] bool macro_needs_home_first(const std::string& macro_name);

/**
 * @brief The steps a stock screen runs around a load macro that is one step of a wizard.
 *
 * Every script is rendered for the temperature the macro ran at. The
 * waiting-generation scripts (feed, flush, release) exist only on firmware where
 * the load macro stops for a hand push, and are sent only after the printer
 * reported that wait: older firmware does not know their commands, and an
 * unknown command aborts the whole script it sits in.
 *
 * Callers send the scripts and ask the functions below; the status probe fields
 * are this module's to read.
 */
struct GuidedLoad {
    /// Grip the pushed filament and extrude it to the nozzle.
    std::string feed;
    /// Extrude again because nothing came out of the nozzle yet.
    std::string retry;
    /// The user saw new filament: purge the old colour out.
    std::string flush;
    /// Put the printer back in its idle state after a wizard that waited.
    std::string release;
    /// What every ending sends: the nozzle off and the filament sensor back on.
    std::string cleanup;

    /// Read by guided_load_status_query() / guided_load_waits() only.
    std::string waiting_object;
    std::string waiting_field;
    std::string waiting_value;
};

/**
 * @brief The wizard behind @p macro_name, or nullopt for a macro with none.
 *
 * @param sent_params The parameters the macro was sent with. The profile picks
 *        out its own temperature parameter and falls back to the macro's default.
 *
 * Case-insensitive, like the other questions here.
 */
[[nodiscard]] std::optional<GuidedLoad>
guided_load(const std::string& macro_name, const std::map<std::string, std::string>& sent_params);

/// A wizard to show without a printer behind it (previews, `ctl demo`), or nullopt.
[[nodiscard]] std::optional<GuidedLoad> guided_load_example();

/// The `printer.objects.query` params that read whether the macro is waiting.
[[nodiscard]] nlohmann::json guided_load_status_query(const GuidedLoad& steps);

/**
 * @brief Whether a reply to guided_load_status_query() reports the hand-push wait.
 *
 * Any missing piece answers false: firmware without the object is firmware
 * whose load macro already extruded.
 */
[[nodiscard]] bool guided_load_waits(const nlohmann::json& query_response, const GuidedLoad& steps);

/**
 * @brief What @p macro_name leaves undone that the caller must send after it.
 *
 * A stock unload that switches the filament sensor off and never back on gets
 * the command that turns it on. Empty for every other macro.
 */
[[nodiscard]] std::string restore_after(const std::string& macro_name);

} // namespace helix::filament_macros
