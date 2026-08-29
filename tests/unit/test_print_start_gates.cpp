// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_start_gates.cpp
 * @brief Pure-rule tests for the print-start gate core (print_start_checks.h).
 *
 * Run with: ./build/bin/helix-tests "[print-start][gate-pipeline]"
 */

#include "ams_types.h"
#include "filament_mapper.h"
#include "moonraker_types.h"
#include "print_start_checks.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
/// Minimal context with the field(s) a rule reads; everything else default.
PrintStartContext ctx_with(std::function<void(PrintStartContext&)> seed) {
    PrintStartContext ctx;
    seed(ctx);
    return ctx;
}

/// Look a gate up by name. Tests must not index the pipeline positionally:
/// inserting a gate renumbers every later one and silently retargets the test.
const PrintStartGate& gate_named(const char* name) {
    const auto& gates = default_print_start_gates();
    const auto it = std::find_if(gates.begin(), gates.end(), [&](const PrintStartGate& g) {
        return g.name == std::string(name);
    });
    REQUIRE(it != gates.end());
    return *it;
}
} // namespace

// ---------------------------------------------------------------------------
// unresolved_tools_in — ported from PrintStartController::unresolved_tools_for
// ---------------------------------------------------------------------------

TEST_CASE("unresolved_tools_in: single-color never warns", "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.filament_color_count = 1;
        ToolMapping m; // would be unresolved if evaluated
        m.tool_index = 0;
        m.is_auto = true;
        c.mappings = {m};
    });
    CHECK(unresolved_tools_in(ctx).empty());
}

TEST_CASE("unresolved_tools_in: bypass suppresses", "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.filament_color_count = 3;
        c.any_bypass_active = true;
        ToolMapping m;
        m.tool_index = 0;
        m.is_auto = true;
        c.mappings = {m};
    });
    CHECK(unresolved_tools_in(ctx).empty());
}

TEST_CASE("unresolved_tools_in: empty mappings stay silent", "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) { c.filament_color_count = 3; });
    CHECK(unresolved_tools_in(ctx).empty());
}

TEST_CASE("unresolved_tools_in: multi-color unresolved tool is reported",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.filament_color_count = 2;
        ToolMapping unresolved;
        unresolved.tool_index = 1;
        unresolved.is_auto = true;
        ToolMapping resolved;
        resolved.tool_index = 0;
        resolved.mapped_slot = 2;
        c.mappings = {resolved, unresolved};
    });
    auto out = unresolved_tools_in(ctx);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 1);
}

// ---------------------------------------------------------------------------
// insufficient_spool_weight_in — ported from initiate() inline math
// ---------------------------------------------------------------------------

TEST_CASE("insufficient_spool_weight_in: no spool / no weight / no metadata",
          "[print-start][gate-pipeline]") {
    CHECK_FALSE(insufficient_spool_weight_in(ctx_with([](PrintStartContext&) {})).has_value());
    CHECK_FALSE(insufficient_spool_weight_in(ctx_with([](PrintStartContext& c) {
                    SlotInfo spool;
                    spool.remaining_weight_g = 5.0f;
                    c.external_spool = spool; // no metadata
                })).has_value());
    CHECK_FALSE(insufficient_spool_weight_in(ctx_with([](PrintStartContext& c) {
                    SlotInfo spool;
                    spool.remaining_weight_g = 0.0f;
                    c.external_spool = spool;
                    FileMetadata md;
                    md.filament_weight_total = 100.0;
                    c.metadata = md;
                })).has_value());
}

TEST_CASE("insufficient_spool_weight_in: weight from metadata, enough vs short",
          "[print-start][gate-pipeline]") {
    auto enough = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 50.0f;
        spool.material = "PLA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 40.0;
        c.metadata = md;
    });
    CHECK_FALSE(insufficient_spool_weight_in(enough).has_value());

    auto short_ = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 30.0f;
        spool.material = "PLA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 40.0;
        c.metadata = md;
    });
    auto r = insufficient_spool_weight_in(short_);
    REQUIRE(r.has_value());
    CHECK(r->first == 40.0f);
    CHECK(r->second == 30.0f);
}

TEST_CASE("insufficient_spool_weight_in: length fallback via material density",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 5.0f;
        spool.material = "PLA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 0.0;
        md.filament_total = 100000.0; // 100000mm (100m)
        c.metadata = md;
    });
    auto r = insufficient_spool_weight_in(ctx);
    REQUIRE(r.has_value());
    // 100m of 1.75mm PLA at 1.24 g/cm3 ≈ 298g — must exceed 5g by a wide margin.
    CHECK(r->first > 250.0f);
    CHECK(r->second == 5.0f);
}

TEST_CASE("insufficient_spool_weight_in: silent on a lane-fed AMS print",
          "[print-start][gate-pipeline]") {
    // K2 Plus, 2026-08-24: the user mapped a large print from T1 to T2 and kept
    // getting "needs about N g but the spool has about 386 g". 386 g was the
    // BYPASS spool - the print was lane-fed and would never touch it. The rule
    // weighs the file's whole-file total against the single external spool, so
    // neither input moves when a tool is remapped: the warning was unanswerable.
    auto lane_fed = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 386.0f;
        spool.material = "ASA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 1900.0;
        c.metadata = md;
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        c.any_bypass_active = false; // feeding from a lane, not the bypass
    });
    CHECK_FALSE(insufficient_spool_weight_in(lane_fed).has_value());
}

TEST_CASE("insufficient_spool_weight_in: still warns when the bypass IS the feed",
          "[print-start][gate-pipeline]") {
    // The narrowing must not silence the case the rule is actually right about:
    // a bypass-fed print does draw from the external spool it weighs.
    auto bypass_fed = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 386.0f;
        spool.material = "ASA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 1900.0;
        c.metadata = md;
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        c.any_bypass_active = true;
    });
    auto r = insufficient_spool_weight_in(bypass_fed);
    REQUIRE(r.has_value());
    CHECK(r->second == 386.0f);
}

TEST_CASE("insufficient_spool_weight_in: still warns with no AMS at all",
          "[print-start][gate-pipeline]") {
    // A single-extruder printer has no lanes to feed from - the original,
    // correct scope of this rule, which must survive the narrowing.
    auto no_ams = ctx_with([](PrintStartContext& c) {
        SlotInfo spool;
        spool.remaining_weight_g = 30.0f;
        spool.material = "PLA";
        c.external_spool = spool;
        FileMetadata md;
        md.filament_weight_total = 40.0;
        c.metadata = md;
        c.ams_manages_filament = false;
        c.has_active_backend = false;
    });
    CHECK(insufficient_spool_weight_in(no_ams).has_value());
}

// ---------------------------------------------------------------------------
// material_mismatches_in — ported from find_material_mismatches()
// ---------------------------------------------------------------------------

TEST_CASE("material_mismatches_in: no detail view -> none", "[print-start][gate-pipeline]") {
    CHECK(material_mismatches_in(ctx_with([](PrintStartContext&) {})).empty());
}

TEST_CASE("material_mismatches_in: AMS path flags mismatched mapped tool",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.ams_available = true;
        ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = 0;
        m.material_mismatch = true;
        c.mappings = {m};
        GcodeToolInfo t;
        t.tool_index = 0;
        t.material = "PETG";
        c.tool_info = {t};
        AvailableSlot s;
        s.slot_index = 0;
        s.backend_index = -1;
        s.material = "PLA";
        c.available_slots = {s};
    });
    auto out = material_mismatches_in(ctx);
    REQUIRE(out.size() == 1);
    CHECK(out[0].expected_material == "PETG");
    CHECK(out[0].loaded_material == "PLA");
}

TEST_CASE("material_mismatches_in: zero-usage tool is skipped when weights known",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.ams_available = true;
        ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = 0;
        m.material_mismatch = true;
        c.mappings = {m};
        GcodeToolInfo t;
        t.tool_index = 0;
        t.material = "PETG";
        c.tool_info = {t};
        AvailableSlot s;
        s.slot_index = 0;
        s.backend_index = -1;
        s.material = "PLA";
        c.available_slots = {s};
        FileMetadata md;
        md.filament_weights = {0.0};
        c.metadata = md;
    });
    CHECK(material_mismatches_in(ctx).empty());
}

TEST_CASE("material_mismatches_in: unknown material on either side is skipped",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.ams_available = true;
        ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = 0;
        m.material_mismatch = true;
        c.mappings = {m}; // tool_info empty -> expected unknown
        AvailableSlot s;
        s.slot_index = 0;
        s.backend_index = -1;
        s.material = "PLA";
        c.available_slots = {s};
    });
    CHECK(material_mismatches_in(ctx).empty());
}

TEST_CASE("material_mismatches_in: non-AMS external spool mismatch",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.ams_available = false;
        c.filament_materials = {"ABS"};
        SlotInfo spool;
        spool.material = "PLA";
        c.external_spool = spool;
    });
    auto out = material_mismatches_in(ctx);
    REQUIRE(out.size() == 1);
    CHECK(out[0].expected_material == "ABS");
    CHECK(out[0].loaded_material == "PLA");
}

TEST_CASE("material_mismatches_in: bypass print compares external spool, not lanes",
          "[print-start][gate-pipeline]") {
    // K2 CFS regression: file sliced ASA-GF on lane 2, bypass engaged,
    // external spool set to Spoolman PLA. The lane mapping is irrelevant —
    // the dialog must name the external spool's material.
    auto make = [](const char* spool_material) {
        return ctx_with([&](PrintStartContext& c) {
            c.has_detail_view = true;
            c.ams_available = true; // AMS present — bypass is what redirects
            c.any_bypass_active = true;
            c.tools_used = {1};
            c.filament_color_count = 4; // full profile palette, one tool used
            GcodeToolInfo t;
            t.tool_index = 1;
            t.material = "ASA-GF";
            c.tool_info = {t};
            ToolMapping m; // lane mapping would be a MATCH — must be ignored
            m.tool_index = 1;
            m.mapped_slot = 1;
            m.material_mismatch = false;
            c.mappings = {m};
            SlotInfo spool;
            spool.material = spool_material;
            c.external_spool = spool;
        });
    };
    auto out = material_mismatches_in(make("PLA"));
    REQUIRE(out.size() == 1);
    CHECK(out[0].tool_index == 1);
    CHECK(out[0].expected_material == "ASA-GF");
    CHECK(out[0].loaded_material == "PLA");

    // Exact same material (case-insensitive) stays silent.
    CHECK(material_mismatches_in(make("asa-gf")).empty());

    // Same comparator as every other path: a same-family grade change
    // (ASA vs ASA-GF) is not a MATERIAL mismatch. It is still reported, as a
    // grade change, by the second pass — see the grade cases below.
    auto grade = material_mismatches_in(make("ASA"));
    REQUIRE(grade.size() == 1);
    CHECK(grade[0].grade_only);
    CHECK(material_mismatches_in(make("ASA-CF"))[0].grade_only);

    // Unknown spool material — nothing to compare, stay silent.
    CHECK(material_mismatches_in(make("")).empty());
}

// ---------------------------------------------------------------------------
// Grade mismatches — same polymer, different filler
// ---------------------------------------------------------------------------

TEST_CASE("material_mismatches_in: non-AMS grade change is flagged grade_only",
          "[print-start][gate-pipeline][grade]") {
    // materials_match() passes (both reduce to ASA, same compat group), so the
    // hard mismatch stays silent. The grade pass is what speaks up.
    auto make = [](const char* file_material, const char* spool_material) {
        return ctx_with([&](PrintStartContext& c) {
            c.has_detail_view = true;
            c.ams_available = false;
            c.filament_materials = {file_material};
            SlotInfo spool;
            spool.material = spool_material;
            c.external_spool = spool;
        });
    };

    auto out = material_mismatches_in(make("ASA-GF", "ASA"));
    REQUIRE(out.size() == 1);
    CHECK(out[0].grade_only);
    CHECK(out[0].expected_material == "ASA-GF");
    CHECK(out[0].loaded_material == "ASA");

    // Marketing grades are not a grade change.
    CHECK(material_mismatches_in(make("PLA+", "PLA")).empty());
    CHECK(material_mismatches_in(make("Silk PLA", "PLA")).empty());

    // A real material mismatch is NOT grade_only — the harder finding wins.
    auto hard = material_mismatches_in(make("ABS", "PLA"));
    REQUIRE(hard.size() == 1);
    CHECK_FALSE(hard[0].grade_only);
}

TEST_CASE("material_mismatches_in: bypass grade change is flagged grade_only",
          "[print-start][gate-pipeline][grade]") {
    // The bypass branch runs the same two comparisons as the non-AMS branch,
    // against the external spool rather than the mapped lane.
    auto make = [](const char* spool_material) {
        return ctx_with([&](PrintStartContext& c) {
            c.has_detail_view = true;
            c.ams_available = true;
            c.any_bypass_active = true;
            c.tools_used = {1};
            c.filament_color_count = 4;
            GcodeToolInfo t;
            t.tool_index = 1;
            t.material = "ASA-GF";
            c.tool_info = {t};
            ToolMapping m;
            m.tool_index = 1;
            m.mapped_slot = 1;
            m.material_mismatch = false;
            c.mappings = {m};
            SlotInfo spool;
            spool.material = spool_material;
            c.external_spool = spool;
        });
    };

    auto out = material_mismatches_in(make("ASA"));
    REQUIRE(out.size() == 1);
    CHECK(out[0].grade_only);
    CHECK(out[0].tool_index == 1);

    // Same grade, and a marketing suffix on the same grade, stay silent.
    CHECK(material_mismatches_in(make("asa-gf")).empty());
    CHECK(material_mismatches_in(make("ASA-GF+")).empty());
}

TEST_CASE("material_mismatches_in: AMS grade change is flagged on a matched lane",
          "[print-start][gate-pipeline][grade]") {
    // material_mismatch is false — FilamentMapper considers the lane a match —
    // yet the lane holds the unfilled grade of a filled file.
    auto make = [](const char* slot_material, double weight) {
        return ctx_with([&](PrintStartContext& c) {
            c.has_detail_view = true;
            c.ams_available = true;
            GcodeToolInfo t;
            t.tool_index = 0;
            t.material = "PLA-CF";
            c.tool_info = {t};
            ToolMapping m;
            m.tool_index = 0;
            m.mapped_slot = 0;
            m.material_mismatch = false;
            c.mappings = {m};
            AvailableSlot s;
            s.slot_index = 0;
            s.backend_index = -1;
            s.material = slot_material;
            c.available_slots = {s};
            FileMetadata md;
            md.filament_weights = {weight};
            c.metadata = md;
        });
    };

    auto out = material_mismatches_in(make("PLA", 12.0));
    REQUIRE(out.size() == 1);
    CHECK(out[0].grade_only);
    CHECK(out[0].expected_material == "PLA-CF");
    CHECK(out[0].loaded_material == "PLA");

    // Same filler -> silent.
    CHECK(material_mismatches_in(make("PLA-CF", 12.0)).empty());
    // A tool the file never extrudes from is skipped, exactly as the hard
    // mismatch pass skips it.
    CHECK(material_mismatches_in(make("PLA", 0.0)).empty());
}

TEST_CASE("gate material_compatibility: grade-only dialog names the abrasive direction",
          "[print-start][gate-pipeline][grade]") {
    const auto& gates = default_print_start_gates();
    const auto& gate = gates.back();
    REQUIRE(gate.name == "material_compatibility");

    auto ctx_for = [](const char* file_material, const char* spool_material) {
        return ctx_with([&](PrintStartContext& c) {
            c.has_detail_view = true;
            c.ams_available = false;
            c.filament_materials = {file_material};
            SlotInfo spool;
            spool.material = spool_material;
            c.external_spool = spool;
        });
    };

    // Loaded spool is the filled one: the direction that costs a nozzle.
    auto abrasive = gate.evaluate(ctx_for("ASA", "ASA-GF"));
    REQUIRE(abrasive.verdict == CheckResult::Verdict::Warn);
    CHECK(abrasive.title != "Material Mismatch"); // lv_tr identity in the test locale
    CHECK(abrasive.body.find("hardened") != std::string::npos);

    // File is the filled one: slower and hotter than needed, no hardware risk.
    auto benign = gate.evaluate(ctx_for("ASA-GF", "ASA"));
    REQUIRE(benign.verdict == CheckResult::Verdict::Warn);
    CHECK(benign.body.find("hardened") == std::string::npos);
    CHECK(benign.body.find("ASA-GF") != std::string::npos);

    // A hard mismatch keeps the original dialog, even alongside a grade row.
    auto hard = gate.evaluate(ctx_for("PETG", "PLA"));
    REQUIRE(hard.verdict == CheckResult::Verdict::Warn);
    CHECK(hard.title == "Material Mismatch");
}

TEST_CASE("material_mismatches_in: bypass falls back to palette materials",
          "[print-start][gate-pipeline]") {
    // No mapping-card tool_info (e.g. scan produced tools_used but the card
    // was never built) — the file's palette still names the used tool's
    // material.
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.any_bypass_active = true;
        c.tools_used = {2};
        c.filament_materials = {"PLA", "PLA", "ASA-GF", "PLA"};
        SlotInfo spool;
        spool.material = "PETG";
        c.external_spool = spool;
    });
    auto out = material_mismatches_in(ctx);
    REQUIRE(out.size() == 1);
    CHECK(out[0].tool_index == 2);
    CHECK(out[0].expected_material == "ASA-GF");
    CHECK(out[0].loaded_material == "PETG");
}

// ---------------------------------------------------------------------------
// default_print_start_gates: order + names
// ---------------------------------------------------------------------------

TEST_CASE("default gate list: names in behavior-preserving order", "[print-start][gate-pipeline]") {
    auto& gates = default_print_start_gates();
    REQUIRE(gates.size() == 7);
    CHECK(gates[0].name == "insufficient_spool_weight");
    // The lane-fed counterpart sits beside its sibling; the two are mutually
    // exclusive by construction, so their relative order is not load-bearing.
    CHECK(gates[1].name == "insufficient_lane_weight");
    CHECK(gates[2].name == "bypass_engaged_lane_print");
    CHECK(gates[3].name == "unaccounted_toolhead_filament");
    CHECK(gates[4].name == "required_filament_present");
    CHECK(gates[5].name == "unresolved_tools");
    CHECK(gates[6].name == "material_compatibility");
}

TEST_CASE("default gate list: the two new gates keep their relative order",
          "[print-start][gate-pipeline]") {
    auto& gates = default_print_start_gates();
    REQUIRE(gates.size() == 7);
    CHECK(gates[2].name == "bypass_engaged_lane_print");
    CHECK(gates[3].name == "unaccounted_toolhead_filament");
    CHECK(gates[4].name == "required_filament_present"); // shifted, order otherwise preserved
}

// ---------------------------------------------------------------------------
// bypass_engaged_lane_print + unaccounted_toolhead_filament (new gates)
// ---------------------------------------------------------------------------

TEST_CASE("gate bypass_engaged_lane_print: fires only on bypass + multi-color",
          "[print-start][gate-pipeline]") {
    auto make = [](bool bypass, size_t colors) {
        return ctx_with([&](PrintStartContext& c) {
            c.any_bypass_active = bypass;
            c.filament_color_count = colors;
        });
    };
    auto& g = gate_named("bypass_engaged_lane_print");
    CHECK(g.evaluate(make(false, 4)).verdict == CheckResult::Verdict::Pass); // no bypass
    CHECK(g.evaluate(make(true, 1)).verdict == CheckResult::Verdict::Pass);  // legit bypass use
    auto r = g.evaluate(make(true, 4));
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.title == "Bypass Is Active");
    CHECK(r.proceed_label == "Start Anyway");
    CHECK(r.body.find("bypass") != std::string::npos);
}

TEST_CASE("gate bypass_engaged_lane_print: used-tool count beats palette size",
          "[print-start][gate-pipeline]") {
    // K2 CFS regression: a single-tool file sliced on a 4-lane profile carries
    // a full palette (PLA;ASA-GF;ASA-GF;PLA) but extrudes from one tool. The
    // print needs no lanes, so bypass is the legitimate source — no warning.
    auto& g = gate_named("bypass_engaged_lane_print");
    auto single_used_tool = ctx_with([](PrintStartContext& c) {
        c.any_bypass_active = true;
        c.filament_color_count = 4; // full profile palette
        c.tools_used = {1};         // gcode scan: only T1 extrudes
    });
    CHECK(g.evaluate(single_used_tool).verdict == CheckResult::Verdict::Pass);

    auto two_used_tools = ctx_with([](PrintStartContext& c) {
        c.any_bypass_active = true;
        c.filament_color_count = 4;
        c.tools_used = {0, 2};
    });
    CHECK(g.evaluate(two_used_tools).verdict == CheckResult::Verdict::Warn);

    // No scan result (empty tools_used) falls back to the palette count.
    auto no_scan = ctx_with([](PrintStartContext& c) {
        c.any_bypass_active = true;
        c.filament_color_count = 4;
    });
    CHECK(g.evaluate(no_scan).verdict == CheckResult::Verdict::Warn);
}

TEST_CASE("gate unaccounted_toolhead_filament: verdict matrix", "[print-start][gate-pipeline]") {
    auto make = [](bool bypass, std::optional<bool> backend_answer) {
        return ctx_with([&](PrintStartContext& c) {
            c.any_bypass_active = bypass;
            if (backend_answer.has_value()) {
                c.toolhead_unaccounted = {*backend_answer};
            }
        });
    };
    auto& g = gate_named("unaccounted_toolhead_filament");
    CHECK(g.evaluate(make(false, std::nullopt)).verdict ==
          CheckResult::Verdict::Pass); // cannot determine
    CHECK(g.evaluate(make(false, std::optional<bool>(false))).verdict ==
          CheckResult::Verdict::Pass);
    CHECK(g.evaluate(make(true, std::optional<bool>(true))).verdict ==
          CheckResult::Verdict::Pass); // bypass accounts
    auto r = g.evaluate(make(false, std::optional<bool>(true)));
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.title == "Filament In The Toolhead");
    CHECK(r.proceed_label == "Start Anyway");
}

// ---------------------------------------------------------------------------
// required_filament_present gate (ported from the old controller chain)
// ---------------------------------------------------------------------------

TEST_CASE("gate required_filament_present: auto-unload backends suppress entirely",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        c.any_auto_unload_backend = true;  // e.g. AD5X IFS post-print retract
        c.empty_required_lanes = {{0, 1}}; // would otherwise warn
    });
    auto& gates = default_print_start_gates();
    auto r = gates[3].evaluate(ctx);
    CHECK(r.verdict == CheckResult::Verdict::Pass);
}

TEST_CASE("gate required_filament_present: empty required lane warns with Start Print",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        // (tool_index, 0-based slot_index); lanes display slot + 1, so tool 0
        // -> "Lane 1".
        c.empty_required_lanes = {{0, 0}, {2, 3}};
    });
    auto r = gate_named("required_filament_present").evaluate(ctx);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.title == "No Filament Detected"); // lv_tr identity in the test locale
    CHECK(r.proceed_label == "Start Print");
    CHECK(r.body.find("Tool 0") != std::string::npos);
    CHECK(r.body.find("Lane 1") != std::string::npos);
}

TEST_CASE("gate required_filament_present: AMS lanes all fed -> pass",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.ams_manages_filament = true;
        c.has_active_backend = true;
    });
    CHECK(gate_named("required_filament_present").evaluate(ctx).verdict ==
          CheckResult::Verdict::Pass);
}

TEST_CASE("gate required_filament_present: single-tool bypass ignores empty lanes",
          "[print-start][gate-pipeline]") {
    // Bypass feeds the toolhead from the external spool; the mapped lane's
    // emptiness is irrelevant for a single-tool print and must not warn.
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        c.any_bypass_active = true;
        c.tools_used = {1};
        c.empty_required_lanes = {{1, 2}}; // would warn without bypass
    });
    CHECK(gate_named("required_filament_present").evaluate(ctx).verdict ==
          CheckResult::Verdict::Pass);

    // Multi-tool bypass still needs lanes — lane truth stays active there.
    auto multi = ctx_with([](PrintStartContext& c) {
        c.ams_manages_filament = true;
        c.has_active_backend = true;
        c.any_bypass_active = true;
        c.tools_used = {0, 2};
        c.empty_required_lanes = {{2, 3}};
    });
    CHECK(gate_named("required_filament_present").evaluate(multi).verdict ==
          CheckResult::Verdict::Warn);
}

TEST_CASE("gate required_filament_present: non-AMS runout says empty -> warn",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.runout_enabled = true;
        c.runout_available = true;
        c.runout_detected = false;
    });
    auto r = gate_named("required_filament_present").evaluate(ctx);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.proceed_label == "Start Print");
}

TEST_CASE("gate required_filament_present: runout disabled/unavailable -> pass",
          "[print-start][gate-pipeline]") {
    CHECK(gate_named("required_filament_present")
              .evaluate(ctx_with([](PrintStartContext&) {}))
              .verdict == CheckResult::Verdict::Pass);
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.runout_enabled = true;
        c.runout_available = false;
        c.runout_detected = false;
    });
    CHECK(gate_named("required_filament_present").evaluate(ctx).verdict ==
          CheckResult::Verdict::Pass);
}

// ---------------------------------------------------------------------------
// unresolved_tools + material gates: verdict shape only (rules covered above)
// ---------------------------------------------------------------------------

TEST_CASE("gate unresolved_tools: warns with Start Anyway and verbatim title",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.filament_color_count = 2;
        ToolMapping unresolved;
        unresolved.tool_index = 1;
        unresolved.is_auto = true;
        c.mappings = {unresolved};
        GcodeToolInfo t;
        t.tool_index = 1;
        t.color_rgb = 0xFF0000;
        t.material = "PLA";
        c.tool_info = {t};
    });
    auto r = gate_named("unresolved_tools").evaluate(ctx);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.title == "Color Mismatch");
    CHECK(r.proceed_label == "Start Anyway");
}

TEST_CASE("gate material_compatibility: warns with verbatim title",
          "[print-start][gate-pipeline]") {
    auto ctx = ctx_with([](PrintStartContext& c) {
        c.has_detail_view = true;
        c.ams_available = false;
        c.filament_materials = {"ABS"};
        SlotInfo spool;
        spool.material = "PLA";
        c.external_spool = spool;
    });
    auto r = gate_named("material_compatibility").evaluate(ctx);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.title == "Material Mismatch");
    CHECK(r.proceed_label == "Start Anyway");
}

// ---------------------------------------------------------------------------
// Runner mechanics (toy gates — no printer state needed)
// ---------------------------------------------------------------------------

#include "ui_modal.h"
#include "ui_print_start_controller.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

namespace {

using namespace helix::ui;

CheckResult warn_result(const char* title) {
    CheckResult r;
    r.verdict = CheckResult::Verdict::Warn;
    r.title = title;
    r.body = "body";
    r.severity = GateSeverity::Warning;
    r.proceed_label = "Start Anyway";
    return r;
}
CheckResult pass_result() {
    return CheckResult{};
}

/// Fixture with a controller wired to counting callbacks. LVGLUITestFixture
/// (not bare LVGLTestFixture): modal_show_confirmation builds XML components.
class GateRunnerFixture : public LVGLUITestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    PrintStartController controller{state, nullptr};
    int button_updates = 0;
    int cancelled = 0;
    bool gate_b_ran = false;

    GateRunnerFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        controller.set_api(api.get());
        controller.set_update_print_button([this]() { ++button_updates; });
        controller.set_on_print_cancelled([this]() { ++cancelled; });
    }
    ~GateRunnerFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void use_toy_gates(bool warn_first) {
        std::vector<PrintStartGate> gates;
        gates.push_back(
            {"toy_a", warn_first ? +[](const PrintStartContext&) { return warn_result("Toy A"); }
                                 : +[](const PrintStartContext&) { return pass_result(); }});
        gates.push_back({"toy_b", +[](const PrintStartContext&) { return pass_result(); }});
        PrintStartControllerTestAccess::set_gates(controller, std::move(gates));
    }
};

} // namespace

TEST_CASE("gate runner: warn stops at gate 0 and shows the modal", "[print-start][gate-pipeline]") {
    GateRunnerFixture fx;
    fx.use_toy_gates(/*warn_first=*/true);

    PrintStartControllerTestAccess::run_gates(fx.controller);
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) != nullptr);
    CHECK(PrintStartControllerTestAccess::gate_resume_index(fx.controller) == 0);
    CHECK(fx.button_updates == 0); // still parked on the dialog
}

TEST_CASE("gate runner: proceed resumes at NEXT gate", "[print-start][gate-pipeline]") {
    GateRunnerFixture fx;
    fx.use_toy_gates(/*warn_first=*/true);

    PrintStartControllerTestAccess::run_gates(fx.controller);
    PrintStartControllerTestAccess::gate_proceed(fx.controller);
    // toy_b passes -> pipeline ran off the end into execute_print_start(),
    // which fails on the missing prep manager: re-enables the button exactly
    // once and shows an error. That once-count IS the completion signal.
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) == nullptr);
    CHECK(fx.button_updates == 1);
    CHECK(fx.cancelled == 0);
}

TEST_CASE("gate runner: cancel re-enables button and fires on_print_cancelled",
          "[print-start][gate-pipeline]") {
    GateRunnerFixture fx;
    fx.use_toy_gates(/*warn_first=*/true);

    PrintStartControllerTestAccess::run_gates(fx.controller);
    PrintStartControllerTestAccess::gate_cancel(fx.controller);
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) == nullptr);
    CHECK(fx.button_updates == 1);
    CHECK(fx.cancelled == 1);
}

TEST_CASE("gate runner: all-pass reaches execute (button re-enabled once, no modal)",
          "[print-start][gate-pipeline]") {
    GateRunnerFixture fx;
    fx.use_toy_gates(/*warn_first=*/false);

    PrintStartControllerTestAccess::run_gates(fx.controller);
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) == nullptr);
    CHECK(fx.button_updates == 1);
}

// ---------------------------------------------------------------------------
// insufficient_lane_weights_in - the lane-fed counterpart
//
// Modelled on the real case that motivated it (K2 Plus, 2026-08-24): an
// OrcaSlicer 2.4.2 file whose footer reads
//   ; filament used [g] = 0.00, 863.07, 0.00, 0.00, 0.00
// i.e. tool 1 needs 863 g. Lane 2 (slot index 1) held a Spoolman spool with
// 65 g left; lane 3 (slot index 2) held 1000 g. The user remapped from the
// first to the second and the old whole-file-vs-external-spool check could not
// tell the difference, because neither of its inputs mentions a tool.
// ---------------------------------------------------------------------------

namespace {

helix::AvailableSlot lane(int slot_index, int backend_index, float remaining_g) {
    helix::AvailableSlot s{};
    s.slot_index = slot_index;
    s.backend_index = backend_index;
    s.is_empty = false;
    s.remaining_weight_g = remaining_g;
    return s;
}

helix::ToolMapping map_tool(int tool, int slot, int backend = 0) {
    helix::ToolMapping m{};
    m.tool_index = tool;
    m.mapped_slot = slot;
    m.mapped_backend = backend;
    return m;
}

/// The motivating file: tool 1 uses 863.07 g, every other tool 0.
PrintStartContext orca_ctx(int mapped_slot, float lane_remaining_g) {
    return ctx_with([mapped_slot, lane_remaining_g](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {0.0, 863.07, 0.0, 0.0, 0.0};
        c.tools_used = {1};
        c.mappings = {map_tool(1, mapped_slot)};
        c.available_slots = {lane(mapped_slot, 0, lane_remaining_g)};
    });
}

} // namespace

TEST_CASE("insufficient_lane_weights_in: warns when the mapped lane is short",
          "[print-start][gate-pipeline]") {
    auto shortfalls = insufficient_lane_weights_in(orca_ctx(/*slot=*/1, /*remaining=*/65.0f));
    REQUIRE(shortfalls.size() == 1);
    CHECK(shortfalls[0].tool_index == 1);
    CHECK(shortfalls[0].mapped_slot == 1);
    CHECK(shortfalls[0].remaining_g == 65.0f);
    CHECK(shortfalls[0].needed_g > 860.0f);
}

TEST_CASE("insufficient_lane_weights_in: silent once remapped to a full lane",
          "[print-start][gate-pipeline]") {
    // The whole point: the SAME file, remapped, must stop warning.
    CHECK(insufficient_lane_weights_in(orca_ctx(/*slot=*/2, /*remaining=*/1000.0f)).empty());
}

TEST_CASE("insufficient_lane_weights_in: an unknown lane weight is not zero",
          "[print-start][gate-pipeline]") {
    // -1 is the "no figure on record" sentinel. Reading it as 0 g would make
    // every unlinked bay a false alarm - the same collision class as the black
    // bypass swatch that read as unset.
    CHECK(insufficient_lane_weights_in(orca_ctx(/*slot=*/3, /*remaining=*/-1.0f)).empty());
}

TEST_CASE("insufficient_lane_weights_in: resolves on the (slot, backend) pair",
          "[print-start][gate-pipeline]") {
    // slot_index is unique only WITHIN a backend. Matching on slot alone picks
    // the wrong lane - the live bug in preflight_validator.cpp's find_slot().
    auto ctx = ctx_with([](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {0.0, 863.07};
        c.tools_used = {1};
        c.mappings = {map_tool(1, /*slot=*/0, /*backend=*/1)};
        // Backend 0 slot 0 is nearly empty; backend 1 slot 0 is full. Only the
        // backend-1 lane is mapped, so there must be no shortfall.
        c.available_slots = {lane(0, 0, 10.0f), lane(0, 1, 1000.0f)};
    });
    CHECK(insufficient_lane_weights_in(ctx).empty());
}

TEST_CASE("insufficient_lane_weights_in: single tool falls back to the file total",
          "[print-start][gate-pipeline]") {
    // No per-tool line (older slicer / Moonraker metadata only), one tool used:
    // the whole-file total IS that tool's, exactly - no split is invented.
    auto ctx = ctx_with([](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {}; // no footer breakdown
        c.tools_used = {1};
        c.mappings = {map_tool(1, 1)};
        c.available_slots = {lane(1, 0, 65.0f)};
    });
    auto shortfalls = insufficient_lane_weights_in(ctx);
    REQUIRE(shortfalls.size() == 1);
    CHECK(shortfalls[0].needed_g > 860.0f);
}

TEST_CASE("insufficient_lane_weights_in: multi-tool with no breakdown stays silent",
          "[print-start][gate-pipeline]") {
    // Deliberate: dividing the total across tools would fabricate a split that
    // is wrong in both directions on any uneven print.
    auto ctx = ctx_with([](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {};
        c.tools_used = {0, 1};
        c.mappings = {map_tool(0, 0), map_tool(1, 1)};
        c.available_slots = {lane(0, 0, 10.0f), lane(1, 0, 10.0f)};
    });
    CHECK(insufficient_lane_weights_in(ctx).empty());
}

TEST_CASE("insufficient_lane_weights_in: a mapping to no known lane is silent",
          "[print-start][gate-pipeline]") {
    // A mapped_slot outside the connected units (e.g. the synthetic external
    // lane index) must never be priced - no lane, no opinion.
    auto ctx = ctx_with([](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {0.0, 863.07};
        c.tools_used = {1};
        c.mappings = {map_tool(1, /*slot=*/4)};
        c.available_slots = {lane(0, 0, 1000.0f), lane(1, 0, 1000.0f)};
    });
    CHECK(insufficient_lane_weights_in(ctx).empty());
}

TEST_CASE("insufficient_lane_weights_in: an unused tool is not a shortfall",
          "[print-start][gate-pipeline]") {
    // tool 0 is mapped to an empty lane but the file never prints with it.
    auto ctx = ctx_with([](PrintStartContext& c) {
        FileMetadata md;
        md.filament_weight_total = 863.07;
        c.metadata = md;
        c.tool_grams = {0.0, 863.07};
        c.tools_used = {1};
        c.mappings = {map_tool(0, 0), map_tool(1, 2)};
        c.available_slots = {lane(0, 0, 0.0f), lane(2, 0, 1000.0f)};
    });
    CHECK(insufficient_lane_weights_in(ctx).empty());
}

TEST_CASE("insufficient_lane_weights_in: bypass single-lane prints defer to the spool check",
          "[print-start][gate-pipeline]") {
    auto ctx = orca_ctx(/*slot=*/1, /*remaining=*/65.0f);
    ctx.any_bypass_active = true;
    CHECK(insufficient_lane_weights_in(ctx).empty());
}

TEST_CASE("gate_insufficient_lane_weight: names the short slot", "[print-start][gate-pipeline]") {
    auto result =
        gate_named("insufficient_lane_weight").evaluate(orca_ctx(/*slot=*/1, /*remaining=*/65.0f));
    REQUIRE(result.verdict == CheckResult::Verdict::Warn);
    // Slot label is 1-indexed to match the picker ("Slot 2", not "Slot 1"),
    // and both magnitudes appear so the user can see which way it is short.
    CHECK(result.body.find("Slot 2") != std::string::npos);
    CHECK(result.body.find("65") != std::string::npos);
    CHECK(result.body.find("863") != std::string::npos);
    CHECK(!result.proceed_label.empty());
}

// ---------------------------------------------------------------------------
// unaccounted_toolhead_filament: the remedy follows the hardware
// ---------------------------------------------------------------------------
//
// The detection was always right (a K2 Plus reported filament_detected=true at
// the toolhead while box.T1.filament read "None"), but the advice was not. The
// CFS has a lane-free heat/cut/retract - bypass_unload_gcode(), built without
// the bay envelopes precisely because a stood-down box cannot answer a bay
// operation - so directing the user to pull filament out of a hot toolhead by
// hand is both wrong and worse than what the printer can do itself. On hardware
// with no such path, the manual pull is still the only answer.

TEST_CASE("gate unaccounted_toolhead_filament: advice depends on whether the backend can clear it",
          "[print-start][gate-pipeline][toolhead]") {
    auto make = [](bool clearable) {
        return ctx_with([&](PrintStartContext& c) {
            c.any_bypass_active = false;
            c.toolhead_unaccounted = {std::optional<bool>(true)};
            c.toolhead_clearable = {clearable};
        });
    };
    auto& g = gate_named("unaccounted_toolhead_filament");

    auto cutter = g.evaluate(make(true));
    REQUIRE(cutter.verdict == CheckResult::Verdict::Warn);
    CHECK(cutter.title == "Filament In The Toolhead");
    CHECK(cutter.body.find("Pull it out manually") == std::string::npos);
    CHECK(cutter.body.find("refuse to load a lane") != std::string::npos);

    auto manual = g.evaluate(make(false));
    REQUIRE(manual.verdict == CheckResult::Verdict::Warn);
    CHECK(manual.body.find("Pull it out manually") != std::string::npos);
}

TEST_CASE("gate unaccounted_toolhead_filament: a missing capability entry reads as cannot-clear",
          "[print-start][gate-pipeline][toolhead]") {
    // toolhead_clearable is filled in lockstep with toolhead_unaccounted, but a
    // shorter vector must degrade to the conservative advice rather than index
    // past the end.
    auto c = ctx_with([](PrintStartContext& ctx) {
        ctx.any_bypass_active = false;
        ctx.toolhead_unaccounted = {std::optional<bool>(true)};
        ctx.toolhead_clearable = {}; // deliberately empty
    });
    auto r = gate_named("unaccounted_toolhead_filament").evaluate(c);
    REQUIRE(r.verdict == CheckResult::Verdict::Warn);
    CHECK(r.body.find("Pull it out manually") != std::string::npos);
}

// ============================================================================
// Dismissing a gate dialog must resolve the pipeline (#1380)
// ============================================================================

// modal_show_confirmation pushes with no owner, so a backdrop tap, ESC, or a
// hot-reload rebuild closes the gate dialog without running either button
// handler. update_print_button_() and on_print_cancelled_() therefore never
// ran, and there is no watchdog on this path - the Print button stayed parked
// with no way back.
TEST_CASE("gate runner: dismissing the dialog resolves like cancel",
          "[print-start][gate-pipeline][1380]") {
    GateRunnerFixture fx;
    fx.use_toy_gates(/*warn_first=*/true);

    PrintStartControllerTestAccess::run_gates(fx.controller);
    lv_obj_t* dlg = PrintStartControllerTestAccess::print_gate_modal(fx.controller);
    REQUIRE(dlg != nullptr);
    REQUIRE(fx.cancelled == 0);

    // Neither button: this is the dismissal path.
    Modal::hide(dlg);
    fx.process_lvgl(50);

    CHECK(fx.cancelled == 1);
    CHECK(fx.button_updates >= 1);
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) == nullptr);
}

// The resolve must not fire when a button DID answer. on_gate_proceed() clears
// print_gate_modal_ and run_gates_from() may immediately put the NEXT gate's
// dialog in it, while the first dialog's DELETE only arrives at the end of its
// exit animation. Treating that late DELETE as a dismissal would cancel a print
// that is still proceeding - and clear the live dialog's handle with it.
TEST_CASE("gate runner: a proceeded gate's late delete does not cancel the print",
          "[print-start][gate-pipeline][1380]") {
    GateRunnerFixture fx;

    // Two warning gates, so proceeding from the first opens a second dialog.
    std::vector<PrintStartGate> gates;
    gates.push_back({"warn_a", +[](const PrintStartContext&) { return warn_result("Warn A"); }});
    gates.push_back({"warn_b", +[](const PrintStartContext&) { return warn_result("Warn B"); }});
    PrintStartControllerTestAccess::set_gates(fx.controller, std::move(gates));

    PrintStartControllerTestAccess::run_gates(fx.controller);
    lv_obj_t* first = PrintStartControllerTestAccess::print_gate_modal(fx.controller);
    REQUIRE(first != nullptr);

    // User proceeds: gate B's dialog replaces gate A's in the handle, while
    // gate A's widget is still animating out.
    PrintStartControllerTestAccess::gate_proceed(fx.controller);
    lv_obj_t* second = PrintStartControllerTestAccess::print_gate_modal(fx.controller);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);

    // Gate A's DELETE lands now.
    fx.process_lvgl(50);

    CHECK(fx.cancelled == 0); // the print was never cancelled
    CHECK(PrintStartControllerTestAccess::print_gate_modal(fx.controller) == second);
}
