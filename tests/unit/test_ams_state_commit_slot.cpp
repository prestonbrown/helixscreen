// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_op_dispatch.h"
#include "lane_apply.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_printer_discovery.h"
#include "test_helpers/seeded_override.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

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
    [[nodiscard]] bool sets_active_spool_on_link() const override {
        return true;
    }
};

/// Happy-Hare-shaped: switches the active spool on a gate load only, so a link
/// edit of the loaded gate is never seen as one.
class ActivatesOnLoadOnlyMock : public AmsBackendMock {
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

/// CommitFixture registers its backend first, so it takes the first id block.
helix::ams::LaneId lane_of(int slot) {
    return helix::ams::lane_id_for(0, slot);
}

/// A stored record for a linked lane: the spool id and the identity that came
/// with the link.
helix::ams::FilamentSlotOverride linked_record(int spoolman_id) {
    helix::ams::FilamentSlotOverride record;
    record.spoolman_id = spoolman_id;
    record.brand = "Polymaker";
    record.material = "PLA";
    return record;
}

/// One AFC_stepper lane object, through the envelope Moonraker delivers.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
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

    /// Same, with a backend that switches the spool only on a load.
    MoonrakerAPIMock* setup_activates_on_load_only(int spoolman_id) {
        return install(std::make_unique<ActivatesOnLoadOnlyMock>(4), spoolman_id);
    }

  private:
    MoonrakerAPIMock* install(std::unique_ptr<AmsBackendMock> owned, int spoolman_id) {
        backend = owned.get();
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);

        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = spoolman_id;
        backend->sync_external_identity(0, slot);
        return &api;
    }
};

/// An AFC backend behind CommitFixture's wiring, restarted from @p stored: lane
/// 0's record in both stores, the way every backend's start leaves it.
struct AfcCommitFixture : CommitFixture {
    // Emplaced once SettingsManager's subjects exist, and torn down before
    // CommitFixture's destructor clears AmsState.
    std::optional<helix::test::RegisteredBackend<AmsBackendAfc>> registration;
    AmsBackendAfc* afc = nullptr;

    explicit AfcCommitFixture(const helix::ams::FilamentSlotOverride& stored) {
        SettingsManager::instance().init_subjects();
        registration.emplace(nullptr, nullptr);
        afc = &**registration;
        AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
        AmsState::instance().set_moonraker_api(&api);

        {
            std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
            AfcTestAccess::overrides(*afc)[0] = stored;
        }
        helix::test::file_override_as_lane_records(*afc, 0, stored);
    }

    [[nodiscard]] helix::ams::LaneId lane() const {
        return registration->lane(0);
    }

    /// Save an edit to lane 0 through the editor's commit: @p change applied to
    /// the slot as it stands when the editor opens.
    template <typename Change> void edit(Change change) {
        REQUIRE(try_edit(change).success());
    }

    /// The same commit, answering with its result rather than requiring success.
    template <typename Change> [[nodiscard]] AmsError try_edit(Change change) {
        const SlotInfo original = afc->get_slot_info(0);
        SlotInfo edited = original;
        change(edited);
        return AmsState::instance().commit_slot_edit(0, original, edited);
    }

    /// Lane 0's stored record as it stands.
    [[nodiscard]] helix::ams::FilamentSlotOverride stored() const {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
        const auto& overrides = AfcTestAccess::overrides(*afc);
        const auto kept = overrides.find(0);
        REQUIRE(kept != overrides.end());
        return kept->second;
    }

    /// Link lane 0 to @p spool through the editor, the way the picker and the
    /// QR scan do, and have Spoolman answer the fetch the link starts.
    void link(const SpoolInfo& spool) {
        edit([&spool](SlotInfo& slot) { apply_spool_to_slot(slot, spool); });
        helix::test::spool_states(*afc, 0, spool);
        REQUIRE(afc->get_slot_info(0).spoolman_id == spool.id);
    }

    /// A person picks a colour and a catalog product for whatever is on lane 0.
    void pick_colour_and_product() {
        edit([](SlotInfo& slot) {
            slot.color_rgb = 0xBCBCBC;
            slot.catalog_id = "sunlu-pla-plus-2-0";
            slot.product_name = "PLA+ 2.0";
        });
        REQUIRE(afc->get_slot_info(0).catalog_id == "sunlu-pla-plus-2-0");
    }
};

/// An AFC lane linked to spool 42 the way a restart leaves it: the stored
/// record in both stores, and firmware naming the same spool. @p stored must
/// name spool 42.
struct RestartedAfcLinkFixture : AfcCommitFixture {
    explicit RestartedAfcLinkFixture(
        const helix::ams::FilamentSlotOverride& stored = linked_record(42))
        : AfcCommitFixture(stored) {
        feed_afc_lane(*afc, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
        REQUIRE(helix::ams::resolve(helix::ams::lane_sources(lane())).spoolman_id == 42);
    }

    /// Relink the lane from spool 42 to @p spool_id through the editor.
    void relink(int spool_id) {
        SlotInfo original = afc->get_slot_info(0);
        REQUIRE(original.spoolman_id == 42);
        SlotInfo relinked = original;
        relinked.spoolman_id = spool_id;
        REQUIRE(AmsState::instance().commit_slot_edit(0, original, relinked).success());
    }

    /// The user's link to @p spool_id stands in the stored record and in the
    /// lane model after firmware's frame naming @p frame.
    void check_link_stands(int spool_id, const char* frame) {
        INFO("after the frame naming " << frame);
        {
            std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
            const auto& overrides = AfcTestAccess::overrides(*afc);
            const auto kept = overrides.find(0);
            REQUIRE(kept != overrides.end());
            CHECK(kept->second.spoolman_id == spool_id);
        }

        CHECK(helix::ams::resolve(helix::ams::lane_sources(lane())).spoolman_id == spool_id);
    }
};

} // namespace

TEST_CASE("commit_slot_edit clears server active spool on unlink", "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(169);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling.
    f.client.spoolman_mock().set_active_spool_id(169);
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());
    helix::ui::UpdateQueue::instance().drain();

    // REQUIRED: the server-side active spool was cleared.
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 0);
    // And the edit itself reached the backend slot.
    REQUIRE(f.backend->get_slot_info(0).spoolman_id == 0);
}

TEST_CASE("commit_slot_edit leaves the active spool alone when the unlinked lane is not loaded",
          "[ams][spoolman][commit][1717]") {
    CommitFixture f;
    // AFC-shaped, so no loaded-lane sync re-asserts the active spool and the
    // check reads only what the commit itself did.
    f.setup_manages_active_spool(169);
    // Lane 0 is the loaded one, and its spool is the active spool.
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    f.client.spoolman_mock().set_active_spool_id(169);

    SlotInfo original = f.backend->get_slot_info(1);
    original.spoolman_id = 147;
    f.backend->sync_external_identity(1, original);
    REQUIRE_FALSE(f.backend->slot_is_actively_loaded(1));

    SlotInfo cleared = original;
    cleared.spoolman_id = 0;
    REQUIRE(AmsState::instance().commit_slot_edit(1, original, cleared).success());

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 169);
    CHECK(f.backend->get_slot_info(1).spoolman_id == 0);
}

TEST_CASE("commit_slot_edit leaves the active spool alone when linking a lane that is not loaded",
          "[ams][spoolman][commit][1717]") {
    CommitFixture f;
    // AFC-shaped, so no loaded-lane sync re-asserts the active spool and the
    // check reads only what the commit itself did.
    f.setup_manages_active_spool(169);
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    f.client.spoolman_mock().set_active_spool_id(169);

    SlotInfo original = f.backend->get_slot_info(1);
    original.spoolman_id = 0;
    f.backend->sync_external_identity(1, original);
    REQUIRE_FALSE(f.backend->slot_is_actively_loaded(1));

    SlotInfo linked = original;
    linked.spoolman_id = 147;
    REQUIRE(AmsState::instance().commit_slot_edit(1, original, linked).success());

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 169);
    CHECK(f.backend->get_slot_info(1).spoolman_id == 147);
}

TEST_CASE("commit_slot_edit makes the spool active when linking the loaded lane",
          "[ams][spoolman][commit][1717]") {
    CommitFixture f;
    f.setup(0);
    REQUIRE(f.backend->slot_is_actively_loaded(0));

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 147;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 147);
}

TEST_CASE("commit_slot_edit leaves the active spool to a backend that takes the link itself",
          "[ams][spoolman][commit][plugin_filament]") {
    // AFC sets the active spool itself when SET_SPOOL_ID names the loaded lane.
    CommitFixture f;
    f.setup_manages_active_spool(0);
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    f.client.spoolman_mock().set_active_spool_id(5);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 147;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 5);
}

TEST_CASE("commit_slot_edit sets the spool for a backend that switches only on a load",
          "[ams][spoolman][commit][plugin_filament]") {
    // Happy Hare activates a gate's spool when the gate loads; a link of the
    // gate already loaded never reaches that, so the edit sets it.
    CommitFixture f;
    f.setup_activates_on_load_only(0);
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    f.client.spoolman_mock().set_active_spool_id(5);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 147;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 147);
}

TEST_CASE("commit_slot_edit leaves server active spool alone on a no-link clear",
          "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(0);

    // Another lane's spool is active server-side. The unlink arm must not
    // touch it just because THIS slot's edit happened to be a clear.
    f.client.spoolman_mock().set_active_spool_id(77);
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 77);

    // A clear on a slot that never had a Spoolman link (original and edited
    // spoolman_id both 0): NO set_active_spool call may fire — not even a
    // clear(0), which would unlink whatever other lane the server tracks.
    // (The mock's demo data links every lane, so stage a link-less one.)
    SlotInfo seeded = f.backend->get_slot_info(1);
    seeded.material = "PLA";
    seeded.spoolman_id = 0;
    f.backend->sync_external_identity(1, seeded);

    SlotInfo original = f.backend->get_slot_info(1);
    REQUIRE(original.spoolman_id == 0);

    SlotInfo cleared = original;
    cleared.material.clear();

    AmsError err = AmsState::instance().commit_slot_edit(1, original, cleared);
    REQUIRE(err.success());

    // REQUIRED: the active spool id is UNCHANGED.
    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 77);
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
    edited.spoolman_filament_id = 55;

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());
    CHECK(f.backend->get_slot_info(0).spoolman_filament_id == 55);

    // REQUIRED: the OLD spool's cached identity was dropped...
    CHECK_FALSE(SpoolmanManager::find_identity(169).has_value());
    // ...while the newly linked spool's cache entry survived untouched.
    CHECK(SpoolmanManager::find_identity(170).has_value());
}

TEST_CASE("commit_slot_edit propagates apply_user_edit failure", "[ams][commit]") {
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
    f.backend->sync_external_identity(0, drifted);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.spoolman_id = 0;

    // Slot 99 does not exist on a 4-slot backend -> apply_user_edit fails.
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
    f.backend->sync_external_identity(0, seeded);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling
    // when the quick-clear only wiped the backend slot.
    f.client.spoolman_mock().set_active_spool_id(169);
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 169);

    // Drive the actual context-menu dispatch the way both AMS panels do.
    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));
    helix::ui::UpdateQueue::instance().drain();

    // REQUIRED: the backend slot was wiped...
    const SlotInfo after = f.backend->get_slot_info(0);
    REQUIRE(after.spoolman_id == 0);
    REQUIRE(after.material.empty());
    // ...AND the server-side active spool was cleared — the F2LNLQCC fix
    // (a restart must not re-assert the cleared spool).
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("context-menu clear names the position in the backend's own word",
          "[ams][commit][context-menu][i18n]") {
    CommitFixture f;
    f.setup(169);

    // AmsBackendMock reports Happy Hare, whose noun is Gate. The confirmation
    // a user reads has to match the word the rest of the UI uses for the thing
    // they just cleared.
    REQUIRE(f.backend->lane_noun() == helix::ui::LaneNoun::Gate);

    std::vector<std::string> notes;
    helix::ui::set_test_notification_info_hook(
        [&notes](const std::string& msg) { notes.push_back(msg); });

    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 2, nullptr));

    helix::ui::set_test_notification_info_hook(nullptr);

    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "Gate 3 spool cleared");
}

TEST_CASE("context-menu clear on the bypass sentinel names the external spool",
          "[ams][commit][context-menu][i18n]") {
    CommitFixture f;
    f.setup(169);

    // The external spool carries an assignment, and a lane carries one too, so
    // the clear has something to remove on both sides and "it was already
    // empty" cannot pass for "it was cleared".
    SlotInfo external;
    external.material = "PETG";
    external.spoolman_id = 42;
    AmsState::instance().commit_external_spool_edit(external);
    REQUIRE(AmsState::instance().get_external_spool_info().value_or(SlotInfo{}).material == "PETG");

    SlotInfo lane = f.backend->get_slot_info(0);
    lane.material = "PLA";
    f.backend->sync_external_identity(0, lane);
    REQUIRE(f.backend->get_slot_info(0).material == "PLA");

    std::vector<std::string> notes;
    helix::ui::set_test_notification_info_hook(
        [&notes](const std::string& msg) { notes.push_back(msg); });

    REQUIRE(ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL,
                                            helix::ui::kExternalSpoolSlot, nullptr));

    helix::ui::set_test_notification_info_hook(nullptr);

    // The bypass spool has no number, so it is named rather than numbered.
    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "External spool cleared");
    CHECK(AmsState::instance().get_external_spool_info().value_or(SlotInfo{}).material.empty());
    // The sentinel is not an index into the backend: no lane was touched.
    CHECK(f.backend->get_slot_info(0).material == "PLA");
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

    f.client.spoolman_mock().set_active_spool_id(169);
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());
    helix::ui::UpdateQueue::instance().drain();

    // REQUIRED: the clear fired anyway.
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 0);
}

// ============================================================================
// The lane source model: what a commit records as the user's own declaration
// ============================================================================

TEST_CASE("an AFC relink after a restart survives its own echo",
          "[ams][commit][lane][spoolman][afc]") {
    RestartedAfcLinkFixture f;
    f.relink(99);
    CHECK(helix::ams::resolve(helix::ams::lane_sources(f.lane())).spoolman_id.value_or(-1) == 99);

    // Firmware reports the spool we wrote. That is our own write coming back,
    // so the declaration the user just made has to survive it, in the stored
    // record and in the lane model.
    feed_afc_lane(*f.afc, "lane1", {{"spool_id", 99}});
    f.check_link_stands(99, "the new spool");
}

TEST_CASE("an edit on a linked AFC lane shows the lane's resolved brand at once",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    const AmsError err = f.try_edit([](SlotInfo& slot) {
        slot.color_rgb = 0xBCBCBC;
        slot.brand = "Sunlu";
    });
    CHECK(err.partially_applied);

    // On a linked lane the brand is the spool's, so the slot shows the spool's
    // brand as every later frame paints it.
    const helix::ams::ResolvedLane resolved =
        helix::ams::resolve(helix::ams::lane_sources(f.lane()));
    REQUIRE(resolved.brand == std::string("Polymaker"));
    CHECK(f.afc->get_slot_info(0).brand == *resolved.brand);
}

TEST_CASE("registration stamps each backend with its own block", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);

    AmsBackend* primary = ams.get_backend(0);
    AmsBackend* secondary = ams.get_backend(second);
    REQUIRE(primary != nullptr);
    REQUIRE(secondary != nullptr);

    // A backend derives its lane ids from the index registration hands it, and
    // asking a backend for its own slot 0 is the only way to see that stamp.
    // Unstamped, a backend names no lane at all, so a declaration written
    // through it would be dropped instead of filed.
    CHECK(primary->backend_index() == 0);
    CHECK(secondary->backend_index() == second);
    CHECK(primary->lane_id(0) == helix::ams::lane_id_for(0, 0));
    CHECK(secondary->lane_id(0) == helix::ams::lane_id_for(second, 0));
    CHECK(secondary->lane_id(0) != primary->lane_id(0));
    CHECK(secondary->lane_id(0) != helix::ams::kInvalidLaneId);
}

TEST_CASE("an edit that moves nothing the spool owns is not a partial save",
          "[ams][commit][lane][spoolman][afc][1653]") {
    // The spool's record states fields the slot has never shown. Taking them is
    // not the user losing anything they moved, so the save is whole.
    RestartedAfcLinkFixture f;
    helix::test::spool_states(*f.afc, 0, make_spool(42, "Polymaker", "PolyLite PLA", "PLA"));
    REQUIRE(f.afc->get_slot_info(0).spool_name.empty());

    const AmsError colour_only = f.try_edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    CHECK(colour_only.success());
    CHECK_FALSE(colour_only.partially_applied);
    CHECK(f.stored().spool_name == "PolyLite PLA");
}

TEST_CASE("a fresh load re-sends its spool after the server's active spool was cleared",
          "[ams][commit][spoolman_sync]") {
    CommitFixture f;
    f.setup(9);
    f.backend->set_operation_delay(0);
    REQUIRE(f.backend->start().success());
    auto& ams = AmsState::instance();

    f.client.spoolman_mock().set_active_spool_id(0);
    ams.sync_current_loaded_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(f.backend->get_current_slot() == 0);
    REQUIRE(f.client.spoolman_mock().get_mock_active_spool_id() == 9);

    // The print ends and something else clears the active spool, then the
    // same lane loads again for the next print.
    REQUIRE(f.backend->unload_filament(0).success());
    f.backend->wait_for_operation_thread();
    ams.sync_current_loaded_from_backend();
    f.client.spoolman_mock().set_active_spool_id(0);

    REQUIRE(f.backend->load_filament(0).success());
    f.backend->wait_for_operation_thread();
    ams.sync_current_loaded_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(f.backend->get_current_slot() == 0);
    REQUIRE(f.backend->is_filament_loaded());
    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 9);
}

namespace {

/// A printer whose firmware or T<n> macros switch the active spool, staged on
/// PrinterState's discovery and the macro cache; both reset on destruction.
struct OwnedActiveSpoolFixture : CommitFixture {
    explicit OwnedActiveSpoolFixture(const nlohmann::json& objects,
                                     const nlohmann::json& config = nlohmann::json::object())
        : printer(objects, config) {}

    helix::test::ScopedPrinterDiscovery printer;

    /// Load lane 0 (linked to spool 9) from an empty active spool; the active
    /// spool afterwards.
    int load_and_sync() {
        setup(9);
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        client.spoolman_mock().set_active_spool_id(0);
        AmsState::instance().sync_current_loaded_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(backend->get_current_slot() == 0);
        return client.spoolman_mock().get_mock_active_spool_id();
    }
};

} // namespace

TEST_CASE("a lane load leaves the active spool to SpoolLink firmware",
          "[ams][commit][spoolman_sync][plugin_filament]") {
    OwnedActiveSpoolFixture f(helix::test::spoollink_printer_objects());
    CHECK(f.load_and_sync() == 0);
}

TEST_CASE("a lane load leaves the active spool to T<n> macros that set it",
          "[ams][commit][spoolman_sync][plugin_filament]") {
    OwnedActiveSpoolFixture f(
        nlohmann::json::array({"toolhead", "extruder", "gcode_macro T0", "gcode_macro T1"}),
        nlohmann::json{
            {"gcode_macro T0", {{"gcode", "SET_ACTIVE_SPOOL ID=3\nACTIVATE_EXTRUDER"}}}});
    CHECK(f.load_and_sync() == 0);
}

TEST_CASE("a lane load sets the active spool when nothing else switches it",
          "[ams][commit][spoolman_sync][plugin_filament]") {
    OwnedActiveSpoolFixture f(nlohmann::json::array({"toolhead", "extruder", "gcode_macro T0"}),
                              nlohmann::json{{"gcode_macro T0", {{"gcode", "ACTIVATE_EXTRUDER"}}}});
    CHECK(f.load_and_sync() == 9);
}

TEST_CASE("linking the loaded lane leaves the active spool to SpoolLink firmware",
          "[ams][commit][spoolman_sync][plugin_filament]") {
    OwnedActiveSpoolFixture f(helix::test::spoollink_printer_objects());
    f.setup(0);
    f.backend->set_operation_delay(0);
    REQUIRE(f.backend->start().success());
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    f.client.spoolman_mock().set_active_spool_id(5);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 147;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());
    CHECK(f.backend->get_slot_info(0).spoolman_id == 147);
    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 5);

    INFO("an unlink still clears: no firmware drops a link Moonraker holds");
    f.client.spoolman_mock().set_active_spool_id(147);
    SlotInfo unlinked = linked;
    unlinked.spoolman_id = 0;
    REQUIRE(AmsState::instance().commit_slot_edit(0, linked, unlinked).success());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("unlinking the loaded lane clears only its own spool, not one made active since",
          "[ams][spoolman][commit][plugin_filament]") {
    CommitFixture f;
    f.setup_manages_active_spool(169);
    REQUIRE(f.backend->slot_is_actively_loaded(0));
    // Something else (a macro, the user in Spoolman) made another spool active.
    f.client.spoolman_mock().set_active_spool_id(55);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, unlinked).success());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == 55);
}
