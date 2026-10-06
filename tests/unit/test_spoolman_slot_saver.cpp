// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include <algorithm>
#include <optional>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: Create a base SlotInfo for tests
// ============================================================================

static SlotInfo make_test_slot() {
    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 42;
    slot.brand = "Polymaker";
    slot.material = "PLA";
    slot.color_rgb = 0xFF0000; // Red
    slot.spoolman_filament_id = 100;
    slot.remaining_weight_g = 800.0f;
    slot.total_weight_g = 1000.0f;
    return slot;
}

// ============================================================================
// detect_changes() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver detect_changes: no changes returns both false",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE_FALSE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: vendor changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "eSUN";

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: material changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "PETG";

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: color changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.color_rgb = 0x00FF00; // Green

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

// A variant swap within one vendor+material — SUNLU "PLA Marble" -> "PLA+ 2.0"
// — leaves brand, material and color identical, so without the product-identity
// fields detect_changes() reports NO change at all and the edit reads as a
// no-op everywhere downstream.
TEST_CASE("SpoolmanSlotSaver detect_changes: catalog product changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    original.catalog_id = "sunlu-pla-marble";
    original.product_name = "PLA Marble";

    SECTION("catalog id alone") {
        SlotInfo edited = original;
        edited.catalog_id = "sunlu-pla-plus-2-0";
        edited.product_name = "PLA+ 2.0";

        auto changes = SpoolmanSlotSaver::detect_changes(original, edited);
        REQUIRE(changes.filament_level);
        REQUIRE_FALSE(changes.spool_level);
    }

    SECTION("display name alone — an id-less pick still counts") {
        SlotInfo edited = original;
        edited.catalog_id.clear();
        SlotInfo base = original;
        base.catalog_id.clear();
        edited.product_name = "PLA+ 2.0";

        auto changes = SpoolmanSlotSaver::detect_changes(base, edited);
        REQUIRE(changes.filament_level);
    }

    SECTION("identical product is not a change") {
        auto changes = SpoolmanSlotSaver::detect_changes(original, original);
        REQUIRE_FALSE(changes.any());
    }
}

TEST_CASE("SpoolmanSlotSaver detect_changes: remaining weight changed sets spool_level only",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = 750.0f;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: weight within threshold is not a change",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = original.remaining_weight_g + 0.05f; // Within 0.1 threshold

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.spool_level);
    REQUIRE_FALSE(changes.any());
}

// A user-entered change to the spool's TOTAL weight is a real spool-level change.
// Without this, the header Save button lights up (is_dirty() includes total_weight_g)
// and the value is stored locally, while Spoolman keeps its old initial_weight forever
// — total_weight_g was only ever sent to Spoolman at spool-creation time.
TEST_CASE("SpoolmanSlotSaver detect_changes: total weight changed sets spool_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.total_weight_g = 750.0f; // was 1000g

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: total weight within threshold is not a change",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.total_weight_g = original.total_weight_g + 0.05f; // within 0.1 threshold

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.spool_level);
    REQUIRE_FALSE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: both filament and weight changed sets both",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "ABS";
    edited.remaining_weight_g = 600.0f;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: spoolman_id changed sets spool_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.spoolman_id = 99; // Different spool, same material/brand/color/weight

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: spoolman_id cleared sets spool_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.spoolman_id = 0; // Unlinked from Spoolman

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

// ============================================================================
// save() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save does nothing for non-spoolman slots", "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original;
    original.spoolman_id = 0; // Not a Spoolman spool
    original.brand = "Polymaker";
    original.material = "PLA";

    SlotInfo edited = original;
    edited.brand = "eSUN"; // Changed but irrelevant since spoolman_id=0

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](const SaveResult& r) {
        callback_called = true;
        callback_success = r.success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success); // No-op success
}

TEST_CASE("SpoolmanSlotSaver save does nothing when no changes detected",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original; // No changes

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](const SaveResult& r) {
        callback_called = true;
        callback_success = r.success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success); // No-op success
}

TEST_CASE("SpoolmanSlotSaver save only updates weight when no filament-level changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Ensure mock has a spool with id=42
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = 650.0f; // Only weight changed

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](const SaveResult& r) {
        callback_called = true;
        callback_success = r.success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    // Verify weight was updated in mock
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(650.0));
            break;
        }
    }
}

TEST_CASE("SpoolmanSlotSaver save repoints spool to existing filament when vendor changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    // Seed the original vendor + filament (id=100, referenced by make_test_slot).
    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");

    // Seed the target vendor + matching filament (eSUN PLA red = id 200).
    api.spoolman_mock().add_vendor(2, "eSUN");
    api.spoolman_mock().add_filament(200, 2, "PLA", "FF0000");

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot(); // Polymaker PLA 0xFF0000, filament_id=100
    SlotInfo edited = original;
    edited.brand = "eSUN"; // Changed vendor — should resolve to filament 200

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 2);
    REQUIRE(got.new_filament_id == 200);

    // Verify spool was PATCHed with new filament_id (not mutating filament 100).
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 200);

    // And the target vendor already existed, so nothing was created.
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

TEST_CASE("SpoolmanSlotSaver save creates new vendor + filament and repoints spool when no match",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    // Seed original vendor/filament so resolution can find them.
    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");

    // Configure mock IDs assigned on create.
    api.spoolman_mock().next_created_vendor_id = 50;
    api.spoolman_mock().next_created_filament_id = 500;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    // Change to something that won't match any existing vendor/filament.
    edited.brand = "UniqueTestBrand";
    edited.material = "Nylon";
    edited.color_rgb = 0x123456;

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 50);
    REQUIRE(got.new_filament_id == 500);

    // New vendor was POSTed with the brand name.
    REQUIRE(api.spoolman_mock().created_vendors.size() == 1);
    REQUIRE(api.spoolman_mock().created_vendors[0]["name"] == "UniqueTestBrand");

    // New filament was POSTed with the right triple.
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    auto& fp = api.spoolman_mock().created_filaments[0];
    REQUIRE(fp["vendor_id"] == 50);
    REQUIRE(fp["material"] == "Nylon");
    REQUIRE(fp["color_hex"] == "123456");

    // Spool was PATCHed to point at the new filament.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 500);
}

TEST_CASE("SpoolmanSlotSaver save chains filament repoint then weight update when both changed",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Need a mock spool for update_spoolman_spool_weight() to find and update.
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    // Configure IDs for the new vendor + filament we expect to be created.
    api.spoolman_mock().next_created_vendor_id = 51;
    api.spoolman_mock().next_created_filament_id = 501;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    // Pick a triple that can't match the spool-synthesized filament entry
    // (which has material=PLA color=FF0000) so find_or_create_filament creates.
    edited.brand = "NewBrandXYZ";
    edited.material = "ABS";
    edited.color_rgb = 0x334455;
    edited.remaining_weight_g = 500.0f;

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 51);
    REQUIRE(got.new_filament_id == 501);

    // Spool was PATCHed to point at the new filament.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 501);

    // And the weight update also went through.
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(500.0));
            break;
        }
    }
}

// ============================================================================
// Filament repoint tests (save() resolves via find-or-create then PATCHes the
// spool's filament_id — it never mutates the existing filament record)
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save creates new filament and repoints spool when material changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");
    api.spoolman_mock().next_created_filament_id = 101;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "PETG"; // Changed material — no existing PETG/red filament

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 1);     // existing Polymaker
    REQUIRE(got.new_filament_id == 101); // newly-created PETG

    // Verify a new filament was POSTed (not a PATCH of filament 100).
    REQUIRE(api.spoolman_mock().filament_updates.empty());
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    REQUIRE(api.spoolman_mock().created_filaments[0]["material"] == "PETG");

    // Verify the spool was PATCHed to point at the new filament.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 101);
}

TEST_CASE("SpoolmanSlotSaver save creates vendor when brand is new", "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    // Seed the original vendor/filament only.
    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");
    api.spoolman_mock().next_created_vendor_id = 52;
    api.spoolman_mock().next_created_filament_id = 502;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "eSUN"; // unknown vendor

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 52);
    REQUIRE(got.new_filament_id == 502);

    // Vendor created with new brand name.
    REQUIRE(api.spoolman_mock().created_vendors.size() == 1);
    REQUIRE(api.spoolman_mock().created_vendors[0]["name"] == "eSUN");

    // Filament created with the new vendor_id.
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    REQUIRE(api.spoolman_mock().created_filaments[0]["vendor_id"] == 52);
    REQUIRE(api.spoolman_mock().created_filaments[0]["material"] == "PLA");

    // Spool repoint.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].patch["filament_id"] == 502);
}

TEST_CASE("SpoolmanSlotSaver save sends color_hex without leading # when creating filament",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");
    api.spoolman_mock().next_created_filament_id = 103;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.color_rgb = 0x00FF00; // No matching filament — new one gets created

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);

    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    const std::string hex = api.spoolman_mock().created_filaments[0]["color_hex"];
    REQUIRE(hex == "00FF00");
    REQUIRE(hex[0] != '#');
}

TEST_CASE("SpoolmanSlotSaver save repoints then updates weight when both change",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Need a mock spool for update_spoolman_spool_weight() to find.
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().add_filament(100, 1, "PLA", "FF0000");
    api.spoolman_mock().next_created_filament_id = 104;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "ABS";
    edited.remaining_weight_g = 500.0f;

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_filament_id == 104);

    // Verify spool was PATCHed.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 104);

    // And weight was also updated.
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(500.0));
            break;
        }
    }
}

TEST_CASE("SpoolmanSlotSaver save succeeds even when original has no filament_id",
          "[spoolman][slot_saver]") {
    // With the repoint model, the original's spoolman_filament_id does not gate
    // saving — we resolve the edited triple and PATCH the spool to point there.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().next_created_filament_id = 105;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    original.spoolman_filament_id = 0; // Originally unlinked at the filament level
    SlotInfo edited = original;
    edited.material = "PETG";

    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_filament_id == 105);

    // Spool PATCH should still go out.
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].patch["filament_id"] == 105);

    // And the filament PATCH endpoint was NOT touched.
    REQUIRE(api.spoolman_mock().filament_updates.empty());
}

// ============================================================================
// color_to_hex format tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver color_to_hex produces hex without # prefix (observed via create)",
          "[spoolman][slot_saver]") {
    // color_to_hex is private; verify the format indirectly via the newly-created
    // filament payload (since no existing filament matches 0xABCDEF).
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    api.spoolman_mock().add_vendor(1, "Polymaker");
    api.spoolman_mock().next_created_filament_id = 999;

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    original.color_rgb = 0x000000;
    SlotInfo edited = original;
    edited.color_rgb = 0xABCDEF;

    bool done = false;
    saver.save(original, edited, [&](const SaveResult&) { done = true; });

    REQUIRE(done);
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    const std::string hex = api.spoolman_mock().created_filaments[0]["color_hex"];
    REQUIRE(hex == "ABCDEF");
    REQUIRE(hex[0] != '#');
}

// ============================================================================
// is_filament_complete() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver is_filament_complete: all fields set returns true",
          "[spoolman][slot_saver]") {
    SlotInfo slot;
    slot.brand = "Polymaker";
    slot.material = "PLA";
    slot.color_rgb = 0xFF0000;
    REQUIRE(SpoolmanSlotSaver::is_filament_complete(slot));
}

TEST_CASE("SpoolmanSlotSaver is_filament_complete: empty brand returns false",
          "[spoolman][slot_saver]") {
    SlotInfo slot;
    slot.brand = "";
    slot.material = "PLA";
    slot.color_rgb = 0xFF0000;
    REQUIRE_FALSE(SpoolmanSlotSaver::is_filament_complete(slot));
}

TEST_CASE("SpoolmanSlotSaver is_filament_complete: empty material returns false",
          "[spoolman][slot_saver]") {
    SlotInfo slot;
    slot.brand = "Polymaker";
    slot.material = "";
    slot.color_rgb = 0xFF0000;
    REQUIRE_FALSE(SpoolmanSlotSaver::is_filament_complete(slot));
}

TEST_CASE("SpoolmanSlotSaver is_filament_complete: default gray color returns false",
          "[spoolman][slot_saver]") {
    SlotInfo slot;
    slot.brand = "Polymaker";
    slot.material = "PLA";
    slot.color_rgb = AMS_DEFAULT_SLOT_COLOR; // 0x808080
    REQUIRE_FALSE(SpoolmanSlotSaver::is_filament_complete(slot));
}

// ============================================================================
// find_or_create_vendor() Tests
// ============================================================================

TEST_CASE(
    "SpoolmanSlotSaver find_or_create_vendor: reuses existing vendor by name (case-insensitive)",
    "[spoolman][slot_saver][vendor]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Pre-seed vendor with id=7, name="Polymaker".
    api.spoolman_mock().add_vendor(7, "Polymaker");

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    bool error_called = false;
    saver.find_or_create_vendor(
        "polymaker", // lower-case input; should match case-insensitively
        [&](int id) { got_id = id; }, [&](const MoonrakerError&) { error_called = true; });

    REQUIRE(got_id == 7);
    REQUIRE_FALSE(error_called);
    // Mock should NOT have received any create_vendor POST.
    REQUIRE(api.spoolman_mock().created_vendors.empty());
}

TEST_CASE("SpoolmanSlotSaver find_or_create_vendor: creates new vendor when name not found",
          "[spoolman][slot_saver][vendor]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Clear any spool-synthesized vendors so lookup definitely misses.
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_vendor(7, "Polymaker");
    api.spoolman_mock().next_created_vendor_id = 42;

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    bool error_called = false;
    saver.find_or_create_vendor(
        "eSUN", [&](int id) { got_id = id; }, [&](const MoonrakerError&) { error_called = true; });

    REQUIRE(got_id == 42);
    REQUIRE_FALSE(error_called);
    REQUIRE(api.spoolman_mock().created_vendors.size() == 1);
    REQUIRE(api.spoolman_mock().created_vendors[0].contains("name"));
    REQUIRE(api.spoolman_mock().created_vendors[0]["name"] == "eSUN");
}

// ============================================================================
// find_or_create_filament() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver find_or_create_filament: matches on vendor+material+color_hex "
          "(case-insensitive hex)",
          "[spoolman][slot_saver][filament]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear(); // avoid synthesized-vendor noise
    api.spoolman_mock().add_filament(100, /*vendor_id*/ 7, "PLA", "ff0000");

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    bool error_called = false;
    saver.find_or_create_filament(
        7, "PLA", "FF0000", /*filament_name*/ "", // upper-case input vs lower-case seed
        [&](int id) { got_id = id; }, [&](const MoonrakerError&) { error_called = true; });

    REQUIRE(got_id == 100);
    REQUIRE_FALSE(error_called);
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

TEST_CASE("SpoolmanSlotSaver find_or_create_filament: mismatched material -> creates new",
          "[spoolman][slot_saver][filament]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_filament(100, 7, "PLA", "FF0000");
    api.spoolman_mock().next_created_filament_id = 101;

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    saver.find_or_create_filament(
        7, "PETG", "FF0000", /*filament_name*/ "", [&](int id) { got_id = id; },
        [&](const MoonrakerError&) { got_id = -99; });

    REQUIRE(got_id == 101);
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    auto& payload = api.spoolman_mock().created_filaments[0];
    REQUIRE(payload["vendor_id"] == 7);
    REQUIRE(payload["material"] == "PETG");
    REQUIRE(payload["color_hex"] == "FF0000");
    REQUIRE(payload["name"] == "PETG");
}

TEST_CASE("SpoolmanSlotSaver find_or_create_filament: mismatched color -> creates new",
          "[spoolman][slot_saver][filament]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_filament(100, 7, "PLA", "FF0000");
    api.spoolman_mock().next_created_filament_id = 101;

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    saver.find_or_create_filament(
        7, "PLA", "00FF00", /*filament_name*/ "", [&](int id) { got_id = id; },
        [&](const MoonrakerError&) { got_id = -99; });

    REQUIRE(got_id == 101);
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    auto& payload = api.spoolman_mock().created_filaments[0];
    REQUIRE(payload["color_hex"] == "00FF00");
}

TEST_CASE("SpoolmanSlotSaver find_or_create_filament: invalid color hex triggers on_error",
          "[spoolman][slot_saver][filament]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    bool error_called = false;
    saver.find_or_create_filament(
        7, "PLA", "XYZ", /*filament_name*/ "", // invalid
        [&](int id) { got_id = id; }, [&](const MoonrakerError&) { error_called = true; });

    REQUIRE(got_id == -1);
    REQUIRE(error_called);
    // No API calls should have been made.
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

TEST_CASE("SpoolmanSlotSaver find_or_create_filament: accepts leading # and strips it",
          "[spoolman][slot_saver][filament]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_filament(100, 7, "PLA", "FF0000");

    SpoolmanSlotSaver saver(&api);
    int got_id = -1;
    saver.find_or_create_filament(
        7, "PLA", "#ff0000", /*filament_name*/ "", [&](int id) { got_id = id; },
        [&](const MoonrakerError&) { got_id = -99; });

    REQUIRE(got_id == 100);
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

// ============================================================================
// repoint_spool() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver repoint_spool: PATCHes spool with new filament_id",
          "[spoolman][slot_saver][repoint]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    SpoolmanSlotSaver saver(&api);

    bool success = false;
    bool error_called = false;
    saver.repoint_spool(/*spool_id*/
                        42, /*new_filament_id*/ 101, [&]() { success = true; },
                        [&](const MoonrakerError&) { error_called = true; });

    REQUIRE(success);
    REQUIRE_FALSE(error_called);

    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 101);
}

// ============================================================================
// Incompleteness + skip-on-match paths (save() behavior)
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save: linked spool + incomplete filament fields + no weight change "
          "-> silent success, no API calls",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = ""; // force incomplete

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE_FALSE(got.repointed_filament);
    REQUIRE(api.spoolman_mock().spool_updates.empty());
    REQUIRE(api.spoolman_mock().filament_updates.empty());
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

TEST_CASE("SpoolmanSlotSaver save: linked spool + incomplete filament fields + weight change "
          "-> weight is still written",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Need the spool to exist for the weight update to land.
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "";                  // incomplete
    edited.remaining_weight_g = 600.0f; // weight change

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE_FALSE(got.repointed_filament);

    // Filament side skipped entirely.
    REQUIRE(api.spoolman_mock().filament_updates.empty());
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());

    // But the weight update landed.
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(600.0));
            break;
        }
    }
}

TEST_CASE("SpoolmanSlotSaver save: linked spool + filament resolves to same filament_id "
          "-> no repoint",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    // Seed vendor + filament that will be matched exactly.
    api.spoolman_mock().add_vendor(7, "Polymaker");
    api.spoolman_mock().add_filament(100, 7, "PLA", "FF0000");

    SlotInfo original = make_test_slot();
    original.spoolman_filament_id = 100; // match the seeded filament

    // Change brand capitalization — detect_changes() sees a filament-level change
    // (string equality), but find_or_create_vendor matches case-insensitively to
    // id=7, and find_or_create_filament returns id=100 → same as original →
    // repoint is skipped.
    SlotInfo edited = original;
    edited.brand = "polymaker";

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE_FALSE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 7);
    REQUIRE(got.new_filament_id == 100);

    // No repoint call was issued.
    REQUIRE(api.spoolman_mock().spool_updates.empty());
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());
}

// ============================================================================
// save(): no linked spool -> create-new-spool path
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save: no linked spool + complete fields -> creates spool and returns "
          "IDs",
          "[spoolman][slot_saver][create]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear(); // isolate from synthesized vendors
    api.spoolman_mock().add_vendor(7, "Polymaker");
    api.spoolman_mock().next_created_filament_id = 101;
    api.spoolman_mock().next_created_spool_id = 500;

    SlotInfo original;
    original.slot_index = 0;
    original.spoolman_id = 0; // no linked spool

    SlotInfo edited = original;
    edited.brand = "Polymaker";
    edited.material = "PLA";
    edited.color_rgb = 0xFF0000;
    edited.remaining_weight_g = 750.0f;

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.created_new_spool);
    REQUIRE_FALSE(got.repointed_filament);
    REQUIRE(got.new_spool_id == 500);
    REQUIRE(got.new_filament_id == 101);
    REQUIRE(got.new_vendor_id == 7); // reused existing vendor

    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.size() == 1);
    REQUIRE(api.spoolman_mock().created_spools.size() == 1);

    auto& spool_payload = api.spoolman_mock().created_spools[0];
    REQUIRE(spool_payload["filament_id"] == 101);
    REQUIRE(spool_payload["remaining_weight"] == 750.0);
}

TEST_CASE("SpoolmanSlotSaver save: no linked spool + incomplete fields -> no Spoolman calls",
          "[spoolman][slot_saver][create]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo edited = original;
    edited.brand = "Polymaker";
    edited.material = ""; // incomplete
    edited.color_rgb = 0xFF0000;

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE_FALSE(got.created_new_spool);
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());
    REQUIRE(api.spoolman_mock().created_spools.empty());
}

TEST_CASE("SpoolmanSlotSaver save: no linked spool + complete fields + zero weight -> creates "
          "spool without weight field",
          "[spoolman][slot_saver][create]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();
    api.spoolman_mock().add_vendor(7, "Polymaker");
    api.spoolman_mock().next_created_filament_id = 101;
    api.spoolman_mock().next_created_spool_id = 500;

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo edited = original;
    edited.brand = "Polymaker";
    edited.material = "PLA";
    edited.color_rgb = 0xFF0000;
    edited.remaining_weight_g = 0.0f; // not entered

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.created_new_spool);
    REQUIRE(api.spoolman_mock().created_spools.size() == 1);
    auto& payload = api.spoolman_mock().created_spools[0];
    REQUIRE(payload["filament_id"] == 101);
    REQUIRE_FALSE(payload.contains("remaining_weight"));
}

// ============================================================================
// save(): combined filament + weight change against an existing target filament
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save: linked spool + filament resolves to existing filament + weight "
          "change -> repoint PATCH and weight write both fire",
          "[spoolman][slot_saver][combined]") {
    // Covers the Trigger-table row "Linked spool; both filament and weight changed"
    // specifically in the find-path (no vendor/filament creation). Distinguishes
    // from existing combined tests which exercise the create-path.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.spoolman_mock().get_mock_spools().clear();

    // Seed the spool so update_spoolman_spool_weight() has a target to mutate.
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    api.spoolman_mock().get_mock_spools().push_back(test_spool);

    // Seed the original vendor/filament AND the target PETG filament so the
    // save path resolves to an existing id (200) without creating anything.
    api.spoolman_mock().add_vendor(7, "Polymaker");
    api.spoolman_mock().add_filament(100, 7, "PLA", "FF0000");
    api.spoolman_mock().add_filament(200, 7, "PETG", "FF0000");

    SlotInfo original = make_test_slot();
    original.spoolman_filament_id = 100;
    SlotInfo edited = original;
    edited.material = "PETG";           // resolves to existing filament 200
    edited.remaining_weight_g = 600.0f; // weight also changed

    SpoolmanSlotSaver saver(&api);
    SaveResult got{};
    saver.save(original, edited, [&](const SaveResult& r) { got = r; });

    REQUIRE(got.success);
    REQUIRE(got.repointed_filament);
    REQUIRE(got.new_vendor_id == 7);
    REQUIRE(got.new_filament_id == 200);

    // Nothing was created — we used existing vendor/filament.
    REQUIRE(api.spoolman_mock().created_vendors.empty());
    REQUIRE(api.spoolman_mock().created_filaments.empty());

    // The repoint PATCH fired (captured in spool_updates — separate path from weight).
    auto& updates = api.spoolman_mock().spool_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].spool_id == 42);
    REQUIRE(updates[0].patch["filament_id"] == 200);

    // The weight update also landed (dedicated path — update_spoolman_spool_weight
    // mutates the mock spool directly; it does not populate spool_updates).
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(600.0));
            break;
        }
    }
}

// ============================================================================
// LinkIntent — the create-vs-update decision must be EXPLICIT
// ============================================================================
//
// save() inferred it from state: `if (!edited.spoolman_id)`. That single line is
// why a lane could never create a new spool once it carried a link — and a lane
// keeps its link across an eject by design. So a user who put a genuinely
// different spool in that lane had no way to say so: the save PATCHed the OLD
// spool instead, which is exactly the corruption reported.
//
// The three outcomes the maintainer specified are now first-class:
//   UpdateLinked      — correcting the linked spool's details
//   CreateAndRebind   — different physical spool; create a new one, leave the old alone
//   UnlinkLocalOnly   — stop tracking this in Spoolman; keep values lane-local

TEST_CASE("SpoolmanSlotSaver CreateAndRebind creates even when a link exists",
          "[spoolman][slot_saver][intent]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SlotInfo original = make_test_slot(); // spoolman_id = 42
    SlotInfo edited = original;
    edited.brand = "Creality";
    edited.material = "PLA";
    edited.color_rgb = 0xE53935;

    SpoolmanSlotSaver saver(&api);
    bool done = false;
    SaveResult captured;
    saver.save(original, edited, SpoolmanSlotSaver::LinkIntent::CreateAndRebind,
               [&](const SaveResult& r) {
                   done = true;
                   captured = r;
               });

    REQUIRE(done);
    CHECK(captured.success);
    // A NEW spool must exist, and the previously linked one must be untouched.
    CHECK(captured.created_new_spool);
    CHECK(captured.new_spool_id != 0);
    CHECK(captured.new_spool_id != original.spoolman_id);
    for (const auto& rec : api.spoolman_mock().spool_updates) {
        CHECK(rec.spool_id != original.spoolman_id);
    }
}

TEST_CASE("SpoolmanSlotSaver UnlinkLocalOnly writes nothing to Spoolman",
          "[spoolman][slot_saver][intent]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "PETG";
    edited.remaining_weight_g = 111.0f;

    SpoolmanSlotSaver saver(&api);
    bool done = false;
    saver.save(original, edited, SpoolmanSlotSaver::LinkIntent::UnlinkLocalOnly,
               [&](const SaveResult& r) {
                   done = true;
                   CHECK(r.success);
               });

    REQUIRE(done);
    CHECK(api.spoolman_mock().spool_updates.empty());
    CHECK(api.spoolman_mock().weight_updates.empty());
}

TEST_CASE("SpoolmanSlotSaver UpdateLinked still patches the linked spool",
          "[spoolman][slot_saver][intent]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = original.remaining_weight_g - 100.0f;

    SpoolmanSlotSaver saver(&api);
    bool done = false;
    saver.save(original, edited, SpoolmanSlotSaver::LinkIntent::UpdateLinked,
               [&](const SaveResult& r) {
                   done = true;
                   CHECK(r.success);
               });

    REQUIRE(done);
    CHECK_FALSE(api.spoolman_mock().weight_updates.empty());
}

TEST_CASE("build_spool_patches: a spool weight edit reaches the spool the display reads",
          "[spoolman][slot_saver]") {
    // Spoolman copies the filament's spool_weight onto a spool when it is
    // created and serves the spool's own value from then on, which is what
    // the edit modal shows.
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    std::optional<SpoolInfo> original;
    api.spoolman().get_spoolman_spool(
        1, [&](const std::optional<SpoolInfo>& s) { original = s; }, nullptr);
    REQUIRE(original.has_value());
    SpoolInfo edited = *original;
    edited.spool_weight_g = original->spool_weight_g + 55.0;

    nlohmann::json spool_patch;
    nlohmann::json filament_patch;
    SpoolmanSlotSaver::build_spool_patches(*original, edited, spool_patch, filament_patch);
    if (!spool_patch.empty()) {
        api.spoolman().update_spoolman_spool(edited.id, spool_patch, nullptr, nullptr);
    }
    if (!filament_patch.empty()) {
        api.spoolman().update_spoolman_filament(edited.filament_id, filament_patch, nullptr,
                                                nullptr);
    }

    std::optional<SpoolInfo> reread;
    api.spoolman().get_spoolman_spool(
        1, [&](const std::optional<SpoolInfo>& s) { reread = s; }, nullptr);
    REQUIRE(reread.has_value());
    CHECK(reread->spool_weight_g == Catch::Approx(edited.spool_weight_g));
}
