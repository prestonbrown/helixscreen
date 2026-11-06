// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_variants.h"
#include "moonraker_error.h"
#include "spoolman_catalog_search.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::SpoolmanCatalogSearch;
using Availability = helix::SpoolmanCatalogSearch::Availability;

namespace {

struct CacheReset {
    CacheReset() {
        SpoolmanCatalogSearch::reset_cache();
    }
    ~CacheReset() {
        SpoolmanCatalogSearch::reset_cache();
    }
};

/// An error shaped as Moonraker sends it over JSON-RPC.
MoonrakerError moonraker_error(int code, const char* message) {
    return MoonrakerError::from_json_rpc({{"code", code}, {"message", message}},
                                         "server.spoolman.proxy");
}

/// Spoolman's 404 through the proxy (moonraker/common.py maps 404 to -32601).
MoonrakerError route_missing() {
    return moonraker_error(-32601, "Not Found");
}

} // namespace

TEST_CASE("SpoolmanCatalogSearch caches availability per connection", "[spoolman_db]") {
    CacheReset reset;

    CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Unknown);

    SECTION("a successful search makes it available on that connection only") {
        SpoolmanCatalogSearch::record_success(5);
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Available);
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Unknown);
    }

    SECTION("Spoolman's 404, relayed as -32601, makes it unavailable") {
        SpoolmanCatalogSearch::record_error(5, route_missing());
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Unavailable);
    }

    SECTION("a missing proxy method proves nothing about the route") {
        SpoolmanCatalogSearch::record_error(5, moonraker_error(-32601, "Method not found"));
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Unknown);
    }

    SECTION("any other error proves nothing about the route") {
        SpoolmanCatalogSearch::record_success(5);
        SpoolmanCatalogSearch::record_error(5, moonraker_error(500, "Internal Server Error"));
        CHECK(SpoolmanCatalogSearch::availability(5) == Availability::Available);

        SpoolmanCatalogSearch::record_error(7, MoonrakerError::connection_lost("x"));
        CHECK(SpoolmanCatalogSearch::availability(7) == Availability::Unknown);
    }

    SECTION("a new connection asks again") {
        SpoolmanCatalogSearch::record_error(5, route_missing());
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Unknown);
        SpoolmanCatalogSearch::record_success(6);
        CHECK(SpoolmanCatalogSearch::availability(6) == Availability::Available);
    }
}

TEST_CASE("SpoolmanCatalogSearch sends queries of two characters or more", "[spoolman_db]") {
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable(""));
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable("p"));
    CHECK_FALSE(SpoolmanCatalogSearch::is_searchable("  p  "));
    CHECK(SpoolmanCatalogSearch::is_searchable("po"));
}

TEST_CASE("SpoolmanCatalogSearch tickets go stale when a newer query starts", "[spoolman_db]") {
    SpoolmanCatalogSearch search;
    const uint64_t older = search.begin();
    CHECK(search.is_current(older));

    const uint64_t newer = search.begin();
    CHECK_FALSE(search.is_current(older));
    CHECK(search.is_current(newer));

    search.invalidate();
    CHECK_FALSE(search.is_current(newer));
}

TEST_CASE("find_catalog_filament matches one catalog product exactly", "[spoolman_db]") {
    using helix::spoolman::find_catalog_filament;

    auto filament = [](int id, int vendor, const char* name, const char* material, const char* hex,
                       const char* multi = "") {
        FilamentInfo f;
        f.id = id;
        f.vendor_id = vendor;
        f.filament_name = name;
        f.material = material;
        f.color_hex = hex;
        f.multi_color_hexes = multi;
        return f;
    };
    const std::vector<FilamentInfo> filaments{
        filament(1, 7, "PolyTerra PLA Black", "PLA", "1A1A1A"),
        filament(2, 7, "PolyLite PLA Black", "PLA", "1a1a1a"),
        filament(3, 7, "Silk PLA Rainbow", "PLA", "", "E53935,FFEB3B"),
        filament(4, 8, "PolyLite PLA Black", "PLA", "1A1A1A"),
    };

    SECTION("name, material and vendor all have to agree") {
        const FilamentInfo* f =
            find_catalog_filament(filaments, 7, " polylite pla black ", "pla", "#1A1A1A");
        REQUIRE(f != nullptr);
        CHECK(f->id == 2);
        CHECK(find_catalog_filament(filaments, 9, "PolyLite PLA Black", "PLA", "1A1A1A") ==
              nullptr);
        CHECK(find_catalog_filament(filaments, 7, "PolyLite PLA Black", "PETG", "1A1A1A") ==
              nullptr);
        CHECK(find_catalog_filament(filaments, 7, "PolyLite PLA Black", "PLA", "1A1A2E") ==
              nullptr);
    }

    SECTION("a multi-colour product matches its colour set, in any order") {
        const FilamentInfo* f =
            find_catalog_filament(filaments, 7, "Silk PLA Rainbow", "PLA", "ffeb3b, e53935");
        REQUIRE(f != nullptr);
        CHECK(f->id == 3);
    }

    SECTION("a multi-colour product never lands on a solid filament") {
        CHECK(find_catalog_filament(filaments, 7, "PolyLite PLA Black", "PLA", "1A1A1A,FFFFFF") ==
              nullptr);
        CHECK(find_catalog_filament(filaments, 7, "Silk PLA Rainbow", "PLA", "E53935") == nullptr);
    }
}

TEST_CASE("slicer_material is the material the slicer and AFC understand",
          "[spoolman_db][filament_naming]") {
    CHECK(filament::slicer_material("ABS+") == "ABS");
    CHECK(filament::slicer_material("PLA+") == "PLA");
    CHECK(filament::slicer_material("Silk PLA") == "PLA");
    CHECK(filament::slicer_material("PLA-CF") == "PLA-CF");
    CHECK(filament::slicer_material(" PETG ") == "PETG");
    CHECK(filament::slicer_material("").empty());
    // A filled grade the library lacks keeps its filler on the base polymer.
    CHECK(filament::slicer_material("PA12-CF") == "PA-CF");
    CHECK(filament::slicer_material("PA66-CF") == "PA-CF");
    CHECK(filament::slicer_material("PAHT-CF") == "PA-CF");
    CHECK(filament::slicer_material("PA6-GF") == "PA-GF");
    CHECK(filament::slicer_material("PA6-CF") == "PA6-CF");
}

TEST_CASE("find_similar_filament offers the nearest colour of the same material",
          "[spoolman_db][filament_naming]") {
    auto make = [](int id, const char* material, const char* hex) {
        FilamentInfo f;
        f.id = id;
        f.material = material;
        f.color_hex = hex;
        return f;
    };
    // A vendor's records as one Spoolman holds them, plus a legacy record
    // filed under its precise type.
    const std::vector<FilamentInfo> filaments{
        make(72, "ABS", "000000"),  make(73, "ABS", "636363"),   make(20, "ABS", "B6B6B6"),
        make(30, "PETG", "4A4A4A"), make(117, "ABS+", "505050"),
    };
    struct Row {
        const char* what;
        const char* material;
        const char* hex;
        int exclude;
        int offered; // 0 = nothing
    };
    const Row rows[] = {
        {"the nearest grey of the same material", "ABS", "#4A4A4A", 0, 117},
        {"a record filed under the precise type counts as its material", "ABS", "525252", 0, 117},
        {"the spool's own record is skipped", "ABS", "525252", 117, 73},
        {"another material, however close the colour", "PLA", "4A4A4A", 0, 0},
        {"a colour past the tolerance", "ABS", "FF0000", 0, 0},
        {"no colour", "ABS", "", 0, 0},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.what) {
            const FilamentInfo* f =
                helix::spoolman::find_similar_filament(filaments, r.material, r.hex, r.exclude);
            CHECK((f ? f->id : 0) == r.offered);
        }
    }
    // A string that is not a colour normalizes to none, so it offers nothing.
    CHECK(helix::spoolman::normalize_color_hex("red").empty());
    CHECK(helix::spoolman::normalize_color_hex("12345").empty());
}

TEST_CASE("filament_display_name names a record with no name by its material and colour",
          "[spoolman_db][similar_filament]") {
    FilamentInfo f;
    f.material = "ABS";
    f.filament_name = " ";
    CHECK(helix::spoolman::filament_display_name(f, "Gray") == "ABS Gray");
    f.filament_name = "Grey ABS+HS";
    CHECK(helix::spoolman::filament_display_name(f, "Gray") == "Grey ABS+HS");
}
