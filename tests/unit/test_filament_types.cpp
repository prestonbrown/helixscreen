// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The material-type table: shipped rows come from assets/filaments.json's
// `types`, and the user overlay's `types` patches or extends them.

#include "core/data_root_resolver.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "helix_test_fixture.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace fs = std::filesystem;
using helix::printer::FilamentCatalog;

namespace {

constexpr const char* kAsset = "assets/filaments.json";

struct TypesFixture : HelixTestFixture {
    fs::path dir;

    TypesFixture() {
        dir = fs::temp_directory_path() /
              ("helix-types-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
    }
    ~TypesFixture() override {
        filament::reload_materials();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string write(const std::string& name, const std::string& body) {
        auto p = (dir / name).string();
        std::ofstream(p) << body;
        return p;
    }
};

size_t asset_type_count() {
    std::ifstream f(kAsset);
    return nlohmann::json::parse(f)["types"].size();
}

} // namespace

TEST_CASE_METHOD(TypesFixture, "every shipped type row loads from the asset", "[filament][types]") {
    filament::load_materials_from(kAsset, "");
    const auto table = filament::materials();
    REQUIRE(table->size() == asset_type_count());
    REQUIRE(table->size() > 50);

    auto pla = filament::find_material("pla");
    REQUIRE(pla);
    CHECK(std::string(pla->name) == "PLA");
    CHECK(pla->nozzle_min == 190);
    CHECK(pla->nozzle_max == 220);
    CHECK(pla->bed_temp == 60);
    CHECK(pla->dry_temp_c == 45);
    CHECK(pla->density_g_cm3 == Catch::Approx(1.24f));
    CHECK(std::string(pla->compat_group) == "PLA");
    CHECK_FALSE(pla->user_defined);

    auto abs = filament::find_material("ABS");
    REQUIRE(abs);
    CHECK(abs->chamber_temp_c == 50);
    CHECK(std::string(abs->category) == "Engineering");
}

TEST_CASE_METHOD(TypesFixture, "a missing asset leaves an empty table, not a crash",
                 "[filament][types]") {
    filament::load_materials_from((dir / "nope.json").string(), "");
    CHECK(filament::materials()->empty());
    CHECK_FALSE(filament::find_material("PLA"));
    CHECK(filament::get_compatibility_group("PLA") == nullptr);
    CHECK(filament::are_materials_compatible("PLA", "ABS"));
}

TEST_CASE_METHOD(TypesFixture, "a corrupt asset leaves an empty table", "[filament][types]") {
    filament::load_materials_from(write("bad.json", "{\"types\": [ {\"name\": "), "");
    CHECK(filament::materials()->empty());
}

TEST_CASE_METHOD(TypesFixture, "an overlay entry patches only the fields it names",
                 "[filament][types]") {
    auto overlay = write("user.json", R"({"types": [{"name": "pla", "bed": 65, "chamber": 0}]})");
    filament::load_materials_from(kAsset, overlay);

    auto pla = filament::find_material("PLA");
    REQUIRE(pla);
    CHECK(pla->bed_temp == 65);
    CHECK(pla->nozzle_min == 190);
    CHECK(pla->nozzle_max == 220);
    CHECK(std::string(pla->name) == "PLA");
    CHECK_FALSE(pla->user_defined);
    CHECK(filament::materials()->size() == asset_type_count());
}

TEST_CASE_METHOD(TypesFixture, "an overlay entry with a new name defines a type",
                 "[filament][types]") {
    auto overlay = write("user.json", R"({"types": [
        {"name": "PEKK", "nozzle_min": 330, "nozzle_max": 360, "bed": 120}
    ]})");
    filament::load_materials_from(kAsset, overlay);

    auto pekk = filament::find_material("pekk");
    REQUIRE(pekk);
    CHECK(pekk->nozzle_min == 330);
    CHECK(pekk->nozzle_max == 360);
    CHECK(pekk->bed_temp == 120);
    CHECK(std::string(pekk->category) == "Custom");
    CHECK(pekk->user_defined);
    // Its own compat group: endless spool must not swap it with anything else.
    CHECK(std::string(pekk->compat_group) == "PEKK");
    CHECK_FALSE(filament::are_materials_compatible("PEKK", "PEEK"));
    CHECK(filament::materials()->size() == asset_type_count() + 1);
}

TEST_CASE_METHOD(TypesFixture, "a new type may name an existing compat group",
                 "[filament][types]") {
    auto overlay = write("user.json", R"({"types": [
        {"name": "Tough PLA", "nozzle_min": 205, "nozzle_max": 235, "bed": 60,
         "compat_group": "PLA"}
    ]})");
    filament::load_materials_from(kAsset, overlay);
    CHECK(filament::are_materials_compatible("Tough PLA", "PLA"));
}

TEST_CASE_METHOD(TypesFixture, "overlay type entries without a name are skipped",
                 "[filament][types]") {
    auto overlay = write("user.json", R"({"types": [{"bed": 90}, 7, {"name": ""}]})");
    filament::load_materials_from(kAsset, overlay);
    CHECK(filament::materials()->size() == asset_type_count());
    CHECK(filament::find_material("PLA")->bed_temp == 60);
}

TEST_CASE_METHOD(TypesFixture, "a bare-array overlay carries products and adds no types",
                 "[filament][types]") {
    auto overlay = write("user.json", R"([{"id": "x", "brand": "B", "name": "N", "type": "PLA"}])");
    filament::load_materials_from(kAsset, overlay);
    CHECK(filament::materials()->size() == asset_type_count());
}

TEST_CASE_METHOD(TypesFixture, "a name handed out survives a reload", "[filament][types]") {
    auto overlay = write(
        "user.json",
        R"({"types": [{"name": "PEKK", "nozzle_min": 330, "nozzle_max": 360, "compat_group": "PAEK"}]})");
    filament::load_materials_from(kAsset, overlay);
    const char* group = filament::get_compatibility_group("PEKK");
    REQUIRE(group != nullptr);

    filament::load_materials_from(kAsset, "");
    CHECK(std::string(group) == "PAEK");
    CHECK_FALSE(filament::find_material("PEKK"));
}

TEST_CASE_METHOD(TypesFixture, "catalog products inherit from a user-defined type",
                 "[filament][types][filament_catalog]") {
    auto overlay = write("user.json", R"({
        "types": [{"name": "PEKK", "nozzle_min": 330, "nozzle_max": 360, "bed": 120}],
        "filaments": [{"id": "acme-pekk", "brand": "Acme", "name": "PEKK", "type": "PEKK"}]
    })");
    filament::load_materials_from(kAsset, overlay);

    auto cat = FilamentCatalog::load_with_overlay(kAsset, overlay);
    const auto* p = cat.resolve_id("acme-pekk");
    REQUIRE(p != nullptr);
    CHECK(p->nozzle_min == 330);
    CHECK(p->nozzle_max == 360);
    CHECK(p->bed_temp == 120);
    CHECK(p->compat_group == "PEKK");
}

TEST_CASE_METHOD(TypesFixture, "catalog products inherit a patched shipped type",
                 "[filament][types][filament_catalog]") {
    auto overlay = write("user.json", R"({"types": [{"name": "ABS", "chamber": 60}]})");
    filament::load_materials_from(kAsset, overlay);

    auto cat = FilamentCatalog::load_with_overlay(kAsset, overlay);
    const auto* p = cat.resolve_id("creality-cr-abs");
    REQUIRE(p != nullptr);
    CHECK(p->chamber_temp_c == 60);
}

TEST_CASE_METHOD(TypesFixture, "saving types keeps the products of a bare-array overlay",
                 "[filament][types][filament_catalog]") {
    // The installer seeds the overlay as `[]`; a hand-edited one may be a
    // product list. A types save must not drop those products.
    auto overlay = write("user.json",
                         R"([{"id": "acme-pla", "brand": "Acme", "name": "PLA", "type": "PLA"}])");
    REQUIRE(FilamentCatalog::save_user_types_to({{{"name", "PLA"}, {"bed", 64}}}, overlay));

    auto products = FilamentCatalog::load_user_products_from(overlay);
    REQUIRE(products.size() == 1);
    CHECK(products[0]["id"] == "acme-pla");
    auto types = FilamentCatalog::load_user_types_from(overlay);
    REQUIRE(types.size() == 1);
    CHECK(types[0]["bed"] == 64);
}

TEST_CASE_METHOD(TypesFixture, "saving products keeps the overlay's types",
                 "[filament][types][filament_catalog]") {
    auto overlay = write("user.json", R"({"types": [{"name": "PLA", "bed": 64}]})");
    REQUIRE(FilamentCatalog::save_user_products_to(
        {{{"id", "acme-pla"}, {"brand", "Acme"}, {"name", "PLA"}, {"type", "PLA"}}}, overlay));
    CHECK(FilamentCatalog::load_user_types_from(overlay).size() == 1);
    CHECK(FilamentCatalog::load_user_products_from(overlay).size() == 1);
}

TEST_CASE_METHOD(TypesFixture, "a null in a type patch keeps the shipped value",
                 "[filament][types][hand_edit]") {
    // merge_patch reads null as "delete", which would zero the field.
    auto overlay = write("user.json", R"({"types": [{"name": "PLA", "nozzle_max": null}]})");
    filament::load_materials_from(kAsset, overlay);
    auto pla = filament::find_material("PLA");
    REQUIRE(pla);
    CHECK(pla->nozzle_max == 220);
    CHECK(pla->nozzle_recommended() == 205);
}

TEST_CASE_METHOD(TypesFixture, "a quoted number in a type patch keeps the shipped value",
                 "[filament][types][hand_edit]") {
    auto overlay = write("user.json", R"({"types": [{"name": "PLA", "nozzle_min": "205"}]})");
    filament::load_materials_from(kAsset, overlay);
    auto pla = filament::find_material("PLA");
    REQUIRE(pla);
    CHECK(pla->nozzle_min == 190);
    CHECK(pla->nozzle_recommended() == 205);
}

TEST_CASE_METHOD(TypesFixture, "a new type with no nozzle_max is skipped",
                 "[filament][types][hand_edit]") {
    // Kept, it would preheat to the midpoint of 200 and 0.
    auto overlay =
        write("user.json", R"({"types": [{"name": "MyPLA", "nozzle_min": 200, "bed": 60}]})");
    filament::load_materials_from(kAsset, overlay);
    CHECK_FALSE(filament::find_material("MyPLA"));
    CHECK(filament::materials()->size() == asset_type_count());
}

TEST_CASE_METHOD(TypesFixture, "a new type with a reversed nozzle range is skipped",
                 "[filament][types][hand_edit]") {
    auto overlay = write("user.json", R"({"types": [
        {"name": "Backwards", "nozzle_min": 260, "nozzle_max": 230, "bed": 60},
        {"name": "Fine", "nozzle_min": 230, "nozzle_max": 230, "bed": 60}
    ]})");
    filament::load_materials_from(kAsset, overlay);
    CHECK_FALSE(filament::find_material("Backwards"));
    CHECK(filament::find_material("Fine"));
}

TEST_CASE_METHOD(TypesFixture, "a new type with no nozzle_min is skipped",
                 "[filament][types][hand_edit]") {
    auto overlay = write("user.json", R"({"types": [{"name": "MaxOnly", "nozzle_max": 230}]})");
    filament::load_materials_from(kAsset, overlay);
    CHECK_FALSE(filament::find_material("MaxOnly"));
}

TEST_CASE_METHOD(TypesFixture,
                 "an empty overlay file reads as no overlay and saves without a backup",
                 "[filament][types][filament_catalog]") {
    const char* body = GENERATE("", "  \n");
    auto overlay = write("user.json", body);
    CHECK(FilamentCatalog::load_user_types_from(overlay).empty());
    CHECK(FilamentCatalog::load_user_products_from(overlay).empty());
    CHECK_FALSE(FilamentCatalog::overlay_file_is_corrupt(overlay));

    REQUIRE(FilamentCatalog::save_user_types_to({{{"name", "PLA"}, {"bed", 64}}}, overlay));
    CHECK_FALSE(fs::exists(overlay + ".bak"));
    CHECK(FilamentCatalog::load_user_types_from(overlay).size() == 1);
}

TEST_CASE_METHOD(TypesFixture, "a truncated overlay file is corrupt",
                 "[filament][types][filament_catalog]") {
    auto overlay = write("user.json", R"({"types": [ )");
    CHECK(FilamentCatalog::overlay_file_is_corrupt(overlay));
    CHECK_FALSE(FilamentCatalog::overlay_file_is_corrupt((dir / "absent.json").string()));
}

TEST_CASE_METHOD(TypesFixture, "the shipped table resolves through the asset root",
                 "[filament][types][asset_root]") {
    // Firmware has no CWD: the table is only reachable under asset_root().
    fs::create_directories(dir / "assets");
    const std::string asset =
        write("assets/filaments.json",
              R"({"types": [{"name": "ZZROOTMAT", "nozzle_min": 200, "nozzle_max": 210}]})");
    struct RootGuard {
        std::string saved = helix::asset_root();
        ~RootGuard() {
            helix::set_asset_root(saved);
        }
    } guard;
    helix::set_asset_root(dir.string());

    CHECK(FilamentCatalog::builtin_asset_path() == asset);
    filament::reload_materials();
    CHECK(filament::find_material("ZZROOTMAT"));
}
