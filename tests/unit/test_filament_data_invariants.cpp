// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sanity / invariant tests for the two-layer filament material database.
//
//   Layer A — material TYPE table: filament::MATERIALS[] in filament_database.h
//   Layer B — brand PRODUCT catalog: assets/filaments.json via FilamentCatalog
//
// These tests exist because the *previous* guard here was a disjunction —
// `find_material(type) || (nozzle_min > 0 && nozzle_max > 0)` — whose right side
// was always satisfied by the Orca importer's explicit nozzle temps, so the left
// side was never actually exercised. Meanwhile the fields that really fell
// through to zero (bed_temp, density, dry_temp/dry_time, compat_group) had no
// guard at all, and `generic-pet` shipped with a 0 °C bed.
//
// Every test here must FAIL if the invariant it names is violated.
//
// An earlier revision pinned three then-live violations behind narrow allowlists
// (duplicate Generic/PETG-CF, generic-eva's 200-200 nozzle range, and the PETG/PA
// groups' divergent drying values). All three have since been fixed at their
// source in the importer and the material table, so the allowlists are GONE and
// those predicates are now unconditional. Prefer fixing the data over adding an
// allowlist back; an allowlist that outlives its fix is just a slower drift.

#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_mock.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "helix_test_fixture.h"
#include "material_settings_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::printer::FilamentCatalog;
using namespace filament;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& x : s)
        out += x + " ";
    return out;
}

/// Absolute envelope for an FDM hotend. Nothing real prints at 50 °C or 900 °C;
/// a value outside this band is a transposed digit, not a material.
constexpr int NOZZLE_FLOOR = 150;
constexpr int NOZZLE_CEIL = 450;

/// Widest plausible product-vs-type disagreement. A brand may legitimately run
/// hotter or cooler than the generic type (LW-PLA foams at 260-270 under type
/// "PLA"), but a 50 °C gulf is the outer edge of "legitimate variant" and past it
/// you are looking at a units error or a wrong `type` field.
constexpr int TYPE_DIVERGENCE_TOLERANCE = 50;

} // namespace

// ============================================================================
// Layer A — physical plausibility
//
// These catch data-entry typos: a transposed digit, a dropped decimal point, a
// value pasted into the wrong column. Every one of them would ship silently.
// ============================================================================

TEST_CASE("MATERIALS - nozzle range is ordered and physically plausible",
          "[filament][database][invariant]") {
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name << " nozzle " << mat.nozzle_min << "-" << mat.nozzle_max);
        // Strict: a zero-width range means the "range" carries no information and
        // nozzle_recommended() degenerates to a single point.
        CHECK(mat.nozzle_min < mat.nozzle_max);
        CHECK(mat.nozzle_min >= NOZZLE_FLOOR);
        CHECK(mat.nozzle_max <= NOZZLE_CEIL);
        // A range wider than 80 °C is not a recommendation, it is a guess. The
        // widest legitimate entry today is PLA-AERO at 60 (foaming grades use
        // temperature as the foaming knob).
        const int span = mat.nozzle_max - mat.nozzle_min;
        CHECK(span >= 10);
        CHECK(span <= 80);
        // nozzle_recommended() is the midpoint; it must land inside the range.
        CHECK(mat.nozzle_recommended() >= mat.nozzle_min);
        CHECK(mat.nozzle_recommended() <= mat.nozzle_max);
    }
}

TEST_CASE("MATERIALS - bed and chamber temps are physically plausible",
          "[filament][database][invariant]") {
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name << " bed " << mat.bed_temp << " chamber "
                          << mat.chamber_temp_c);
        // A 0 °C bed is the exact bug that shipped in generic-pet. No FDM
        // material prints on an unheated-and-below-ambient bed.
        CHECK(mat.bed_temp >= 20);
        CHECK(mat.bed_temp <= 160);
        // chamber_temp_c == 0 means "open frame, no chamber requirement".
        if (mat.chamber_temp_c != 0) {
            CHECK(mat.chamber_temp_c >= 30);
            CHECK(mat.chamber_temp_c <= 120);
            // A chamber hotter than the bed is thermodynamically backwards: the
            // bed is the chamber's heat source on every printer we support.
            CHECK(mat.chamber_temp_c <= mat.bed_temp);
        }
        // needs_enclosure() must agree with the raw field it reads.
        CHECK(mat.needs_enclosure() == (mat.chamber_temp_c > 0));
    }
}

TEST_CASE("MATERIALS - density is in the thermoplastic band", "[filament][database][invariant]") {
    // Unfilled thermoplastics run ~0.90 (PP) to ~1.45 (PPS-CF). Heavily filled
    // decorative grades can reach ~2.0. A 0.0 is the inheritance bug; a 12.0 is a
    // misplaced decimal. Either poisons weight_to_length_m() and every remaining
    // -filament estimate derived from it.
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name << " density " << mat.density_g_cm3);
        CHECK(mat.density_g_cm3 >= 0.80f);
        CHECK(mat.density_g_cm3 <= 2.20f);
    }
}

TEST_CASE("MATERIALS - drying temp is safely below the material's softening point",
          "[filament][database][invariant]") {
    // A dryer set at or near the extrusion temperature deforms the spool and
    // welds the coil. Every hygroscopic row today clears nozzle_min by at least
    // 135 °C (tightest: CoPE 55 vs 190, PVA 45 vs 180), so a 100 °C margin is a
    // real constraint with headroom, not a rubber stamp.
    for (const auto& mat : MATERIALS) {
        if (mat.dry_temp_c == 0)
            continue;
        INFO("material: " << mat.name << " dry " << mat.dry_temp_c << " nozzle_min "
                          << mat.nozzle_min);
        CHECK(mat.dry_temp_c <= mat.nozzle_min - 100);
        // Absolute band: consumer filament dryers top out around 90 °C; the
        // 100 °C HIGH_TEMP rows assume an industrial oven.
        CHECK(mat.dry_temp_c >= 30);
        CHECK(mat.dry_temp_c <= 120);
        // Magnitude, not just presence: under an hour dries nothing, and over
        // 24 h is a typo (720 min / 12 h is the current maximum, for HIGH_TEMP).
        CHECK(mat.dry_time_min >= 60);
        CHECK(mat.dry_time_min <= 1440);
        CHECK(mat.needs_drying());
    }
}

// ============================================================================
// Layer A — structural / relational
// ============================================================================

TEST_CASE("MATERIALS - every compat group is served by a drying preset or is wholly dry",
          "[filament][database][invariant][drying]") {
    // get_drying_presets_by_group() SKIPS materials with dry_temp_c == 0. A group
    // whose members are all non-hygroscopic therefore has no preset at all — that
    // is correct, not a bug, but it must stay a deliberate short list. A group
    // silently dropping out of the dryer UI because someone zeroed its last
    // dry_temp_c is exactly the failure this pins.
    static const std::set<std::string> INTENTIONALLY_NO_PRESET = {
        "PE",  // polyethylene: negligible moisture uptake
        "EVA", // low Vicat point, drying would deform it
    };

    std::map<std::string, bool> group_has_hygroscopic;
    for (const auto& mat : MATERIALS) {
        REQUIRE(mat.compat_group != nullptr);
        auto& flag = group_has_hygroscopic[mat.compat_group];
        flag = flag || (mat.dry_temp_c > 0);
    }

    std::set<std::string> preset_names;
    for (const auto& p : get_drying_presets_by_group())
        preset_names.insert(p.name);

    for (const auto& [group, hygroscopic] : group_has_hygroscopic) {
        INFO("compat group: " << group);
        if (hygroscopic) {
            CHECK(preset_names.count(group) == 1);
        } else {
            // Not servable — assert it is one of the known-dry groups so a new
            // all-dry group cannot appear unnoticed.
            CHECK(INTENTIONALLY_NO_PRESET.count(group) == 1);
        }
    }

    // ...and no preset may reference a group that has no members at all.
    for (const auto& name : preset_names) {
        INFO("preset group: " << name);
        CHECK(group_has_hygroscopic.count(name) == 1);
    }
}

TEST_CASE("MATERIALS - a group's drying preset never under-dries any of its members",
          "[filament][database][invariant][drying]") {
    // get_drying_presets_by_group() emits ONE preset per compat group. It used to
    // take the FIRST hygroscopic row's values, which silently under-dried 8
    // materials (PET/PET-CF/PET-GF wanted 65 °C but got PETG's 55; PA66/PA6-CF/
    // PPA/PPA-CF/PPA-GF wanted 80 °C but got PA's 70). It now takes the group MAX.
    //
    // Under-drying is the harm; over-drying inside a compat group is not, because
    // group members are chemically interchangeable by construction. So this
    // asserts the preset is >= every member (never under-dries) AND is exactly the
    // max (never over-dries beyond what some member actually asked for — which
    // would mean someone hardcoded a value instead of deriving it).
    std::map<std::string, std::pair<int, int>> group_max; // group -> {temp, time}
    for (const auto& mat : MATERIALS) {
        if (mat.dry_temp_c == 0)
            continue;
        auto& mx = group_max[mat.compat_group];
        mx.first = std::max(mx.first, mat.dry_temp_c);
        mx.second = std::max(mx.second, mat.dry_time_min);
    }

    // Qualify: ams_types.h declares an unrelated ::DryingPreset (the AMS dryer UI
    // struct), which this file now sees via the ams_backend_* includes.
    std::map<std::string, filament::DryingPreset> presets;
    for (const auto& p : get_drying_presets_by_group())
        presets.emplace(p.name, p);

    for (const auto& mat : MATERIALS) {
        if (mat.dry_temp_c == 0)
            continue;
        INFO("material: " << mat.name << " (group " << mat.compat_group << ")");
        auto it = presets.find(mat.compat_group);
        REQUIRE(it != presets.end());
        // Never under-dry a member.
        CHECK(it->second.temp_c >= mat.dry_temp_c);
        CHECK(it->second.time_min >= mat.dry_time_min);
    }

    for (const auto& [group, mx] : group_max) {
        INFO("compat group: " << group);
        auto it = presets.find(group);
        REQUIRE(it != presets.end());
        // ...and never invent a value no member asked for.
        CHECK(it->second.temp_c == mx.first);
        CHECK(it->second.time_min == mx.second);
    }
}

TEST_CASE("MATERIALS - a group's drying preset is safe for the LOWEST-melting member",
          "[filament][database][invariant][drying]") {
    // The counterweight to taking the group max. Raising a group's preset to cover
    // its most demanding member is only safe if the result still clears the
    // softening point of the group's most delicate member — otherwise the dryer
    // deforms the spool and welds the coil.
    //
    // The per-material version of this check (dry_temp_c <= nozzle_min - 100) is
    // above; this is the group-wide version, and it is what makes the max-taking
    // in get_drying_presets_by_group() defensible rather than merely convenient.
    //
    // Tightest margins today are CoPE (preset 55 °C vs group-lowest nozzle_min
    // 190 -> 35 °C of slack) and PLA (45 °C vs PVA's 180 -> 35 °C). Raising any
    // member's dry_temp_c enough to eat that slack fails here, naming the group.
    std::map<std::string, int> group_min_nozzle;
    for (const auto& mat : MATERIALS) {
        auto it = group_min_nozzle.find(mat.compat_group);
        if (it == group_min_nozzle.end())
            group_min_nozzle[mat.compat_group] = mat.nozzle_min;
        else
            it->second = std::min(it->second, mat.nozzle_min);
    }

    for (const auto& preset : get_drying_presets_by_group()) {
        INFO("preset: " << preset.name << " at " << preset.temp_c
                        << " C, group's lowest nozzle_min " << group_min_nozzle[preset.name]);
        REQUIRE(group_min_nozzle.count(preset.name) == 1);
        CHECK(preset.temp_c <= group_min_nozzle[preset.name] - 100);
    }
}

TEST_CASE("drying data has exactly ONE source, and every consumer derives from it",
          "[filament][database][invariant][drying]") {
    // THE anti-regression gate for this whole file.
    //
    // There used to be THREE independent drying opinions:
    //   (a) MATERIALS[] dry_temp_c / dry_time_min          <- the real one
    //   (b) get_drying_presets_by_group()                  <- first-member, under-dried 8
    //   (c) get_comfort_range()'s own hardcoded 10-row list <- contradicted (a) outright:
    //                                                          PLA 55 vs 45, PETG 65 vs 55,
    //                                                          ABS 80 vs 60
    // (b) and (c) are now both derived from (a). This test fails if anyone
    // reintroduces an independent opinion in either place, because the moment a
    // hardcoded number reappears it stops matching the derivation below.
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name << " (group " << mat.compat_group << ")");
        const auto comfort = get_comfort_range(mat.name);
        REQUIRE(comfort.has_value());

        const auto preset = get_drying_preset_for_material(mat.name);
        if (preset.has_value()) {
            // (c) must report exactly what (b) offers, which is exactly what (a) says.
            CHECK(comfort->dry_temp_c == preset->temp_c);
            CHECK(comfort->dry_time_hours == preset->time_min / 60);
            // ...and (b) must cover this material's own (a) values.
            CHECK(preset->temp_c >= mat.dry_temp_c);
            CHECK(preset->time_min >= mat.dry_time_min);
        } else {
            // No preset only happens when the whole group is non-hygroscopic.
            CHECK(mat.dry_temp_c == 0);
            CHECK(comfort->dry_temp_c == 0);
            CHECK(comfort->dry_time_hours == 0);
        }
    }

    // Aliases must not open a side door to different drying numbers.
    for (const auto& alias : MATERIAL_ALIASES) {
        INFO("alias: " << alias.alias << " -> " << alias.canonical);
        const auto via_alias = get_comfort_range(alias.alias);
        const auto via_canonical = get_comfort_range(alias.canonical);
        REQUIRE(via_alias.has_value());
        REQUIRE(via_canonical.has_value());
        CHECK(via_alias->dry_temp_c == via_canonical->dry_temp_c);
        CHECK(via_alias->dry_time_hours == via_canonical->dry_time_hours);
        CHECK(via_alias->max_humidity_good == via_canonical->max_humidity_good);
    }
}

TEST_CASE("get_categories returns exactly the distinct categories in MATERIALS",
          "[filament][database][invariant]") {
    // NOTE: get_categories() / get_materials_by_category() are currently DEAD
    // CODE — nothing in src/ calls them. They are a documented seam for future
    // grouped picker headings, so they are tested to stay correct rather than
    // left to rot silently.
    std::set<std::string> expected;
    for (const auto& mat : MATERIALS) {
        REQUIRE(mat.category != nullptr);
        CHECK(std::string_view(mat.category) != "");
        expected.insert(mat.category);
    }

    auto actual_vec = get_categories();
    std::set<std::string> actual(actual_vec.begin(), actual_vec.end());

    INFO("expected: " << join(expected));
    INFO("actual:   " << join(actual));
    CHECK(actual == expected);
    // No duplicates: the vector and the deduped set must be the same size.
    CHECK(actual_vec.size() == actual.size());
}

TEST_CASE("get_materials_by_category partitions MATERIALS exactly",
          "[filament][database][invariant]") {
    // Every material lands in exactly one bucket: none dropped, none counted
    // twice. A stray whitespace or case difference in a category string would
    // orphan a material from a grouped picker without any other symptom.
    std::map<std::string, int> seen;
    size_t total = 0;

    for (const auto* cat : get_categories()) {
        auto bucket = get_materials_by_category(cat);
        INFO("category: " << cat);
        CHECK_FALSE(bucket.empty());
        total += bucket.size();
        for (const auto& mat : bucket) {
            CHECK(std::string_view(mat.category) == std::string_view(cat));
            seen[mat.name]++;
        }
    }

    CHECK(total == MATERIAL_COUNT);
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name);
        CHECK(seen[mat.name] == 1);
    }

    // An unknown category yields nothing rather than everything.
    CHECK(get_materials_by_category("NoSuchCategory").empty());
    CHECK(get_materials_by_category("").empty());
}

TEST_CASE("resolve_alias is idempotent", "[filament][database][invariant][alias]") {
    // Resolving an already-resolved name must be a no-op, otherwise an alias
    // chain (A -> B where B is itself an alias) would resolve differently
    // depending on how many times it was passed through.
    for (const auto& alias : MATERIAL_ALIASES) {
        INFO("alias: " << alias.alias << " -> " << alias.canonical);
        auto once = resolve_alias(alias.alias);
        auto twice = resolve_alias(once);
        CHECK(once == twice);
        CHECK(once == std::string_view(alias.canonical));
    }
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name);
        CHECK(resolve_alias(mat.name) == std::string_view(mat.name));
    }
}

TEST_CASE("find_material is case-insensitive for every material and alias",
          "[filament][database][invariant]") {
    // Catalog `type` strings arrive in whatever case the source used ("SILK" from
    // the AD5X firmware whitelist, "PLA" from Orca). A row that only matches in
    // its declared case would strand every product that spells it differently.
    for (const auto& mat : MATERIALS) {
        const std::string name = mat.name;
        std::string up = name, lo = name;
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);

        INFO("material: " << name);
        for (const auto& variant : {name, up, lo}) {
            INFO("  variant: " << variant);
            auto found = find_material(variant);
            REQUIRE(found.has_value());
            CHECK(std::string_view(found->name) == std::string_view(mat.name));
        }
    }

    for (const auto& alias : MATERIAL_ALIASES) {
        std::string up = alias.alias, lo = lower(alias.alias);
        std::transform(up.begin(), up.end(), up.begin(), ::toupper);

        INFO("alias: " << alias.alias);
        for (const auto& variant : {std::string(alias.alias), up, lo}) {
            INFO("  variant: " << variant);
            auto found = find_material(variant);
            REQUIRE(found.has_value());
            CHECK(std::string_view(found->name) == std::string_view(alias.canonical));
        }
    }
}

TEST_CASE("find_material agrees with resolve_alias", "[filament][database][invariant][alias]") {
    // find_material() runs resolve_alias() first. The two must never disagree, or
    // a caller that pre-resolves gets a different material than one that does not.
    for (const auto& alias : MATERIAL_ALIASES) {
        INFO("alias: " << alias.alias);
        auto via_alias = find_material(alias.alias);
        auto via_canonical = find_material(resolve_alias(alias.alias));
        REQUIRE(via_alias.has_value());
        REQUIRE(via_canonical.has_value());
        CHECK(std::string_view(via_alias->name) == std::string_view(via_canonical->name));
        CHECK(via_alias->nozzle_min == via_canonical->nozzle_min);
        CHECK(via_alias->nozzle_max == via_canonical->nozzle_max);
        CHECK(via_alias->bed_temp == via_canonical->bed_temp);
        CHECK(std::string_view(via_alias->compat_group) ==
              std::string_view(via_canonical->compat_group));
    }
}

TEST_CASE("are_materials_compatible is reflexive and symmetric",
          "[filament][database][invariant][compat]") {
    // Endless spool asks this question in an arbitrary argument order. An
    // asymmetric answer means a swap is allowed in one direction and refused in
    // the other, which surfaces as an intermittent, unreproducible bug.
    for (const auto& a : MATERIALS) {
        INFO("material: " << a.name);
        CHECK(are_materials_compatible(a.name, a.name));
        for (const auto& b : MATERIALS) {
            const bool ab = are_materials_compatible(a.name, b.name);
            const bool ba = are_materials_compatible(b.name, a.name);
            const bool same_group =
                std::string_view(a.compat_group) == std::string_view(b.compat_group);
            INFO("  vs: " << b.name << " (" << a.compat_group << " / " << b.compat_group << ")");
            CHECK(ab == ba);
            // Equivalence-relation sanity: same group <=> compatible, since both
            // names are known here.
            CHECK(ab == same_group);
        }
    }
}

TEST_CASE("are_materials_compatible is DELIBERATELY permissive for unknown materials",
          "[filament][database][invariant][compat]") {
    // Pinning intent, not just behaviour. An unknown material name returns a null
    // compat group, and the function then returns TRUE — it permits the swap.
    //
    // This is the chosen tradeoff: a user-typed or vendor-specific material we do
    // not recognise must not be blocked from endless spool just because we lack a
    // row for it. Flipping this to a fail-closed default would silently disable
    // endless spool for every custom filament in the field, so any such change
    // must be a conscious decision that breaks this test first.
    CHECK(are_materials_compatible("NotAMaterial", "PLA"));
    CHECK(are_materials_compatible("PLA", "NotAMaterial"));
    CHECK(are_materials_compatible("NotAMaterial", "AlsoNotAMaterial"));
    CHECK(are_materials_compatible("", "PLA"));
    CHECK(are_materials_compatible("PLA", ""));
    // And the underlying reason: unknown => null group.
    CHECK(get_compatibility_group("NotAMaterial") == nullptr);
    // Contrast — two KNOWN materials in different groups are still refused, so
    // permissiveness is scoped to ignorance and does not leak into known data.
    CHECK_FALSE(are_materials_compatible("PLA", "ABS"));
}

TEST_CASE("MATERIALS - names survive JSON and Orca string matching",
          "[filament][database][invariant]") {
    // Material names are written into settings.json, matched against Orca's
    // filament type strings, and compared verbatim by AMS backends. A quote,
    // backslash, control character or stray edge whitespace breaks one of those
    // paths without breaking the build.
    for (const auto& mat : MATERIALS) {
        const std::string name = mat.name;
        INFO("material: [" << name << "]");
        REQUIRE_FALSE(name.empty()); // front()/back() below need a non-empty string
        CHECK(name.find('"') == std::string::npos);
        CHECK(name.find('\\') == std::string::npos);
        CHECK(name.find(',') == std::string::npos); // breaks CSV-ish gcode params
        CHECK(name.front() != ' ');
        CHECK(name.back() != ' ');
        for (unsigned char c : name) {
            INFO("  char: " << static_cast<int>(c));
            CHECK(c >= 0x20); // no control chars
            CHECK(c < 0x7F);  // pure ASCII; non-ASCII breaks byte-wise matching
        }
        // Double spaces are almost always an accidental paste artifact.
        CHECK(name.find("  ") == std::string::npos);
    }
}

// ============================================================================
// Cross-consumer guards — independent hardcoded lists that drift apart
// ============================================================================

#if HELIX_HAS_IFS
TEST_CASE("AD5X firmware material whitelist all resolves", "[filament][database][invariant]") {
    // Derived from AmsBackendAd5xIfs::STOCK_WHITELIST — the same array
    // get_supported_materials() seeds from, so this cannot drift from the thing it
    // guards. The stock AD5X firmware rejects anything outside the list, so these
    // are the only types that printer's picker can offer. If one stops resolving in
    // Layer A it inherits a 0 °C bed and no drying data, and the picker breaks.
    for (const auto* type : AmsBackendAd5xIfs::STOCK_WHITELIST) {
        INFO("AD5X whitelist type: " << type);
        auto mat = find_material(type);
        REQUIRE(mat.has_value());
        CHECK(mat->nozzle_min > 0);
        CHECK(mat->bed_temp > 0);
        CHECK(mat->density_g_cm3 > 0.0f);
        CHECK(std::string_view(mat->compat_group) != "");
    }
}
#endif // HELIX_HAS_IFS

TEST_CASE("DEFAULT_PRESET_MATERIALS all resolve", "[filament][database][invariant]") {
    // include/material_settings_manager.h — the four types the settings UI seeds
    // temperature-override presets for.
    for (const auto* name : helix::DEFAULT_PRESET_MATERIALS) {
        INFO("preset material: " << name);
        auto mat = find_material(name);
        REQUIRE(mat.has_value());
        CHECK(mat->nozzle_min > 0);
        CHECK(mat->nozzle_max > mat->nozzle_min);
        CHECK(mat->bed_temp > 0);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "every mock-backend fixture material resolves",
                 "[filament][database][invariant][mock]") {
    // ~100 material strings live as fixture CONTENT inside the mock backends
    // (ams_backend_mock.cpp and friends). We deliberately do NOT consolidate them —
    // varied fixtures are the point. But a typo there ("PTEG", "PLA_CF") produces a
    // slot that silently inherits a 0 °C bed and no drying data, and every screenshot
    // and demo built on that fixture is quietly wrong. This walks each scripted
    // configuration the mock can be put into and asserts the database resolves what
    // the mock emits, aliases included.
    //
    // If a fixture string legitimately should not resolve, name it in
    // NON_RESOLVING_FIXTURE_MATERIALS below with a reason — do not weaken the check.
    static const std::set<std::string> NON_RESOLVING_FIXTURE_MATERIALS = {
        // (empty) — every mock fixture material currently resolves.
    };

    // Each entry drives the mock into one scripted configuration. Adding a new
    // mock mode? Add it here so its fixture strings are covered too.
    using ModeSetter = std::function<void(AmsBackendMock&)>;
    const std::vector<std::pair<const char*, ModeSetter>> MODES = {
        {"default", [](AmsBackendMock&) {}},
        {"tool_changer", [](AmsBackendMock& b) { b.set_tool_changer_mode(true); }},
        {"afc", [](AmsBackendMock& b) { b.set_afc_mode(true); }},
        {"multi_unit", [](AmsBackendMock& b) { b.set_multi_unit_mode(true); }},
        {"mixed_topology", [](AmsBackendMock& b) { b.set_mixed_topology_mode(true); }},
        {"vivid_mixed", [](AmsBackendMock& b) { b.set_vivid_mixed_mode(true); }},
        {"htlf_toolchanger", [](AmsBackendMock& b) { b.set_htlf_toolchanger_mode(true); }},
        {"ifs", [](AmsBackendMock& b) { b.set_ifs_mode(true); }},
        {"snapmaker", [](AmsBackendMock& b) { b.set_snapmaker_mode(true); }},
    };

    size_t checked = 0;
    for (const auto& [mode_name, apply_mode] : MODES) {
        AmsBackendMock backend;
        apply_mode(backend);
        backend.start();

        const auto info = backend.get_system_info();
        for (int i = 0; i < info.total_slots; ++i) {
            const auto slot = backend.get_slot_info(i);
            if (slot.material.empty()) {
                continue; // empty slot — nothing to resolve
            }
            if (NON_RESOLVING_FIXTURE_MATERIALS.count(slot.material) > 0) {
                continue;
            }
            INFO("mock mode: " << mode_name << " slot " << i << " material: " << slot.material);
            CHECK(find_material(slot.material).has_value());
            ++checked;
        }
    }

    // Guard the guard: if the mock stops producing slots (API drift, start() failing
    // silently), the loop above would vacuously pass while checking nothing.
    INFO("materials checked across " << MODES.size() << " mock configurations");
    CHECK(checked >= 20);
}

TEST_CASE("get_comfort_range covers EVERY material, and only real materials",
          "[filament][database][invariant]") {
    // get_comfort_range() used to carry its own hardcoded 10-name list, which
    // covered 10 of 66 materials and disagreed with MATERIALS on drying temps.
    // It is now derived: humidity from the material's compat group, drying from
    // get_drying_presets_by_group(). Coverage is therefore total by construction,
    // and this asserts that construction actually holds — a compat group with no
    // GROUP_HUMIDITY_RANGES row would fall into the fail-closed nullopt branch and
    // silently drop that material off the AMS humidity indicator.
    for (const auto& mat : MATERIALS) {
        INFO("material: " << mat.name << " (group " << mat.compat_group << ")");
        const auto range = get_comfort_range(mat.name);
        REQUIRE(range.has_value());
        CHECK(std::string_view(range->material) == std::string_view(mat.name));
        CHECK(range->max_humidity_good > 0.0f);
        CHECK(range->max_humidity_good < range->max_humidity_warn);
        CHECK(range->max_humidity_warn <= 100.0f);
        // Drying is 0/0 exactly when the material is non-hygroscopic.
        if (mat.dry_temp_c == 0 && !get_drying_preset_for_material(mat.name).has_value()) {
            CHECK(range->dry_temp_c == 0);
        } else {
            CHECK(range->dry_temp_c > 0);
            CHECK(range->dry_time_hours > 0);
        }
    }

    // Aliases resolve; unknown names do not.
    for (const auto& alias : MATERIAL_ALIASES) {
        INFO("alias: " << alias.alias);
        CHECK(get_comfort_range(alias.alias).has_value());
    }
    CHECK_FALSE(get_comfort_range("NoSuchMaterial").has_value());
    CHECK_FALSE(get_comfort_range("").has_value());

    // Pin the user-visible thresholds that the AMS indicator colour-codes against,
    // so a careless edit to GROUP_HUMIDITY_RANGES shows up as a test failure rather
    // than as a green badge on a wet spool.
    CHECK(get_comfort_range("PLA")->max_humidity_good == Catch::Approx(50.0f));
    CHECK(get_comfort_range("PETG")->max_humidity_good == Catch::Approx(40.0f));
    CHECK(get_comfort_range("ABS")->max_humidity_good == Catch::Approx(35.0f));
    CHECK(get_comfort_range("PA")->max_humidity_good == Catch::Approx(20.0f));
    CHECK(get_comfort_range("Nylon")->max_humidity_good == Catch::Approx(20.0f)); // via alias
}

TEST_CASE("MATERIAL_HUMIDITY_OVERRIDES stay small, real, and earned",
          "[filament][database][invariant]") {
    // The override table exists because a few materials are genuinely unlike their
    // compat group in moisture sensitivity (PVA/BVOH dissolve in humid air but
    // group with PLA; HIPS is styrenic but groups with ABS/ASA). That is a narrow
    // licence, and this keeps it narrow: every override must name a real material
    // AND actually differ from its group, so an entry that merely restates the
    // group value — or one whose group later moved to match it — gets deleted
    // instead of accumulating into the parallel list this replaced.
    CHECK(std::size(MATERIAL_HUMIDITY_OVERRIDES) <= 6);

    for (const auto& ovr : MATERIAL_HUMIDITY_OVERRIDES) {
        INFO("override: " << ovr.group);
        auto mat = find_material(ovr.group);
        REQUIRE(mat.has_value());
        // Exact spelling, not just resolvable: the lookup compares verbatim.
        CHECK(std::string_view(mat->name) == std::string_view(ovr.group));
        CHECK(ovr.max_humidity_good < ovr.max_humidity_warn);

        const GroupHumidityRange* group_row = nullptr;
        for (const auto& gr : GROUP_HUMIDITY_RANGES) {
            if (std::string_view(gr.group) == std::string_view(mat->compat_group)) {
                group_row = &gr;
                break;
            }
        }
        REQUIRE(group_row != nullptr);
        INFO("  group " << mat->compat_group << " is " << group_row->max_humidity_good << "/"
                        << group_row->max_humidity_warn);
        CHECK((ovr.max_humidity_good != group_row->max_humidity_good ||
               ovr.max_humidity_warn != group_row->max_humidity_warn));
    }
}

TEST_CASE("GROUP_HUMIDITY_RANGES covers every compat group exactly once",
          "[filament][database][invariant]") {
    // The structural reason get_comfort_range() can be total. A new compat group
    // in MATERIALS[] with no humidity row fails HERE, naming the group, rather
    // than surfacing later as a material that mysteriously has no indicator.
    std::set<std::string> groups;
    for (const auto& mat : MATERIALS)
        groups.insert(mat.compat_group);

    std::set<std::string> rows;
    for (const auto& gr : GROUP_HUMIDITY_RANGES) {
        INFO("humidity row: " << gr.group);
        CHECK(gr.max_humidity_good < gr.max_humidity_warn);
        CHECK(gr.max_humidity_warn <= 100.0f);
        CHECK(rows.insert(gr.group).second); // no duplicate rows
    }

    INFO("groups: " << join(groups));
    INFO("rows:   " << join(rows));
    CHECK(rows == groups); // both directions: no gap, no orphan row
}

// ============================================================================
// Layer B — catalog integrity
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "catalog - shipped file loads a substantial catalog",
                 "[filament_data][invariant]") {
    // read_products() swallows parse errors and returns {} — a truncated or
    // corrupted assets/filaments.json therefore yields an EMPTY catalog with only
    // a spdlog warning. These floors turn that silent degradation into a test
    // failure. They are deliberately well below the shipped counts (344 products,
    // 42 types, 21 brands) so ordinary curation does not trip them.
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    INFO("product count: " << all.size());
    CHECK(all.size() >= 300);
    CHECK(cat.all_brands().size() >= 15);

    std::set<std::string> types;
    for (const auto* p : all)
        types.insert(p->type);
    INFO("distinct type count: " << types.size());
    CHECK(types.size() >= 35);
}

TEST_CASE_METHOD(HelixTestFixture, "catalog - malformed input yields an empty catalog, not garbage",
                 "[filament_data][invariant]") {
    // Pins the loader's failure mode so it stays a clean empty rather than a
    // partially-populated catalog. Paired with the floors above, an empty result
    // for the SHIPPED file is what fails the suite.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "helix_filament_invariants";
    fs::create_directories(dir);

    struct Case {
        const char* label;
        const char* body;
    };
    const Case cases[] = {
        {"truncated", "{\"filaments\": [{\"id\": \"a\", \"brand\": \"B\""},
        {"not json", "this is not json at all"},
        {"empty file", ""},
        // Valid JSON, wrong schema: "filaments" is an object, not an array.
        {"filaments not an array", "{\"filaments\": {\"id\": \"a\"}}"},
        // Valid JSON, key missing entirely.
        {"no filaments key", "{\"something_else\": []}"},
        // nlohmann gotcha (L087): a JSON null must not reach .value() unguarded.
        {"null document", "null"},
    };

    for (const auto& c : cases) {
        const auto path = dir / (std::string(c.label) + ".json");
        {
            std::ofstream out(path);
            out << c.body;
        }
        INFO("case: " << c.label << " path: " << path.string());
        auto cat = FilamentCatalog::load_from_file(path.string(), false, "");
        CHECK(cat.all_products().empty());
        CHECK(cat.resolve_id("a") == nullptr);
        fs::remove(path);
    }

    // A path that does not exist behaves the same way.
    auto missing =
        FilamentCatalog::load_from_file((dir / "does_not_exist.json").string(), false, "");
    CHECK(missing.all_products().empty());

    fs::remove_all(dir);
}

TEST_CASE_METHOD(HelixTestFixture, "catalog - identity fields are present and unique",
                 "[filament_data][invariant]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    // {brand, name} is what the picker actually shows the user. Two rows with the
    // same pair render as two identical, indistinguishable entries.
    //
    // This was allowlisted for Generic/PETG-CF, which shipped twice because the
    // importer's seed-merge key included `type` and Orca mislabels their Generic
    // PETG-CF profile as type "PETG" — so the correctly-typed seed entry missed
    // and got appended as `generic-petg-cf-2`. Fixed at the source (the merge key
    // dropped `type`), so there is no allowlist here any more and any duplicate
    // fails outright.
    std::map<std::pair<std::string, std::string>, int> pair_counts;
    std::set<std::string> ids;

    for (const auto* p : all) {
        INFO("product: " << p->id);
        // REQUIRE, not CHECK: front()/back() below are UB on an empty string.
        REQUIRE_FALSE(p->id.empty());
        REQUIRE_FALSE(p->brand.empty());
        REQUIRE_FALSE(p->name.empty());
        REQUIRE_FALSE(p->type.empty());
        // No edge whitespace — it survives into settings.json and breaks lookups.
        CHECK(p->id.front() != ' ');
        CHECK(p->id.back() != ' ');
        CHECK(p->brand.front() != ' ');
        CHECK(p->brand.back() != ' ');
        CHECK(p->name.front() != ' ');
        CHECK(p->name.back() != ' ');
        CHECK(p->type.front() != ' ');
        CHECK(p->type.back() != ' ');
        CHECK(ids.insert(p->id).second); // ids are the merge key for the overlay
        pair_counts[{p->brand, p->name}]++;
    }

    for (const auto& [key, count] : pair_counts) {
        INFO("duplicate {brand, name}: " << key.first << " / " << key.second << " x" << count);
        CHECK(count == 1);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "catalog - every effective field is fully resolved",
                 "[filament_data][invariant]") {
    // to_effective() falls through to zero for EVERY inherited field when the type
    // does not resolve — not just the nozzle range. This walks all of them on all
    // products, which is the check that would have caught generic-pet's 0 °C bed.
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    // A zero-width nozzle range carries no information: the picker renders
    // "200-200 C" and any range slider has zero travel.
    //
    // This was allowlisted for "generic-eva", where the importer wrote a profile's
    // single nozzle temperature (200) into BOTH keys. Fixed at the source
    // (build_product() now discards a degenerate Orca range and falls back to the
    // type range, and _assert_no_degenerate_ranges() refuses to emit a catalog
    // containing one from ANY source), so this is now an unconditional gate.
    for (const auto* p : all) {
        INFO("product: " << p->id << " (type " << p->type << ")");
        REQUIRE(find_material(p->type).has_value());

        CHECK(p->nozzle_min > 0);
        CHECK(p->nozzle_max > 0);
        CHECK(p->nozzle_min < p->nozzle_max);
        CHECK(p->nozzle_recommended > 0);
        CHECK(p->nozzle_min <= p->nozzle_recommended);
        CHECK(p->nozzle_recommended <= p->nozzle_max);
        CHECK(p->nozzle_min >= NOZZLE_FLOOR);
        CHECK(p->nozzle_max <= NOZZLE_CEIL);

        CHECK(p->bed_temp >= 20);
        CHECK(p->bed_temp <= 160);

        CHECK(p->density_g_cm3 >= 0.80f);
        CHECK(p->density_g_cm3 <= 2.20f);

        CHECK_FALSE(p->compat_group.empty());

        // chamber/dry are inherited wholesale from the type, so they must match
        // the type row exactly — a mismatch means to_effective() grew a bug.
        auto base = find_material(p->type);
        CHECK(p->chamber_temp_c == base->chamber_temp_c);
        CHECK(p->dry_temp_c == base->dry_temp_c);
        CHECK(p->dry_time_min == base->dry_time_min);
        CHECK(p->compat_group == std::string(base->compat_group));
        // Product-level drying pairing, same rule as Layer A.
        if (p->dry_temp_c == 0)
            CHECK(p->dry_time_min == 0);
        else
            CHECK(p->dry_time_min > 0);

        // Code values must be non-empty, or resolve_code() indexes on "".
        for (const auto& [scheme, code] : p->codes) {
            INFO("  code scheme: " << scheme);
            CHECK_FALSE(scheme.empty());
            CHECK_FALSE(code.empty());
        }
    }
}

TEST_CASE_METHOD(HelixTestFixture, "catalog - product temps do not wildly contradict their type",
                 "[filament_data][invariant]") {
    // A brand legitimately diverges from the generic type — LW-PLA foams at
    // 260-270 under type "PLA", Polymaker's Fiberon nylons use a 40 °C bed against
    // a type default of 80-90. So this is a WIDE envelope (50 °C) aimed at units
    // errors and wrong-`type` fields, not at second-guessing vendor data.
    //
    // The tighter divergences are surfaced as a WARN listing below so they stay
    // visible without gating the build.
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    std::set<std::string> notable;

    for (const auto* p : all) {
        auto base = find_material(p->type);
        REQUIRE(base.has_value());
        INFO("product: " << p->id << " (type " << p->type << ")"
                         << " nozzle " << p->nozzle_min << "-" << p->nozzle_max << " vs type "
                         << base->nozzle_min << "-" << base->nozzle_max << ", bed " << p->bed_temp
                         << " vs type " << base->bed_temp);

        CHECK(p->nozzle_min >= base->nozzle_min - TYPE_DIVERGENCE_TOLERANCE);
        CHECK(p->nozzle_max <= base->nozzle_max + TYPE_DIVERGENCE_TOLERANCE);
        CHECK(p->bed_temp >= base->bed_temp - TYPE_DIVERGENCE_TOLERANCE);
        CHECK(p->bed_temp <= base->bed_temp + TYPE_DIVERGENCE_TOLERANCE);

        if (p->nozzle_min < base->nozzle_min - 25 || p->nozzle_max > base->nozzle_max + 25 ||
            std::abs(p->bed_temp - base->bed_temp) > 25) {
            notable.insert(p->id + " (" + p->type + ")");
        }
    }

    WARN("products diverging >25 C from their type's range: " << join(notable));
}

TEST_CASE_METHOD(HelixTestFixture, "two-layer closure - every material type is reachable or is",
                 "[filament_data][invariant][closure]") {
    // ============ THE structural guard for two-layer drift ============
    //
    // Layer A (MATERIALS[], types) and Layer B (filaments.json, products) drift
    // because nothing forces them to agree, and every gap so far has been found by
    // a human audit: ASA-GF/ABS-CF/PC-CF/PC-GF/PET-GF/PLA-GF were invisible in the
    // picker for months, then 25 more were found the same way. An audit that only
    // runs when someone thinks to run it is not a guard.
    //
    // This closes the loop in BOTH directions and gates on it:
    //
    //   every MATERIALS row  ->  has a catalog product  XOR  is RESOLUTION_ONLY
    //   every catalog type   ->  resolves to a MATERIALS row
    //
    // So a new type added to filament_database.h now FAILS the build until someone
    // decides, explicitly and in the data, which of the two it is — a Generic seed
    // entry in scripts/fixtures/cfs_seed.json, or a line in
    // RESOLUTION_ONLY_MATERIALS with the reason it is not user-selectable.
    // Neither choice can be made by accident, and neither can silently rot: a
    // RESOLUTION_ONLY entry that later gains a product fails here too.
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    // Reachability must resolve ALIASES: the catalog ships type "SILK" (the AD5X
    // firmware whitelist spelling), which is an alias of the "Silk PLA" row. A
    // raw string compare would report Silk PLA as unreachable and send the next
    // audit chasing a product that already exists.
    std::set<std::string> reachable;
    for (const auto* p : all) {
        auto base = find_material(p->type);
        INFO("catalog product " << p->id << " has type " << p->type);
        REQUIRE(base.has_value()); // no orphan types in Layer B
        reachable.insert(base->name);
    }

    std::set<std::string> unreachable;
    for (const auto& mat : MATERIALS) {
        if (reachable.count(mat.name) == 0)
            unreachable.insert(mat.name);
    }

    std::set<std::string> declared;
    for (const auto* m : RESOLUTION_ONLY_MATERIALS) {
        INFO("RESOLUTION_ONLY entry: " << m);
        // Must name a real row, spelled exactly as MATERIALS spells it.
        auto mat = find_material(m);
        REQUIRE(mat.has_value());
        CHECK(std::string_view(mat->name) == std::string_view(m));
        CHECK(declared.insert(m).second); // no duplicate declarations
    }

    INFO("unreachable: " << join(unreachable));
    INFO("declared resolution-only: " << join(declared));

    // Direction 1: nothing is invisible by accident.
    for (const auto& name : unreachable) {
        INFO("material with no catalog product: " << name);
        CHECK(is_resolution_only(name));
    }
    // Direction 2: nothing is declared resolution-only while a product exists.
    for (const auto& name : declared) {
        INFO("resolution-only material: " << name);
        CHECK(unreachable.count(name) == 1);
    }
    // Both at once, so the failure message shows the whole picture.
    CHECK(unreachable == declared);
    CHECK(declared.size() == RESOLUTION_ONLY_COUNT);
    CHECK(reachable.size() + declared.size() == MATERIAL_COUNT);
}
