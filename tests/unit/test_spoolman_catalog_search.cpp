// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

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
