// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_unlink_stale_echo.cpp
 * @brief A lane unlinked from HelixScreen stays unlinked while AFC keeps
 *        restating the old spool_id (remember_spool lanes), across a reconnect
 *        or restart too (#1717).
 *
 * A live subscription only carries changed fields, so the restatement that
 * matters is the full snapshot after a reconnect. Each restart here is a fresh
 * backend loading the first one's records from the same mock Moonraker DB.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/unique_temp_dir.h"
#include "ams_backend_afc.h"
#include "app_globals.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcUnlinkHelper : public AmsBackendAfc {
  public:
    explicit AfcUnlinkHelper(IMoonrakerAPI* api) : AmsBackendAfc(api, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
        on_started();
        helix::ui::UpdateQueue::instance().drain();
    }

    ~AfcUnlinkHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed_stepper(const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper lane1"] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    void forget_displayed_id() {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.get_mut(0)->info.spoolman_id = 0;
    }

    [[nodiscard]] bool sent_spool_id_write() const {
        for (const auto& g : captured_gcodes) {
            if (g.rfind("SET_SPOOL_ID", 0) == 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] int spool_id() const {
        return get_slot_info(0).spoolman_id;
    }

    void unlink() {
        link(0);
    }

    void link(int spool_id) {
        SlotInfo info = get_slot_info(0);
        info.spoolman_id = spool_id;
        set_slot_info(0, info, /*persist=*/true);
    }

    [[nodiscard]] int persisted_unlink() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = overrides_.find(0);
        return it == overrides_.end() ? 0 : it->second.unlinked_spool_id;
    }

    [[nodiscard]] std::optional<helix::ams::FilamentSlotOverride> stored_override() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = overrides_.find(0);
        if (it == overrides_.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<std::string> captured_gcodes;
};

namespace {

nlohmann::json loaded(const nlohmann::json& spool_id) {
    return {{"prep", true}, {"load", true}, {"spool_id", spool_id}};
}

/// One mock Moonraker, with its database, that successive backends share the
/// way a reconnect or restart does; plus a local override cache of its own so
/// no other test's records load as a fallback.
struct UnlinkFixture : LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, get_printer_state()};
    std::filesystem::path previous_cache = helix::ams::detail::slot_override_cache_dir_ref();
    std::filesystem::path cache = helix::test::unique_temp_dir("afc_unlink_cache");

    UnlinkFixture() {
        std::filesystem::create_directories(cache);
        helix::ams::detail::slot_override_cache_dir_ref() = cache;
    }

    ~UnlinkFixture() override {
        helix::ams::detail::slot_override_cache_dir_ref() = previous_cache;
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);
    }

    /// Unlink spool 127 from lane1 in one backend, run @p before_restart on
    /// it, then hand a fresh backend on the same database to @p after_restart.
    template <typename Before, typename After>
    void across_restart(Before before_restart, After after_restart) {
        {
            AfcUnlinkHelper afc(&api);
            afc.feed_stepper(loaded(127));
            afc.unlink();
            REQUIRE(afc.spool_id() == 0);
            before_restart(afc);
        }
        AfcUnlinkHelper afc(&api);
        after_restart(afc);
    }
};

} // namespace

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: restated old spool_id stays cleared (#1717)",
                 "[1717][ams][afc]") {
    AfcUnlinkHelper afc(&api);
    afc.feed_stepper(loaded(42));
    REQUIRE(afc.spool_id() == 42);

    afc.unlink();
    REQUIRE(afc.spool_id() == 0);

    afc.feed_stepper(loaded(42));
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 0);
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: a different id afterwards is accepted (#1717)",
                 "[1717][ams][afc]") {
    AfcUnlinkHelper afc(&api);
    afc.feed_stepper(loaded(42));
    afc.unlink();
    afc.feed_stepper(loaded(42));
    REQUIRE(afc.spool_id() == 0);

    afc.feed_stepper(loaded(77));
    CHECK(afc.spool_id() == 77);
    // The unlink ended with the new spool: the old id is a real link again.
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 42);
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: no unlink means a restated id is accepted (#1717)",
                 "[1717][ams][afc]") {
    AfcUnlinkHelper afc(&api);
    afc.feed_stepper(loaded(42));
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 42);
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: ends on lane unload and on a null id (#1717)",
                 "[1717][ams][afc]") {
    AfcUnlinkHelper afc(&api);

    SECTION("lane unload") {
        afc.feed_stepper(loaded(42));
        afc.unlink();
        afc.feed_stepper({{"prep", false}, {"load", false}});
        afc.feed_stepper(loaded(42));
        CHECK(afc.spool_id() == 42);
    }

    SECTION("null spool_id from AFC") {
        afc.feed_stepper(loaded(42));
        afc.unlink();
        afc.feed_stepper(loaded(nullptr));
        afc.feed_stepper(loaded(42));
        CHECK(afc.spool_id() == 42);
    }
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: no clear sent means no unlink (#1717)",
                 "[1717][ams][afc]") {
    AfcUnlinkHelper afc(&api);
    afc.feed_stepper(loaded(42));
    // The slot shows no id although firmware holds one, so the edit has no
    // link to clear and sends no SET_SPOOL_ID.
    afc.forget_displayed_id();
    SlotInfo info = afc.get_slot_info(0);
    afc.set_slot_info(0, info, /*persist=*/true);
    CHECK_FALSE(afc.sent_spool_id_write());
    afc.feed_stepper(loaded(42));
    CHECK(afc.spool_id() == 42);
}

// =============================================================================
// The unlink survives a reconnect and a restart
// =============================================================================

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: a restart restating the old id stays unlinked (#1717)",
                 "[1717][ams][afc]") {
    SECTION("unlink through an edit") {
        across_restart([](AfcUnlinkHelper&) {},
                       [](AfcUnlinkHelper& afc) {
                           afc.feed_stepper(loaded(127));
                           CHECK(afc.spool_id() == 0);
                       });
    }

    SECTION("Clear Spool, which also drops the slot's override") {
        across_restart(
            [](AfcUnlinkHelper& afc) {
                afc.clear_slot_override(0);
                // Only the marker stays: the clear drops every identity field.
                const auto kept = afc.stored_override();
                REQUIRE(kept.has_value());
                CHECK(kept->unlinked_spool_id == 127);
                CHECK(kept->spoolman_id == 0);
                CHECK(kept->material.empty());
                CHECK_FALSE(kept->color_set);
                CHECK(kept->catalog_id.empty());
            },
            [](AfcUnlinkHelper& afc) {
                afc.feed_stepper(loaded(127));
                CHECK(afc.spool_id() == 0);
            });
    }
}

TEST_CASE_METHOD(UnlinkFixture,
                 "AFC unlink: a different id after a restart ends the unlink and links (#1717)",
                 "[1717][ams][afc]") {
    across_restart([](AfcUnlinkHelper&) {},
                   [](AfcUnlinkHelper& afc) {
                       afc.feed_stepper(loaded(77));
                       CHECK(afc.spool_id() == 77);
                       CHECK(afc.persisted_unlink() == 0);
                       afc.feed_stepper(loaded(127));
                       CHECK(afc.spool_id() == 127);
                   });
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: linking from HelixScreen ends the unlink (#1717)",
                 "[1717][ams][afc]") {
    across_restart(
        [](AfcUnlinkHelper& afc) {
            afc.link(127);
            CHECK(afc.persisted_unlink() == 0);
        },
        [](AfcUnlinkHelper& afc) {
            afc.feed_stepper(loaded(127));
            CHECK(afc.spool_id() == 127);
        });
}

TEST_CASE_METHOD(UnlinkFixture, "AFC unlink: emptying the lane ends the unlink (#1717)",
                 "[1717][ams][afc]") {
    across_restart(
        [](AfcUnlinkHelper& afc) {
            afc.feed_stepper({{"prep", false}, {"load", false}});
            CHECK(afc.persisted_unlink() == 0);
        },
        [](AfcUnlinkHelper& afc) {
            afc.feed_stepper(loaded(127));
            CHECK(afc.spool_id() == 127);
        });
}

TEST_CASE_METHOD(UnlinkFixture,
                 "AFC unlink: a remember_spool lane keeps the unlink through a spool swap (#1717)",
                 "[1717][ams][afc]") {
    // AFC skips clear_values() on eject for these lanes, so the spool inserted
    // next is reported under the old id.
    const nlohmann::json remembering = {{"remember_spool", true}};
    across_restart(
        [&](AfcUnlinkHelper& afc) {
            afc.feed_stepper(remembering);
            afc.feed_stepper({{"prep", false}, {"load", false}});
            afc.feed_stepper(loaded(127));
            CHECK(afc.spool_id() == 0);
            CHECK(afc.persisted_unlink() == 127);
        },
        [&](AfcUnlinkHelper& afc) {
            afc.feed_stepper(remembering);
            afc.feed_stepper(loaded(127));
            CHECK(afc.spool_id() == 0);
        });
}

TEST_CASE_METHOD(UnlinkFixture,
                 "AFC unlink: the marker loads from HelixScreen's namespace, not AFC's lane_data "
                 "(#1717)",
                 "[1717][ams][afc]") {
    {
        AfcUnlinkHelper afc(&api);
        afc.feed_stepper(loaded(127));
        afc.unlink();
    }
    const auto stored = api.mock_get_db_value("helix-screen-afc-overrides", "lane1");
    REQUIRE(stored.is_object());
    CHECK(stored.value("helix_unlinked_spool_id", 0) == 127);

    // AFC rewrites its own lane_data record without our key, and an empty
    // local cache leaves nothing to fall back on.
    api.mock_set_db_value("lane_data", "lane1", {{"lane", "0"}, {"spool_id", 127}, {"td", ""}});
    std::filesystem::remove_all(cache);
    std::filesystem::create_directories(cache);
    {
        AfcUnlinkHelper afc(&api);
        CHECK(afc.persisted_unlink() == 127);
        afc.feed_stepper(loaded(127));
        CHECK(afc.spool_id() == 0);
    }
}

TEST_CASE("FilamentSlotOverride unlink marker reads from both stores (#1717)",
          "[1717][filament_slot_override]") {
    const auto record = helix::ams::from_lane_data_record(
        nlohmann::json{{"lane", "0"}, {"helix_unlinked_spool_id", 127}});
    REQUIRE(record.has_value());
    CHECK(record->second.unlinked_spool_id == 127);

    helix::ams::FilamentSlotOverride o;
    o.unlinked_spool_id = 127;
    const auto cached = helix::ams::to_json(o);
    CHECK(cached.value("unlinked_spool_id", 0) == 127);
    CHECK(helix::ams::from_json(cached).unlinked_spool_id == 127);
}
