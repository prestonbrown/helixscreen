// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// AFC-shaped mock: reports manages_active_spool()==true the way the real AFC
/// backend does. AmsBackendMock itself never does (it has no firmware behind
/// it), so the F2LNLQCC regression has to stage the capability here.
class ManagesActiveSpoolMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    [[nodiscard]] bool manages_active_spool() const override {
        return true;
    }
};

/// A Spoolman record the identity cache will accept (mirrors the helper in
/// test_spoolman_identity_cache.cpp — needs a name to be cacheable).
SpoolInfo make_spool(int id, std::string vendor, std::string filament_name, std::string material) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = std::move(vendor);
    spool.filament_name = std::move(filament_name);
    spool.material = std::move(material);
    spool.color_hex = "FFB6C1";
    spool.filament_id = 300 + id;
    spool.vendor_id = 400 + id;
    spool.remaining_weight_g = 850.0;
    spool.initial_weight_g = 1000.0;
    return spool;
}

struct CommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    CommitFixture() : api(client, get_printer_state()) {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // A previous test file's SpoolmanManager::deinit_subjects() may have
        // latched its shutdown flag — every static identity entry point
        // (cache_identity / find_identity / invalidate_identity) no-ops while
        // it is set. init_subjects() unlatches it.
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    ~CommitFixture() override {
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; queued backend-event
        // syncs from this test must not leak into the next one.
        helix::ui::UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    /// Install a mock backend + the mock API into AmsState, seed slot 0 with
    /// spoolman_id, and return the mock API (mirrors the wiring shape of
    /// test_consumption_sink_ams.cpp / test_spoolman_identity_cache.cpp).
    MoonrakerAPIMock* setup(int spoolman_id) {
        return install(std::make_unique<AmsBackendMock>(4), spoolman_id);
    }

    /// Same, but with a backend whose manages_active_spool() reports true.
    MoonrakerAPIMock* setup_manages_active_spool(int spoolman_id) {
        auto owned = std::make_unique<ManagesActiveSpoolMock>(4);
        owned->set_afc_mode(true);
        return install(std::move(owned), spoolman_id);
    }

  private:
    MoonrakerAPIMock* install(std::unique_ptr<AmsBackendMock> owned, int spoolman_id) {
        backend = owned.get();
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);

        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = spoolman_id;
        backend->set_slot_info(0, slot, /*persist=*/false);
        return &api;
    }
};

} // namespace

TEST_CASE("commit_slot_edit clears server active spool on unlink", "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(169);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling.
    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the server-side active spool was cleared.
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
    // And the edit itself reached the backend slot.
    REQUIRE(f.backend->get_slot_info(0).spoolman_id == 0);
}

TEST_CASE("commit_slot_edit leaves server active spool alone on a no-link clear",
          "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(0);

    // Another lane's spool is active server-side. The unlink arm must not
    // touch it just because THIS slot's edit happened to be a clear.
    mock_api->spoolman_mock().set_active_spool(77, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 77);

    // A clear on a slot that never had a Spoolman link (original and edited
    // spoolman_id both 0): NO set_active_spool call may fire — not even a
    // clear(0), which would unlink whatever other lane the server tracks.
    // (The mock's demo data links every lane, so stage a link-less one.)
    SlotInfo seeded = f.backend->get_slot_info(1);
    seeded.material = "PLA";
    seeded.spoolman_id = 0;
    f.backend->set_slot_info(1, seeded, /*persist=*/false);

    SlotInfo original = f.backend->get_slot_info(1);
    REQUIRE(original.spoolman_id == 0);

    SlotInfo cleared = original;
    cleared.material.clear();

    AmsError err = AmsState::instance().commit_slot_edit(1, original, cleared);
    REQUIRE(err.success());

    // REQUIRED: the active spool id is UNCHANGED.
    CHECK(mock_api->spoolman_mock().get_mock_active_spool_id() == 77);
}

TEST_CASE("commit_slot_edit invalidates identity cache on link change", "[ams][spoolman][commit]") {
    CommitFixture f;
    f.setup(169);

    SpoolmanManager::cache_identity(make_spool(169, "Polymaker", "Ambrosia Pink", "PLA"));
    SpoolmanManager::cache_identity(make_spool(170, "eSUN", "Silk Blue", "PETG"));
    REQUIRE(SpoolmanManager::find_identity(169).has_value());
    REQUIRE(SpoolmanManager::find_identity(170).has_value());

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.spoolman_id = 170; // relink 169 -> 170

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the OLD spool's cached identity was dropped...
    CHECK_FALSE(SpoolmanManager::find_identity(169).has_value());
    // ...while the newly linked spool's cache entry survived untouched.
    CHECK(SpoolmanManager::find_identity(170).has_value());
}

TEST_CASE("commit_slot_edit propagates set_slot_info failure", "[ams][commit]") {
    CommitFixture f;
    f.setup(169);
    auto& ams = AmsState::instance();

    // Seed the slot subjects from the backend; the color is derived from the
    // backend, not hardcoded, so the final assertion is independent.
    const int seeded_color = static_cast<int>(f.backend->get_slot_info(0).color_rgb);
    ams.sync_from_backend();
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(0)) == seeded_color);

    // Drift the backend's slot 0 color behind AmsState's back. If a failed
    // commit still ran sync_from_backend(), this color would land in the
    // subject — that is exactly what must NOT happen.
    SlotInfo drifted = f.backend->get_slot_info(0);
    drifted.color_rgb = 0xCC2244;
    f.backend->set_slot_info(0, drifted, /*persist=*/false);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.spoolman_id = 0;

    // Slot 99 does not exist on a 4-slot backend -> set_slot_info fails.
    AmsError err = ams.commit_slot_edit(99, original, edited);

    // REQUIRED: the backend failure propagates to the caller.
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::INVALID_SLOT);

    // REQUIRED: sync_from_backend() was NOT re-run — the subject still shows
    // the color from the last explicit sync, not the drifted backend value.
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(0)) == seeded_color);
}

TEST_CASE("context-menu clear wipes slot and clears server active spool",
          "[ams][commit][context-menu]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(169);

    // Give the slot a material so the wipe itself is observable, not just the
    // unlink. The dispatch constructs its own cleared copy from get_slot_info.
    SlotInfo seeded = f.backend->get_slot_info(0);
    seeded.material = "PLA";
    f.backend->set_slot_info(0, seeded, /*persist=*/false);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling
    // when the quick-clear only wiped the backend slot.
    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    // Drive the actual context-menu dispatch the way both AMS panels do.
    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));

    // REQUIRED: the backend slot was wiped...
    const SlotInfo after = f.backend->get_slot_info(0);
    REQUIRE(after.spoolman_id == 0);
    REQUIRE(after.material.empty());
    // ...AND the server-side active spool was cleared — the F2LNLQCC fix
    // (a restart must not re-assert the cleared spool).
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("commit_slot_edit clears active spool even when backend manages it",
          "[ams][spoolman][commit][regression]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup_manages_active_spool(169);

    // Premise of the regression: the backend reports that firmware manages the
    // active spool (AFC sends SET_SPOOL_ID on load). The old
    // sync_active_spool_after_edit() (since removed) gated on this and never
    // cleared — but AFC's SET_SPOOL_ID SPOOL_ID= does NOT unlink server-side
    // either.
    REQUIRE(AmsState::instance().get_backend()->manages_active_spool());

    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the clear fired anyway.
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
}
