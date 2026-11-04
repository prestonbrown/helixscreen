// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_override_store_lifecycle.cpp
 * @brief A user's slot identity has to still be there after a restart.
 *
 * `helix::ams::make_loaded_override_store()` is the one way a backend gets a
 * FilamentSlotOverrideStore for its per-slot overrides, and it returns the
 * store together with the map it loaded. These tests cover the helper directly
 * and then through the two backends that keep their records in a private
 * Moonraker namespace, because their own Klipper plugins own `lane_data` and
 * rewrite it on boot.
 *
 * The backend assertions are a full save-then-reload across two instances, not
 * a call count. Every save_async and clear_async site is guarded on
 * `override_store_`, so a backend holding no store persists nothing while every
 * in-session read of `overrides_` still hands back the user's edit - the
 * failure is invisible until the next launch.
 */

#include "../test_helpers/ace_test_access.h"
#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/happy_hare_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "ams_backend_ace.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

/// Per-test read-cache sandbox. The store falls back to its on-disk cache when
/// the Moonraker DB is unreachable, and one file serves every backend in the
/// process, so a neighbouring test's records are reachable from here unless
/// each test gets its own directory.
struct ScopedOverrideCacheDir {
    std::filesystem::path path;
    std::filesystem::path previous;

    explicit ScopedOverrideCacheDir(const std::string& suffix) {
        previous = helix::ams::detail::slot_override_cache_dir_ref();
        path = std::filesystem::temp_directory_path() /
               ("ams_override_lifecycle_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        helix::ams::detail::slot_override_cache_dir_ref() = path;
    }

    ~ScopedOverrideCacheDir() {
        helix::ams::detail::slot_override_cache_dir_ref() = previous;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/// The identity a user attaches in the AMS edit modal. Only `material` and
/// `color_rgb` have any firmware counterpart on these backends; brand, spool
/// name and the weights exist nowhere but the override record.
helix::SlotInfo user_edit() {
    helix::SlotInfo info;
    info.color_rgb = 0x1E5AA8;
    info.color_name = "Blue";
    info.material = "PETG";
    info.brand = "Polymaker";
    info.spool_name = "Blue PETG 1kg";
    info.spoolman_id = 42;
    info.remaining_weight_g = 730;
    info.total_weight_g = 1000;
    return info;
}

/// The same identity typed onto a lane the user never linked to Spoolman.
///
/// An unlinked lane is where authorship has to be carried by the record
/// itself: a record naming a spool id is the server's word whatever else it
/// says, so a linked lane cannot show whether the user's own statement
/// survived the trip through the store.
helix::SlotInfo unlinked_user_edit() {
    helix::SlotInfo info = user_edit();
    info.spoolman_id = 0;
    info.spoolman_vendor_id = 0;
    return info;
}

class StoreBackedHappyHare : public helix::AmsBackendHappyHare {
  public:
    explicit StoreBackedHappyHare(helix::IMoonrakerAPI* api)
        : helix::AmsBackendHappyHare(api, nullptr) {
        std::vector<std::string> gates{"0", "1", "2", "3"};
        helix::HappyHareTestAccess::slots(*this).initialize("MMU", gates);
        system_info_.total_slots = 4;
        running_ = true;
    }

    ~StoreBackedHappyHare() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return helix::AmsErrorHelper::success();
    }

    /// The REAL on_started(), which is where the backend acquires its loaded
    /// store. start() is final on AmsSubscriptionBackend and drags in
    /// subscription setup a store round-trip has no use for; the four config
    /// queries on_started() also runs are null-client guarded, so with no
    /// client they are inert and the store load is all that happens.
    using helix::AmsBackendHappyHare::on_started;

    /// A printer.mmu frame carrying per-gate Spoolman ids. This is the parse
    /// path that re-supplies the user's identity on top of the gate map.
    void feed_gate_spool_ids(const std::vector<int>& ids) {
        nlohmann::json mmu;
        mmu["gate_spool_id"] = ids;
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        helix::HappyHareTestAccess::handle_status_update(*this, notification);
    }

    /// A printer.mmu frame stating the gate map's own material and colour,
    /// which mmu_vars.cfg remembers and the backend files as a vendor cache.
    ///
    /// Sent twice because the gate map is filed after the gates are painted
    /// within a single frame, so the paint that sees it is the next poll's.
    void feed_gate_identity(const std::vector<std::string>& materials,
                            const std::vector<int>& colors) {
        for (int pass = 0; pass < 2; ++pass) {
            nlohmann::json mmu;
            mmu["gate_material"] = materials;
            mmu["gate_color_rgb"] = colors;
            mmu["gate_spool_id"] = std::vector<int>(materials.size(), 0);
            nlohmann::json params;
            params["mmu"] = mmu;
            nlohmann::json notification;
            notification["params"] = nlohmann::json::array({params, 0.0});
            helix::HappyHareTestAccess::handle_status_update(*this, notification);
        }
    }
};

class StoreBackedAfc : public helix::AmsBackendAfc {
  public:
    explicit StoreBackedAfc(helix::IMoonrakerAPI* api) : helix::AmsBackendAfc(api, nullptr) {
        std::vector<std::string> lanes{"lane1", "lane2", "lane3", "lane4"};
        helix::AfcTestAccess::initialize_slots(*this, lanes);
        system_info_.total_slots = 4;
        running_ = true;
    }

    ~StoreBackedAfc() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return helix::AmsErrorHelper::success();
    }

    /// As above. AFC's on_started() additionally runs detect_afc_version() and
    /// query_lane_data() (both null-client guarded) and load_afc_configs(),
    /// which defers a completion callback onto the UpdateQueue. Drain it here,
    /// while the backend is still alive, rather than leaving it for teardown.
    void on_started() override {
        helix::AmsBackendAfc::on_started();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// An AFC_stepper frame stating the lane's own material and colour, which
    /// the AFC plugin keeps in its own config and the backend files as a
    /// vendor cache.
    void feed_lane_identity(const std::string& lane_name, const std::string& material,
                            const std::string& color) {
        nlohmann::json params;
        params["AFC_stepper " + lane_name] =
            nlohmann::json{{"material", material}, {"color", color}, {"status", "Loaded"}};
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        helix::AfcTestAccess::handle_status_update(*this, notification);
    }
};

class StoreBackedAce : public helix::AmsBackendAce {
  public:
    /// Unlike the AFC and Happy Hare fixtures above, this one needs a client:
    /// AmsBackendAce::on_started() sends its Klipper query through client_
    /// without a null guard.
    StoreBackedAce(helix::IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : helix::AmsBackendAce(api, client) {
        running_ = true;
    }

    ~StoreBackedAce() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return helix::AmsErrorHelper::success();
    }

    /// The REAL on_started(), which is where the backend acquires its loaded
    /// store.
    using helix::AmsBackendAce::on_started;

    /// A filament_hub frame stating each bay's own status, colour and type.
    /// The hub reports no identity of its own, so what it carries here is its
    /// memory of what was last in the bay, which the backend files as a vendor
    /// cache.
    void feed_hub_bays(const std::vector<std::tuple<std::string, uint32_t, std::string>>& bays) {
        nlohmann::json slots = nlohmann::json::array();
        for (const auto& [status, rgb, material] : bays) {
            slots.push_back(nlohmann::json{
                {"status", status},
                {"color",
                 nlohmann::json::array({(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF})},
                {"type", material}});
        }
        helix::AceTestAccess::parse_ace(
            *this, nlohmann::json{{"model", "ACE Pro"}, {"status", "ready"}, {"slots", slots}});
    }
};

} // namespace

// =============================================================================
// The shared helper, on its own
// =============================================================================

TEST_CASE("make_loaded_override_store returns the store and its loaded map together",
          "[ams][filament_slot_override]") {
    ScopedOverrideCacheDir cache("helper");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    mock_printer.client.mock_db_set("private-ns", "lane2",
                                    nlohmann::json{{"lane", "1"}, {"material", "PETG"}});

    auto loaded = helix::ams::make_loaded_override_store(&api, "somebackend", helix::AmsType::AFC,
                                                         "[TEST]", "private-ns");

    REQUIRE(loaded.store != nullptr);
    CHECK(loaded.store->namespace_for_test() == "private-ns");
    // The load has already run: the caller never gets the chance to skip it.
    REQUIRE(loaded.overrides.count(1) == 1);
    CHECK(loaded.overrides.at(1).material == "PETG");
}

TEST_CASE("make_loaded_override_store defaults to the shared lane_data namespace",
          "[ams][filament_slot_override]") {
    ScopedOverrideCacheDir cache("helper_default_ns");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    auto loaded =
        helix::ams::make_loaded_override_store(&api, "somebackend", helix::AmsType::AFC, "[TEST]");
    REQUIRE(loaded.store != nullptr);
    CHECK(loaded.store->namespace_for_test() == "lane_data");
}

TEST_CASE("make_loaded_override_store with no API yields no store and no overrides",
          "[ams][filament_slot_override]") {
    // A backend constructed without a connection, which is how most unit
    // fixtures build one. Returning an empty pair rather than a store that
    // cannot reach anything is what keeps every save_async guard meaningful.
    auto loaded = helix::ams::make_loaded_override_store(nullptr, "somebackend",
                                                         helix::AmsType::AFC, "[TEST]");
    CHECK(loaded.store == nullptr);
    CHECK(loaded.overrides.empty());
}

// =============================================================================
// Through the backends that use a private namespace
// =============================================================================

TEST_CASE("Happy Hare slot identity survives a restart",
          "[ams][happyhare][filament_slot_override]") {
    ScopedOverrideCacheDir cache("hh");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // --- session 1: the user edits gate 1 -----------------------------------
    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> hh_reg(&api);
        StoreBackedHappyHare& hh = *hh_reg;
        hh.on_started();
        CHECK(helix::HappyHareTestAccess::store_namespace(hh) == "helix-screen-hh-overrides");

        REQUIRE(helix::test::apply_edit(hh, 1, user_edit()).success());
    }

    // The record has to be in the DB, under Happy Hare's own namespace. Gate 1
    // is the 0-based inner index, so the outer key is the 1-based "lane2".
    auto stored = mock_printer.client.mock_db_get("helix-screen-hh-overrides", "lane2");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1");
    CHECK(stored["material"] == "PETG");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // Nothing in the SHARED namespace: that one belongs to the Happy Hare
    // plugin, which rewrites it on every Klipper boot.
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane2").is_null());

    // --- session 2: relaunch, nothing in memory -----------------------------
    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> fresh_reg(&api);
        StoreBackedHappyHare& fresh = *fresh_reg;
        CHECK(helix::HappyHareTestAccess::overrides(fresh).empty());

        fresh.on_started();

        const auto& loaded = helix::HappyHareTestAccess::overrides(fresh);
        REQUIRE(loaded.count(1) == 1);
        CHECK(loaded.at(1).brand == "Polymaker");
        CHECK(loaded.at(1).spool_name == "Blue PETG 1kg");
        CHECK(loaded.at(1).material == "PETG");
        CHECK(loaded.at(1).color_rgb == 0x1E5AA8u);
        CHECK(loaded.at(1).spoolman_id == 42);
        CHECK(loaded.at(1).total_weight_g == 1000.0f);

        // What the user actually sees. The gate map carries the spool id and
        // nothing else the edit modal offered, so the first status frame is
        // where the override has to re-appear on the slot itself. Gate 1 is
        // reported with the same id the override holds, so neither the re-bind
        // nor the eject rule fires.
        fresh.feed_gate_spool_ids({0, 42, 0, 0});

        auto slot = fresh.get_slot_info(1);
        CHECK(slot.brand == "Polymaker");
        CHECK(slot.spool_name == "Blue PETG 1kg");
        CHECK(slot.material == "PETG");
        CHECK(slot.color_rgb == 0x1E5AA8u);

        // A gate the user never touched stays untouched.
        CHECK(fresh.get_slot_info(0).brand.empty());
    }
}

TEST_CASE("Happy Hare cleared slot override does not return after a restart",
          "[ams][happyhare][filament_slot_override]") {
    ScopedOverrideCacheDir cache("hh_clear");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // --- session 1: the user edits gate 1, then clears it -------------------
    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> hh_reg(&api);
        StoreBackedHappyHare& hh = *hh_reg;
        hh.on_started();
        REQUIRE(helix::test::apply_edit(hh, 1, user_edit()).success());
        hh.clear_slot_override(1);
    }

    CHECK(mock_printer.client.mock_db_get("helix-screen-hh-overrides", "lane2").is_null());

    // --- session 2: relaunch, nothing in memory -----------------------------
    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> fresh_reg(&api);
        StoreBackedHappyHare& fresh = *fresh_reg;
        fresh.on_started();

        const auto& loaded = helix::HappyHareTestAccess::overrides(fresh);
        CHECK(loaded.count(1) == 0);

        auto slot = fresh.get_slot_info(1);
        CHECK(slot.brand.empty());
        CHECK(slot.spool_name.empty());
    }
}

TEST_CASE("AFC slot identity survives a restart", "[ams][afc][filament_slot_override]") {
    ScopedOverrideCacheDir cache("afc");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    {
        helix::test::RegisteredBackend<StoreBackedAfc> afc_reg(&api);
        StoreBackedAfc& afc = *afc_reg;
        afc.on_started();
        CHECK(helix::AfcTestAccess::store_namespace(afc) == "helix-screen-afc-overrides");

        REQUIRE(helix::test::apply_edit(afc, 1, user_edit()).success());
    }

    auto stored = mock_printer.client.mock_db_get("helix-screen-afc-overrides", "lane2");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1");
    CHECK(stored["material"] == "PETG");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // AFC's plugin deletes the shared namespace on every boot and full-POSTs
    // each lane record, so a record written there is gone by the next launch.
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane2").is_null());

    {
        helix::test::RegisteredBackend<StoreBackedAfc> fresh_reg(&api);
        StoreBackedAfc& fresh = *fresh_reg;
        CHECK(helix::AfcTestAccess::overrides(fresh).empty());

        fresh.on_started();

        const auto& loaded = helix::AfcTestAccess::overrides(fresh);
        REQUIRE(loaded.count(1) == 1);
        CHECK(loaded.at(1).brand == "Polymaker");
        CHECK(loaded.at(1).spool_name == "Blue PETG 1kg");
        CHECK(loaded.at(1).material == "PETG");
        CHECK(loaded.at(1).color_rgb == 0x1E5AA8u);
        CHECK(loaded.at(1).spoolman_id == 42);
        CHECK(loaded.at(1).total_weight_g == 1000.0f);
    }
}

// =============================================================================
// A typed identity outranks firmware after the app is restarted
// =============================================================================
//
// The record is the only thing that survives the restart, so it has to carry
// WHO said what and not just what was said. A record that comes back unsigned
// is something the store merely remembered, which firmware's own word for the
// lane outranks, and the next poll after launch paints over the user.

TEST_CASE(
    "AFC keeps the material and colour the user typed when firmware disagrees after a restart",
    "[ams][afc][filament_slot_override]") {
    ScopedOverrideCacheDir cache("afc_authorship");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    {
        helix::test::RegisteredBackend<StoreBackedAfc> afc_reg(&api);
        StoreBackedAfc& afc = *afc_reg;
        afc.on_started();
        REQUIRE(helix::test::apply_edit(afc, 1, unlinked_user_edit()).success());
    }

    {
        helix::test::RegisteredBackend<StoreBackedAfc> fresh_reg(&api);
        StoreBackedAfc& fresh = *fresh_reg;
        fresh.on_started();

        // AFC's own config says this lane holds red PLA.
        fresh.feed_lane_identity("lane2", "PLA", "#FF0000");

        const auto slot = fresh.get_slot_info(1);
        CHECK(slot.material == "PETG");
        CHECK(slot.color_rgb == 0x1E5AA8u);
    }
}

TEST_CASE("Happy Hare keeps the material and colour the user typed when the gate map disagrees "
          "after a restart",
          "[ams][happyhare][filament_slot_override]") {
    ScopedOverrideCacheDir cache("hh_authorship");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> hh_reg(&api);
        StoreBackedHappyHare& hh = *hh_reg;
        hh.on_started();
        REQUIRE(helix::test::apply_edit(hh, 1, unlinked_user_edit()).success());
    }

    {
        helix::test::RegisteredBackend<StoreBackedHappyHare> fresh_reg(&api);
        StoreBackedHappyHare& fresh = *fresh_reg;
        fresh.on_started();

        // mmu_vars.cfg says gate 1 holds red PLA.
        fresh.feed_gate_identity({"PLA", "PLA", "PLA", "PLA"},
                                 {0xFF0000, 0xFF0000, 0xFF0000, 0xFF0000});

        const auto slot = fresh.get_slot_info(1);
        CHECK(slot.material == "PETG");
        CHECK(slot.color_rgb == 0x1E5AA8u);
    }
}

TEST_CASE("ACE keeps the material and colour the user typed when the hub disagrees after a restart",
          "[ams][ace][filament_slot_override]") {
    ScopedOverrideCacheDir cache("ace_authorship");
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    const std::vector<std::tuple<std::string, uint32_t, std::string>> hub_says_red_pla{
        {"ready", 0xFF0000, "PLA"},
        {"empty", 0x000000, ""},
        {"empty", 0x000000, ""},
        {"empty", 0x000000, ""},
    };

    {
        helix::test::RegisteredBackend<StoreBackedAce> ace_reg(&api, &client);
        StoreBackedAce& ace = *ace_reg;
        ace.on_started();
        ace.feed_hub_bays(hub_says_red_pla);
        REQUIRE(helix::test::apply_edit(ace, 0, unlinked_user_edit()).success());
    }

    {
        helix::test::RegisteredBackend<StoreBackedAce> fresh_reg(&api, &client);
        StoreBackedAce& fresh = *fresh_reg;
        fresh.on_started();

        fresh.feed_hub_bays(hub_says_red_pla);

        const auto slot = fresh.get_slot_info(0);
        CHECK(slot.material == "PETG");
        CHECK(slot.color_rgb == 0x1E5AA8u);
    }
}
