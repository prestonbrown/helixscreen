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

#include "ui_update_queue.h"

#include "../test_helpers/afc_test_access.h"
#include "../test_helpers/happy_hare_test_access.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_types.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

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

class StoreBackedHappyHare : public helix::AmsBackendHappyHare {
  public:
    explicit StoreBackedHappyHare(IMoonrakerAPI* api) : helix::AmsBackendHappyHare(api, nullptr) {
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
    helix::AmsError execute_gcode(const std::string&, std::function<void()>) override {
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
};

class StoreBackedAfc : public helix::AmsBackendAfc {
  public:
    explicit StoreBackedAfc(IMoonrakerAPI* api) : helix::AmsBackendAfc(api, nullptr) {
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
    helix::AmsError execute_gcode(const std::string&, std::function<void()>) override {
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
};

} // namespace

// =============================================================================
// The shared helper, on its own
// =============================================================================

TEST_CASE("make_loaded_override_store returns the store and its loaded map together",
          "[ams][filament_slot_override]") {
    ScopedOverrideCacheDir cache("helper");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_set_db_value("private-ns", "lane2",
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
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

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
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // --- session 1: the user edits gate 1 -----------------------------------
    {
        StoreBackedHappyHare hh(&api);
        hh.on_started();
        CHECK(helix::HappyHareTestAccess::store_namespace(hh) == "helix-screen-hh-overrides");

        REQUIRE(hh.set_slot_info(1, user_edit(), /*persist=*/true).success());
    }

    // The record has to be in the DB, under Happy Hare's own namespace. Gate 1
    // is the 0-based inner index, so the outer key is the 1-based "lane2".
    auto stored = api.mock_get_db_value("helix-screen-hh-overrides", "lane2");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1");
    CHECK(stored["material"] == "PETG");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // Nothing in the SHARED namespace: that one belongs to the Happy Hare
    // plugin, which rewrites it on every Klipper boot.
    CHECK(api.mock_get_db_value("lane_data", "lane2").is_null());

    // --- session 2: relaunch, nothing in memory -----------------------------
    {
        StoreBackedHappyHare fresh(&api);
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

TEST_CASE("AFC slot identity survives a restart", "[ams][afc][filament_slot_override]") {
    ScopedOverrideCacheDir cache("afc");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    {
        StoreBackedAfc afc(&api);
        afc.on_started();
        CHECK(helix::AfcTestAccess::store_namespace(afc) == "helix-screen-afc-overrides");

        REQUIRE(afc.set_slot_info(1, user_edit(), /*persist=*/true).success());
    }

    auto stored = api.mock_get_db_value("helix-screen-afc-overrides", "lane2");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1");
    CHECK(stored["material"] == "PETG");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // AFC's plugin deletes the shared namespace on every boot and full-POSTs
    // each lane record, so a record written there is gone by the next launch.
    CHECK(api.mock_get_db_value("lane_data", "lane2").is_null());

    {
        StoreBackedAfc fresh(&api);
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
