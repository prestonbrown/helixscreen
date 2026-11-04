// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_op_router.h
 * @brief Tiers 2 and 3 of the shared filament dispatch ladder.
 *
 * filament_op_dispatch.h answers *which* tier a Load/Unload takes. This header
 * owns what the two non-backend tiers actually do, so the three dispatch
 * surfaces (FilamentPanel, AmsOperationSidebar, FilamentRunoutHandler) share
 * one macro-parameter policy and one raw-gcode fallback instead of three.
 *
 * Tier 1 stays with the callers: the backend call is inseparable from each
 * surface's own guard/stepper/spinner bookkeeping.
 */

#pragma once

#include "i_moonraker_api.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "tool_state.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief The process-wide MacroParamModal every filament surface shares.
 *
 * A function-local static, exactly as it was when it lived in
 * ui_panel_filament.cpp: one instance for the whole app, alive for the process
 * lifetime.
 *
 * That lifetime is load-bearing and easy to miss. MacroParamModal stores its
 * on_execute_ callback and does NOT clear it on dismiss — only the next
 * show_for_*() overwrites it. So a callback handed to this modal can fire
 * arbitrarily later, long after the object that built it is gone. FilamentPanel
 * is an immortal singleton and may capture [this] directly [L012]. Every other
 * caller MUST route through an AsyncLifetimeGuard token; AmsOperationSidebar is
 * destroyed whenever the AMS panel closes, which is a live use-after-free
 * without one.
 */
helix::MacroParamModal& get_filament_param_modal();

/// Whether a tier-2 dispatch may interrupt the user to collect parameters.
enum class ParamPolicy {
    /// Raise MacroParamModal when the macro takes (or may take) parameters.
    Prompt,
    /// Run with no parameters and never raise a modal. Required for any surface
    /// that already owns a dialog — a second modal would stack on top of a live
    /// one whose observers keep firing underneath it.
    Suppress,
};

/**
 * @brief How a parameter prompt is raised.
 *
 * The default installs get_filament_param_modal() on lv_screen_active(). It is
 * an injection point so the prompt branch is reachable in a test binary with no
 * screen, and so a future surface can present parameters its own way without
 * forking dispatch_filament_macro().
 */
using ParamPrompter = std::function<void(
    const std::string& macro_name, const helix::CachedMacroInfo& cached,
    const std::map<std::string, std::string>& prefill, helix::MacroExecuteCallback on_execute)>;

/// Install a prompter. Pass a default-constructed ParamPrompter to restore the
/// shared-modal default.
void set_filament_param_prompter(ParamPrompter prompter);

/**
 * @brief Tier 2: dispatch the user's configured macro.
 *
 * Resolves @p macro_name against MacroParamCache. Under ParamPolicy::Suppress
 * @p run always fires immediately with an empty MacroParamResult. Under
 * ParamPolicy::Prompt:
 *   - a macro with no parameters runs immediately with an empty result;
 *   - a KNOWN_PARAMS macro whose every parameter @p known_values names runs
 *     immediately with exactly those values, and no prompt;
 *   - otherwise the prompter is raised, with the @p known_values the macro reads
 *     typed in, and @p run fires only if the user confirms. An UNKNOWN macro
 *     always prompts, with nothing typed in.
 *
 * @param known_values Values the surface already knows, keyed by parameter name.
 *        Names the macro does not read are never sent.
 *
 * @warning When a prompt is raised, @p run outlives this call and is retained
 *          by the shared modal past dismissal. Callers that are not immortal
 *          must capture an AsyncLifetimeGuard token in @p run, not a bare
 *          `this`.
 *
 * @return true if a prompt was raised (@p run fires later, or never). false if
 *         @p run already ran synchronously, before this call returned: with no
 *         parameters, or with @p known_values filling every parameter.
 */
bool dispatch_filament_macro(const std::string& macro_name, ParamPolicy policy,
                             helix::MacroExecuteCallback run,
                             const std::map<std::string, std::string>& known_values = {});

/// The filament macro a nozzle-temperature prefill is for. Each reads the
/// temperature under its own parameter names.
enum class FilamentMacroOp { Load, Unload, Purge };

/**
 * @brief The nozzle temperature a filament surface can hand @p op's macro, keyed
 *        by the parameter names that kind of macro reads it under.
 *
 * The temperature is filament_op_nozzle_temp() of @p material_temp_c and the LIVE
 * @p extruder_target_c. A backend load shares that rule but raises the material to
 * the latched last non-zero target instead (AmsOperationSidebar::handle_load_with_preheat),
 * so a nozzle whose target has dropped to 0 still reheats there to purge, while a
 * prefill offers the material temperature alone.
 *
 * Nothing is offered at or below @p min_extrude_c, or above @p max_nozzle_c; the
 * dialog asks instead. M109 returns within a degree of its target and Klipper checks
 * its smoothed temperature against the minimum, so a macro heated to exactly the
 * minimum can still be refused extrusion. Klipper rejects a target above the hotend's
 * max_temp outright, and a macro sent with no dialog leaves the user no way to lower it.
 *
 * Load and Unload offer EXTRUDER_TEMP, NOZZLE_TEMP and TEMP; Purge offers only
 * PURGE_TEMP. A load macro that also reads PURGE_TEMP is left an unfilled
 * parameter, so it prompts rather than having its purge temperature replaced.
 *
 * @param min_extrude_c Whole-degree extrusion minimum, from temperature::extrusion_floor_c()
 * @param max_nozzle_c  Whole-degree hotend ceiling, from temperature::nozzle_max_temp_c()
 */
[[nodiscard]] std::map<std::string, std::string>
nozzle_temp_prefill(FilamentMacroOp op, int extruder_target_c, std::optional<int> material_temp_c,
                    int min_extrude_c, int max_nozzle_c);

/**
 * @brief The Klipper extruder that tool @p tool heats.
 *
 * That tool's extruder_name, or @p active_extruder when @p tool is no tool (< 0),
 * is not among @p tools, or names no extruder.
 */
[[nodiscard]] std::string extruder_for_tool(int tool, const std::vector<helix::ToolInfo>& tools,
                                            const std::string& active_extruder);

/**
 * @brief nozzle_temp_prefill() for a filament op acting on @p slot_index, held to
 *        the extruder that slot feeds.
 *
 * The extruder is extruder_for_tool() of the slot's mapped tool, and its own live
 * target, min_extrude_temp and max_temp bound the offer, so each tool of a
 * toolchanger is held to its own hotend. An op on no slot, or on a slot whose
 * tool or extruder is unknown, uses the active extruder.
 *
 * @param material_temp_c The material temperature the surface resolved for the slot.
 */
[[nodiscard]] std::map<std::string, std::string>
slot_nozzle_temp_prefill(FilamentMacroOp op, int slot_index, std::optional<int> material_temp_c,
                         helix::PrinterState& state, const SafetyLimits& limits);

/**
 * @brief Tier 3 load fallback: fast move through the bowden, then a slow push
 *        into the melt zone. M83 = relative extrusion.
 */
[[nodiscard]] std::string filament_load_fallback_gcode();

/**
 * @brief Tier 3 unload fallback: tip-shape (push, quick pull, dwell) then a
 *        long retract. M83 = relative extrusion.
 */
[[nodiscard]] std::string filament_unload_fallback_gcode();

/**
 * @brief Run @p purge once the active filament backend's purge preamble
 *        (AmsBackend::purge_preamble_gcode(), asked with whether a job holds the
 *        machine right now) has completed.
 *
 * Every Purge surface sends its macro or raw fallback through this, so the
 * preamble cannot be skipped by one of them.
 *
 * No backend or an empty preamble: @p purge runs synchronously, nothing is sent.
 * Otherwise the preamble goes out first and @p purge runs on the main thread once
 * it succeeds; if it fails, @p on_error runs on the main thread instead and
 * @p purge never does.
 */
void run_after_purge_preamble(IMoonrakerAPI* api, const char* log_tag,
                              std::function<void()> purge,
                              std::function<void(const MoonrakerError&)> on_error);

} // namespace helix::ui
