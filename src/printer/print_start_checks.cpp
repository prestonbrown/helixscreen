// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file print_start_checks.cpp
 * @brief Pure core of the print-start gate pipeline: rules + gate fns.
 *
 * Rule bodies are ports of the imperative checks in
 * src/ui/ui_print_start_controller.cpp (unresolved_tools_for, the initiate()
 * spool-weight math, find_material_mismatches); the gate evaluate-functions
 * port the dialog message builders (build_empty_lane_message,
 * show_*_warning) verbatim. Behavior is ported as-is; the only deltas are the
 * PrintStartContext substitutions the design mandates (detail view / AmsState /
 * SettingsManager reads become ctx fields). Evaluate fns build strings
 * (lv_tr/fmt) but never touch lv objects or subjects.
 */

#include "print_start_checks.h"

#include "ui_filament_mapping_card.h"

#include "color_utils.h"
#include "filament_database.h"
#include "filament_variants.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <lvgl.h>

namespace helix {

namespace {
/// All-clear result shared by every gate's pass paths.
CheckResult pass_result() {
    CheckResult r;
    r.verdict = CheckResult::Verdict::Pass;
    return r;
}

/// Context-shaped wrapper over the exported rule (print_start_checks.h), which
/// the filament mapping card shares so the two cannot drift.
size_t print_lane_requirement(const PrintStartContext& ctx) {
    return helix::print_lane_requirement(ctx.tools_used, ctx.filament_color_count);
}

/// The one tool a bypass print runs on: the (single) used tool when the scan
/// knows it, else tool 0. Valid only when print_lane_requirement() <= 1.
int bypass_print_tool(const PrintStartContext& ctx) {
    if (!ctx.tools_used.empty()) {
        return *ctx.tools_used.begin();
    }
    return 0;
}

/// The two-step material comparison every branch runs. Kept in one place so a
/// branch cannot answer the same question differently: FilamentMapper decides
/// whether the polymer is right, filament::grades_match() whether the grade is.
/// An unknown material on either side is not something to warn about.
enum class MaterialVerdict { Ok, GradeChange, Mismatch };

MaterialVerdict compare_material(const std::string& expected, const std::string& loaded) {
    if (expected.empty() || loaded.empty()) {
        return MaterialVerdict::Ok;
    }
    if (!FilamentMapper::materials_match(expected, loaded)) {
        return MaterialVerdict::Mismatch;
    }
    return filament::grades_match(expected, loaded) ? MaterialVerdict::Ok
                                                    : MaterialVerdict::GradeChange;
}

/// Log label for a verdict, so the two spdlog lines say which finding fired.
const char* detail_kind(MaterialVerdict v) {
    return v == MaterialVerdict::GradeChange ? "grade change" : "material mismatch";
}

/// Warning result with the dialog strings a gate mandates.
CheckResult warn_result(std::string title, std::string body, std::string proceed_label) {
    CheckResult r;
    r.verdict = CheckResult::Verdict::Warn;
    r.title = std::move(title);
    r.body = std::move(body);
    r.proceed_label = std::move(proceed_label);
    r.severity = GateSeverity::Warning;
    return r;
}

/// Ported verbatim from PrintStartController::build_empty_lane_message.
std::string build_empty_lane_message(const std::vector<std::pair<int, int>>& empty) {
    // Name the offending tool(s) and the AMS lane each routes to so the user
    // knows exactly which lane to load. Lane numbers are 1-based for display
    // (slot 0 -> "Lane 1") to match the rest of the slot UI.
    std::string message;
    if (empty.size() == 1) {
        message = fmt::format(lv_tr("Tool {} → Lane {}: no filament loaded."), empty[0].first,
                              empty[0].second + 1);
    } else {
        message = lv_tr("These tools have no filament loaded:");
        message += "\n\n";
        for (const auto& [tool, slot] : empty) {
            message += fmt::format("  {} {} {} → {} {}\n", LV_SYMBOL_BULLET, lv_tr("Tool"), tool,
                                   lv_tr("Lane"), slot + 1);
        }
    }
    message += "\n\n";
    message += lv_tr("Start print anyway?");
    return message;
}

// ---- gate evaluate-functions (dialog-bearing halves of the rules above) ----

CheckResult gate_insufficient_spool_weight(const PrintStartContext& ctx) {
    auto weights = insufficient_spool_weight_in(ctx);
    if (!weights.has_value()) {
        return pass_result();
    }

    // Body ported verbatim from show_insufficient_filament_warning.
    char body[256];
    std::snprintf(body, sizeof(body),
                  lv_tr("This print needs about %.0fg but the spool has about %.0fg "
                        "remaining. Start anyway?"),
                  weights->first, weights->second);

    return warn_result(lv_tr("Not Enough Filament"), body, lv_tr("Start Anyway"));
}

CheckResult gate_bypass_engaged_lane_print(const PrintStartContext& ctx) {
    // Single-tool prints with bypass engaged are the LEGITIMATE bypass use
    // case — silent. "Single-tool" is the used-tool count, not the palette
    // size (see print_lane_requirement). Multi-tool means the file needs AMS
    // lanes, and firmware (AFC _check_bypass, verified) refuses a lane load
    // while bypass filament is in the toolhead.
    if (!ctx.any_bypass_active || print_lane_requirement(ctx) <= 1) {
        return pass_result();
    }
    return warn_result(
        lv_tr("Bypass Is Active"),
        lv_tr("Bypass is active, but this print uses AMS lanes. The printer may refuse "
              "to load a lane while bypass filament is in the toolhead. Remove the "
              "filament and turn bypass off before printing. Start anyway?"),
        lv_tr("Start Anyway"));
}

CheckResult gate_unaccounted_toolhead_filament(const PrintStartContext& ctx) {
    // Bypass accounts for toolhead filament (previous gate's case). nullopt =
    // backend cannot determine -> stay silent rather than guess.
    if (ctx.any_bypass_active) {
        return pass_result();
    }
    for (const auto& answer : ctx.toolhead_unaccounted) {
        if (answer.has_value() && *answer) {
            return warn_result(lv_tr("Filament In The Toolhead"),
                               lv_tr("The toolhead has filament but no AMS lane reports it loaded. "
                                     "Pull it out manually before printing. Start anyway?"),
                               lv_tr("Start Anyway"));
        }
    }
    return pass_result();
}

CheckResult gate_required_filament_present(const PrintStartContext& ctx) {
    // Backends that auto-unload the toolhead after each print (e.g. AD5X IFS)
    // leave the extruder empty by design, so a "no filament" reading at
    // print-start is expected and the warning is noise. ANY such backend
    // suppresses the warning.
    if (ctx.any_auto_unload_backend) {
        return pass_result();
    }

    // AMS lane-truth path: scoped to the tools the print actually uses,
    // consulting the backend's authoritative per-slot presence instead of the
    // aggregate motion sensors (avoids staged-but-retracted and unused-empty
    // false positives).
    //
    // A single-tool bypass print feeds from the external spool, not from any
    // lane — the lanes it happens to map to are irrelevant, and an empty one
    // must not block the print. Bypass engage itself proved filament at the
    // toolhead. (Multi-tool bypass keeps the check; gate 2 already warned.)
    if (ctx.any_bypass_active && print_lane_requirement(ctx) <= 1) {
        return pass_result();
    }
    if (ctx.ams_manages_filament && ctx.has_active_backend) {
        if (!ctx.empty_required_lanes.empty()) {
            spdlog::info("[PrintStartController] {} required tool(s) have an empty lane - "
                         "showing pre-print warning",
                         ctx.empty_required_lanes.size());
            return warn_result(lv_tr("No Filament Detected"),
                               build_empty_lane_message(ctx.empty_required_lanes),
                               lv_tr("Start Print"));
        }
        return pass_result();
    }

    // Non-AMS / no-active-backend fallback: aggregate runout-sensor check.
    if (ctx.runout_enabled && ctx.runout_available && !ctx.runout_detected) {
        spdlog::info("[PrintStartController] Runout sensor shows no filament - showing pre-print "
                     "warning");
        return warn_result(
            lv_tr("No Filament Detected"),
            lv_tr("The runout sensor indicates no filament is loaded. Start print anyway?"),
            lv_tr("Start Print"));
    }

    return pass_result();
}

CheckResult gate_unresolved_tools(const PrintStartContext& ctx) {
    auto unresolved = unresolved_tools_in(ctx);
    if (unresolved.empty()) {
        return pass_result();
    }

    // Body ported verbatim from show_color_mismatch_warning (no static buffer:
    // modal_show_confirmation copies the message string).
    std::string message = lv_tr("These tools have no matching filament loaded:");
    message += "\n\n";
    for (int tool_idx : unresolved) {
        // Look up by real tool_index — tool_info may be used-filtered
        // (compacted), so its vector position no longer equals the tool number.
        const auto* tool = ui::FilamentMappingCard::find_by_tool_index(ctx.tool_info, tool_idx);
        if (tool) {
            std::string color_name = describe_color(tool->color_rgb);
            message += "  " + std::string(LV_SYMBOL_BULLET) + " T" + std::to_string(tool_idx) +
                       ": " + color_name;
            if (!tool->material.empty()) {
                message += " (" + tool->material + ")";
            }
            message += "\n";
        }
    }
    message += "\n";
    message += lv_tr("Load the required filaments or start anyway?");

    return warn_result(lv_tr("Color Mismatch"), std::move(message), lv_tr("Start Anyway"));
}

/// Dialog for mismatches that are ALL grade changes: same polymer, different
/// filler. Separate from the material dialog because the advice is different
/// and because calling a correct-polymer lane a "material mismatch" trains the
/// user to click through the dialog that matters.
CheckResult grade_change_warning(const std::vector<MaterialMismatchDetail>& mismatches) {
    std::string message;

    if (mismatches.size() == 1) {
        const auto& m = mismatches[0];
        // The risk is not symmetric. Filled filament loaded against an unfilled
        // profile runs abrasive material at the base polymer's flow rate, on
        // what may well be a brass nozzle. The reverse just prints slower and
        // hotter than the spool needs, which costs time and nothing else.
        if (filament::is_filled_grade(m.loaded_material)) {
            message = fmt::format(lv_tr("The loaded spool is {}, but this file was sliced "
                                        "for {}. Filled filament runs at a lower flow rate "
                                        "and needs a hardened nozzle."),
                                  m.loaded_material, m.expected_material);
        } else {
            message = fmt::format(lv_tr("This file was sliced for {}, but {} is loaded. "
                                        "It will print slower and hotter than the loaded "
                                        "spool needs."),
                                  m.expected_material, m.loaded_material);
        }
    } else {
        message = lv_tr("These tools have a different filament grade loaded:");
        message += "\n\n";
        for (const auto& m : mismatches) {
            message += fmt::format("  {} T{}: {} {}: {} {}\n", LV_SYMBOL_BULLET, m.tool_index,
                                   lv_tr("needs"), m.expected_material, lv_tr("you have"),
                                   m.loaded_material);
        }
    }

    message += "\n\n";
    message += lv_tr("Start print anyway?");

    return warn_result(lv_tr("Filament Grade Mismatch"), std::move(message), lv_tr("Start Anyway"));
}

CheckResult gate_material_compatibility(const PrintStartContext& ctx) {
    auto mismatches = material_mismatches_in(ctx);
    if (mismatches.empty()) {
        return pass_result();
    }

    // A hard mismatch anywhere keeps the material dialog, grade rows included:
    // the wrong polymer is the more urgent finding, and two dialogs on one tap
    // is worse than one that lists everything.
    if (std::all_of(mismatches.begin(), mismatches.end(),
                    [](const MaterialMismatchDetail& m) { return m.grade_only; })) {
        return grade_change_warning(mismatches);
    }

    // Body ported verbatim from show_material_mismatch_warning (no static
    // buffer: modal_show_confirmation copies the message string).
    std::string message;

    if (mismatches.size() == 1) {
        // Single-tool format: "This file was sliced for X but Y is loaded."
        const auto& m = mismatches[0];
        message = fmt::format(lv_tr("This file was sliced for {} but {} is loaded."),
                              m.expected_material, m.loaded_material);

        // Add temperature details if available
        if (m.expected_nozzle_min > 0 && m.loaded_nozzle_min > 0) {
            message += "\n\n";
            message +=
                fmt::format("  {} {}: {}\u2013{}°C {}, {}°C {}\n"
                            "  {} {}: {}\u2013{}°C {}, {}°C {}",
                            LV_SYMBOL_BULLET, m.expected_material, m.expected_nozzle_min,
                            m.expected_nozzle_max, lv_tr("nozzle"), m.expected_bed_temp,
                            lv_tr("bed"), LV_SYMBOL_BULLET, m.loaded_material, m.loaded_nozzle_min,
                            m.loaded_nozzle_max, lv_tr("nozzle"), m.loaded_bed_temp, lv_tr("bed"));
        }
    } else {
        // Multi-tool format: list each mismatched tool
        message = lv_tr("These tools have incompatible materials loaded:");
        message += "\n\n";
        for (const auto& m : mismatches) {
            std::string expected_temps;
            if (m.expected_nozzle_min > 0) {
                expected_temps =
                    fmt::format(" ({}\u2013{}°C)", m.expected_nozzle_min, m.expected_nozzle_max);
            }
            std::string loaded_temps;
            if (m.loaded_nozzle_min > 0) {
                loaded_temps =
                    fmt::format(" ({}\u2013{}°C)", m.loaded_nozzle_min, m.loaded_nozzle_max);
            }
            // "needs X (range): You have Y (range)" — clearer than the old
            // "X -> Y" form, which read as a transformation rather than a
            // comparison. Two short clauses joined by a colon scan well.
            message += fmt::format("  {} T{}: {} {}{}: {} {}{}\n", LV_SYMBOL_BULLET, m.tool_index,
                                   lv_tr("needs"), m.expected_material, expected_temps,
                                   lv_tr("you have"), m.loaded_material, loaded_temps);
        }
    }

    message += "\n\n";
    message += lv_tr("Printing with the wrong material can cause clogs, poor adhesion, "
                     "or failed prints.");

    return warn_result(lv_tr("Material Mismatch"), std::move(message), lv_tr("Start Anyway"));
}
/// Material mismatch detail for a print running on the external spool:
/// temperature context from the filament database (or the spool's own preset
/// when the user set one). Shared by the non-AMS path and the bypass path.
MaterialMismatchDetail external_spool_mismatch(const SlotInfo& spool, int tool_index,
                                               const std::string& expected) {
    MaterialMismatchDetail detail;
    detail.tool_index = tool_index;
    detail.expected_material = expected;
    detail.loaded_material = spool.material;

    // Temperature from filament database for expected material
    if (auto expected_info = filament::find_material(expected)) {
        detail.expected_nozzle_min = expected_info->nozzle_min;
        detail.expected_nozzle_max = expected_info->nozzle_max;
        detail.expected_bed_temp = expected_info->bed_temp;
    }

    // Temperature from external spool (user-set) or fall back to database
    if (spool.nozzle_temp_min > 0 && spool.nozzle_temp_max > 0) {
        detail.loaded_nozzle_min = spool.nozzle_temp_min;
        detail.loaded_nozzle_max = spool.nozzle_temp_max;
        detail.loaded_bed_temp = spool.bed_temp;
    } else if (auto loaded_info = filament::find_material(spool.material)) {
        detail.loaded_nozzle_min = loaded_info->nozzle_min;
        detail.loaded_nozzle_max = loaded_info->nozzle_max;
        detail.loaded_bed_temp = loaded_info->bed_temp;
    }
    return detail;
}

} // namespace

size_t print_lane_requirement(const std::set<int>& tools_used, size_t filament_color_count) {
    if (!tools_used.empty()) {
        return tools_used.size();
    }
    return filament_color_count;
}

std::vector<int> unresolved_tools_in(const PrintStartContext& ctx) {
    // Single-color prints need no mapping.
    if (ctx.filament_color_count <= 1) {
        return {};
    }

    // Bypass / external spool: filament reaches the nozzle without passing through
    // any slot, so every tool is "unresolved" by construction and the warning is
    // guaranteed noise. Same reasoning as PreflightValidator's bypass early-out —
    // this is the second gate on the same Print tap, and skipping only the first
    // one just moves the nag rather than removing it.
    if (ctx.any_bypass_active) {
        spdlog::debug("[PrintStartController] Bypass active - skipping unresolved-tool check");
        return {};
    }

    if (ctx.mappings.empty()) {
        // No mappings = AMS not available or card not shown
        spdlog::debug("[PrintStartController] No filament mappings available");
        return {};
    }

    auto unresolved = FilamentMapper::find_unresolved_tools(ctx.mappings);
    if (!unresolved.empty()) {
        spdlog::info("[PrintStartController] {} tools have no matching AMS slot",
                     unresolved.size());
    }
    return unresolved;
}

std::optional<std::pair<float, float>> insufficient_spool_weight_in(const PrintStartContext& ctx) {
    const auto& spool = ctx.external_spool;
    if (!spool.has_value() || !(spool->remaining_weight_g > 0.0f)) {
        return std::nullopt;
    }
    if (!ctx.metadata.has_value()) {
        return std::nullopt;
    }

    float needed_g = static_cast<float>(ctx.metadata->filament_weight_total);
    if (needed_g <= 0.0f && ctx.metadata->filament_total > 0.0) {
        // Fall back to length-based estimate using the spool's material.
        auto mat = filament::find_material(spool->material);
        if (mat.has_value() && mat->density_g_cm3 > 0.0f) {
            needed_g = filament::length_to_weight_g(
                static_cast<float>(ctx.metadata->filament_total), mat->density_g_cm3, 1.75f);
        }
    }
    if (needed_g > 0.0f && needed_g > spool->remaining_weight_g) {
        spdlog::info("[PrintStartController] Pre-print warning: needs {} g, spool has {} g",
                     needed_g, spool->remaining_weight_g);
        return std::make_pair(needed_g, spool->remaining_weight_g);
    }
    return std::nullopt;
}

std::vector<MaterialMismatchDetail> material_mismatches_in(const PrintStartContext& ctx) {
    std::vector<MaterialMismatchDetail> mismatches;

    if (!ctx.has_detail_view) {
        return mismatches;
    }

    // Per-tool filament weights from gcode metadata. Used to skip tools the
    // slicer assigned a material to but that never actually extrude (common
    // when a multi-tool profile prints a single-tool file: T0..T2 inherit the
    // profile's defaults but only T3 is used). Empty vector means the slicer
    // didn't emit per-tool data — in that case we keep the old behavior and
    // check every tool. Graceful fallback, no false-negatives possible.
    std::vector<double> filament_weights;
    if (ctx.metadata.has_value()) {
        filament_weights = ctx.metadata->filament_weights;
    }
    auto tool_is_used = [&filament_weights](int tool_index) -> bool {
        if (filament_weights.empty()) {
            return true; // No data → check everything (old behavior).
        }
        if (tool_index < 0 || tool_index >= static_cast<int>(filament_weights.size())) {
            return true; // Out-of-range → can't prove unused, be safe.
        }
        return filament_weights[tool_index] > 0.0;
    };

    if (ctx.any_bypass_active && print_lane_requirement(ctx) <= 1) {
        // Single-tool bypass print: the material actually printing is the
        // EXTERNAL spool's, whatever the lane mapping happens to say. Compare
        // the file's material for the used tool against the external spool —
        // same comparison the non-AMS path runs, scoped to the bypass tool.
        // The mapped lanes describe filament that is not being printed with.
        std::string expected;
        const int tool = bypass_print_tool(ctx);
        if (const auto* t = ui::FilamentMappingCard::find_by_tool_index(ctx.tool_info, tool)) {
            expected = t->material;
        } else if (tool < static_cast<int>(ctx.filament_materials.size())) {
            expected = ctx.filament_materials[tool];
        }
        if (expected.empty() || !ctx.external_spool.has_value() ||
            ctx.external_spool->material.empty()) {
            return mismatches;
        }
        const auto verdict = compare_material(expected, ctx.external_spool->material);
        if (verdict != MaterialVerdict::Ok) {
            auto detail = external_spool_mismatch(ctx.external_spool.value(), tool, expected);
            detail.grade_only = (verdict == MaterialVerdict::GradeChange);
            mismatches.push_back(std::move(detail));
            spdlog::info("[PrintStartController] Bypass print: {} vs external spool ({} vs {})",
                         detail_kind(verdict), expected, ctx.external_spool->material);
        }
        return mismatches;
    }

    if (ctx.ams_available) {
        // AMS path: check ToolMapping.material_mismatch flags
        const auto& mappings = ctx.mappings;
        const auto& tool_info = ctx.tool_info;
        const auto& slots = ctx.available_slots;

        for (const auto& m : mappings) {
            MaterialMismatchDetail detail;
            detail.tool_index = m.tool_index;

            // Get expected material from gcode tool info. Look up by real
            // tool_index — tool_info may be used-filtered (compacted), so its
            // vector position no longer equals the tool number.
            if (const auto* tool =
                    ui::FilamentMappingCard::find_by_tool_index(tool_info, m.tool_index)) {
                detail.expected_material = tool->material;
            }

            // Get loaded material from the mapped AMS slot
            for (const auto& slot : slots) {
                if (slot.slot_index == m.mapped_slot && slot.backend_index == m.mapped_backend) {
                    detail.loaded_material = slot.material;
                    break;
                }
            }

            // Skip if either material is unknown (can't warn about unknowns)
            if (detail.expected_material.empty() || detail.loaded_material.empty()) {
                continue;
            }

            // The mapper already ruled on the polymer, so trust its flag rather
            // than re-deciding it here; a lane it considers a MATCH still gets
            // the grade question asked, which is the only new work in this loop.
            if (!m.material_mismatch) {
                if (filament::grades_match(detail.expected_material, detail.loaded_material)) {
                    continue;
                }
                detail.grade_only = true;
            }

            // Deliberately after the two "nothing to report" outcomes above: a
            // tool the file never extrudes from is only worth a log line when
            // it would otherwise have produced a dialog row.
            if (!tool_is_used(m.tool_index)) {
                spdlog::debug("[PrintStartController] Skipping T{} {} — "
                              "tool has zero filament usage in gcode",
                              m.tool_index, detail.grade_only ? "grade change" : "mismatch");
                continue;
            }

            // Look up temperature ranges from the filament database
            auto expected_info = filament::find_material(detail.expected_material);
            if (expected_info) {
                detail.expected_nozzle_min = expected_info->nozzle_min;
                detail.expected_nozzle_max = expected_info->nozzle_max;
                detail.expected_bed_temp = expected_info->bed_temp;
            }

            auto loaded_info = filament::find_material(detail.loaded_material);
            if (loaded_info) {
                detail.loaded_nozzle_min = loaded_info->nozzle_min;
                detail.loaded_nozzle_max = loaded_info->nozzle_max;
                detail.loaded_bed_temp = loaded_info->bed_temp;
            }

            mismatches.push_back(std::move(detail));
        }
    } else {
        // Non-AMS path: compare gcode filament_type vs external spool.
        // ctx.external_spool is sourced from AmsState's layered getter
        // (SettingsManager + in-memory override) rather than raw
        // SettingsManager — deliberate unification; the in-memory override
        // only carries consumption hot-updates, so material values are
        // identical in practice.
        const auto& gcode_materials = ctx.filament_materials;
        if (gcode_materials.empty()) {
            return mismatches;
        }

        const auto& spool_info = ctx.external_spool;
        if (!spool_info || spool_info->material.empty()) {
            return mismatches;
        }

        // Check the first tool (single extruder)
        const auto& expected = gcode_materials[0];
        if (expected.empty()) {
            return mismatches;
        }

        const auto verdict = compare_material(expected, spool_info->material);
        if (verdict != MaterialVerdict::Ok) {
            auto detail = external_spool_mismatch(*spool_info, 0, expected);
            detail.grade_only = (verdict == MaterialVerdict::GradeChange);
            mismatches.push_back(std::move(detail));
        }
    }

    if (!mismatches.empty()) {
        spdlog::info("[PrintStartController] {} material mismatch(es) detected", mismatches.size());
    }
    return mismatches;
}

const std::vector<PrintStartGate>& default_print_start_gates() {
    // Order is behavior-preserving: the pre-pipeline check order, with the two
    // new gates (bypass + unaccounted toolhead) ahead of the ported four.
    static const std::vector<PrintStartGate> gates = {
        {"insufficient_spool_weight", gate_insufficient_spool_weight},
        {"bypass_engaged_lane_print", gate_bypass_engaged_lane_print},
        {"unaccounted_toolhead_filament", gate_unaccounted_toolhead_filament},
        {"required_filament_present", gate_required_filament_present},
        {"unresolved_tools", gate_unresolved_tools},
        {"material_compatibility", gate_material_compatibility},
    };
    return gates;
}

} // namespace helix
