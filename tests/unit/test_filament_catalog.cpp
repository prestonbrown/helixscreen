// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "filament_product_form.h"
#include "helix_test_fixture.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::printer::FilamentCatalog;
using helix::ui::build_product_json;
using helix::ui::derive_product_id;
using helix::ui::FilamentFormValues;
using helix::ui::validate_product_form;

namespace {
constexpr const char* FIX = "tests/fixtures/filaments_test.json";
constexpr const char* USER_FIX = "tests/fixtures/user_filaments_test.json";
} // namespace

TEST_CASE_METHOD(HelixTestFixture, "resolve_code cfs hit and miss", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, /*codes_only=*/true, "cfs");
    const auto* p = cat.resolve_code("cfs", "01001");
    REQUIRE(p != nullptr);
    CHECK(p->brand == "Creality");
    CHECK(p->type == "PLA");
    CHECK(cat.resolve_code("cfs", "99999") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "effective inherits type range when thin",
                 "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* pla = cat.resolve_id("polymaker-pla-pro"); // no explicit range
    REQUIRE(pla != nullptr);
    CHECK(pla->nozzle_min == 190); // inherited from PLA type
    CHECK(pla->nozzle_max == 220);
    CHECK(pla->bed_temp == 60); // inherited from PLA type
    CHECK(pla->compat_group == std::string("PLA"));
}

TEST_CASE_METHOD(HelixTestFixture, "explicit override wins over type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 270); // explicit, outside generic ABS range
    CHECK(abs->nozzle_max == 290);
    CHECK(abs->bed_temp == 105); // explicit bed
}

TEST_CASE_METHOD(HelixTestFixture, "load_codes materializes only coded slice",
                 "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, true, "cfs");
    CHECK(cat.all_products().size() == 1); // only the cfs-coded entry
    CHECK(cat.resolve_id("polymaker-abs-pro") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "queries by brand and type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    CHECK(cat.products_for_brand("Polymaker").size() == 2);
    CHECK(cat.products_for_type("PLA").size() == 2);
}

TEST_CASE_METHOD(HelixTestFixture, "user overlay overrides and adds", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_with_overlay("tests/fixtures/filaments_test.json",
                                                  "tests/fixtures/user_filaments_test.json");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 265); // overridden by user
    CHECK(abs->nozzle_max == 285);
    const auto* added = cat.resolve_id("acme-custom-petg");
    REQUIRE(added != nullptr); // new user product
    CHECK(added->brand == "Acme");
    CHECK(added->bed_temp == 80); // inherited from PETG type
}

TEST_CASE_METHOD(HelixTestFixture, "user overlay accepts legacy bare-array product form",
                 "[filament_catalog]") {
    // Backward-compat: a pre-#1120 overlay is a bare JSON array of products with
    // no orca_type_map. The reader must still merge those products — this is the
    // legacy read path #1120 explicitly preserves. Regression guard: the shared
    // fixture moved to object form, which left this branch otherwise uncovered.
    constexpr const char* TMP = "/tmp/helix_user_bare_array_products_test.json";
    {
        std::ofstream out(TMP);
        out << R"([{"id":"legacy-user-petg","brand":"Legacy","name":"Old PETG",)"
            << R"("type":"PETG","nozzle":230}])";
    }
    auto cat = FilamentCatalog::load_with_overlay("tests/fixtures/filaments_test.json", TMP);
    const auto* added = cat.resolve_id("legacy-user-petg");
    REQUIRE(added != nullptr);
    CHECK(added->brand == "Legacy");
    CHECK(added->nozzle_recommended == 230);
    std::remove(TMP);
}

TEST_CASE_METHOD(HelixTestFixture, "load_user_orca_type_map_from reads object form",
                 "[filament_catalog]") {
    auto m = FilamentCatalog::load_user_orca_type_map_from(USER_FIX);
    REQUIRE(m.size() == 3);
    CHECK(m.at("PLA-BioTough") == "PLA");
    CHECK(m.at("CustomASA") == "ASA");
    // Empty value is the documented "suppress" case — must round-trip verbatim,
    // not be dropped or normalized to a default. orca_match_type() step 1 treats
    // "" as "emit nothing for this type".
    CHECK(m.at("WeirdResin") == "");
}

TEST_CASE_METHOD(HelixTestFixture, "load_user_orca_type_map_from missing file returns empty",
                 "[filament_catalog]") {
    CHECK(FilamentCatalog::load_user_orca_type_map_from("tests/fixtures/does_not_exist.json")
              .empty());
}

TEST_CASE_METHOD(HelixTestFixture, "load_user_orca_type_map_from empty path returns empty",
                 "[filament_catalog]") {
    // SubjectInitializer passes the result of first_existing(), which is "" when
    // no user overlay is present on the device. Must not throw, must not log a
    // parse warning — just return empty silently.
    CHECK(FilamentCatalog::load_user_orca_type_map_from("").empty());
}

TEST_CASE_METHOD(HelixTestFixture, "load_user_orca_type_map_from bare array returns empty",
                 "[filament_catalog]") {
    // A bare-array overlay is product-only (the historical minimum) and must
    // not be treated as an error — it simply carries no Orca hints.
    constexpr const char* TMP = "/tmp/helix_user_bare_array_test.json";
    {
        std::ofstream out(TMP);
        out << R"([{"id":"x","type":"PLA"}])";
    }
    auto m = FilamentCatalog::load_user_orca_type_map_from(TMP);
    CHECK(m.empty());
    std::remove(TMP);
}

TEST_CASE_METHOD(HelixTestFixture, "load_user_orca_type_map_from object without key returns empty",
                 "[filament_catalog]") {
    // Object form but only filaments[] (the shape shipped Phase 1 expected) —
    // valid overlay, just no Orca contribution. Not an error.
    constexpr const char* TMP = "/tmp/helix_user_no_key_test.json";
    {
        std::ofstream out(TMP);
        out << R"({"filaments":[{"id":"x","type":"PLA"}]})";
    }
    auto m = FilamentCatalog::load_user_orca_type_map_from(TMP);
    CHECK(m.empty());
    std::remove(TMP);
}

// ---- save_user_products_to ----

namespace {
// Helper: read a small file into a string (empty string on failure).
std::string read_small_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr const char* SAVE_TMP = "/tmp/helix_user_save_test.json";

void remove_save_tmp() {
    std::remove(SAVE_TMP);
}
} // namespace

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to round-trips through load_with_overlay",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    std::vector<nlohmann::json> products = {
        {{"id", "acme-test-pla"},
         {"brand", "Acme"},
         {"name", "Test PLA"},
         {"type", "PLA"},
         {"nozzle", 220},
         {"source", "user"}},
        {{"id", "brand-x-abs"},
         {"brand", "Brand X"},
         {"name", "Fast ABS"},
         {"type", "ABS"},
         {"nozzle_min", 240},
         {"nozzle_max", 260},
         {"source", "user"}},
    };

    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    // File exists, is parseable, carries the filaments section.
    const std::string raw = read_small_file(SAVE_TMP);
    INFO("overlay content: " << raw);
    auto doc = nlohmann::json::parse(raw);
    REQUIRE(doc.is_object());
    REQUIRE(doc.contains("filaments"));
    REQUIRE(doc["filaments"].is_array());
    REQUIRE(doc["filaments"].size() == 2);
    CHECK(doc["filaments"][0]["id"] == "acme-test-pla");
    CHECK(doc["filaments"][1]["nozzle_max"] == 260);

    // Functional round-trip: load_with_overlay against the built-in fixture
    // merges the new entries over the existing ones.
    auto cat = FilamentCatalog::load_with_overlay(FIX, SAVE_TMP);
    const auto* added = cat.resolve_id("acme-test-pla");
    REQUIRE(added != nullptr);
    CHECK(added->brand == "Acme");
    CHECK(added->nozzle_recommended == 220); // "nozzle" key

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to preserves existing orca_type_map",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    // Seed the file with both sections.
    {
        std::ofstream out(SAVE_TMP);
        out << R"({"filaments":[{"id":"old-entry","type":"PLA"}],)"
            << R"("orca_type_map":{"MyResin":"PLA","WeirdResin":""}})";
    }

    std::vector<nlohmann::json> fresh_products = {
        {{"id", "new-entry"}, {"type", "PETG"}, {"source", "user"}},
    };
    REQUIRE(FilamentCatalog::save_user_products_to(fresh_products, SAVE_TMP));

    // Products were replaced (old-entry is gone, new-entry is present).
    auto doc = nlohmann::json::parse(read_small_file(SAVE_TMP));
    REQUIRE(doc["filaments"].size() == 1);
    CHECK(doc["filaments"][0]["id"] == "new-entry");

    // orca_type_map is preserved verbatim, including the "" suppress entry.
    auto m = FilamentCatalog::load_user_orca_type_map_from(SAVE_TMP);
    REQUIRE(m.size() == 2);
    CHECK(m.at("MyResin") == "PLA");
    CHECK(m.at("WeirdResin") == "");

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to migrates legacy bare-array overlay",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    // Seed with the legacy bare-array form (pre-#1120). Migration: keep the
    // products we're given (the caller is replacing them anyway), drop the
    // bare-array shape, write proper object form. The legacy form never
    // carried orca_type_map, so nothing is lost.
    {
        std::ofstream out(SAVE_TMP);
        out << R"([{"id":"legacy-entry","type":"PLA"}])";
    }

    std::vector<nlohmann::json> products = {
        {{"id", "post-migration"}, {"type", "PETG"}, {"source", "user"}},
    };
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    auto doc = nlohmann::json::parse(read_small_file(SAVE_TMP));
    REQUIRE(doc.is_object()); // migrated to object form
    REQUIRE(doc["filaments"].size() == 1);
    CHECK(doc["filaments"][0]["id"] == "post-migration");
    // Reader accepts the new shape.
    auto cat = FilamentCatalog::load_with_overlay(FIX, SAVE_TMP);
    CHECK(cat.resolve_id("post-migration") != nullptr);

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to creates missing parent directories",
                 "[filament_catalog][user_save]") {
    const std::string nested = "/tmp/helix_save_nested_dir_test/overlay.json";
    std::remove(nested.c_str());
    std::filesystem::remove_all("/tmp/helix_save_nested_dir_test");

    std::vector<nlohmann::json> products = {
        {{"id", "nested-test"}, {"type", "PLA"}, {"source", "user"}},
    };
    REQUIRE(FilamentCatalog::save_user_products_to(products, nested));
    CHECK(std::filesystem::exists(nested));

    std::filesystem::remove_all("/tmp/helix_save_nested_dir_test");
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to handles corrupt existing file",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    {
        std::ofstream out(SAVE_TMP);
        out << R"(this is not json)";
    }

    std::vector<nlohmann::json> products = {
        {{"id", "post-corrupt"}, {"type", "PLA"}, {"source", "user"}},
    };
    // Corrupt existing file must not block the save — start fresh, log a warn.
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    auto doc = nlohmann::json::parse(read_small_file(SAVE_TMP));
    REQUIRE(doc["filaments"].size() == 1);
    CHECK(doc["filaments"][0]["id"] == "post-corrupt");

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to backs up a corrupt existing file",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    const std::string bak = std::string(SAVE_TMP) + ".bak";
    std::remove(bak.c_str());
    // A hand-authored file that's been truncated mid-edit: the orca_type_map is
    // real data the user would want back, but the file no longer parses.
    {
        std::ofstream out(SAVE_TMP);
        out << R"({"orca_type_map":{"PreciousHint":"PLA"}, "filaments":[)"; // truncated
    }

    std::vector<nlohmann::json> products = {
        {{"id", "post-corrupt"}, {"type", "PLA"}, {"source", "user"}},
    };
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    // The unparseable original is preserved to <path>.bak for hand-recovery,
    // with its content intact.
    REQUIRE(std::filesystem::exists(bak));
    const std::string recovered = read_small_file(bak);
    CHECK(recovered.find("PreciousHint") != std::string::npos);

    std::remove(bak.c_str());
    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "choose_overlay_write_path falls back to canonical path",
                 "[filament_catalog][user_save]") {
    // Fresh install: neither candidate exists on disk. The write target must
    // still resolve to a creatable path (the first candidate), not "" —
    // otherwise the first save from the edit modal has nowhere to write.
    const char* none[] = {"/tmp/helix_choose_path_a.json", "/tmp/helix_choose_path_b.json"};
    std::remove(none[0]);
    std::remove(none[1]);
    CHECK(FilamentCatalog::choose_overlay_write_path(none, 2) == std::string(none[0]));

    // When a later candidate already exists, it is preferred over the fallback.
    {
        std::ofstream out(none[1]);
        out << "[]";
    }
    CHECK(FilamentCatalog::choose_overlay_write_path(none, 2) == std::string(none[1]));
    std::remove(none[1]);
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to empty path returns false",
                 "[filament_catalog][user_save]") {
    // SubjectInitializer passes the result of first_existing(), which is "" when
    // no overlay directory exists yet. Must not throw, must not create a file
    // named "" — just return false cleanly.
    CHECK_FALSE(FilamentCatalog::save_user_products_to({}, ""));
}

TEST_CASE_METHOD(HelixTestFixture, "save_user_products_to empty product list writes valid overlay",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    // Seed with orca_type_map so we can verify it survives an empty-products save.
    {
        std::ofstream out(SAVE_TMP);
        out << R"({"filaments":[{"id":"will-be-replaced"}],"orca_type_map":{"X":"PLA"}})";
    }

    REQUIRE(FilamentCatalog::save_user_products_to({}, SAVE_TMP));

    auto doc = nlohmann::json::parse(read_small_file(SAVE_TMP));
    REQUIRE(doc["filaments"].is_array());
    CHECK(doc["filaments"].empty());
    // orca_type_map preserved.
    CHECK(doc["orca_type_map"]["X"] == "PLA");

    remove_save_tmp();
}

// ---- load_user_products + upsert/remove (read-modify-write for the edit UI) ----

TEST_CASE_METHOD(HelixTestFixture, "load_user_products_from reads authored products",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    // Object form: return the sparse authored entries verbatim, no inheritance.
    {
        std::ofstream out(SAVE_TMP);
        out << R"({"filaments":[{"id":"a","type":"PLA"},{"id":"b","nozzle":250}],)"
            << R"("orca_type_map":{"X":"PLA"}})";
    }
    auto products = FilamentCatalog::load_user_products_from(SAVE_TMP);
    REQUIRE(products.size() == 2);
    CHECK(products[0]["id"] == "a");
    CHECK(products[1]["nozzle"] == 250); // authored value, not resolved/inherited

    // Legacy bare-array form is accepted too.
    {
        std::ofstream out(SAVE_TMP);
        out << R"([{"id":"legacy","type":"ABS"}])";
    }
    auto legacy = FilamentCatalog::load_user_products_from(SAVE_TMP);
    REQUIRE(legacy.size() == 1);
    CHECK(legacy[0]["id"] == "legacy");

    // Missing file -> empty, no throw.
    CHECK(FilamentCatalog::load_user_products_from("/tmp/helix_no_such_overlay.json").empty());

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "upsert_product replaces by id or appends",
                 "[filament_catalog][user_save]") {
    std::vector<nlohmann::json> products = {
        {{"id", "a"}, {"type", "PLA"}},
        {{"id", "b"}, {"type", "ABS"}},
    };

    // Edit: same id replaces in place, preserves order, returns true.
    CHECK(FilamentCatalog::upsert_product(products, {{"id", "a"}, {"type", "PETG"}}));
    REQUIRE(products.size() == 2);
    CHECK(products[0]["id"] == "a");      // still first
    CHECK(products[0]["type"] == "PETG"); // replaced
    CHECK(products[1]["id"] == "b");      // untouched

    // Add: new id appends, returns false.
    CHECK_FALSE(FilamentCatalog::upsert_product(products, {{"id", "c"}, {"type", "TPU"}}));
    REQUIRE(products.size() == 3);
    CHECK(products[2]["id"] == "c");

    // No/empty id appends rather than clobbering entry 0.
    CHECK_FALSE(FilamentCatalog::upsert_product(products, {{"name", "nameless"}}));
    REQUIRE(products.size() == 4);
    CHECK(products[0]["id"] == "a");
}

TEST_CASE_METHOD(HelixTestFixture, "remove_product removes by id",
                 "[filament_catalog][user_save]") {
    std::vector<nlohmann::json> products = {
        {{"id", "a"}, {"type", "PLA"}},
        {{"id", "b"}, {"type", "ABS"}},
    };
    CHECK(FilamentCatalog::remove_product(products, "a"));
    REQUIRE(products.size() == 1);
    CHECK(products[0]["id"] == "b");
    // Absent id: no-op, returns false.
    CHECK_FALSE(FilamentCatalog::remove_product(products, "zzz"));
    CHECK(products.size() == 1);
}

TEST_CASE_METHOD(HelixTestFixture, "product edit round-trip preserves orca_type_map",
                 "[filament_catalog][user_save]") {
    remove_save_tmp();
    // Seed as the modal will find it: existing user products + an orca_type_map.
    {
        std::ofstream out(SAVE_TMP);
        out << R"({"filaments":[{"id":"user-pla","type":"PLA","nozzle":215}],)"
            << R"("orca_type_map":{"CustomPLA":"PLA"}})";
    }

    // --- Add a new product (the modal's "+ Add custom filament" flow) ---
    auto products = FilamentCatalog::load_user_products_from(SAVE_TMP);
    CHECK_FALSE(FilamentCatalog::upsert_product(
        products, {{"id", "user-petg"}, {"type", "PETG"}, {"nozzle", 240}, {"source", "user"}}));
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    // Both products present; orca_type_map survived the products-only write.
    auto after_add = FilamentCatalog::load_user_products_from(SAVE_TMP);
    REQUIRE(after_add.size() == 2);
    auto map1 = FilamentCatalog::load_user_orca_type_map_from(SAVE_TMP);
    CHECK(map1.at("CustomPLA") == "PLA");

    // --- Edit an existing product (override-by-id) ---
    FilamentCatalog::upsert_product(
        after_add, {{"id", "user-pla"}, {"type", "PLA"}, {"nozzle", 225}, {"source", "user"}});
    REQUIRE(FilamentCatalog::save_user_products_to(after_add, SAVE_TMP));
    auto cat = FilamentCatalog::load_with_overlay(FIX, SAVE_TMP);
    const auto* edited = cat.resolve_id("user-pla");
    REQUIRE(edited != nullptr);
    CHECK(edited->nozzle_recommended == 225); // edit applied

    // --- Delete / Restore-Defaults (remove the overlay entry) ---
    auto after_edit = FilamentCatalog::load_user_products_from(SAVE_TMP);
    CHECK(FilamentCatalog::remove_product(after_edit, "user-petg"));
    REQUIRE(FilamentCatalog::save_user_products_to(after_edit, SAVE_TMP));
    auto final_products = FilamentCatalog::load_user_products_from(SAVE_TMP);
    REQUIRE(final_products.size() == 1);
    CHECK(final_products[0]["id"] == "user-pla");
    // orca_type_map still intact after the whole add/edit/delete cycle.
    auto map2 = FilamentCatalog::load_user_orca_type_map_from(SAVE_TMP);
    CHECK(map2.at("CustomPLA") == "PLA");

    remove_save_tmp();
}

// ---------------------------------------------------------------------------
// FilamentProductEditModal form logic (pure, LVGL-free): build_product_json,
// derive_product_id, validate_product_form. Tag [user_save] with the rest of
// the write-path tests.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture, "build_product_json always emits id, type, source",
                 "[filament_catalog][user_save]") {
    FilamentFormValues v;
    v.id = "my-pla";
    v.type = "PLA";
    auto j = build_product_json(v);
    CHECK(j.value("id", "") == "my-pla");
    CHECK(j.value("type", "") == "PLA");
    CHECK(j.value("source", "") == "user");
    // Blank optional fields are omitted entirely (must not clobber inheritance).
    CHECK_FALSE(j.contains("brand"));
    CHECK_FALSE(j.contains("name"));
    CHECK_FALSE(j.contains("nozzle_min"));
    CHECK_FALSE(j.contains("nozzle_max"));
    CHECK_FALSE(j.contains("nozzle"));
    CHECK_FALSE(j.contains("bed"));
    CHECK_FALSE(j.contains("density"));
}

TEST_CASE_METHOD(HelixTestFixture, "build_product_json emits provided fields with correct keys",
                 "[filament_catalog][user_save]") {
    FilamentFormValues v;
    v.id = "poly-x";
    v.brand = "Polymaker";
    v.name = "PolyTerra";
    v.type = "PLA";
    v.nozzle_min = "195";
    v.nozzle_max = "230";
    v.nozzle = "210"; // recommended -> key "nozzle"
    v.bed = "55";     // bed_temp   -> key "bed"
    v.density = "1.24";
    auto j = build_product_json(v);

    CHECK(j.value("brand", "") == "Polymaker");
    CHECK(j.value("name", "") == "PolyTerra");
    CHECK(j.value("nozzle_min", 0) == 195);
    CHECK(j.value("nozzle_max", 0) == 230);
    CHECK(j.value("nozzle", 0) == 210);
    CHECK(j.value("bed", 0) == 55);
    CHECK(j["density"].get<float>() == Catch::Approx(1.24f));
    // The struct field names must NOT appear as JSON keys.
    CHECK_FALSE(j.contains("nozzle_recommended"));
    CHECK_FALSE(j.contains("bed_temp"));
    CHECK_FALSE(j.contains("density_g_cm3"));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "build_product_json omits blank numerics but keeps explicit zero",
                 "[filament_catalog][user_save]") {
    FilamentFormValues v;
    v.id = "z";
    v.type = "PLA";
    v.nozzle_min = ""; // blank -> omit
    v.bed = "0";       // explicit zero -> keep (user intent)
    auto j = build_product_json(v);
    CHECK_FALSE(j.contains("nozzle_min"));
    REQUIRE(j.contains("bed"));
    CHECK(j.value("bed", -1) == 0);
}

TEST_CASE_METHOD(HelixTestFixture, "derive_product_id slugs brand+name when id blank",
                 "[filament_catalog][user_save]") {
    FilamentFormValues v;
    v.brand = "Poly Maker";
    v.name = "PolyTerra PLA";
    CHECK(derive_product_id(v) == "poly-maker-polyterra-pla");
    // The built product carries the derived id.
    v.type = "PLA";
    CHECK(build_product_json(v).value("id", "") == "poly-maker-polyterra-pla");

    // An explicit id wins verbatim.
    v.id = "explicit-id";
    CHECK(derive_product_id(v) == "explicit-id");
}

TEST_CASE_METHOD(HelixTestFixture, "validate_product_form rejects blank id and blank type",
                 "[filament_catalog][user_save]") {
    std::string err;
    FilamentFormValues blank;
    CHECK_FALSE(validate_product_form(blank, err)); // no id derivable
    CHECK_FALSE(err.empty());

    FilamentFormValues no_type;
    no_type.id = "x";
    err.clear();
    CHECK_FALSE(validate_product_form(no_type, err)); // type required
    CHECK_FALSE(err.empty());

    FilamentFormValues ok;
    ok.id = "x";
    ok.type = "PLA";
    err.clear();
    CHECK(validate_product_form(ok, err));
    CHECK(err.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "validate_product_form rejects nozzle min greater than max",
                 "[filament_catalog][user_save]") {
    std::string err;
    FilamentFormValues bad;
    bad.id = "x";
    bad.type = "PLA";
    bad.nozzle_min = "250";
    bad.nozzle_max = "200";
    CHECK_FALSE(validate_product_form(bad, err));
    CHECK_FALSE(err.empty());

    // Equal is allowed; only one bound provided is allowed.
    FilamentFormValues eq = bad;
    eq.nozzle_max = "250";
    err.clear();
    CHECK(validate_product_form(eq, err));

    FilamentFormValues one_bound;
    one_bound.id = "x";
    one_bound.type = "PLA";
    one_bound.nozzle_min = "250"; // max blank -> no comparison
    err.clear();
    CHECK(validate_product_form(one_bound, err));
}

// ---- load_codes_cached ----
//
// The CFS box parser calls this on every full box update. load_codes() re-parses
// the whole 100 KB bundled catalog each time, so the cache is what keeps a spool
// move from costing ~872 kB of transient heap on a 114 MB printer.

TEST_CASE_METHOD(HelixTestFixture, "load_codes_cached reuses one snapshot",
                 "[filament_catalog][codes_cache]") {
    auto a = FilamentCatalog::load_codes_cached("cfs");
    auto b = FilamentCatalog::load_codes_cached("cfs");

    REQUIRE(a != nullptr);
    // Same object, not merely equal contents — that is the whole point.
    CHECK(a.get() == b.get());
}

TEST_CASE_METHOD(HelixTestFixture, "load_codes_cached agrees with an uncached load",
                 "[filament_catalog][codes_cache]") {
    // Caching must not change what the parser sees. Compare against a direct
    // load of the same scheme rather than against hardcoded expectations.
    auto direct = FilamentCatalog::load_codes("cfs");
    auto cached = FilamentCatalog::load_codes_cached("cfs");
    REQUIRE(cached != nullptr);

    auto direct_products = direct.all_products();
    auto cached_products = cached->all_products();
    REQUIRE(cached_products.size() == direct_products.size());

    for (const auto* p : direct_products) {
        const auto* c = cached->resolve_id(p->id);
        REQUIRE(c != nullptr);
        CHECK(c->brand == p->brand);
        CHECK(c->type == p->type);
        CHECK(c->codes == p->codes);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "separate schemes get separate snapshots",
                 "[filament_catalog][codes_cache]") {
    auto cfs = FilamentCatalog::load_codes_cached("cfs");
    auto other = FilamentCatalog::load_codes_cached("no_such_scheme");
    REQUIRE(cfs != nullptr);
    REQUIRE(other != nullptr);
    CHECK(cfs.get() != other.get());
    CHECK(other->all_products().empty()); // nothing carries that scheme
    // The cfs entry must survive caching a second scheme.
    CHECK(FilamentCatalog::load_codes_cached("cfs").get() == cfs.get());
}

TEST_CASE_METHOD(HelixTestFixture, "a user-overlay save retires the snapshot",
                 "[filament_catalog][codes_cache][user_save]") {
    remove_save_tmp();
    auto before = FilamentCatalog::load_codes_cached("cfs");
    const uint64_t gen_before = FilamentCatalog::user_overlay_generation();

    std::vector<nlohmann::json> products = {
        {{"id", "acme-cache-pla"}, {"brand", "Acme"}, {"name", "Cache PLA"}, {"type", "PLA"}}};
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));

    CHECK(FilamentCatalog::user_overlay_generation() > gen_before);
    auto after = FilamentCatalog::load_codes_cached("cfs");
    // A stale snapshot here is the bug this guards: a product the user just
    // added would stay invisible until restart.
    CHECK(after.get() != before.get());

    remove_save_tmp();
}

TEST_CASE_METHOD(HelixTestFixture, "a held snapshot outlives its retirement",
                 "[filament_catalog][codes_cache][user_save]") {
    // The CFS parser runs on the WebSocket thread while a save can land on the
    // main thread. Holding the shared_ptr must keep that catalog readable even
    // after the cache has moved on, or the parse reads freed memory.
    remove_save_tmp();
    auto held = FilamentCatalog::load_codes_cached("cfs");
    REQUIRE(held != nullptr);
    const size_t count_before = held->all_products().size();
    const std::string first_id =
        count_before > 0 ? held->all_products().front()->id : std::string();

    std::vector<nlohmann::json> products = {
        {{"id", "acme-cache-pla2"}, {"brand", "Acme"}, {"name", "Cache PLA 2"}, {"type", "PLA"}}};
    REQUIRE(FilamentCatalog::save_user_products_to(products, SAVE_TMP));
    auto replacement = FilamentCatalog::load_codes_cached("cfs");
    REQUIRE(replacement.get() != held.get());

    // Old snapshot still intact and unchanged.
    CHECK(held->all_products().size() == count_before);
    if (!first_id.empty()) {
        CHECK(held->resolve_id(first_id) != nullptr);
    }

    remove_save_tmp();
}
