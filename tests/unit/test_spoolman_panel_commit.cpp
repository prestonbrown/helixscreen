// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_spoolman.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/spoolman_panel_test_access.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A Spoolman record the identity cache will accept (mirrors the helper in
/// test_ams_state_commit_slot.cpp — needs a name to be cacheable).
SpoolInfo make_spool(int id, std::string filament_name) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = "Polymaker";
    spool.filament_name = std::move(filament_name);
    spool.material = "PLA";
    spool.color_hex = "FFB6C1";
    return spool;
}

/// ExternalSpoolCommitFixture's settings isolation (test_external_spool.cpp —
/// the S5 arm writes settings.json) plus the mock-API wiring shape of
/// CommitFixture (test_ams_state_commit_slot.cpp), plus the panel itself with
/// the global API pointer its set_active_spool() reads. The panel is
/// constructed WITHOUT create(): the commit path under test touches no
/// widgets (update_active_indicators self-guards on an un-setup list view).
struct SpoolmanPanelCommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    SpoolmanPanel panel;
    std::string temp_dir;
    std::string config_path;

    SpoolmanPanelCommitFixture() : api(client, get_printer_state()) {
        temp_dir = std::filesystem::temp_directory_path().string() +
                   "/helix_spoolman_panel_commit_" + std::to_string(rand());
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Same cross-test contamination guard as ExternalSpoolCommitFixture:
        // Config::init() restores from backups when the config file is
        // missing, so stale backups would leak in.
        std::filesystem::remove(AppConstants::Update::config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::env_backup_fallback());

        Config::get_instance()->init(config_path);
        // Resets the in-memory override too, not just the settings record.
        AmsState::instance().clear_external_spool_info();

        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // Unlatch SpoolmanManager's shutdown flag so the static identity
        // entry points (cache_identity / find_identity) are live (see
        // CommitFixture).
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();

        // Absorb the mock client's deferred discovery updates before any
        // identity is cached: discovery pushes Spoolman availability, and
        // SpoolmanManager clears the identity cache on every availability
        // notify (LVGL notifies on set, not on change). Same recipe as
        // test_spoolman_identity_cache.cpp.
        helix::ui::UpdateQueue::instance().drain();
        get_printer_state().set_spoolman_available(true);
        helix::ui::UpdateQueue::instance().drain();

        ams.set_moonraker_api(&api);
        // SpoolmanPanel::set_active_spool fetches the API through the global.
        set_moonraker_api(&api);
    }

    ~SpoolmanPanelCommitFixture() override {
        // Detach the mocks BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        set_moonraker_api(nullptr);
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; the commit path
        // queues its store write, which must not leak into the next test.
        helix::ui::UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
        Config::get_instance()->clear_path();
        std::filesystem::remove_all(temp_dir);
    }

    /// Link spoolman_id as the external spool on every store the panel's
    /// assignment replaces (settings + server), the state #1283 staged.
    void seed_external_link(int spoolman_id) {
        SlotInfo seeded;
        seeded.spoolman_id = spoolman_id;
        seeded.material = "PLA";
        AmsState::instance().set_external_spool_info(seeded);
        api.spoolman_mock().set_mock_spoolman_enabled(true);
        api.spoolman_mock().set_active_spool(spoolman_id, nullptr, nullptr);
    }
};

} // namespace

TEST_CASE("panel set_active_spool commits through the shared external-spool path",
          "[spoolman][commit][1283]") {
    SpoolmanPanelCommitFixture f;
    f.seed_external_link(169);

    SpoolmanManager::cache_identity(make_spool(169, "Ambrosia Pink"));
    SpoolmanManager::cache_identity(make_spool(170, "Jet Black"));
    SpoolmanManager::cache_identity(make_spool(999, "Bystander"));
    REQUIRE(SpoolmanManager::find_identity(169).has_value());
    REQUIRE(SpoolmanManager::find_identity(170).has_value());

    SpoolmanPanelTestAccess::seed_cached_spools(
        f.panel, {make_spool(169, "Ambrosia Pink"), make_spool(170, "Jet Black")});

    SpoolmanPanelTestAccess::set_active_spool(f.panel, 170);
    helix::ui::UpdateQueue::instance().drain();

    // S1 — the server was told which spool is active.
    REQUIRE(f.api.spoolman_mock().get_mock_active_spool_id() == 170);
    // S5 — the settings store now holds spool 170.
    auto persisted = helix::SettingsManager::instance().get_external_spool_info();
    REQUIRE(persisted.has_value());
    CHECK(persisted->spoolman_id == 170);
    // S6 — the replaced link's identity cache entry was dropped while the
    // newly assigned spool's (and an uninvolved bystander's) survived. This
    // is the assertion the old hand-rolled panel writer (direct
    // set_external_spool_info) failed: it never invalidated.
    CHECK_FALSE(SpoolmanManager::find_identity(169).has_value());
    CHECK(SpoolmanManager::find_identity(170).has_value());
    CHECK(SpoolmanManager::find_identity(999).has_value());
    // The panel's own state advanced too.
    CHECK(SpoolmanPanelTestAccess::active_spool_id(f.panel) == 170);
}

TEST_CASE("panel set_active_spool server failure leaves every store untouched",
          "[spoolman][commit][1283]") {
    SpoolmanPanelCommitFixture f;
    f.seed_external_link(169);

    // Fail every Spoolman call like an unavailable component.
    f.api.spoolman_mock().set_mock_spoolman_enabled(false);

    SpoolmanPanelTestAccess::seed_cached_spools(f.panel, {make_spool(170, "Jet Black")});

    SpoolmanPanelTestAccess::set_active_spool(f.panel, 170);
    helix::ui::UpdateQueue::instance().drain();

    // Server-first semantics preserved: on server failure the store subset
    // must NOT run — settings still hold the old link, and the panel's
    // active id never advanced.
    auto persisted = helix::SettingsManager::instance().get_external_spool_info();
    REQUIRE(persisted.has_value());
    CHECK(persisted->spoolman_id == 169);
    CHECK(SpoolmanPanelTestAccess::active_spool_id(f.panel) == -1);
}
