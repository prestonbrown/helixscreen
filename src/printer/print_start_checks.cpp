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
#include "display_numbering.h"
#include "filament_database.h"
#include "filament_variants.h"
#include "gcode_ops_detector.h"
#include "macro_param_cache.h"
#include "operation_patterns.h"

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

/// The AvailableSlot for the (slot, backend) pair, resolved the same way
/// FilamentMapper::resolve_mapped_slot() and the mapping card do. nullptr
/// when the pair is not among @p available_slots.
const AvailableSlot* resolve_slot_for_display(const std::vector<AvailableSlot>& available_slots,
                                              int slot_index, int backend_index) {
    for (const auto& slot : available_slots) {
        if (slot.slot_index == slot_index && slot.backend_index == backend_index) {
            return &slot;
        }
    }
    return nullptr;
}

/// Label a lane for a print-start dialog: the unit-scoped label the mapping
/// card and remap modal use when the (slot, backend) pair resolves to a live
/// AvailableSlot, else a generic Slot label from the global index. A generic
/// label beats losing the whole warning over one unresolvable lane.
std::string lane_display_label(const std::vector<AvailableSlot>& available_slots, int slot_index,
                               int backend_index) {
    if (const auto* slot = resolve_slot_for_display(available_slots, slot_index, backend_index)) {
        return helix::ui::lane_label(slot->noun, slot->unit_display_name, slot->local_slot_index);
    }
    return helix::ui::lane_label(helix::ui::LaneNoun::Slot, slot_index);
}

/// Ported verbatim from PrintStartController::build_empty_lane_message.
///
/// @p empty pairs always come from FilamentSensorManager::scan_required_lanes(),
/// which scans backend 0 only (AmsState::get_backend()), so every lookup into
/// @p available_slots is pinned to that backend.
std::string build_empty_lane_message(const std::vector<std::pair<int, int>>& empty,
                                     const std::vector<AvailableSlot>& available_slots) {
    // Name the offending tool(s) and the AMS lane each routes to so the user
    // knows exactly which lane to load. tool_label() spells the gcode tool
    // ("T0"); lane_display_label() spells the backend's own word for the
    // position, numbered within its own unit ("Slot 1", "Turtle 2 · Lane 1").
    std::string message;
    if (empty.size() == 1) {
        message = fmt::format(lv_tr("{} → {}: no filament loaded."),
                              helix::ui::tool_label(empty[0].first),
                              lane_display_label(available_slots, empty[0].second, 0));
    } else {
        message = lv_tr("These tools have no filament loaded:");
        message += "\n\n";
        for (const auto& [tool, slot] : empty) {
            message += fmt::format("  {} {} → {}\n", LV_SYMBOL_BULLET, helix::ui::tool_label(tool),
                                   lane_display_label(available_slots, slot, 0));
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

CheckResult gate_insufficient_lane_weight(const PrintStartContext& ctx) {
    const auto shortfalls = insufficient_lane_weights_in(ctx);
    if (shortfalls.empty()) {
        return pass_result();
    }

    char body[512];
    if (shortfalls.size() == 1) {
        const auto& sf = shortfalls.front();
        std::snprintf(
            body, sizeof(body),
            lv_tr("%s has about %.0fg but %s needs about %.0fg. "
                  "Start anyway?"),
            helix::ui::lane_label(sf.noun, sf.unit_display_name, sf.local_slot_index).c_str(),
            sf.remaining_g, helix::ui::tool_label(sf.tool_index).c_str(), sf.needed_g);
    } else {
        // Name every short lane: the user's next move is to remap one of them,
        // and a count alone would not say which.
        std::string list;
        for (const auto& sf : shortfalls) {
            char one[128];
            std::snprintf(
                one, sizeof(one), lv_tr("%s: %.0fg of %.0fg needed"),
                helix::ui::lane_label(sf.noun, sf.unit_display_name, sf.local_slot_index).c_str(),
                sf.remaining_g, sf.needed_g);
            if (!list.empty()) {
                list += "\n";
            }
            list += one;
        }
        std::snprintf(body, sizeof(body), "%s\n%s\n%s",
                      lv_tr("Some mapped slots are short on filament:"), list.c_str(),
                      lv_tr("Start anyway?"));
    }
    return warn_result(lv_tr("Not Enough Filament"), body, lv_tr("Start Anyway"));
}

CheckResult gate_bypass_engaged_lane_print(const PrintStartContext& ctx) {
    // Single-tool prints with bypass engaged are the LEGITIMATE bypass use
    // case — silent. "Single-tool" is the used-tool count, not the palette
    // size (see print_lane_requirement). Multi-tool means the file needs AMS
    // lanes, and firmware (AFC _check_bypass, verified) refuses a lane load
    // while bypass filament is in the toolhead.
    //
    // With no filament system presented the external spool is the printer's
    // only path: there is no bypass to turn off, and the fit check answers a
    // file that wants more tools than the printer has.
    if (!ctx.any_bypass_active || !ctx.ams_available || print_lane_requirement(ctx) <= 1) {
        return pass_result();
    }
    return warn_result(
        lv_tr("Bypass Is Active"),
        lv_tr("Bypass is active, but this print uses AMS slots. The printer may refuse "
              "to load a slot while bypass filament is in the toolhead. Remove the "
              "filament and turn bypass off before printing. Start anyway?"),
        lv_tr("Start Anyway"));
}

CheckResult gate_unaccounted_toolhead_filament(const PrintStartContext& ctx) {
    // Bypass accounts for toolhead filament (previous gate's case). nullopt =
    // backend cannot determine -> stay silent rather than guess.
    if (ctx.any_bypass_active) {
        return pass_result();
    }
    for (size_t i = 0; i < ctx.toolhead_unaccounted.size(); ++i) {
        const auto& answer = ctx.toolhead_unaccounted[i];
        if (!answer.has_value() || !*answer) {
            continue;
        }
        // The detection is the same either way; the REMEDY is not. On hardware
        // with a lane-free heat/cut/retract (the CFS runs one for bypass
        // unloads) "pull it out manually" is both wrong and worse than what the
        // printer would do itself, and it is aimed at a hot toolhead. Where no
        // such path exists, a manual pull IS the answer, so that wording stays.
        const bool clearable = i < ctx.toolhead_clearable.size() && ctx.toolhead_clearable[i];
        return warn_result(
            lv_tr("Filament In The Toolhead"),
            clearable
                ? lv_tr("The toolhead has filament but no AMS slot reports it loaded. The "
                        "printer may refuse to load a slot until it is cleared. Start anyway?")
                : lv_tr("The toolhead has filament but no AMS slot reports it loaded. "
                        "Pull it out manually before printing. Start anyway?"),
            lv_tr("Start Anyway"));
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
    //
    // A backend presenting no filament system has no lanes to consult and an
    // always-active bypass that proves nothing, so it takes the sensor check.
    if (ctx.ams_available && ctx.any_bypass_active && print_lane_requirement(ctx) <= 1) {
        return pass_result();
    }
    if (ctx.ams_manages_filament && ctx.has_active_backend) {
        if (!ctx.empty_required_lanes.empty()) {
            spdlog::info("[PrintStartController] {} required tool(s) have an empty lane - "
                         "showing pre-print warning",
                         ctx.empty_required_lanes.size());
            return warn_result(
                lv_tr("No Filament Detected"),
                build_empty_lane_message(ctx.empty_required_lanes, ctx.available_slots),
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
    // modal_confirm copies the message string).
    std::string message = lv_tr("These tools have no matching filament loaded:");
    message += "\n\n";
    for (int tool_idx : unresolved) {
        // Look up by real tool_index — tool_info may be used-filtered
        // (compacted), so its vector position no longer equals the tool number.
        const auto* tool = ui::FilamentMappingCard::find_by_tool_index(ctx.tool_info, tool_idx);
        if (tool) {
            std::string color_name = describe_color(tool->color_rgb);
            message += "  " + std::string(LV_SYMBOL_BULLET) + " " +
                       helix::ui::tool_label(tool_idx) + ": " + color_name;
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
            message += fmt::format("  {} {}: {} {}: {} {}\n", LV_SYMBOL_BULLET,
                                   helix::ui::tool_label(m.tool_index), lv_tr("needs"),
                                   m.expected_material, lv_tr("you have"), m.loaded_material);
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
    // buffer: modal_confirm copies the message string).
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
            message += fmt::format("  {} {}: {} {}{}: {} {}{}\n", LV_SYMBOL_BULLET,
                                   helix::ui::tool_label(m.tool_index), lv_tr("needs"),
                                   m.expected_material, expected_temps, lv_tr("you have"),
                                   m.loaded_material, loaded_temps);
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

/// The database row for a slicer's material name: the name itself, else its
/// base polymer ("PLA Matte Basic" reads as PLA).
std::optional<filament::MaterialInfo> resolve_material_info(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    if (auto info = filament::find_material(name)) {
        return info;
    }
    return filament::find_material(filament::extract_base_material(name));
}

/// The material the file assigns to @p tool: the mapping card's tool info,
/// else the slicer palette. Empty when neither names one.
std::string file_material_for_tool(const PrintStartContext& ctx, int tool) {
    if (const auto* t = ui::FilamentMappingCard::find_by_tool_index(ctx.tool_info, tool)) {
        if (!t->material.empty()) {
            return t->material;
        }
    }
    if (tool >= 0 && tool < static_cast<int>(ctx.filament_materials.size())) {
        return ctx.filament_materials[tool];
    }
    return {};
}

CheckResult gate_mixed_temperature_materials(const PrintStartContext& ctx) {
    const auto pair = mixed_temperature_materials_in(ctx);
    if (!pair) {
        return pass_result();
    }

    // The risk depends on where the two meet: separate nozzles share only the
    // bed and chamber, one nozzle runs both through the same hot end.
    std::string message;
    if (ctx.multiple_toolheads) {
        message = fmt::format(lv_tr("This file prints {0} and {1} together. The chamber and bed "
                                    "heat {1} needs can soften {0} and jam its nozzle."),
                              pair->cooler, pair->hotter);
    } else {
        message = fmt::format(lv_tr("This file prints {0} and {1} through one nozzle. {0} can "
                                    "burn and clog the nozzle at {1} temperatures."),
                              pair->cooler, pair->hotter);
    }
    message += "\n\n";
    message += lv_tr("Start print anyway?");

    return warn_result(lv_tr("Mixed-Temperature Materials"), std::move(message),
                       lv_tr("Start Anyway"));
}

} // namespace

/// "a", "a and b", "a, b and c": the list read as one phrase. Both joins are
/// translated patterns, so a locale uses its own separator and final pair.
static std::string spoken_list(const std::vector<std::string>& names) {
    if (names.empty()) {
        return {};
    }
    std::string head = names.front();
    for (size_t i = 1; i + 1 < names.size(); ++i) {
        head = fmt::format(fmt::runtime(lv_tr("{}, {}")), head, names[i]);
    }
    if (names.size() == 1) {
        return head;
    }
    return fmt::format(fmt::runtime(lv_tr("{} and {}")), head, names.back());
}

CheckResult gate_door_open(const PrintStartContext& ctx) {
    const auto material = door_warning_material(ctx.door_check_enabled, !ctx.open_doors.empty(),
                                                materials_in_print(ctx));
    if (!material) {
        return pass_result();
    }
    const std::string names = spoken_list(ctx.open_doors);
    spdlog::info("[PrintStartController] Door open ({}) on a {} print - showing pre-print warning",
                 names, *material);
    const char* body =
        ctx.open_doors.size() == 1
            ? lv_tr("{0} is open. {1} prints best in a warm, closed enclosure; an open door "
                    "cools the chamber and can cause warping or layer splits. Close it, or "
                    "start anyway.")
            : lv_tr("{0} are open. {1} prints best in a warm, closed enclosure; an open door "
                    "cools the chamber and can cause warping or layer splits. Close them, or "
                    "start anyway.");
    return warn_result(lv_tr("Door Open"), fmt::format(fmt::runtime(body), names, *material),
                       lv_tr("Start Anyway"));
}

CheckResult gate_printer_stopping_command(const PrintStartContext& ctx) {
    const PrinterStopCheck& stop = ctx.printer_stop;
    if (stop.state != PrinterStopCheck::State::Stops) {
        return pass_result();
    }
    CheckResult r;
    r.verdict = CheckResult::Verdict::Block;
    r.severity = GateSeverity::Error;
    r.title = lv_tr("File Will Stop the Printer");
    r.body = fmt::format(fmt::runtime(lv_tr("Line {} calls {}, which this printer treats as an "
                                            "emergency stop. Re-slice the file with a profile "
                                            "made for this printer.")),
                         stop.line_number, stop.command);
    if (!stop.stop_message.empty()) {
        r.body += "\n\n";
        r.body += fmt::format(fmt::runtime(lv_tr("Printer message: {}")), stop.stop_message);
    }
    return r;
}

CheckResult gate_fit_check(const PrintStartContext& ctx) {
    if (!ctx.fit_machine) {
        return pass_result();
    }
    const std::vector<FitFinding> findings = check_fit(*ctx.fit_machine, ctx.fit_file);
    // A note (another printer model) shows in the file details only.
    const bool blocks = fit_blocks(findings);
    if (!blocks && !fit_warns(findings)) {
        return pass_result();
    }
    const std::string lines =
        fit_findings_text(findings, blocks ? FitSeverity::Block : FitSeverity::Warn);
    if (blocks) {
        CheckResult r;
        r.verdict = CheckResult::Verdict::Block;
        r.severity = GateSeverity::Error;
        r.title = lv_tr("File Does Not Fit This Printer");
        r.body = lines + "\n\n" + lv_tr("Re-slice the file with a profile made for this printer.");
        return r;
    }
    return warn_result(lv_tr("Sliced for a Different Setup"), lines, lv_tr("Start Anyway"));
}

PrinterStopCheck printer_stop_check_in(const std::string& content, size_t limit) {
    PrinterStopCheck check;
    auto& macros = MacroParamCache::instance();
    // Read first, so a populate landing mid-scan can only stamp the answer older
    // than the data it describes. That direction costs a rescan; the reverse
    // would let a stale answer pass for a current one.
    check.macro_generation = macros.generation();
    if (!macros.is_populated()) {
        check.not_run_reason = "the printer's macros have not been read";
        return check;
    }
    const std::map<std::string, std::string> commands = macros.printer_stop_commands();
    std::set<std::string> names;
    for (const auto& [name, message] : commands) {
        names.insert(name);
    }
    std::string_view scanned(content);
    if (content.size() >= limit) {
        const size_t last_newline = scanned.rfind('\n');
        scanned = last_newline == std::string_view::npos ? std::string_view{}
                                                         : scanned.substr(0, last_newline);
    }
    check.state = PrinterStopCheck::State::Clean;
    if (auto hit = gcode::GCodeOpsDetector::find_first_command(scanned, names)) {
        check.state = PrinterStopCheck::State::Stops;
        check.command = hit->command;
        check.line_number = hit->line_number;
        check.stop_message = commands.at(helix::text_io::to_upper(hit->command));
    }
    return check;
}

PrinterStopCheck printer_stop_not_run(std::string reason) {
    PrinterStopCheck check;
    check.not_run_reason = std::move(reason);
    check.macro_generation = MacroParamCache::instance().generation();
    return check;
}

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

    // The firmware picks every lane when the print starts, so a tool with no
    // lane yet is the expected answer, not a gap.
    if (ctx.lanes_picked_at_print) {
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
    // The external spool is the ONE spool this rule can weigh, and a lane-fed
    // print does not draw from it. Every sibling rule here already scopes itself
    // on bypass; this one never did, because it predates bypass existing (it is
    // from the single-extruder era, 39a576ddc).
    //
    // The consequence is not a merely-inaccurate warning, it is an unanswerable
    // one: the rule compares the file's WHOLE-FILE total against that spool, so
    // no lane assignment or tool remap can change either input. On a K2 Plus the
    // user mapped a print from T1 to T2 and kept getting "needs about 1900g but
    // the spool has about 386g" - 386g being the bypass spool, which the print
    // was never going to touch. Worse, assigning a spool to a LANE calls
    // set_active_spool(), and Moonraker's active spool is what populates
    // external_spool - so editing a lane silently re-aims this comparison.
    //
    // A real per-lane check cannot be written yet: SlotInfo carries
    // remaining_weight_g but AvailableSlot drops it, so nothing downstream can
    // compare a tool's grams against its mapped lane's grams. Staying silent is
    // the honest answer until that data is threaded through.
    if (ctx.ams_manages_filament && ctx.has_active_backend && !ctx.any_bypass_active) {
        return std::nullopt;
    }

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
            needed_g =
                filament::length_to_weight_g(static_cast<float>(ctx.metadata->filament_total),
                                             mat->density_g_cm3, ctx.filament_diameter_mm);
        }
    }
    if (needed_g > 0.0f && needed_g > spool->remaining_weight_g) {
        spdlog::info("[PrintStartController] Pre-print warning: needs {} g, spool has {} g",
                     needed_g, spool->remaining_weight_g);
        return std::make_pair(needed_g, spool->remaining_weight_g);
    }
    return std::nullopt;
}

std::vector<LaneWeightShortfall> insufficient_lane_weights_in(const PrintStartContext& ctx) {
    std::vector<LaneWeightShortfall> shortfalls;
    if (!ctx.metadata.has_value() || ctx.mappings.empty() || ctx.available_slots.empty()) {
        return shortfalls;
    }
    // Bypass feeds from the external spool, which insufficient_spool_weight_in()
    // already owns. Same split the material check draws.
    if (ctx.any_bypass_active && print_lane_requirement(ctx) <= 1) {
        return shortfalls;
    }

    const auto& grams = ctx.tool_grams;
    const bool have_per_tool = !grams.empty();
    const double file_total_g = ctx.metadata->filament_weight_total;

    for (const auto& m : ctx.mappings) {
        if (m.tool_index < 0 || m.mapped_slot < 0) {
            continue;
        }
        // Only tools the file actually prints with. tools_used is the slicer's
        // own answer; an unused tool mapped to an empty lane is not a shortfall.
        if (!ctx.tools_used.empty() && ctx.tools_used.count(m.tool_index) == 0) {
            continue;
        }

        double needed_g = 0.0;
        if (have_per_tool) {
            const size_t idx = static_cast<size_t>(m.tool_index);
            if (idx >= grams.size()) {
                continue; // no figure for this tool
            }
            needed_g = grams[idx];
        } else if (ctx.tools_used.size() == 1) {
            // No per-tool line, but only one tool prints - the whole-file total
            // is that tool's, exactly. Multi-tool without a breakdown stays
            // silent rather than inventing a split.
            needed_g = file_total_g;
        } else {
            continue;
        }
        if (!(needed_g > 0.0)) {
            continue;
        }

        // Resolve on the (slot, backend) PAIR: slot_index is unique only within
        // a backend, so matching on slot alone can price the wrong lane.
        const AvailableSlot* lane = nullptr;
        for (const auto& slot : ctx.available_slots) {
            if (slot.slot_index == m.mapped_slot && slot.backend_index == m.mapped_backend) {
                lane = &slot;
                break;
            }
        }
        if (lane == nullptr || !(lane->remaining_weight_g >= 0.0f)) {
            continue; // unknown lane, or a lane with no weight on record
        }

        if (static_cast<double>(lane->remaining_weight_g) < needed_g) {
            LaneWeightShortfall sf;
            sf.tool_index = m.tool_index;
            sf.mapped_slot = m.mapped_slot;
            sf.mapped_backend = m.mapped_backend;
            sf.needed_g = static_cast<float>(needed_g);
            sf.remaining_g = lane->remaining_weight_g;
            // Carry the lane's display identity forward so the gate dialog
            // labels it within its own unit instead of re-deriving from the
            // global mapped_slot.
            sf.noun = lane->noun;
            sf.unit_display_name = lane->unit_display_name;
            sf.local_slot_index = lane->local_slot_index;
            shortfalls.push_back(sf);
            spdlog::info("[PrintStartController] Lane short: tool {} -> slot {} needs {:.0f} g, "
                         "has {:.0f} g",
                         sf.tool_index, sf.mapped_slot, sf.needed_g, sf.remaining_g);
        }
    }
    return shortfalls;
}

std::vector<MaterialMismatchDetail> material_mismatches_in(const PrintStartContext& ctx) {
    std::vector<MaterialMismatchDetail> mismatches;

    if (!ctx.has_file_facts) {
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

bool materials_conflict_in_temperature(std::string_view a, std::string_view b) {
    const auto info_a = resolve_material_info(a);
    const auto info_b = resolve_material_info(b);
    if (!info_a || !info_b) {
        return false;
    }
    const bool a_hotter = info_a->nozzle_recommended() > info_b->nozzle_recommended();
    const auto& hotter = a_hotter ? *info_a : *info_b;
    const auto& cooler = a_hotter ? *info_b : *info_a;
    // A hotter material printed without a heated chamber leaves the cooler one
    // in room air, and two ranges that overlap have a nozzle temperature both
    // print at.
    return hotter.needs_enclosure() && cooler.nozzle_max < hotter.nozzle_min;
}

std::vector<std::string> materials_in_print(const PrintStartContext& ctx) {
    if (!ctx.has_file_facts) {
        return {};
    }

    std::set<int> used = ctx.tools_used;
    if (used.empty() && ctx.metadata.has_value()) {
        const auto& weights = ctx.metadata->filament_weights;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (weights[i] > 0.0) {
                used.insert(static_cast<int>(i));
            }
        }
    }
    // A single-material file needs no usage data to say what it prints. With
    // several materials and none, which ones print is unknown.
    if (used.empty() && ctx.filament_materials.size() == 1) {
        used.insert(0);
    }

    std::vector<std::string> materials;
    for (int tool : used) {
        auto material = file_material_for_tool(ctx, tool);
        if (!material.empty() &&
            std::find(materials.begin(), materials.end(), material) == materials.end()) {
            materials.push_back(std::move(material));
        }
    }
    return materials;
}

std::optional<std::string> enclosure_material_in(const std::vector<std::string>& materials) {
    std::optional<std::string> warmest;
    int warmest_chamber_c = 0;
    for (const auto& material : materials) {
        const auto info = resolve_material_info(material);
        if (info && info->needs_enclosure() && info->chamber_temp_c > warmest_chamber_c) {
            warmest = material;
            warmest_chamber_c = info->chamber_temp_c;
        }
    }
    return warmest;
}

std::optional<std::string> door_warning_material(bool door_check_enabled, bool any_door_open,
                                                 const std::vector<std::string>& materials) {
    if (!door_check_enabled || !any_door_open) {
        return std::nullopt;
    }
    return enclosure_material_in(materials);
}

std::optional<MixedTemperatureMaterials>
mixed_temperature_materials_in(const PrintStartContext& ctx) {
    const std::vector<std::string> materials = materials_in_print(ctx);
    for (size_t i = 0; i < materials.size(); ++i) {
        for (size_t j = i + 1; j < materials.size(); ++j) {
            if (!materials_conflict_in_temperature(materials[i], materials[j])) {
                continue;
            }
            const auto info_i = resolve_material_info(materials[i]);
            const auto info_j = resolve_material_info(materials[j]);
            const bool i_cooler = info_i->nozzle_recommended() < info_j->nozzle_recommended();
            MixedTemperatureMaterials pair{i_cooler ? materials[i] : materials[j],
                                           i_cooler ? materials[j] : materials[i]};
            spdlog::info("[PrintStartController] Mixed-temperature materials: {} with {}",
                         pair.cooler, pair.hotter);
            return pair;
        }
    }
    return std::nullopt;
}

const std::vector<PrintStartGate>& default_print_start_gates() {
    // Order is the order the questions are asked in: the file itself first, then
    // filament supply, then what is at the toolhead, then the mapping.
    static const std::vector<PrintStartGate> gates = {
        // A file that shuts the printer down outranks every filament question.
        {"printer_stopping_command", gate_printer_stopping_command},
        // Then a file the printer cannot run as sliced.
        {"fit_check", gate_fit_check},
        // Then a file whose materials cannot share one print.
        {"mixed_temperature_materials", gate_mixed_temperature_materials},
        {"insufficient_spool_weight", gate_insufficient_spool_weight},
        // The lane-fed counterpart. Mutually exclusive with the spool check by
        // construction: that one returns early on a lane-fed AMS print, this
        // one returns early on bypass.
        {"insufficient_lane_weight", gate_insufficient_lane_weight},
        {"bypass_engaged_lane_print", gate_bypass_engaged_lane_print},
        {"unaccounted_toolhead_filament", gate_unaccounted_toolhead_filament},
        {"required_filament_present", gate_required_filament_present},
        {"unresolved_tools", gate_unresolved_tools},
        {"material_compatibility", gate_material_compatibility},
        // Last: a door is closed in a second, so it is asked after anything
        // that needs the user to think about the file or the filament.
        {"door_open", gate_door_open},
    };
    return gates;
}

void warn_printer_stop_check_skipped(std::string_view start_path, std::string_view filename,
                                     std::string_view reason) {
    spdlog::warn("[PrintStart] Starting {} from {} without the printer-stopping command check: {}",
                 filename, start_path, reason);
}

} // namespace helix
