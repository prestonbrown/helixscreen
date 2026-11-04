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

#include "macro_param_cache.h"
#include "macro_param_modal.h"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace helix::ui {

/// Whether a tier-2 dispatch may interrupt the user to collect parameters.
enum class ParamPolicy {
    /// Raise MacroParamModal when the macro takes (or may take) parameters.
    Prompt,
    /// Run with no parameters and never raise a modal. Required for any surface
    /// that already owns a dialog — a second modal would stack on top of a live
    /// one whose observers keep firing underneath it.
    Suppress,
    /// Never raise a modal, but send the values the surface computed (and the
    /// saved defaults). For a dispatch nobody is watching any more: the op the
    /// user asked for still runs with the temperature they would have been offered.
    Unattended,
};

/**
 * @brief How a parameter prompt is raised.
 *
 * The default raises helix::get_macro_param_modal() on lv_screen_active(). It is
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
 * @brief Purge fallback: extrude a fixed 50mm at 10mm/s. M83 = relative
 *        extrusion. Purge has no plan_purge()/AmsBackend tier, so this is the
 *        whole tier-2 ladder alongside the configured macro, not "tier 3" of
 *        three.
 */
[[nodiscard]] std::string filament_purge_fallback_gcode();

} // namespace helix::ui
