// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"
#include "filament_database.h"

#include <set>
#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace filament;
using Catch::Approx;

// ============================================================================
// find_material tests
// ============================================================================

TEST_CASE("find_material - exact name lookup", "[filament][database]") {
    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PLA");
    CHECK(result->nozzle_min == 190);
    CHECK(result->nozzle_max == 220);
    CHECK(result->bed_temp == 60);
}

TEST_CASE("find_material - case insensitive lowercase", "[filament][database]") {
    auto result = find_material("pla");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PLA");
}

TEST_CASE("find_material - case insensitive mixed case", "[filament][database]") {
    auto result = find_material("Pla");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PLA");
}

TEST_CASE("find_material - unknown material returns nullopt", "[filament][database]") {
    auto result = find_material("FooBar");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("find_material - empty string returns nullopt", "[filament][database]") {
    auto result = find_material("");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("find_material - verifies all new fields populated", "[filament][database]") {
    auto result = find_material("ABS");
    REQUIRE(result.has_value());

    // Basic fields
    CHECK(std::string_view(result->name) == "ABS");
    CHECK(std::string_view(result->category) == "Engineering");

    // Temperature fields
    CHECK(result->nozzle_min > 0);
    CHECK(result->nozzle_max > result->nozzle_min);
    CHECK(result->bed_temp > 0);

    // Drying fields
    CHECK(result->dry_temp_c == 60);
    CHECK(result->dry_time_min == 240);

    // Physical properties
    CHECK(result->density_g_cm3 == Approx(1.04f).epsilon(0.01f));

    // Classification
    CHECK(result->chamber_temp_c == 50);
    CHECK(std::string_view(result->compat_group) == "ABS_ASA");
}

// ============================================================================
// resolve_alias tests
// ============================================================================

TEST_CASE("resolve_alias - Nylon resolves to PA", "[filament][database][alias]") {
    auto resolved = resolve_alias("Nylon");
    CHECK(resolved == "PA");
}

TEST_CASE("resolve_alias - ULTEM resolves to PEI", "[filament][database][alias]") {
    auto resolved = resolve_alias("ULTEM");
    CHECK(resolved == "PEI");
}

TEST_CASE("resolve_alias - case insensitive", "[filament][database][alias]") {
    auto resolved = resolve_alias("nylon");
    CHECK(resolved == "PA");
}

TEST_CASE("resolve_alias - non-alias returns original", "[filament][database][alias]") {
    auto resolved = resolve_alias("PLA");
    CHECK(resolved == "PLA");
}

TEST_CASE("resolve_alias - empty string returns empty", "[filament][database][alias]") {
    auto resolved = resolve_alias("");
    CHECK(resolved == "");
}

TEST_CASE("resolve_alias - Polycarbonate resolves to PC", "[filament][database][alias]") {
    auto resolved = resolve_alias("Polycarbonate");
    CHECK(resolved == "PC");
}

// ============================================================================
// find_material with aliases
// ============================================================================

TEST_CASE("find_material - Nylon alias returns PA info", "[filament][database][alias]") {
    auto result = find_material("Nylon");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PA");
    CHECK(std::string_view(result->compat_group) == "PA");
}

TEST_CASE("find_material - Polycarbonate alias returns PC info", "[filament][database][alias]") {
    auto result = find_material("Polycarbonate");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PC");
    CHECK(std::string_view(result->compat_group) == "PC");
}

TEST_CASE("find_material - ULTEM alias returns PEI info", "[filament][database][alias]") {
    auto result = find_material("ULTEM");
    REQUIRE(result.has_value());
    CHECK(std::string_view(result->name) == "PEI");
    CHECK(std::string_view(result->compat_group) == "HIGH_TEMP");
}

// ============================================================================
// are_materials_compatible tests
// ============================================================================

TEST_CASE("are_materials_compatible - same group PLA variants", "[filament][database][compat]") {
    CHECK(are_materials_compatible("PLA", "PLA-CF"));
    CHECK(are_materials_compatible("PLA", "PLA+"));
    CHECK(are_materials_compatible("Silk PLA", "Matte PLA"));
}

TEST_CASE("are_materials_compatible - same group ABS and ASA", "[filament][database][compat]") {
    CHECK(are_materials_compatible("ABS", "ASA"));
    CHECK(are_materials_compatible("ABS", "HIPS"));   // HIPS is in ABS_ASA group
    CHECK(are_materials_compatible("PC-ABS", "ASA")); // PC-ABS blend is ABS_ASA group
}

TEST_CASE("are_materials_compatible - different groups incompatible",
          "[filament][database][compat]") {
    CHECK_FALSE(are_materials_compatible("PLA", "PETG"));
    CHECK_FALSE(are_materials_compatible("PLA", "ABS"));
    CHECK_FALSE(are_materials_compatible("PETG", "ABS"));
    CHECK_FALSE(are_materials_compatible("PC", "PA")); // Different engineering groups
}

TEST_CASE("are_materials_compatible - unknown material compatible with everything",
          "[filament][database][compat]") {
    CHECK(are_materials_compatible("FooBar", "PLA"));
    CHECK(are_materials_compatible("PLA", "FooBar"));
    CHECK(are_materials_compatible("FooBar", "ABS"));
}

TEST_CASE("are_materials_compatible - both unknown returns true", "[filament][database][compat]") {
    CHECK(are_materials_compatible("FooBar", "BazQux"));
}

// ============================================================================
// get_compatibility_group tests
// ============================================================================

TEST_CASE("get_compatibility_group - known material returns group",
          "[filament][database][compat]") {
    CHECK(std::string_view(get_compatibility_group("PLA")) == "PLA");
    CHECK(std::string_view(get_compatibility_group("PETG")) == "PETG");
    CHECK(std::string_view(get_compatibility_group("ABS")) == "ABS_ASA");
    CHECK(std::string_view(get_compatibility_group("PA")) == "PA");
    CHECK(std::string_view(get_compatibility_group("TPU")) == "TPU");
    CHECK(std::string_view(get_compatibility_group("PC")) == "PC");
    CHECK(std::string_view(get_compatibility_group("PEEK")) == "HIGH_TEMP");
}

TEST_CASE("get_compatibility_group - unknown material returns nullptr",
          "[filament][database][compat]") {
    CHECK(get_compatibility_group("FooBar") == nullptr);
    CHECK(get_compatibility_group("UnknownMaterial") == nullptr);
}

// ============================================================================
// get_drying_presets_by_group tests
// ============================================================================

TEST_CASE("get_drying_presets_by_group - returns non-empty vector",
          "[filament][database][drying]") {
    auto presets = get_drying_presets_by_group();
    CHECK_FALSE(presets.empty());
}

TEST_CASE("get_drying_presets_by_group - contains expected groups",
          "[filament][database][drying]") {
    auto presets = get_drying_presets_by_group();

    auto has_group = [&presets](std::string_view name) {
        for (const auto& p : presets) {
            if (std::string_view(p.name) == name) {
                return true;
            }
        }
        return false;
    };

    CHECK(has_group("PLA"));
    CHECK(has_group("PETG"));
    CHECK(has_group("ABS_ASA"));
    CHECK(has_group("PC"));
    CHECK(has_group("PA"));
    CHECK(has_group("TPU"));
    CHECK(has_group("HIGH_TEMP"));
}

TEST_CASE("get_drying_presets_by_group - each preset has reasonable values",
          "[filament][database][drying]") {
    auto presets = get_drying_presets_by_group();

    for (const auto& preset : presets) {
        INFO("Checking preset: " << preset.name);
        CHECK(preset.temp_c > 0);
        CHECK(preset.temp_c <= 120); // Reasonable upper bound
        CHECK(preset.time_min > 0);
        CHECK(preset.time_min <= 720); // 12 hours max
    }
}

TEST_CASE("get_drying_presets_by_group - presets have unique groups",
          "[filament][database][drying]") {
    auto presets = get_drying_presets_by_group();

    for (size_t i = 0; i < presets.size(); i++) {
        for (size_t j = i + 1; j < presets.size(); j++) {
            CHECK(std::string_view(presets[i].name) != std::string_view(presets[j].name));
        }
    }
}

// ============================================================================
// weight_to_length_m tests
// ============================================================================

TEST_CASE("weight_to_length_m - 1kg PLA calculation", "[filament][database][weight]") {
    // 1kg PLA (density 1.24 g/cm³) at 1.75mm diameter
    // Expected: approximately 335m (standard industry value)
    float length = weight_to_length_m(1000.0f, 1.24f, 1.75f);

    // Allow 5% tolerance
    CHECK(length == Approx(335.0f).epsilon(0.05f));
}

TEST_CASE("weight_to_length_m - zero weight returns zero", "[filament][database][weight]") {
    float length = weight_to_length_m(0.0f, 1.24f, 1.75f);
    CHECK(length == 0.0f);
}

TEST_CASE("weight_to_length_m - different diameters", "[filament][database][weight]") {
    // 2.85mm filament should give shorter length for same weight
    float length_175 = weight_to_length_m(1000.0f, 1.24f, 1.75f);
    float length_285 = weight_to_length_m(1000.0f, 1.24f, 2.85f);

    CHECK(length_285 < length_175);
    // 2.85mm is ~1.63x the diameter, so area is ~2.65x larger, length should be ~2.65x shorter
    CHECK(length_175 / length_285 == Approx(2.65f).epsilon(0.05f));
}

TEST_CASE("weight_to_length_m - different densities", "[filament][database][weight]") {
    // Higher density = shorter length for same weight
    float length_pla = weight_to_length_m(1000.0f, 1.24f); // PLA
    float length_abs = weight_to_length_m(1000.0f, 1.04f); // ABS

    CHECK(length_abs > length_pla); // ABS is less dense, more length per kg
}

// ============================================================================
// MaterialInfo helper method tests
// ============================================================================

TEST_CASE("MaterialInfo::needs_enclosure - PLA does not need enclosure",
          "[filament][database][helpers]") {
    auto pla = find_material("PLA");
    REQUIRE(pla.has_value());
    CHECK(pla->chamber_temp_c == 0);
    CHECK_FALSE(pla->needs_enclosure());
}

TEST_CASE("MaterialInfo::needs_enclosure - ABS needs enclosure", "[filament][database][helpers]") {
    auto abs = find_material("ABS");
    REQUIRE(abs.has_value());
    CHECK(abs->chamber_temp_c == 50);
    CHECK(abs->needs_enclosure());
}

TEST_CASE("MaterialInfo::needs_enclosure - PETG does not need enclosure",
          "[filament][database][helpers]") {
    auto petg = find_material("PETG");
    REQUIRE(petg.has_value());
    CHECK_FALSE(petg->needs_enclosure());
}

TEST_CASE("MaterialInfo::needs_enclosure - PC needs enclosure", "[filament][database][helpers]") {
    auto pc = find_material("PC");
    REQUIRE(pc.has_value());
    CHECK(pc->needs_enclosure());
}

TEST_CASE("MaterialInfo::needs_drying - PLA needs drying", "[filament][database][helpers]") {
    auto pla = find_material("PLA");
    REQUIRE(pla.has_value());
    CHECK(pla->dry_temp_c == 45);
    CHECK(pla->needs_drying());
}

TEST_CASE("MaterialInfo::needs_drying - every material except the known non-hygroscopic ones",
          "[filament][database][helpers]") {
    // Nearly every material in the database is hygroscopic enough to want
    // drying. The two exceptions are deliberate and must be spelled out by
    // name rather than tolerated generically, so that a dry_temp_c that gets
    // zeroed by ACCIDENT on any other row still fails this test:
    //
    //   PE  — polyolefin, negligible moisture uptake; drying buys nothing.
    //   EVA — low Vicat softening point makes oven drying actively risky.
    const std::set<std::string_view> INTENTIONALLY_NOT_DRIED{"PE", "EVA"};

    std::set<std::string_view> seen_not_dried;
    for (const auto& mat : MATERIALS) {
        INFO("Checking material: " << mat.name);
        if (INTENTIONALLY_NOT_DRIED.count(mat.name) > 0) {
            // Pin the exception: if someone later gives PE or EVA a real dry
            // temperature, this fires and the exception list must be updated.
            CHECK_FALSE(mat.needs_drying());
            seen_not_dried.insert(mat.name);
        } else {
            CHECK(mat.needs_drying());
        }
    }

    // Guard against the exception list rotting if a row is renamed or removed.
    CHECK(seen_not_dried == INTENTIONALLY_NOT_DRIED);
}

TEST_CASE("MaterialInfo::nozzle_recommended - returns midpoint", "[filament][database][helpers]") {
    auto pla = find_material("PLA");
    REQUIRE(pla.has_value());

    // PLA: 190-220, midpoint = 205
    CHECK(pla->nozzle_recommended() == (190 + 220) / 2);
    CHECK(pla->nozzle_recommended() == 205);
}

TEST_CASE("MaterialInfo::nozzle_recommended - ABS midpoint", "[filament][database][helpers]") {
    auto abs = find_material("ABS");
    REQUIRE(abs.has_value());

    // ABS: 240-270, midpoint = 255
    CHECK(abs->nozzle_recommended() == (240 + 270) / 2);
    CHECK(abs->nozzle_recommended() == 255);
}

TEST_CASE("MaterialInfo::nozzle_recommended - PEEK high temp", "[filament][database][helpers]") {
    auto peek = find_material("PEEK");
    REQUIRE(peek.has_value());

    // PEEK: 370-420, midpoint = 395
    CHECK(peek->nozzle_recommended() == (370 + 420) / 2);
    CHECK(peek->nozzle_recommended() == 395);
}

// ============================================================================
// Additional coverage tests
// ============================================================================

TEST_CASE("get_materials_by_category - Standard category", "[filament][database]") {
    auto materials = get_materials_by_category("Standard");
    CHECK_FALSE(materials.empty());

    bool has_pla = false;
    bool has_petg = false;
    for (const auto& mat : materials) {
        if (std::string_view(mat.name) == "PLA")
            has_pla = true;
        if (std::string_view(mat.name) == "PETG")
            has_petg = true;
    }
    CHECK(has_pla);
    CHECK(has_petg);
}

TEST_CASE("get_categories - returns all categories", "[filament][database]") {
    auto categories = get_categories();
    CHECK_FALSE(categories.empty());

    auto has_category = [&categories](std::string_view name) {
        for (const auto* cat : categories) {
            if (std::string_view(cat) == name)
                return true;
        }
        return false;
    };

    CHECK(has_category("Standard"));
    CHECK(has_category("Engineering"));
    CHECK(has_category("Flexible"));
    CHECK(has_category("Support"));
    CHECK(has_category("Specialty"));
    CHECK(has_category("High-Temp"));
    CHECK(has_category("Recycled"));
}

TEST_CASE("get_all_material_names - returns all materials", "[filament][database]") {
    auto names = get_all_material_names();
    CHECK(names.size() == MATERIAL_COUNT);
}

TEST_CASE("MATERIAL_COUNT matches array size", "[filament][database]") {
    size_t count = 0;
    for (const auto& mat : MATERIALS) {
        (void)mat;
        count++;
    }
    CHECK(count == MATERIAL_COUNT);
}

// ============================================================================
// Phase 1: New materials tests (composites, nylon variants, TPU variants, recycled)
// ============================================================================

TEST_CASE("Phase 1 - ABS composites exist", "[filament][database][phase1]") {
    // Carbon and glass fiber ABS variants
    auto abs_cf = find_material("ABS-CF");
    REQUIRE(abs_cf.has_value());
    CHECK(std::string_view(abs_cf->compat_group) == "ABS_ASA");
    CHECK(abs_cf->chamber_temp_c > 0); // Needs enclosure

    auto abs_gf = find_material("ABS-GF");
    REQUIRE(abs_gf.has_value());
    CHECK(std::string_view(abs_gf->compat_group) == "ABS_ASA");
}

TEST_CASE("Phase 1 - ASA composites exist", "[filament][database][phase1]") {
    // Carbon and glass fiber ASA variants
    auto asa_cf = find_material("ASA-CF");
    REQUIRE(asa_cf.has_value());
    CHECK(std::string_view(asa_cf->compat_group) == "ABS_ASA");

    auto asa_gf = find_material("ASA-GF");
    REQUIRE(asa_gf.has_value());
    CHECK(std::string_view(asa_gf->compat_group) == "ABS_ASA");
}

TEST_CASE("Phase 1 - Nylon variants exist", "[filament][database][phase1]") {
    // PA66 and PPA (polyphthalamide)
    auto pa66 = find_material("PA66");
    REQUIRE(pa66.has_value());
    CHECK(std::string_view(pa66->compat_group) == "PA");
    CHECK(pa66->chamber_temp_c > 0); // Needs enclosure

    auto ppa = find_material("PPA");
    REQUIRE(ppa.has_value());
    CHECK(std::string_view(ppa->compat_group) == "PA");
}

TEST_CASE("Phase 1 - TPU Shore hardness variants exist", "[filament][database][phase1]") {
    // Specific Shore hardness variants
    auto tpu_95a = find_material("TPU-95A");
    REQUIRE(tpu_95a.has_value());
    CHECK(std::string_view(tpu_95a->compat_group) == "TPU");

    auto tpu_85a = find_material("TPU-85A");
    REQUIRE(tpu_85a.has_value());
    CHECK(std::string_view(tpu_85a->compat_group) == "TPU");
}

TEST_CASE("Phase 1 - PCTG exists in PETG group", "[filament][database][phase1]") {
    auto pctg = find_material("PCTG");
    REQUIRE(pctg.has_value());
    CHECK(std::string_view(pctg->compat_group) == "PETG");
    CHECK(pctg->chamber_temp_c == 0); // No enclosure needed
}

TEST_CASE("Phase 1 - Recycled materials exist", "[filament][database][phase1]") {
    // Recycled PLA and PETG
    auto rpla = find_material("rPLA");
    REQUIRE(rpla.has_value());
    CHECK(std::string_view(rpla->compat_group) == "PLA");

    auto rpetg = find_material("rPETG");
    REQUIRE(rpetg.has_value());
    CHECK(std::string_view(rpetg->compat_group) == "PETG");
}

TEST_CASE("Phase 1 - PC-GF exists", "[filament][database][phase1]") {
    auto pc_gf = find_material("PC-GF");
    REQUIRE(pc_gf.has_value());
    CHECK(std::string_view(pc_gf->compat_group) == "PC");
    CHECK(pc_gf->chamber_temp_c > 0); // Needs enclosure
}

TEST_CASE("Phase 1 - Material count increased", "[filament][database][phase1]") {
    // After Phase 1, should have ~50 materials (35 original + ~15 new)
    CHECK(MATERIAL_COUNT >= 48);
}

TEST_CASE("Phase 1 - All compat groups have representatives", "[filament][database][phase1]") {
    // Verify each compatibility group has at least one material
    std::set<std::string_view> groups_found;

    for (const auto& mat : MATERIALS) {
        if (mat.compat_group != nullptr) {
            groups_found.insert(mat.compat_group);
        }
    }

    // All 7 groups should be represented
    CHECK(groups_found.count("PLA") == 1);
    CHECK(groups_found.count("PETG") == 1);
    CHECK(groups_found.count("ABS_ASA") == 1);
    CHECK(groups_found.count("PA") == 1);
    CHECK(groups_found.count("TPU") == 1);
    CHECK(groups_found.count("PC") == 1);
    CHECK(groups_found.count("HIGH_TEMP") == 1);
}

// ============================================================================
// Phase 2: ams_types.h integration tests (get_default_drying_presets delegates to filament)
// ============================================================================

TEST_CASE("Phase 2 - get_default_drying_presets covers all compat groups",
          "[filament][database][phase2][ams]") {
    // get_default_drying_presets() should return one preset per compat group
    auto presets = get_default_drying_presets();

    // Should have presets for all 7 compat groups
    CHECK(presets.size() >= 7);

    // Verify we have the expected groups
    auto has_preset = [&presets](std::string_view name) {
        for (const auto& p : presets) {
            if (p.name == name)
                return true;
        }
        return false;
    };

    CHECK(has_preset("PLA"));
    CHECK(has_preset("PETG"));
    CHECK(has_preset("ABS_ASA"));
    CHECK(has_preset("PA"));
    CHECK(has_preset("TPU"));
    CHECK(has_preset("PC"));
    CHECK(has_preset("HIGH_TEMP"));
}

TEST_CASE("Phase 2 - get_default_drying_presets matches filament database",
          "[filament][database][phase2][ams]") {
    auto ams_presets = get_default_drying_presets();
    auto filament_presets = get_drying_presets_by_group();

    // For each filament preset, there should be a matching ams preset with same temp/time
    for (const auto& fp : filament_presets) {
        bool found = false;
        for (const auto& ap : ams_presets) {
            if (ap.name == fp.name) {
                found = true;
                // Temperature should match (ams uses float, filament uses int)
                CHECK(static_cast<int>(ap.temp_c) == fp.temp_c);
                // Duration should match
                CHECK(ap.duration_min == fp.time_min);
                break;
            }
        }
        INFO("Missing preset for: " << fp.name);
        CHECK(found);
    }
}

TEST_CASE("Phase 2 - get_default_drying_presets includes fan_pct",
          "[filament][database][phase2][ams]") {
    auto presets = get_default_drying_presets();

    // All presets should have reasonable fan_pct values (0-100)
    for (const auto& p : presets) {
        INFO("Checking preset: " << p.name);
        CHECK(p.fan_pct >= 0);
        CHECK(p.fan_pct <= 100);
    }
}

// ============================================================================
// Orphan-type closure: types that shipped in assets/filaments.json with no
// filament_database.h entry, so they inherited nothing (0 °C bed, 0 density,
// empty compat group) through filament_catalog.cpp::to_effective().
// ============================================================================

TEST_CASE("orphan catalog types now resolve with complete data", "[filament][database]") {
    // Every one of these had >= 1 product in assets/filaments.json but no
    // database row. Deleting any row fails this test.
    static const char* FORMER_ORPHANS[] = {
        "ASA-AERO", "CoPE",  "EVA",   "PA6-CF", "PE",     "PET", "PHA",    "PLA-AERO",
        "PP",       "PP-CF", "PP-GF", "PPA-CF", "PPA-GF", "PPS", "PPS-CF", "SBS",
    };

    for (const auto* name : FORMER_ORPHANS) {
        INFO("material: " << name);
        auto mat = find_material(name);
        REQUIRE(mat.has_value());
        CHECK(mat->nozzle_min > 0);
        CHECK(mat->nozzle_max > mat->nozzle_min);
        CHECK(mat->bed_temp > 0);         // the actual bug: silently defaulted to 0
        CHECK(mat->density_g_cm3 > 0.0f); // ditto
        CHECK(mat->category != nullptr);
        CHECK(std::string_view(mat->category) != "");
        CHECK(mat->compat_group != nullptr);
        CHECK(std::string_view(mat->compat_group) != "");
    }
}

TEST_CASE("SILK catalog type resolves via the Silk alias", "[filament][database][alias]") {
    // "Generic Silk PLA" ships with type "SILK" (an AD5X firmware whitelist name).
    // resolve_alias() is case-insensitive, so the existing {"Silk", "Silk PLA"}
    // alias already covers it — no separate SILK row is needed. Removing or
    // renaming that alias would strand the product, so pin the behaviour here.
    auto mat = find_material("SILK");
    REQUIRE(mat.has_value());
    CHECK(std::string_view(mat->name) == "Silk PLA");
    CHECK(mat->bed_temp > 0);
}

TEST_CASE("polyolefins keep isolated compat groups", "[filament][database]") {
    // PP and PE bond only to themselves. If they shared a group with PETG/PLA,
    // endless spool would happily swap in a filament that will not stick.
    CHECK_FALSE(are_materials_compatible("PP", "PETG"));
    CHECK_FALSE(are_materials_compatible("PP", "PLA"));
    CHECK_FALSE(are_materials_compatible("PE", "PP"));
    CHECK(are_materials_compatible("PP", "PP-CF"));
    CHECK(are_materials_compatible("PP-CF", "PP-GF"));
}

TEST_CASE("semi-flexibles are not interchangeable with TPU", "[filament][database]") {
    CHECK_FALSE(are_materials_compatible("SBS", "TPU"));
    CHECK_FALSE(are_materials_compatible("CoPE", "TPU"));
    CHECK_FALSE(are_materials_compatible("EVA", "TPU"));
}

TEST_CASE("no duplicate material names in database", "[filament][database]") {
    std::set<std::string> seen;
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name);
        CHECK(seen.insert(mat.name).second);
    }
}

TEST_CASE("every material row is internally consistent", "[filament][database]") {
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name);
        CHECK(mat.nozzle_min > 0);
        CHECK(mat.nozzle_max >= mat.nozzle_min);
        CHECK(mat.bed_temp > 0);
        CHECK(mat.density_g_cm3 > 0.0f);
        CHECK(std::string_view(mat.category) != "");
        CHECK(std::string_view(mat.compat_group) != "");
        // dry_temp_c == 0 means "not hygroscopic"; then dry_time_min must be 0 too.
        if (mat.dry_temp_c == 0)
            CHECK(mat.dry_time_min == 0);
        else
            CHECK(mat.dry_time_min > 0);
    }
}

TEST_CASE("no material alias shadows a real material name", "[filament][database][alias]") {
    // An alias whose name also exists as a MATERIALS row would silently redirect
    // lookups away from that row (resolve_alias runs first in find_material).
    for (const auto& alias : MATERIAL_ALIASES) {
        INFO("alias: " << alias.alias);
        bool shadows = false;
        for (const auto& mat : MATERIALS) {
            std::string a(alias.alias), m(mat.name);
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(m.begin(), m.end(), m.begin(), ::tolower);
            if (a == m)
                shadows = true;
        }
        CHECK_FALSE(shadows);
        // ...and every alias must point at something real.
        bool resolves = false;
        for (const auto& mat : MATERIALS)
            if (std::string_view(mat.name) == std::string_view(alias.canonical))
                resolves = true;
        CHECK(resolves);
    }
}
