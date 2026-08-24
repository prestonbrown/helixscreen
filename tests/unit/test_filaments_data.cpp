// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "filament_database.h"
#include "helix_test_fixture.h"

#include <set>

#include "../catch_amalgamated.hpp"

using helix::printer::FilamentCatalog;

TEST_CASE_METHOD(HelixTestFixture, "shipped filaments.json is well-formed", "[filament_data]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    std::set<std::string> ids;
    std::set<std::string> cfs_codes;
    for (const auto* p : all) {
        CHECK(ids.insert(p->id).second); // no duplicate ids
        CHECK(p->nozzle_min <= p->nozzle_recommended);
        CHECK(p->nozzle_recommended <= p->nozzle_max);
        auto it = p->codes.find("cfs");
        if (it != p->codes.end())
            CHECK(cfs_codes.insert(it->second).second); // no dup cfs codes
    }
}

// The old form of this check was a disjunction — a product's type could fail to
// resolve in filament_database.h as long as the product carried its own explicit
// nozzle range. That let 16 orphan types ship while silently inheriting ZERO bed
// temp, drying params, density and compat_group from the fallthrough in
// filament_catalog.cpp::to_effective(). Every catalog type must now resolve.
TEST_CASE_METHOD(HelixTestFixture, "every catalog type resolves in filament_database",
                 "[filament_data]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    std::set<std::string> unresolved;
    for (const auto* p : all) {
        if (!filament::find_material(p->type).has_value())
            unresolved.insert(p->type);
    }
    INFO("catalog types with no filament_database.h entry (or alias): " << [&] {
        std::string s;
        for (const auto& t : unresolved)
            s += t + " ";
        return s;
    }());
    CHECK(unresolved.empty());
}

// Resolution alone isn't enough: the fields a product inherits when it omits them
// must actually be populated, or the product ships with a 0 °C bed / 0 density.
TEST_CASE_METHOD(HelixTestFixture, "catalog products inherit non-zero physical defaults",
                 "[filament_data]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    for (const auto* p : cat.all_products()) {
        INFO("product: " << p->id << " (type " << p->type << ")");
        CHECK(p->nozzle_min > 0);
        CHECK(p->nozzle_max > 0);
        CHECK(p->bed_temp > 0);
        CHECK(p->density_g_cm3 > 0.0f);
        CHECK(!p->compat_group.empty());
    }
}

// The material picker builds its list from the catalog (Layer B), so a
// filament_database.h type with zero products is invisible in the UI no matter
// how correct its temperatures are. These six were unreachable — a user looking
// for ASA-GF had to settle for ASA-CF, a genuinely different material.
// Reverting the Generic products in assets/filaments.json fails this test.
TEST_CASE_METHOD(HelixTestFixture, "previously-unreachable material types have products",
                 "[filament_data]") {
    static const char* MUST_BE_SELECTABLE[] = {
        "ASA-GF", "ABS-CF", "PC-CF", "PC-GF", "PET-GF", "PLA-GF",
    };
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    for (const auto* type : MUST_BE_SELECTABLE) {
        INFO("type: " << type);
        CHECK(!cat.products_for_type(type).empty());
    }
}

// CF and GF are different materials (stiffness, abrasiveness, print temps), so the
// picker must never collapse them. Guards against a future "simplification" that
// aliases one onto the other.
TEST_CASE_METHOD(HelixTestFixture, "CF and GF variants stay distinct", "[filament_data]") {
    static const char* kPairs[][2] = {
        {"ASA-CF", "ASA-GF"},   {"ABS-CF", "ABS-GF"}, {"PC-CF", "PC-GF"}, {"PET-CF", "PET-GF"},
        {"PETG-CF", "PETG-GF"}, {"PA-CF", "PA-GF"},   {"PP-CF", "PP-GF"}, {"PPA-CF", "PPA-GF"},
    };
    for (const auto& pair : kPairs) {
        INFO("pair: " << pair[0] << " vs " << pair[1]);
        auto cf = filament::find_material(pair[0]);
        auto gf = filament::find_material(pair[1]);
        REQUIRE(cf.has_value());
        REQUIRE(gf.has_value());
        // Distinct database rows, not one aliased onto the other.
        CHECK(std::string(cf->name) != std::string(gf->name));
        CHECK(std::string(cf->name) == pair[0]);
        CHECK(std::string(gf->name) == pair[1]);
    }
}
