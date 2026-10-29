// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_slot_memory.cpp
 * @brief Per-tool spool metadata has to survive rediscovery.
 *
 * AmsBackendToolChanger was the only AMS backend with no
 * FilamentSlotOverrideStore. Its own comment admitted the consequence: "No
 * override store on this backend, so this in-memory copy is the only thing
 * keeping the editor's catalog pick visible until the next parse."
 *
 * The wipe is concrete. klipper-toolchanger reports nothing about filament -
 * parse_tool_state() reads `mounted` and `active` and nothing else - so unlike
 * every other backend there is no firmware reading underneath for the user's
 * edit to layer over. initialize_tools() then resets every slot to
 * kAmsDefaultSlotColor with the tool name as a placeholder spool_name, and
 * that runs on every set_discovered_tools(), i.e. on every rediscovery. On a
 * 4-hotend changer that is the whole per-tool colour scheme, gone.
 *
 * These tests drive the layering with a null API, so the store itself is absent
 * and only the in-memory half runs. That is deliberate: the bug being pinned is
 * the re-layering, not the Moonraker round-trip.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "consumption_sink.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

class SlotMemoryHelper : public helix::AmsBackendToolChanger {
  public:
    explicit SlotMemoryHelper(int tool_count) : helix::AmsBackendToolChanger(nullptr, nullptr) {
        set_tools(tool_count);
        running_ = true;
    }

    ~SlotMemoryHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()>) override {
        sent_.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    /// Re-run discovery exactly as AmsState does on reconnect. This is the wipe.
    void set_tools(int tool_count) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
    }

    void feed_ready(int tool_number) {
        handle_status_update(nlohmann::json{
            {"method", "notify_status_update"},
            {"params", nlohmann::json::array(
                           {nlohmann::json{{"toolchanger",
                                            {{"status", "ready"}, {"tool_number", tool_number}}}},
                            0.0})}});
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

/// Redirects the local override cache for the test's lifetime.
///
/// NOT the same as TmpCacheDir in test_filament_slot_override_store.cpp: that
/// one pairs with FilamentSlotOverrideStoreTestAccess::set_cache_directory(),
/// which needs the store object. Here the store lives inside the backend and is
/// built by additional_start_checks(), so the only reachable lever is the
/// HELIX_CONFIG_DIR env var that get_user_config_dir() reads. Without this the
/// store's cache write lands in the developer's real ~/.helixscreen.
struct ScopedCacheDir {
    std::filesystem::path path;
    std::string previous;
    bool had_previous = false;

    explicit ScopedCacheDir(const std::string& suffix) {
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR"); prev != nullptr) {
            previous = prev;
            had_previous = true;
        }
        path = std::filesystem::temp_directory_path() /
               ("toolchanger_slot_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        ::setenv("HELIX_CONFIG_DIR", path.c_str(), 1);
    }

    ~ScopedCacheDir() {
        if (had_previous) {
            ::setenv("HELIX_CONFIG_DIR", previous.c_str(), 1);
        } else {
            ::unsetenv("HELIX_CONFIG_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

helix::SlotInfo blue_petg() {
    helix::SlotInfo info;
    info.color_rgb = 0x1E5AA8;
    info.color_name = "Blue";
    info.material = "PETG";
    info.brand = "Polymaker";
    info.spool_name = "Blue PETG 1kg";
    info.spoolman_id = 42;
    info.spoolman_filament_id = 55;
    info.remaining_weight_g = 730;
    info.total_weight_g = 1000;
    return info;
}

/// What Spoolman says spool 42, the one blue_petg() links, is.
SpoolInfo blue_petg_spool() {
    SpoolInfo spool;
    spool.id = 42;
    spool.filament_id = 55;
    spool.vendor = "Polymaker";
    spool.filament_name = "Blue PETG 1kg";
    spool.material = "PETG";
    spool.color_hex = "1E5AA8";
    spool.remaining_weight_g = 730;
    spool.initial_weight_g = 1000;
    return spool;
}

} // namespace

TEST_CASE("A tool's spool metadata survives rediscovery", "[ams][toolchanger][slot_memory]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    helix::test::edit_slot_as_user(h, 1, blue_petg());
    REQUIRE(h.get_slot_info(1).spoolman_filament_id == 55);
    helix::test::spool_states(h, 1, blue_petg_spool());

    // The reconnect path: AmsState calls set_discovered_tools() again, which
    // re-runs initialize_tools() and resets every slot to default grey.
    h.set_tools(4);

    auto slot = h.get_slot_info(1);
    CHECK(slot.color_rgb == 0x1E5AA8);
    CHECK(slot.material == "PETG");
    CHECK(slot.brand == "Polymaker");
    CHECK(slot.spool_name == "Blue PETG 1kg");
    CHECK(slot.spoolman_id == 42);
    CHECK(slot.spoolman_filament_id == 55);
    CHECK(slot.remaining_weight_g == 730);
}

TEST_CASE("Rediscovery does not leak one tool's spool onto another",
          "[ams][toolchanger][slot_memory]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    helix::test::edit_slot_as_user(h, 1, blue_petg());
    helix::test::spool_states(h, 1, blue_petg_spool());
    h.set_tools(4);

    // Slot 0 was never edited: it must still read the untouched default, not
    // slot 1's colour.
    auto untouched = h.get_slot_info(0);
    CHECK(untouched.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(untouched.material.empty());
}

TEST_CASE("Clear Spool blanks a tool's record live and after rediscovery",
          "[ams][toolchanger][slot_memory][1661]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;

    // #1661's own shape: an UNLINKED pick - no Spoolman id, just a colour
    // pick, a typed weight and a colour name a person entered.
    helix::SlotInfo picked = blue_petg();
    picked.spoolman_id = 0;
    helix::test::edit_slot_as_user(h, 1, picked);
    REQUIRE(h.get_slot_info(1).color_rgb == 0x1E5AA8);
    REQUIRE(h.get_slot_info(1).remaining_weight_g == 730);

    // The Clear Spool funnel's sequence (ui_ams_detail.cpp): the commit
    // states the blank, then clear_slot_override() drops the standing record
    // whole - the half that makes live agree with a restart.
    helix::SlotInfo cleared = h.get_slot_info(1);
    cleared.material.clear();
    cleared.color_rgb = helix::kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.multi_color_hexes.clear();
    cleared.brand.clear();
    cleared.catalog_id.clear();
    cleared.product_name.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;
    helix::test::edit_slot_as_user(h, 1, cleared);
    h.clear_slot_override(1);

    // Blank on the very next read.
    auto slot = h.get_slot_info(1);
    CHECK(slot.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(slot.material.empty());
    CHECK(slot.color_name.empty());
    CHECK(slot.brand.empty());
    CHECK(slot.remaining_weight_g == -1);

    // And blank after the reconnect wipe: no record survives to re-layer.
    h.set_tools(4);
    slot = h.get_slot_info(1);
    CHECK(slot.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(slot.material.empty());
    CHECK(slot.color_name.empty());
    CHECK(slot.remaining_weight_g == -1);
}

TEST_CASE("A clear without a preceding commit still drops the record",
          "[ams][toolchanger][slot_memory][1661]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    helix::SlotInfo picked = blue_petg();
    picked.spoolman_id = 0;
    helix::test::edit_slot_as_user(h, 1, picked);
    REQUIRE(h.get_slot_info(1).color_rgb == 0x1E5AA8);

    // The funnel always commits a blank first, but clear_slot_override()'s own
    // contract is "drop the standing record" - a caller that skips the commit
    // must not have the pick re-layered by the next rediscovery.
    h.clear_slot_override(1);
    h.set_tools(4);

    auto slot = h.get_slot_info(1);
    CHECK(slot.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(slot.material.empty());
    CHECK(slot.remaining_weight_g == -1);
}

TEST_CASE("A status frame does not undo the user's edit", "[ams][toolchanger][slot_memory]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    helix::test::edit_slot_as_user(h, 2, blue_petg());
    helix::test::spool_states(h, 2, blue_petg_spool());

    // refresh_slot_statuses_locked() runs inside the parse and rewrites slot
    // status; the override has to be re-layered after it, not before.
    h.feed_ready(0);

    auto slot = h.get_slot_info(2);
    CHECK(slot.color_rgb == 0x1E5AA8);
    CHECK(slot.material == "PETG");
}

TEST_CASE("persist=false is a preview, not a memory", "[ams][toolchanger][slot_memory]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    helix::SlotInfo info = blue_petg();
    REQUIRE(h.sync_external_identity(1, info).success());

    // Visible immediately, because the sync still writes the live SlotInfo.
    CHECK(h.get_slot_info(1).color_rgb == 0x1E5AA8);

    // But nothing was staged, so the wipe takes it.
    h.set_tools(4);
    CHECK(h.get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);
}

TEST_CASE("An identity sync never remaps a tool", "[ams][toolchanger][slot_memory][1652]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;

    helix::SlotInfo info = h.get_slot_info(1);
    const int mapped_before = info.mapped_tool;
    REQUIRE(mapped_before != 3);
    // Only a person's edit moves the tool map, so a synced value naming another
    // tool number leaves the lane answering to the one it had.
    info.mapped_tool = 3;

    REQUIRE(h.sync_external_identity(1, info).success());

    CHECK(h.sent().empty());
    CHECK(h.get_slot_info(1).mapped_tool == mapped_before);
}

TEST_CASE("An identity sync carries multi-color, temperatures and the Spoolman vendor",
          "[ams][toolchanger][slot_memory][1936]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;

    helix::SlotInfo info = blue_petg();
    info.multi_color_hexes = "#1E5AA8,#F2C14E";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    info.spoolman_vendor_id = 17;

    REQUIRE(h.sync_external_identity(1, info).success());

    const auto slot = h.get_slot_info(1);
    CHECK(slot.multi_color_hexes == info.multi_color_hexes);
    CHECK(slot.nozzle_temp_min == 230);
    CHECK(slot.nozzle_temp_max == 250);
    CHECK(slot.bed_temp == 80);
    CHECK(slot.spoolman_vendor_id == 17);
}

TEST_CASE("An edit that also remaps a tool keeps both", "[ams][toolchanger][slot_memory]") {
    // apply_user_edit() does double duty: metadata AND an ASSIGN_TOOL remap when
    // mapped_tool changed. The remap path returns early, so a persist placed
    // after it would silently drop the metadata on exactly this call.
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;

    helix::SlotInfo info = blue_petg();
    info.mapped_tool = 3; // slot 1 should answer to T3

    helix::test::edit_slot_as_user(h, 1, info);
    helix::test::spool_states(h, 1, blue_petg_spool());

    REQUIRE(h.sent().size() == 1);
    CHECK(h.sent()[0] == "ASSIGN_TOOL TOOL=\"tool T1\" N=3");

    h.set_tools(4);
    CHECK(h.get_slot_info(1).color_rgb == 0x1E5AA8);
    CHECK(h.get_slot_info(1).material == "PETG");
}

// ============================================================================
// A slot write reaches AmsState's slot subjects
// ============================================================================

namespace {

/// A registered tool changer whose slots AmsState publishes, over the
/// PrinterState subject AmsState::init_subjects() observes.
struct PublishedToolChanger : LVGLTestFixture {
    std::optional<helix::test::RegisteredBackend<SlotMemoryHelper>> registration;

    PublishedToolChanger() {
        auto& ams = helix::AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);
        registration.emplace(4);
    }

    ~PublishedToolChanger() override {
        registration.reset();
        helix::AmsState::instance().deinit_subjects();
    }

    [[nodiscard]] SlotMemoryHelper& backend() const {
        return **registration;
    }

    /// Run every queued update, including the full sync a status frame asks for.
    static void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// One pass of the update queue: what a slot event queued, and nothing that
    /// work queues in turn.
    static void deliver_events() {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(PublishedToolChanger, "a meter tick on a tool changer reaches the slot's subjects",
                 "[ams][toolchanger][consumption_sink][slot_refresh]") {
    SlotMemoryHelper& h = backend();
    auto& ams = helix::AmsState::instance();

    helix::SlotInfo seeded = h.get_slot_info(0);
    seeded.material = "PLA";
    seeded.remaining_weight_g = 500.0F;
    seeded.total_weight_g = 1000.0F;
    REQUIRE(h.sync_external_identity(0, seeded).success());
    h.feed_ready(0);
    ams.sync_from_backend();
    settle();
    REQUIRE(h.is_filament_loaded());
    REQUIRE(h.get_current_slot() == 0);
    REQUIRE(lv_subject_get_int(ams.get_slot_fill_subject(0)) == 50);
    REQUIRE(std::string(lv_subject_get_string(ams.get_current_weight_text_subject())) == "500g");

    helix::AmsSlotSink sink(0, 0);
    sink.snapshot(0.0F);
    REQUIRE(sink.is_trackable());
    // About 100 g of 1.75 mm PLA, far past the sink's write threshold.
    sink.apply_delta(33600.0F);
    const helix::SlotInfo metered = h.get_slot_info(0);
    REQUIRE(metered.remaining_weight_g < 450.0F);

    deliver_events();

    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(0)) == metered.display_fill_pct());
    CHECK(std::string(lv_subject_get_string(ams.get_slot_remaining_subject(0))) ==
          metered.remaining_length_display());
    char loaded_weight[32];
    snprintf(loaded_weight, sizeof(loaded_weight), "%.0fg", metered.remaining_weight_g);
    CHECK(std::string(lv_subject_get_string(ams.get_current_weight_text_subject())) ==
          loaded_weight);
}

TEST_CASE_METHOD(PublishedToolChanger,
                 "a person's edit on a tool changer reaches the slot's subjects",
                 "[ams][toolchanger][slot_refresh]") {
    SlotMemoryHelper& h = backend();
    auto& ams = helix::AmsState::instance();
    ams.sync_from_backend();
    settle();
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(1)) ==
            static_cast<int>(helix::kAmsDefaultSlotColor));

    SECTION("an identity edit") {
        helix::test::edit_slot_as_user(h, 1, blue_petg());
        REQUIRE(h.sent().empty());
    }
    SECTION("an edit that also remaps the tool through ASSIGN_TOOL") {
        helix::SlotInfo remapped = blue_petg();
        remapped.mapped_tool = 3;
        helix::test::edit_slot_as_user(h, 1, remapped);
        REQUIRE(h.sent().size() == 1);
    }
    REQUIRE(h.get_slot_info(1).color_rgb == 0x1E5AA8);

    deliver_events();

    CHECK(lv_subject_get_int(ams.get_slot_color_subject(1)) == 0x1E5AA8);
    CHECK(std::string(lv_subject_get_string(ams.get_slot_material_subject(1))) == "PETG");
}

// ============================================================================
// Moonraker round-trip
// ============================================================================
//
// The tests above run with a null API on purpose: they pin the re-layering,
// which is where the wipe was. They cannot see whether anything reaches
// Moonraker at all. These do, because "survives rediscovery" and "survives a
// restart" are different claims and only the first was proven.

namespace {

/// A backend wired to a mock Moonraker, so additional_start_checks() actually
/// builds the store and does the blocking load.
class StoreBackedHelper : public helix::AmsBackendToolChanger {
  public:
    explicit StoreBackedHelper(helix::IMoonrakerAPI* api, int tool_count)
        : helix::AmsBackendToolChanger(api, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
    }

    ~StoreBackedHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    helix::AmsError execute_gcode(const std::string&) override {
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string&, helix::SharedCallback<void()>) override {
        return helix::AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE("Tool-changer slot metadata round-trips through Moonraker",
          "[ams][toolchanger][slot_memory][filament_slot_override][slow]") {
    ScopedCacheDir tmp("roundtrip");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // --- session 1: the user edits tool 1 -----------------------------------
    {
        // Sequential, not simultaneous: each harness opens and closes before the
        // next, since a second live registration would clear the first.
        helix::test::RegisteredBackend<StoreBackedHelper> h_reg(&api, 4);
        StoreBackedHelper& h = *h_reg;
        helix::ToolChangerTestAccess::call_on_started(h);

        // The SHARED namespace, not a private one. AFC and Happy Hare must use a
        // private namespace because their Klipper plugins own lane_data and AFC
        // wipes it every boot; klipper-toolchanger has no such plugin, so these
        // records are meant to interoperate.
        CHECK(helix::ToolChangerTestAccess::store_namespace(h) == "lane_data");

        helix::test::edit_slot_as_user(h, 1, blue_petg());
        helix::test::spool_states(h, 1, blue_petg_spool());
    }

    // --- what actually landed in the DB -------------------------------------
    // T<n>, NOT laneN. lane_key_style_for(TOOL_CHANGER) picks the Tool style so
    // HelixScreen overwrites Mainsail's records for the same tool instead of
    // duplicating them into a second key nobody else reads.
    auto stored = mock_printer.client.mock_db_get("lane_data", "T1");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1"); // inner index stays 0-based
    CHECK(stored["material"] == "PETG");
    CHECK(stored["color"] == "#1E5AA8");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // The laneN key must NOT also exist, or two records describe one tool and
    // whichever a reader happens to pick decides what the user sees.
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane2").is_null());

    // --- session 2: restart, nothing in memory ------------------------------
    {
        // Sequential, not simultaneous: each harness opens and closes before the
        // next, since a second live registration would clear the first.
        helix::test::RegisteredBackend<StoreBackedHelper> fresh_reg(&api, 4);
        StoreBackedHelper& fresh = *fresh_reg;
        // Before the load, the slot is whatever initialize_tools() built.
        CHECK(fresh.get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);

        helix::ToolChangerTestAccess::call_on_started(fresh);

        // set_discovered_tools() ran in the constructor, before the store
        // loaded, so the slots predate the overrides. The load has to re-layer
        // them itself or the panel reads grey until the first status frame.
        auto slot = fresh.get_slot_info(1);
        CHECK(slot.color_rgb == 0x1E5AA8);
        CHECK(slot.material == "PETG");
        CHECK(slot.brand == "Polymaker");
        CHECK(slot.spoolman_id == 42);

        // A tool the user never touched stays untouched.
        CHECK(fresh.get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);
    }
}

TEST_CASE("A cleared tool-changer record is gone after a reload",
          "[ams][toolchanger][slot_memory][filament_slot_override][slow][1661]") {
    ScopedCacheDir tmp("clearreload");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    {
        helix::test::RegisteredBackend<StoreBackedHelper> h_reg(&api, 4);
        StoreBackedHelper& h = *h_reg;
        helix::ToolChangerTestAccess::call_on_started(h);

        helix::SlotInfo picked = blue_petg();
        picked.spoolman_id = 0;
        helix::test::edit_slot_as_user(h, 1, picked);
        // The edit is live before anything is cleared.
        REQUIRE(h.get_slot_info(1).color_rgb == 0x1E5AA8);

        h.clear_slot_override(1);
        CHECK(h.get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);
        CHECK(h.get_slot_info(1).material.empty());
    }

    // The record is GONE, not blank: a blank record would still re-apply on
    // reload, pinning the lane's declared set to nothing instead of freeing it.
    CHECK(mock_printer.client.mock_db_get("lane_data", "T1").is_null());

    // Restart: nothing in memory, nothing in the store, tool 1 reads default.
    {
        helix::test::RegisteredBackend<StoreBackedHelper> fresh_reg(&api, 4);
        StoreBackedHelper& fresh = *fresh_reg;
        helix::ToolChangerTestAccess::call_on_started(fresh);
        CHECK(fresh.get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);
        CHECK(fresh.get_slot_info(1).material.empty());
        CHECK(fresh.get_slot_info(1).spoolman_id == 0);
    }
}

TEST_CASE("Starting with a live API does not deadlock",
          "[ams][toolchanger][slot_memory][filament_slot_override][slow]") {
    // Regression guard with a blast radius of one test.
    //
    // The store load blocks the caller AND needs mutex_ to publish its result.
    // AmsSubscriptionBackend::start() calls additional_start_checks() with
    // mutex_ already held, so loading from there self-deadlocks on a
    // non-recursive mutex - the app hangs on connect, on every klipper-toolchanger
    // printer, not just in tests. on_started() runs after the lock is released.
    //
    // This was caught by the factory print-gate test, which builds and starts
    // every backend type - but it surfaced there as a SIGTERM'd shard inside 54
    // cases, which reads like infrastructure flake. Fail here instead.
    ScopedCacheDir tmp("deadlock");
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    helix::AmsBackendToolChanger backend(&api, &client);
    backend.set_discovered_tools({"T0", "T1"});

    // If the load moves back into additional_start_checks(), this call never
    // returns and the test times out rather than failing cleanly. That is still
    // a far better signal than a SIGTERM two files away.
    REQUIRE(backend.start().success());
    CHECK(backend.is_running());

    backend.stop();
    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// A lane whose only identity is its Spoolman record
// ============================================================================
//
// No stored override describes such a lane, so every place the backend lays its
// lanes onto freshly built slots has to paint it whether or not overrides_
// holds anything.

namespace {

constexpr uint32_t kSpoolmanOnlyColor = 0x2E7D32;

/// Files a Spoolman record naming a colour, and nothing else, on @p lane.
void file_spoolman_colour(helix::ams::LaneId lane) {
    helix::ams::Observation filed(helix::ams::ObservationSource::Spoolman);
    filed.spoolman_id = 42;
    filed.color_rgb = kSpoolmanOnlyColor;
    helix::ams::ingest(lane, filed);
}

} // namespace

TEST_CASE("a tool changer rediscovery keeps a lane whose only identity is its Spoolman record",
          "[lane][toolchanger][1653]") {
    helix::test::RegisteredBackend<SlotMemoryHelper> h_reg(4);
    SlotMemoryHelper& h = *h_reg;
    file_spoolman_colour(h_reg.lane(1));

    // A preview write puts a name on the live slot that nothing but the
    // rediscovery's reset takes off again, and stages no override.
    helix::SlotInfo preview = h.get_slot_info(1);
    preview.spool_name = "Preview name";
    REQUIRE(h.sync_external_identity(1, preview).success());
    REQUIRE(h.get_slot_info(1).spool_name == "Preview name");
    REQUIRE_FALSE(helix::ToolChangerTestAccess::has_overrides(h));

    h.set_tools(4);

    // The reset ran: the slot is back on its tool-name placeholder.
    REQUIRE(h.get_slot_info(1).spool_name == "T1");
    CHECK(h.get_slot_info(1).color_rgb == kSpoolmanOnlyColor);
}

TEST_CASE("a tool changer start paints a lane whose only identity is its Spoolman record",
          "[lane][toolchanger][1653][slow]") {
    ScopedCacheDir tmp("spoolman_only_start");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<StoreBackedHelper> h_reg(&api, 4);
    StoreBackedHelper& h = *h_reg;
    file_spoolman_colour(h_reg.lane(1));
    REQUIRE(h.get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);

    helix::ToolChangerTestAccess::call_on_started(h);

    // The load ran, against a store holding nothing for any tool.
    REQUIRE(helix::ToolChangerTestAccess::store_namespace(h) == "lane_data");
    REQUIRE_FALSE(helix::ToolChangerTestAccess::has_overrides(h));
    CHECK(h.get_slot_info(1).color_rgb == kSpoolmanOnlyColor);
}
