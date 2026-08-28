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
 * AMS_DEFAULT_SLOT_COLOR with the tool name as a placeholder spool_name, and
 * that runs on every set_discovered_tools(), i.e. on every rediscovery. On a
 * 4-hotend changer that is the whole per-tool colour scheme, gone.
 *
 * These tests drive the layering with a null API, so the store itself is absent
 * and only the in-memory half runs. That is deliberate: the bug being pinned is
 * the re-layering, not the Moonraker round-trip.
 */

#include "ui_update_queue.h"

#include "../test_helpers/toolchanger_test_access.h"
#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

class SlotMemoryHelper : public AmsBackendToolChanger {
  public:
    explicit SlotMemoryHelper(int tool_count) : AmsBackendToolChanger(nullptr, nullptr) {
        set_tools(tool_count);
        running_ = true;
    }

    ~SlotMemoryHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()>) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
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

SlotInfo blue_petg() {
    SlotInfo info;
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

} // namespace

TEST_CASE("A tool's spool metadata survives rediscovery", "[ams][toolchanger][slot_memory]") {
    SlotMemoryHelper h(4);
    REQUIRE(h.set_slot_info(1, blue_petg(), /*persist=*/true).success());

    // The reconnect path: AmsState calls set_discovered_tools() again, which
    // re-runs initialize_tools() and resets every slot to default grey.
    h.set_tools(4);

    auto slot = h.get_slot_info(1);
    CHECK(slot.color_rgb == 0x1E5AA8);
    CHECK(slot.material == "PETG");
    CHECK(slot.brand == "Polymaker");
    CHECK(slot.spool_name == "Blue PETG 1kg");
    CHECK(slot.spoolman_id == 42);
    CHECK(slot.remaining_weight_g == 730);
}

TEST_CASE("Rediscovery does not leak one tool's spool onto another",
          "[ams][toolchanger][slot_memory]") {
    SlotMemoryHelper h(4);
    REQUIRE(h.set_slot_info(1, blue_petg(), /*persist=*/true).success());
    h.set_tools(4);

    // Slot 0 was never edited: it must still read the untouched default, not
    // slot 1's colour.
    auto untouched = h.get_slot_info(0);
    CHECK(untouched.color_rgb == AMS_DEFAULT_SLOT_COLOR);
    CHECK(untouched.material.empty());
}

TEST_CASE("A status frame does not undo the user's edit", "[ams][toolchanger][slot_memory]") {
    SlotMemoryHelper h(4);
    REQUIRE(h.set_slot_info(2, blue_petg(), /*persist=*/true).success());

    // refresh_slot_statuses_locked() runs inside the parse and rewrites slot
    // status; the override has to be re-layered after it, not before.
    h.feed_ready(0);

    auto slot = h.get_slot_info(2);
    CHECK(slot.color_rgb == 0x1E5AA8);
    CHECK(slot.material == "PETG");
}

TEST_CASE("persist=false is a preview, not a memory", "[ams][toolchanger][slot_memory]") {
    SlotMemoryHelper h(4);
    SlotInfo info = blue_petg();
    REQUIRE(h.set_slot_info(1, info, /*persist=*/false).success());

    // Visible immediately, because set_slot_info still writes the live SlotInfo.
    CHECK(h.get_slot_info(1).color_rgb == 0x1E5AA8);

    // But nothing was staged, so the wipe takes it.
    h.set_tools(4);
    CHECK(h.get_slot_info(1).color_rgb == AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE("An edit that also remaps a tool keeps both", "[ams][toolchanger][slot_memory]") {
    // set_slot_info() does double duty: metadata AND an ASSIGN_TOOL remap when
    // mapped_tool changed. The remap path returns early, so a persist placed
    // after it would silently drop the metadata on exactly this call.
    SlotMemoryHelper h(4);

    SlotInfo info = blue_petg();
    info.mapped_tool = 3; // slot 1 should answer to T3

    REQUIRE(h.set_slot_info(1, info, /*persist=*/true).success());

    REQUIRE(h.sent().size() == 1);
    CHECK(h.sent()[0] == "ASSIGN_TOOL TOOL=T1 N=3");

    h.set_tools(4);
    CHECK(h.get_slot_info(1).color_rgb == 0x1E5AA8);
    CHECK(h.get_slot_info(1).material == "PETG");
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
class StoreBackedHelper : public AmsBackendToolChanger {
  public:
    explicit StoreBackedHelper(IMoonrakerAPI* api, int tool_count)
        : AmsBackendToolChanger(api, nullptr) {
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

    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string&, std::function<void()>) override {
        return AmsErrorHelper::success();
    }
};

} // namespace

TEST_CASE("Tool-changer slot metadata round-trips through Moonraker",
          "[ams][toolchanger][slot_memory][filament_slot_override][slow]") {
    ScopedCacheDir tmp("roundtrip");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // --- session 1: the user edits tool 1 -----------------------------------
    {
        StoreBackedHelper h(&api, 4);
        ToolChangerTestAccess::call_on_started(h);

        // The SHARED namespace, not a private one. AFC and Happy Hare must use a
        // private namespace because their Klipper plugins own lane_data and AFC
        // wipes it every boot; klipper-toolchanger has no such plugin, so these
        // records are meant to interoperate.
        CHECK(ToolChangerTestAccess::store_namespace(h) == "lane_data");

        REQUIRE(h.set_slot_info(1, blue_petg(), /*persist=*/true).success());
    }

    // --- what actually landed in the DB -------------------------------------
    // T<n>, NOT laneN. lane_key_style_for(TOOL_CHANGER) picks the Tool style so
    // HelixScreen overwrites Mainsail's records for the same tool instead of
    // duplicating them into a second key nobody else reads.
    auto stored = api.mock_get_db_value("lane_data", "T1");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["lane"] == "1"); // inner index stays 0-based
    CHECK(stored["material"] == "PETG");
    CHECK(stored["color"] == "#1E5AA8");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);

    // The laneN key must NOT also exist, or two records describe one tool and
    // whichever a reader happens to pick decides what the user sees.
    CHECK(api.mock_get_db_value("lane_data", "lane2").is_null());

    // --- session 2: restart, nothing in memory ------------------------------
    {
        StoreBackedHelper fresh(&api, 4);
        // Before the load, the slot is whatever initialize_tools() built.
        CHECK(fresh.get_slot_info(1).color_rgb == AMS_DEFAULT_SLOT_COLOR);

        ToolChangerTestAccess::call_on_started(fresh);

        // set_discovered_tools() ran in the constructor, before the store
        // loaded, so the slots predate the overrides. The load has to re-layer
        // them itself or the panel reads grey until the first status frame.
        auto slot = fresh.get_slot_info(1);
        CHECK(slot.color_rgb == 0x1E5AA8);
        CHECK(slot.material == "PETG");
        CHECK(slot.brand == "Polymaker");
        CHECK(slot.spoolman_id == 42);

        // A tool the user never touched stays untouched.
        CHECK(fresh.get_slot_info(0).color_rgb == AMS_DEFAULT_SLOT_COLOR);
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
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendToolChanger backend(&api, &client);
    backend.set_discovered_tools({"T0", "T1"});

    // If the load moves back into additional_start_checks(), this call never
    // returns and the test times out rather than failing cleanly. That is still
    // a far better signal than a SIGTERM two files away.
    REQUIRE(backend.start().success());
    CHECK(backend.is_running());

    backend.stop();
    helix::ui::UpdateQueue::instance().drain();
}
