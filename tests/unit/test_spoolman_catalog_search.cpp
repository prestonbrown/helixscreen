// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_error.h"
#include "spoolman_catalog_search.h"

#include "../catch_amalgamated.hpp"

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

TEST_CASE("find_matching_filament is the one Spoolman filament match", "[spoolman_db]") {
    using helix::spoolman::find_matching_filament;
    using helix::spoolman::normalize_color_hex;

    FilamentInfo red;
    red.id = 4;
    red.material = "PLA";
    red.color_hex = "ff0000";
    FilamentInfo clear;
    clear.id = 9;
    clear.material = "PETG";
    clear.color_hex = "FFFFFF33";
    const std::vector<FilamentInfo> filaments{red, clear};

    SECTION("colour compares without case or '#'") {
        const FilamentInfo* f = find_matching_filament(filaments, "PLA", "#FF0000");
        REQUIRE(f != nullptr);
        CHECK(f->id == 4);
    }

    SECTION("an alpha channel is part of the colour") {
        const FilamentInfo* f = find_matching_filament(filaments, "PETG", "ffffff33");
        REQUIRE(f != nullptr);
        CHECK(f->id == 9);
        CHECK(find_matching_filament(filaments, "PETG", "FFFFFF") == nullptr);
    }

    SECTION("material must match") {
        CHECK(find_matching_filament(filaments, "PETG", "FF0000") == nullptr);
    }

    SECTION("no colour matches nothing") {
        CHECK(find_matching_filament(filaments, "PLA", "") == nullptr);
        CHECK(normalize_color_hex("red").empty());
        CHECK(normalize_color_hex("12345").empty());
    }
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

TEST_CASE("material_for_type records the material the slicer and AFC understand",
          "[spoolman_db][filament_naming]") {
    using helix::spoolman::material_for_type;
    CHECK(material_for_type("ABS+") == "ABS");
    CHECK(material_for_type("PLA+") == "PLA");
    CHECK(material_for_type("Silk PLA") == "PLA");
    CHECK(material_for_type("PLA-CF") == "PLA-CF");
    CHECK(material_for_type(" PETG ") == "PETG");
    CHECK(material_for_type("").empty());
}

TEST_CASE("new_filament_name puts the product before the colour",
          "[spoolman_db][filament_naming]") {
    using helix::spoolman::new_filament_name;
    CHECK(new_filament_name("ABS+", "Deep Gray") == "ABS+ Deep Gray");
    CHECK(new_filament_name("Silk PLA", " Gold ") == "Silk PLA Gold");
    CHECK(new_filament_name("PETG", "") == "PETG");
    CHECK(new_filament_name("", "Red") == "Red");
    CHECK(new_filament_name(" ", " ").empty());
}

TEST_CASE("temperature_for_range is the midpoint, or the one end set",
          "[spoolman_db][filament_naming]") {
    using helix::spoolman::temperature_for_range;
    CHECK(temperature_for_range(240, 270) == 255);
    CHECK(temperature_for_range(0, 230) == 230);
    CHECK(temperature_for_range(210, 0) == 210);
    CHECK(temperature_for_range(0, 0) == 0);
}

TEST_CASE("find_similar_filament offers the nearest colour of the same material",
          "[spoolman_db][filament_naming]") {
    using helix::spoolman::find_similar_filament;
    auto make = [](int id, const char* name, const char* material, const char* hex) {
        FilamentInfo f;
        f.id = id;
        f.filament_name = name;
        f.material = material;
        f.color_hex = hex;
        return f;
    };
    // A vendor's records as one Spoolman holds them.
    const std::vector<FilamentInfo> filaments{
        make(72, "Black ABS+HS", "ABS", "000000"),
        make(73, "Grey ABS+HS", "ABS", "636363"),
        make(20, "Silver ABS+HS", "ABS", "B6B6B6"),
        make(30, "Grey PETG", "PETG", "4A4A4A"),
    };

    SECTION("a nearby grey of the same material is offered over a farther one") {
        const FilamentInfo* f = find_similar_filament(filaments, "ABS", "#4A4A4A");
        REQUIRE(f != nullptr);
        CHECK(f->id == 73);
    }

    SECTION("another material is never offered, however close the colour") {
        CHECK(find_similar_filament(filaments, "PLA", "4A4A4A") == nullptr);
    }

    SECTION("a colour past the tolerance is not offered") {
        CHECK(find_similar_filament(filaments, "ABS", "FF0000") == nullptr);
    }

    SECTION("a record filed under the precise type still counts as its material") {
        const std::vector<FilamentInfo> legacy{make(117, "ABS+", "ABS+", "4A4A4A")};
        const FilamentInfo* f = find_similar_filament(legacy, "ABS", "4A4A4A");
        REQUIRE(f != nullptr);
        CHECK(f->id == 117);
    }

    SECTION("no colour offers nothing") {
        CHECK(find_similar_filament(filaments, "ABS", "") == nullptr);
    }
}
