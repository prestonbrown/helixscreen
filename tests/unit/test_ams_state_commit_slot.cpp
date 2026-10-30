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

std::string shown(const std::optional<std::string>& text) {
    return text ? "\"" + *text + "\"" : "none";
}

std::string shown(const std::optional<uint32_t>& rgb) {
    if (!rgb) {
        return "none";
    }
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string hex = "#000000";
    for (int nibble = 0; nibble < 6; ++nibble) {
        hex[6 - nibble] = digits[(*rgb >> (4 * nibble)) & 0xF];
    }
    return hex;
}

/// The identity fields a lane and its stored record have to agree on, in one
/// line, so a disagreement names the field. Takes an Observation or a
/// ResolvedLane, which spell these fields alike.
template <typename Identity> std::string shown_identity(const Identity& identity) {
    return "colour " + shown(identity.color_rgb) + " brand " + shown(identity.brand) +
           " material " + shown(identity.material) + " catalog_id " + shown(identity.catalog_id) +
           " product_name " + shown(identity.product_name);
}

/// A rung with no record states none of the fields, the same as a record that
/// carries none of them.
std::string shown_identity(const std::optional<helix::ams::Observation>& record) {
    return shown_identity(
        record.value_or(helix::ams::Observation(helix::ams::ObservationSource::Remembered)));
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

    /// What lane 0's stored record resolves to when the next start files it.
    [[nodiscard]] helix::ams::ResolvedLane reloaded() const {
        return helix::ams::resolve(reloaded_sources());
    }

    /// The sources lane 0's stored record files when the next start reads it.
    [[nodiscard]] helix::ams::LaneSources reloaded_sources() const {
        const helix::ams::FilamentSlotOverride record = stored();
        return helix::ams::sources_from_record(record, helix::ams::to_lane_data_record(0, record),
                                               helix::ams::LegacyLockKeys::LaneData);
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

    /// The lane shows what its stored record reloads as: the same resolved
    /// identity, carried by the same declaring rungs. A value the lane holds
    /// as Remembered and the record reloads as LocalUser resolves alike today
    /// and parts at the next firmware frame, which may correct only the first.
    void check_lane_matches_reload() const {
        const helix::ams::LaneSources live = helix::ams::lane_sources(lane());
        const helix::ams::LaneSources reload = reloaded_sources();
        CHECK(shown_identity(helix::ams::resolve(live)) ==
              shown_identity(helix::ams::resolve(reload)));
        CHECK(shown_identity(live.spoolman) == shown_identity(reload.spoolman));
        CHECK(shown_identity(live.local_user) == shown_identity(reload.local_user));
        CHECK(shown_identity(live.remembered) == shown_identity(reload.remembered));
    }
};

/// A stored record for an unlinked lane that locks nothing, so the identity it
/// carries loads as Remembered rather than as anyone's declaration.
helix::ams::FilamentSlotOverride remembered_record() {
    helix::ams::FilamentSlotOverride record;
    record.brand = "Polymaker";
    record.material = "PLA";
    return record;
}

/// An AFC lane restarted from a record that names no spool, so the brand and
/// material it carries stand on the lane as Remembered.
struct RememberedAfcLaneFixture : AfcCommitFixture {
    RememberedAfcLaneFixture() : AfcCommitFixture(remembered_record()) {
        feed_afc_lane(*afc, "lane1", {{"prep", true}, {"status", "Loaded"}});
        REQUIRE(helix::ams::lane_sources(lane()).remembered.has_value());
    }

    /// The AFC plugin binds @p spool to the lane and Spoolman answers the fetch
    /// that follows: a link made outside the editor, which declares nothing of
    /// the user's and leaves what the lane remembered standing.
    void firmware_links(const SpoolInfo& spool) {
        feed_afc_lane(*afc, "lane1", {{"spool_id", spool.id}});
        helix::test::spool_states(*afc, 0, spool);
        REQUIRE(afc->get_slot_info(0).spoolman_id == spool.id);
        REQUIRE(helix::ams::lane_sources(lane()).remembered.has_value());
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

        const auto sources = helix::ams::lane_sources(lane());
        REQUIRE(sources.local_user.has_value());
        CHECK(sources.local_user->spoolman_id == spool_id);
        CHECK(helix::ams::resolve(sources).spoolman_id == spool_id);
    }
};

} // namespace

// Lane 0 is the loaded lane. Unlinking it clears the server's active spool, even
// where the backend manages the active spool (AFC's SET_SPOOL_ID SPOOL_ID= does
// not clear it server-side). Linking it makes the spool active unless the
// backend takes the link itself (AFC); Happy Hare switches only on a load, so
// the edit sets it there. An edit on a lane that is not loaded, or a clear of a
// lane that never had a link, leaves the server's active spool alone.
TEST_CASE("commit_slot_edit moves the server's active spool only for the loaded lane",
          "[ams][spoolman][commit][1717][plugin_filament]") {
    enum class Backend { Plain, ManagesActive, ActivatesOnLoad };
    struct Row {
        const char* name;
        Backend backend;
        int lane0_spool;
        int lane;
        int before, after; // the lane's spool id before and after the edit
        int active_before, active_after;
    };
    const Row rows[] = {
        {"unlink the loaded lane", Backend::Plain, 169, 0, 169, 0, 169, 0},
        {"unlink the loaded lane where the backend manages it", Backend::ManagesActive, 169, 0, 169,
         0, 169, 0},
        {"unlink a lane that is not loaded", Backend::ManagesActive, 169, 1, 147, 0, 169, 169},
        {"link a lane that is not loaded", Backend::ManagesActive, 169, 1, 0, 147, 169, 169},
        {"link the loaded lane", Backend::Plain, 0, 0, 0, 147, 0, 147},
        {"link the loaded lane where the backend takes the link", Backend::ManagesActive, 0, 0, 0,
         147, 5, 5},
        {"link the loaded lane where the backend switches on a load", Backend::ActivatesOnLoad, 0,
         0, 0, 147, 5, 147},
        // No set_active_spool may fire, not even a clear to 0.
        {"clear a lane that never had a link", Backend::Plain, 0, 1, 0, 0, 77, 77},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        CommitFixture f;
        switch (row.backend) {
        case Backend::Plain:
            f.setup(row.lane0_spool);
            break;
        case Backend::ManagesActive:
            f.setup_manages_active_spool(row.lane0_spool);
            break;
        case Backend::ActivatesOnLoad:
            f.setup_activates_on_load_only(row.lane0_spool);
            break;
        }
        REQUIRE(f.backend->slot_is_actively_loaded(0));
        f.client.spoolman_mock().set_active_spool_id(row.active_before);

        SlotInfo original = f.backend->get_slot_info(row.lane);
        if (row.lane != 0) {
            if (row.before == 0 && row.after == 0)
                original.material = "PLA";
            original.spoolman_id = row.before;
            f.backend->sync_external_identity(row.lane, original);
            REQUIRE_FALSE(f.backend->slot_is_actively_loaded(row.lane));
        }
        REQUIRE(f.backend->get_slot_info(row.lane).spoolman_id == row.before);

        SlotInfo edited = original;
        edited.spoolman_id = row.after;
        if (row.before == 0 && row.after == 0)
            edited.material.clear();
        REQUIRE(AmsState::instance().commit_slot_edit(row.lane, original, edited).success());
        helix::ui::UpdateQueue::instance().drain();

        CHECK(f.client.spoolman_mock().get_mock_active_spool_id() == row.active_after);
        CHECK(f.backend->get_slot_info(row.lane).spoolman_id == row.after);
    }
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

TEST_CASE("context-menu clear leaves the live lane as a restart would show it",
          "[ams][commit][context-menu][1661]") {
    CommitFixture f;
    f.setup(0); // slot 0 unlinked

    // What the machine reports on the lane, the way a parse files it.
    const helix::ams::LaneId lane = f.backend->lane_id(0);
    helix::ams::Observation machine(helix::ams::ObservationSource::VendorCache);
    machine.material = "PETG";
    machine.brand = "Firmware Brand";
    machine.color_rgb = 0x30C05F;
    helix::ams::ingest(lane, machine);

    // The standing state a clear has to take with it: a colour pick, its name
    // and a typed weight, none of which Clear Spool's sentinel/-1 values ever
    // engage as a clear, so the commit alone leaves them on the user rung.
    // 640g because the mock seeds slot 0 at 850: a typed number the lane
    // already holds is not a move, and only a moved field is the user's
    // statement (user_edit_observation).
    SlotInfo picked = f.backend->get_slot_info(0);
    picked.color_rgb = 0xE67E22;
    picked.color_name = "Signal Orange";
    picked.remaining_weight_g = 640.0F;
    helix::test::edit_slot_as_user(*f.backend, 0, picked);

    SlotInfo live;
    helix::ams::apply_resolved(live, helix::ams::resolve(helix::ams::lane_sources(lane)));
    REQUIRE(live.color_rgb == 0xE67E22);
    REQUIRE(live.color_name == "Signal Orange");
    REQUIRE(live.remaining_weight_g == 640.0F);

    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));

    // Live: the lane's user record is gone, so resolve() states only what the
    // machine reports.
    const helix::ams::LaneSources after = helix::ams::lane_sources(lane);
    CHECK_FALSE(after.local_user.has_value());
    SlotInfo shown;
    helix::ams::apply_resolved(shown, helix::ams::resolve(after));

    // Restart: the clear dropped the stored record, so boot rebuilds the lane
    // from the machine's own report alone.
    helix::ams::LaneSources reload;
    reload.vendor_cache = machine;
    SlotInfo rebooted;
    helix::ams::apply_resolved(rebooted, helix::ams::resolve(reload));

    CHECK(shown.material == rebooted.material);
    CHECK(shown.brand == rebooted.brand);
    CHECK(shown.color_rgb == rebooted.color_rgb);
    CHECK(shown.color_name == rebooted.color_name);
    CHECK(shown.remaining_weight_g == rebooted.remaining_weight_g);
    CHECK(shown.total_weight_g == rebooted.total_weight_g);
    CHECK(shown.spoolman_vendor_id == rebooted.spoolman_vendor_id);
    // The colour name goes with the pick - no asymmetric survivor - and what
    // both sides show is the machine's readings.
    CHECK(shown.color_rgb == 0x30C05F);
    CHECK(shown.color_name.empty());
    CHECK(shown.material == "PETG");
    CHECK(shown.remaining_weight_g < 0);
}

TEST_CASE("context-menu clear on a lane the machine is silent on shows blank, live and reloaded",
          "[ams][commit][context-menu][1661]") {
    CommitFixture f;
    f.setup(0); // slot 0 unlinked; nothing filed on the lane but the pick below

    // 640g, not the mock's seeded 850: a typed number the lane already holds
    // is not a move, and only a moved field files as the user's statement.
    SlotInfo picked = f.backend->get_slot_info(0);
    picked.color_rgb = 0xE67E22;
    picked.color_name = "Signal Orange";
    picked.remaining_weight_g = 640.0F;
    helix::test::edit_slot_as_user(*f.backend, 0, picked);
    const auto rung = helix::ams::lane_sources(f.backend->lane_id(0)).local_user;
    REQUIRE(rung.has_value());
    REQUIRE(rung->remaining_weight_g.has_value()); // the typed weight stands

    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));

    // Live and a restart both resolve a lane nobody states anything on.
    const helix::ams::LaneSources after = helix::ams::lane_sources(f.backend->lane_id(0));
    CHECK_FALSE(after.local_user.has_value());
    SlotInfo shown;
    helix::ams::apply_resolved(shown, helix::ams::resolve(after));
    SlotInfo rebooted;
    helix::ams::apply_resolved(rebooted, helix::ams::resolve(helix::ams::LaneSources{}));
    CHECK(shown.color_rgb == rebooted.color_rgb);
    CHECK(shown.color_name == rebooted.color_name);
    CHECK(shown.material == rebooted.material);
    CHECK(shown.remaining_weight_g == rebooted.remaining_weight_g);
    CHECK(shown.color_rgb == kAmsDefaultSlotColor);
    CHECK(shown.material.empty());
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

// ============================================================================
// The lane source model: what a commit records as the user's own declaration
// ============================================================================

TEST_CASE("commit_slot_edit records only the fields the user changed", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.color_rgb = 0xFFFFFF;
    original.material = "PETG";
    f.backend->sync_external_identity(0, original);
    original = f.backend->get_slot_info(0);

    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, edited).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
    // The user changed a colour, not a material. Recording the material too
    // would file a value the user never chose as their own declaration.
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("a spool link does not record the spool's colour as the user's", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 7;
    linked.brand = "Kingroon";
    linked.material = "PETG";
    linked.color_rgb = 0xFFFFFF;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 7);
    // The colour arrived with the binding. Only the server's own record may
    // assert it, so the user's record must not claim it.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->brand.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
    // The rest of what a spool carries, held to the same rule. These are the
    // fields an exception list would reach for first.
    CHECK_FALSE(sources.local_user->spoolman_vendor_id.has_value());
    CHECK_FALSE(sources.local_user->remaining_weight_g.has_value());
    CHECK_FALSE(sources.local_user->total_weight_g.has_value());
    CHECK_FALSE(sources.local_user->spool_name.has_value());
}

TEST_CASE("a field the user chose in the same commit as a link is not recorded",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 7;
    linked.material = "ASA"; // the person really did pick this

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 7);
    // A commit carrying a binding change cannot say which of its other fields
    // the person moved and which the binding brought, so it claims none of
    // them. The person re-picks the material as an ordinary edit, which then
    // records. This assertion is what makes that cost deliberate.
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("an unlink records the binding, not the fields it cleared", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(7);

    SlotInfo original = f.backend->get_slot_info(0);
    original.brand = "Kingroon";
    original.material = "PETG";
    original.color_rgb = 0xFFFFFF;
    f.backend->sync_external_identity(0, original);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 7);

    SlotInfo cleared = original;
    cleared.spoolman_id = 0;
    cleared.brand.clear();
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, cleared).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The unbinding cleared those fields; the user did not choose an empty
    // material or a default colour, so neither becomes their declaration.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->brand.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("an unlink after a restart stops the lane naming the unlinked spool",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(42);

    // A restart files the stored record the way every backend's start does,
    // and a record naming a spool files its whole identity as the server's.
    helix::test::file_override_as_lane_records(*f.backend, 0, linked_record(42));
    REQUIRE(helix::ams::lane_sources(lane_of(0)).spoolman.has_value());
    REQUIRE(helix::ams::resolve(helix::ams::lane_sources(lane_of(0))).spoolman_id == 42);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, unlinked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The server's record describes the spool the user just unbound, and it
    // outranks the user's own record, so standing it would undo the unlink.
    const auto resolved = helix::ams::resolve(sources);
    CHECK(resolved.spoolman_id.value_or(-1) == 0);
    CHECK(resolved.brand.value_or("") != "Polymaker");
}

TEST_CASE("an edit that keeps the same spool leaves the server's record standing",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(42);
    helix::test::file_override_as_lane_records(*f.backend, 0, linked_record(42));

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    REQUIRE(original.color_rgb != 0xBCBCBC);
    SlotInfo recoloured = original;
    recoloured.color_rgb = 0xBCBCBC;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, recoloured).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
    // The binding did not move, so the server's account of the spool still
    // describes what is loaded.
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK(sources.spoolman->brand == "Polymaker");
}

TEST_CASE("an unlink the backend refuses leaves the server's record standing",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(0);

    // Slot 5 is past the mock's four slots, so apply_user_edit refuses it, but
    // its id is inside this backend's block, so the store holds a record there.
    helix::test::file_override_as_lane_records(*f.backend, 5, linked_record(42));
    REQUIRE(helix::ams::lane_sources(lane_of(5)).spoolman.has_value());

    SlotInfo original;
    original.spoolman_id = 42;
    SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;

    REQUIRE_FALSE(AmsState::instance().commit_slot_edit(5, original, unlinked).success());

    // The edit never happened, so the spool the server describes is still the
    // one bound to the lane.
    const auto sources = helix::ams::lane_sources(lane_of(5));
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK_FALSE(sources.local_user.has_value());
}

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

TEST_CASE("an AFC relink after a restart survives a stale frame naming the old spool",
          "[ams][commit][lane][spoolman][afc]") {
    RestartedAfcLinkFixture f;
    f.relink(99);

    // Firmware keeps reporting the spool it held until the write lands, so a
    // frame still naming 42 is our own write in flight, not another writer
    // re-binding the lane.
    feed_afc_lane(*f.afc, "lane1", {{"spool_id", 42}});
    f.check_link_stands(99, "the old spool");

    feed_afc_lane(*f.afc, "lane1", {{"spool_id", 99}});
    f.check_link_stands(99, "the new spool");
}

TEST_CASE("a link drops the colour the user picked before it",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RememberedAfcLaneFixture f;
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    {
        const auto picked = helix::ams::lane_sources(f.lane());
        REQUIRE(picked.local_user.has_value());
        REQUIRE(picked.local_user->color_rgb == 0xBCBCBCu);
    }

    // Linking carries the spool's own identity into the slot, and Spoolman
    // answers the fetch the link starts. make_spool's colour is FFB6C1.
    const SpoolInfo spool = make_spool(42, "eSUN", "Silk Blue", "PETG");
    f.edit([&spool](SlotInfo& slot) {
        slot.spoolman_id = spool.id;
        slot.brand = spool.vendor;
        slot.material = spool.material;
        slot.color_rgb = 0xFFB6C1;
    });
    helix::test::spool_states(*f.afc, 0, spool);

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 42);
    // The colour described the filament on the lane before the link, and the
    // link says a different spool is there now. What we remembered about that
    // filament goes with it.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.remembered.has_value());

    // The stored record takes the linked spool's colour, so a lane showing any
    // other would change colour at the next start with nobody touching it.
    const auto reloaded_colour = f.reloaded().color_rgb;
    REQUIRE(reloaded_colour == 0xFFB6C1u);
    CHECK(helix::ams::resolve(sources).color_rgb == reloaded_colour);
}

TEST_CASE("an unlink drops the colour the user picked for the unlinked spool",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RememberedAfcLaneFixture f;
    f.firmware_links(make_spool(42, "eSUN", "Silk Blue", "PETG"));
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    {
        const auto picked = helix::ams::lane_sources(f.lane());
        REQUIRE(picked.local_user.has_value());
        REQUIRE(picked.local_user->color_rgb == 0xBCBCBCu);
        REQUIRE(picked.remembered.has_value());
    }
    const std::string kept_brand = f.afc->get_slot_info(0).brand;
    REQUIRE_FALSE(kept_brand.empty());

    f.edit([](SlotInfo& slot) { slot.spoolman_id = 0; });

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The colour described the spool that was bound, and an unlink stops
    // saying which spool is loaded.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    // After an unlink that keeps the slot's identity, that identity is remembered, not declared.
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->color_rgb == 0xBCBCBCu);
    CHECK(sources.remembered->brand == kept_brand);
    f.check_lane_matches_reload();
}

TEST_CASE("an unlink and a relink to the same spool each drop the colour",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });

    f.edit([](SlotInfo& slot) { slot.spoolman_id = 0; });
    {
        const auto unlinked = helix::ams::lane_sources(f.lane());
        REQUIRE(unlinked.local_user.has_value());
        CHECK_FALSE(unlinked.local_user->color_rgb.has_value());
    }

    f.edit([](SlotInfo& slot) { slot.color_rgb = 0x1E5AA8; });
    {
        const auto picked = helix::ams::lane_sources(f.lane());
        REQUIRE(picked.local_user.has_value());
        REQUIRE(picked.local_user->color_rgb == 0x1E5AA8u);
    }

    // Relinking the spool the lane held before is a binding change like any
    // other: the colour picked while unlinked goes, and the one typed for
    // spool 42 does not come back. The id returning is not evidence the spool
    // did. Telling spool 42 back from a different spool carrying id 42 would
    // take asking Spoolman, and a colour kept on the wrong spool is the worse
    // mistake, so the lane stops claiming one.
    f.edit([](SlotInfo& slot) { slot.spoolman_id = 42; });

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 42);
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
}

TEST_CASE("an edit that keeps the same spool leaves the user's earlier declaration standing",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RememberedAfcLaneFixture f;
    f.firmware_links(make_spool(42, "eSUN", "Silk Blue", "PETG"));
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    // A linked spool owns its material, so the second edit's material is not the
    // user's to declare.
    const AmsError material = f.try_edit([](SlotInfo& slot) { slot.material = "ASA"; });
    CHECK(material.partially_applied);

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
    CHECK(helix::ams::resolve(sources).material == std::string("PETG"));
    // The binding did not move, so everything the lane held about the spool
    // still describes the one loaded.
    CHECK(sources.local_user->color_rgb == 0xBCBCBCu);
    CHECK(sources.remembered.has_value());
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
}

TEST_CASE("a binding change leaves the lane showing what its stored record reloads as",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RememberedAfcLaneFixture f;
    const SpoolInfo first = make_spool(42, "eSUN", "Silk Blue", "PETG");
    const SpoolInfo second = make_spool(99, "Sunlu", "PLA Plus", "PLA");
    // Nothing but a person produces a catalog pick, and it describes one
    // spool's product. Only an unlink that keeps the rest of the slot keeps it.
    std::string kept_product;
    int bound = 0;
    bool cleared = false;

    SECTION("a link") {
        f.pick_colour_and_product();
        f.link(first);
        bound = first.id;
    }
    SECTION("a relink to a different spool") {
        f.link(first);
        f.pick_colour_and_product();
        f.link(second);
        bound = second.id;
    }
    SECTION("an unlink that keeps the slot identity") {
        f.link(first);
        f.pick_colour_and_product();
        // The editor's Save-to-Spoolman-off unlink.
        f.edit([](SlotInfo& slot) { slot.clear_spoolman_link(); });
        kept_product = "sunlu-pla-plus-2-0";
    }
    SECTION("Clear Spool") {
        f.link(first);
        f.pick_colour_and_product();
        REQUIRE(ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0,
                                                nullptr));
        cleared = true;
    }

    const SlotInfo slot = f.afc->get_slot_info(0);
    REQUIRE(slot.spoolman_id == bound);
    if (cleared) {
        // Clear Spool is the one binding change that leaves no record to
        // reload: the gesture erases the stored record with the lane's own
        // (#1661), so a restart reloads nothing and the live lane agrees.
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*f.afc));
        CHECK(AfcTestAccess::overrides(*f.afc).find(0) == AfcTestAccess::overrides(*f.afc).end());
        CHECK_FALSE(helix::ams::lane_sources(f.lane()).local_user.has_value());
        CHECK(slot.catalog_id.empty());
        return;
    }
    f.check_lane_matches_reload();
    // The editor reopens on the slot's own pick, so the slot has to agree too.
    CHECK(slot.catalog_id == kept_product);
    CHECK(f.reloaded().catalog_id.value_or("") == kept_product);
}

TEST_CASE("a relink does not carry the previous spool's catalog pick onto the new spool",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    f.pick_colour_and_product();

    f.link(make_spool(99, "Sunlu", "PLA Plus", "PLA"));

    const SlotInfo slot = f.afc->get_slot_info(0);
    const helix::ams::LaneSources reload = f.reloaded_sources();
    INFO("slot catalog_id " << slot.catalog_id << " product_name " << slot.product_name
                            << "; reloaded LocalUser " << shown_identity(reload.local_user));
    CHECK(slot.catalog_id.empty());
    CHECK(slot.product_name.empty());
    CHECK(shown_identity(reload.local_user) ==
          shown_identity(std::optional<helix::ams::Observation>{}));
}

TEST_CASE("an unlink that keeps the slot identity leaves it remembered and not declared",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RememberedAfcLaneFixture f;
    f.firmware_links(make_spool(42, "eSUN", "Silk Blue", "PETG"));
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    REQUIRE(helix::ams::declares_color(f.stored()));

    f.edit([](SlotInfo& slot) { slot.clear_spoolman_link(); });

    const SlotInfo slot = f.afc->get_slot_info(0);
    REQUIRE(slot.spoolman_id == 0);
    REQUIRE_FALSE(slot.brand.empty());
    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The person stopped tracking the spool, which says nothing about what is
    // loaded. What the slot kept is still shown, but as what we remember, so
    // the next firmware frame may correct it.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->brand.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->color_rgb == 0xBCBCBCu);
    CHECK(sources.remembered->brand == slot.brand);

    const helix::ams::FilamentSlotOverride stored = f.stored();
    CHECK_FALSE(helix::ams::declares_color(stored));
    CHECK_FALSE(helix::ams::declares_material(stored));
    CHECK_FALSE(stored.declared.any());
    f.check_lane_matches_reload();
}

TEST_CASE("a relink voids the authorship the stored record held for the previous spool",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    // A linked spool owns the brand and the material, so the colour is what a
    // person can declare on this lane.
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    {
        const helix::ams::FilamentSlotOverride picked = f.stored();
        REQUIRE(helix::ams::declares_color(picked));
        REQUIRE(picked.declared.any());
    }

    // The relink keeps every other value on the slot, so each declaration
    // still stands over the value it was set on.
    f.relink(99);

    const helix::ams::FilamentSlotOverride stored = f.stored();
    REQUIRE(stored.spoolman_id == 99);
    CHECK_FALSE(helix::ams::declares_color(stored));
    CHECK_FALSE(helix::ams::declares_material(stored));
    CHECK_FALSE(stored.declared.any());
    // Spoolman has not answered for spool 99 yet, and what the slot still
    // holds describes spool 42.
    const auto sources = helix::ams::lane_sources(f.lane());
    CHECK_FALSE(sources.spoolman.has_value());
    CHECK_FALSE(sources.remembered.has_value());
}

TEST_CASE("Clear Spool on a linked lane leaves nothing remembered and no catalog pick",
          "[ams][commit][lane][spoolman][afc][context-menu][1653]") {
    RememberedAfcLaneFixture f;
    f.firmware_links(make_spool(42, "eSUN", "Silk Blue", "PETG"));
    f.pick_colour_and_product();

    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));

    const SlotInfo slot = f.afc->get_slot_info(0);
    REQUIRE(slot.spoolman_id == 0);
    const auto sources = helix::ams::lane_sources(f.lane());
    // The clear drops the lane's whole user record, the unlink statement
    // included: a clear is not a declaration, so nothing of it stands on the
    // lane (prestonbrown/helixscreen#1661).
    CHECK_FALSE(sources.local_user.has_value());
    CHECK_FALSE(sources.remembered.has_value());
    CHECK_FALSE(helix::ams::resolve(sources).color_rgb.has_value());
    // The pick names a product of the material the clear just removed.
    CHECK(slot.catalog_id.empty());
    CHECK(slot.product_name.empty());
    // And no identity reloads: the stored record keeps only the unlink, since
    // a remember_spool lane restates the old id on the next start (#1717).
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*f.afc));
        const auto& overrides = AfcTestAccess::overrides(*f.afc);
        const auto kept = overrides.find(0);
        REQUIRE(kept != overrides.end());
        CHECK(kept->second.unlinked_spool_id == 42);
        CHECK(kept->second.spoolman_id == 0);
        CHECK(kept->second.material.empty());
        CHECK_FALSE(kept->second.color_set);
        CHECK(kept->second.catalog_id.empty());
        CHECK_FALSE(kept->second.declared.any());
    }
}

TEST_CASE("an edit that keeps the same spool keeps the stored record's authorship",
          "[ams][commit][lane][spoolman][afc][1653]") {
    // The stored record names no brand, so the lane states none. A linked spool
    // owns its brand and material all the same, so neither edit declares them,
    // and the colour the first edit declared stands through the second.
    helix::ams::FilamentSlotOverride unbranded = linked_record(42);
    unbranded.brand.clear();
    RestartedAfcLinkFixture f(unbranded);
    REQUIRE_FALSE(helix::ams::resolve(helix::ams::lane_sources(f.lane())).brand.has_value());

    const AmsError colour_and_brand = f.try_edit([](SlotInfo& slot) {
        slot.color_rgb = 0xBCBCBC;
        slot.brand = "Sunlu";
    });
    CHECK(colour_and_brand.partially_applied);

    const AmsError material = f.try_edit([](SlotInfo& slot) { slot.material = "ASA"; });
    CHECK(material.partially_applied);

    const helix::ams::FilamentSlotOverride stored = f.stored();
    REQUIRE(stored.spoolman_id == 42);
    CHECK(stored.brand.empty());
    CHECK(stored.material == "PLA");
    CHECK(helix::ams::declared_field_names(stored.declared) ==
          nlohmann::json::array({"color_rgb"}));
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

TEST_CASE("a later edit amends the user's record instead of replacing it", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo picked_colour = original;
    picked_colour.color_rgb = 0xBCBCBC;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, picked_colour).success());

    SlotInfo after_colour = f.backend->get_slot_info(0);
    SlotInfo picked_material = after_colour;
    picked_material.material = "ASA";
    REQUIRE(AmsState::instance().commit_slot_edit(0, after_colour, picked_material).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->material == "ASA");
    // Two statements by one person, not one statement replacing another.
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
}

TEST_CASE("a second backend's lane 0 is not the first backend's", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(ams.commit_slot_edit(0, original, edited).success());

    // Each backend owns a block of ids, which a flat slot index could not
    // express: slot 0 on one backend is not slot 0 on the other.
    CHECK(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK_FALSE(
        helix::ams::lane_sources(helix::ams::lane_id_for(second, 0)).local_user.has_value());
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

TEST_CASE("an edit files under the backend it was written through", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);
    ams.set_active_backend(second);
    REQUIRE(ams.active_backend_index() == second);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(ams.commit_slot_edit(0, original, edited).success());

    // commit_slot_edit writes through get_backend(), which is the primary and
    // not the active one. The declaration has to name the same lane the edit
    // itself reached, or the record describes a slot nobody edited.
    CHECK(f.backend->get_slot_info(0).color_rgb == 0xBCBCBC);
    CHECK(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK_FALSE(
        helix::ams::lane_sources(helix::ams::lane_id_for(second, 0)).local_user.has_value());
}

TEST_CASE("tearing down the backends clears their lane declarations", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, edited).success());
    REQUIRE(helix::ams::lane_sources(lane_of(0)).local_user.has_value());

    // The next printer's first backend is stamped with this one's index, so a
    // declaration that outlived the backend it was made through would be read
    // back as a statement about hardware nobody edited.
    AmsState::instance().clear_backends();

    CHECK_FALSE(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a commit the backend rejects records no declaration", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;

    // Slot 5 is past the mock's four slots, so apply_user_edit refuses it.
    const AmsError err = AmsState::instance().commit_slot_edit(5, original, edited);
    REQUIRE_FALSE(err.success());

    // The edit reached the backend and was refused, so the user declared
    // nothing. Asked of the whole store rather than one lane: a refused commit
    // must not leave a record anywhere, including on a lane it mis-addressed.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a commit that changes nothing writes no user record", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, original).success());

    // The REQUIRE above is the proof the path ran: commit_slot_edit reached
    // the backend and the backend accepted. Having run it in full, it recorded
    // no declaration, because nothing was declared.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("clearing an already-unlinked slot records no declaration", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0); // unlinked, so the binding-change rule does not fire

    SlotInfo original = f.backend->get_slot_info(0);
    original.color_rgb = 0x1188CC;
    original.material = "PETG";
    original.remaining_weight_g = 620.0f;
    original.total_weight_g = 1000.0f;
    f.backend->sync_external_identity(0, original);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 0);

    // The shape MenuAction::CLEAR_SPOOL commits (ui_ams_detail.cpp).
    SlotInfo cleared = original;
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.brand.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, cleared).success());

    // Nothing in a clear is a value a rung may stand over: the sentinels spell
    // "no reading", and a cleared text field hands the field to whatever the
    // machine reports rather than recording an emptiness
    // (prestonbrown/helixscreen#1661). The colour's NAME is the one empty that
    // stays: a colour gone means its name gone too, and an empty name beside a
    // stated colour is how a pick clears a contradictory one (lane_resolver.cpp).
    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->remaining_weight_g.has_value());
    CHECK_FALSE(sources.local_user->total_weight_g.has_value());
}

TEST_CASE("a field cleared to empty withdraws the standing declaration", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.material = "PETG";
    f.backend->sync_external_identity(0, original);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.material == "PETG");

    // The user states a material, then reopens the editor and clears it.
    SlotInfo typed = original;
    typed.material = "ABS";
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, typed).success());
    {
        const auto sources = helix::ams::lane_sources(lane_of(0));
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->material.has_value());
        CHECK(*sources.local_user->material == "ABS");
    }

    SlotInfo emptied = typed;
    emptied.material.clear();
    REQUIRE(AmsState::instance().commit_slot_edit(0, typed, emptied).success());

    // A clear means "I don't know; whatever the machine reports": it withdraws
    // the declaration rather than replacing it with an emptiness, so nothing
    // sits over the field and weaker sources show through at once
    // (prestonbrown/helixscreen#1661). What the lane then shows is pinned in
    // test_lane_write_paths.cpp.
    const auto sources = helix::ams::lane_sources(lane_of(0));
    CHECK_FALSE(sources.local_user.has_value());
}

TEST_CASE("an auto-highlighted catalog product is not the user's declaration",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo saved = original;
    // What an untouched open-and-Save produces: the editor preselects a
    // product and copies it in, with nothing else moved.
    saved.catalog_id = "sunlu-pla-plus-2-0";
    saved.product_name = "PLA+ 2.0";

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, saved).success());

    // Nothing was declared, so no lane was written at all.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a weight edit finer than the editor's own tolerance is not a declaration",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.remaining_weight_g = 620.0f;
    original.total_weight_g = 1000.0f;
    f.backend->sync_external_identity(0, original);
    original = f.backend->get_slot_info(0);

    SlotInfo drifted = original;
    drifted.remaining_weight_g = 619.95f; // a consumption tick, not a keystroke

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, drifted).success());
    CHECK(helix::ams::known_lanes().empty());

    SlotInfo typed = original;
    typed.remaining_weight_g = 500.0f;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, typed).success());
    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->remaining_weight_g == 500.0f);
}

TEST_CASE("a frame that lands while the editor is open does not become the user's declaration",
          "[ams][commit][lane][afc][1652]") {
    CommitFixture f;
    SettingsManager::instance().init_subjects();
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    AmsState::instance().set_moonraker_api(&f.api);

    // A stored record that locks nothing: the brand is remembered, not declared.
    helix::ams::FilamentSlotOverride stored;
    stored.brand = "Polymaker";
    stored.material = "PLA";
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
        AfcTestAccess::overrides(*afc)[0] = stored;
    }
    helix::test::file_override_as_lane_records(*afc, 0, stored);

    feed_afc_lane(*afc, "lane1", {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}});
    // The editor takes its snapshot on opening and keeps it until it commits.
    const SlotInfo original = afc->get_slot_info(0);
    REQUIRE(original.color_rgb == 0xED2C2Cu);

    // Firmware restates the colour while the editor is open. Nobody declared it.
    feed_afc_lane(*afc, "lane1", {{"color", "#1E5AA8"}});
    REQUIRE(afc->get_slot_info(0).color_rgb == 0x1E5AA8u);

    SlotInfo rebranded = original;
    rebranded.brand = "Elegoo";
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, rebranded).success());

    const auto sources = helix::ams::lane_sources(afc.lane(0));
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand == "Elegoo");

    bool record_declares_colour = false;
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
        const auto& overrides = AfcTestAccess::overrides(*afc);
        const auto kept = overrides.find(0);
        REQUIRE(kept != overrides.end());
        record_declares_colour = helix::ams::declares_color(kept->second);
    }
    // The stored record and the lane carry one declaration between them, and
    // the user moved the brand alone.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK(record_declares_colour == sources.local_user->color_rgb.has_value());
}

TEST_CASE("a binding change that reached firmware is filed even though the call returned an error",
          "[ams][commit][lane][spoolman][afc][1652]") {
    RestartedAfcLinkFixture f;

    SlotInfo original = f.afc->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    SlotInfo relinked = original;
    relinked.spoolman_id = 99;
    // AFC sends the spool id first, then refuses a material name it cannot put
    // in a G-code parameter.
    relinked.material = "PLA;G28";

    const AmsError err = AmsState::instance().commit_slot_edit(0, original, relinked);

    // The user is still told the material did not save.
    REQUIRE_FALSE(err.success());
    CHECK(err.result == AmsResult::COMMAND_FAILED);
    CHECK_FALSE(err.user_msg.empty());

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 99);
    // The server's record describes spool 42, and the lane holds spool 99.
    CHECK_FALSE(sources.spoolman.has_value());
}

TEST_CASE("a Happy Hare binding change that reached firmware is filed despite a refused material",
          "[ams][commit][lane][spoolman][happy_hare][1652]") {
    CommitFixture f;
    helix::test::RegisteredBackend<AmsBackendHappyHare> hh(nullptr, nullptr);
    HappyHareTestAccess::slots(*hh).initialize("MMU", std::vector<std::string>{"0", "1"});
    AmsState::instance().set_moonraker_api(&f.api);

    SlotInfo linked = hh->get_slot_info(0);
    linked.spoolman_id = 42;
    REQUIRE(hh->sync_external_identity(0, linked).success());
    helix::test::file_override_as_lane_records(*hh, 0, linked_record(42));
    REQUIRE(helix::ams::lane_sources(hh.lane(0)).spoolman.has_value());

    const SlotInfo original = hh->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    SlotInfo relinked = original;
    relinked.spoolman_id = 99;
    relinked.material = "PLA;G28";

    const AmsError err = AmsState::instance().commit_slot_edit(0, original, relinked);

    REQUIRE_FALSE(err.success());
    CHECK(err.result == AmsResult::COMMAND_FAILED);

    const auto sources = helix::ams::lane_sources(hh.lane(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 99);
    CHECK_FALSE(sources.spoolman.has_value());
}

TEST_CASE("an AD5X lane shows a re-declared field as soon as the commit returns",
          "[ams][commit][lane][ad5x_ifs][1652]") {
    CommitFixture f;
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> ad5x(nullptr, nullptr);

    // A persisted AD5X edit writes Adventurer5M.json, so give it a file of its own.
    struct RemoveOnExit {
        std::filesystem::path path;
        ~RemoveOnExit() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } json_file{std::filesystem::temp_directory_path() /
                ("helix_commit_ad5x_" + std::to_string(::getpid()) + ".json")};
    Ad5xIfsTestAccess::set_local_adventurer_json_path(*ad5x, json_file.path.string());

    Ad5xIfsTestAccess::set_port_presence(*ad5x, 0, true);
    Ad5xIfsTestAccess::set_color(*ad5x, 0, "898989");
    Ad5xIfsTestAccess::set_material(*ad5x, 0, "PETG");

    SlotInfo original = ad5x->get_slot_info(0);
    SlotInfo branded = original;
    branded.brand = "Sunlu";
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, branded).success());
    REQUIRE(ad5x->get_slot_info(0).brand == "Sunlu");

    // AD5X paints the slot from the lane while it applies the edit, and the
    // lane still holds the first declaration at that moment.
    original = ad5x->get_slot_info(0);
    SlotInfo rebranded = original;
    rebranded.brand = "Elegoo";
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, rebranded).success());

    CHECK(ad5x->get_slot_info(0).brand == "Elegoo");
}

TEST_CASE("a same-spool edit on a linked lane files neither brand nor material as the user's",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    const SpoolInfo spool = make_spool(42, "Polymaker", "PolyLite PLA", "PLA");
    helix::test::spool_states(*f.afc, 0, spool);

    const AmsError identity = f.try_edit([](SlotInfo& slot) {
        slot.color_rgb = 0xBCBCBC;
        slot.brand = "Sunlu";
        slot.spool_name = "Bench spool";
        slot.spoolman_vendor_id = 9;
    });
    CHECK_FALSE(identity.success());
    CHECK(identity.partially_applied);
    CHECK(identity.user_msg == "Brand and material come from Spoolman");
    {
        // Right after the edit that moved them, before a later edit repaints.
        const helix::ams::FilamentSlotOverride after_identity = f.stored();
        CHECK(after_identity.brand == "Polymaker");
        CHECK(after_identity.spool_name == "PolyLite PLA");
        CHECK(after_identity.spoolman_vendor_id == spool.vendor_id);
    }

    const AmsError material = f.try_edit([](SlotInfo& slot) { slot.material = "ASA"; });
    CHECK_FALSE(material.success());
    CHECK(material.partially_applied);

    // The stored record holds the spool's identity and claims only the colour.
    const helix::ams::FilamentSlotOverride stored = f.stored();
    CHECK(stored.brand == "Polymaker");
    CHECK(stored.material == "PLA");
    CHECK(stored.spool_name == "PolyLite PLA");
    CHECK(stored.spoolman_vendor_id == spool.vendor_id);
    CHECK(stored.color_rgb == 0xBCBCBCu);
    CHECK(helix::ams::declared_field_names(stored.declared) ==
          nlohmann::json::array({"color_rgb"}));

    // The lane's user record carries the colour and nothing the spool owns.
    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBCu);
    CHECK_FALSE(sources.local_user->brand.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
    CHECK_FALSE(sources.local_user->spool_name.has_value());
    CHECK_FALSE(sources.local_user->spoolman_vendor_id.has_value());
}

TEST_CASE(
    "a linked lane shows the same brand and material after a restart with Spoolman unreachable",
    "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    helix::test::spool_states(*f.afc, 0, make_spool(42, "Polymaker", "PolyLite PLA", "PLA"));
    (void)f.try_edit([](SlotInfo& slot) {
        slot.color_rgb = 0xBCBCBC;
        slot.brand = "Sunlu";
        slot.material = "ASA";
    });
    const helix::ams::ResolvedLane live = helix::ams::resolve(helix::ams::lane_sources(f.lane()));
    REQUIRE(live.brand == std::string("Polymaker"));
    REQUIRE(live.material == std::string("PLA"));

    // A restart with Spoolman unreachable has only the stored record to file.
    helix::ams::reset_lane_sources();
    REQUIRE(helix::ams::file_lane_sources(f.lane(), f.reloaded_sources()));
    const helix::ams::ResolvedLane restarted =
        helix::ams::resolve(helix::ams::lane_sources(f.lane()));
    CHECK(restarted.brand == live.brand);
    CHECK(restarted.material == live.material);
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

TEST_CASE("a linked lane shows the user's colour after a restart and a fetch",
          "[ams][commit][lane][spoolman][afc][1653]") {
    RestartedAfcLinkFixture f;
    f.edit([](SlotInfo& slot) { slot.color_rgb = 0xBCBCBC; });
    f.check_lane_matches_reload();

    // A restart has only the stored record to file...
    helix::ams::reset_lane_sources();
    REQUIRE(helix::ams::file_lane_sources(f.lane(), f.reloaded_sources()));

    // ...and the first fetch of spool 42 states the spool's own colour.
    SpoolInfo spool = make_spool(42, "Polymaker", "PolyLite PLA", "PLA");
    spool.color_hex = "FF0000";
    helix::test::spool_states(*f.afc, 0, spool);

    const auto sources = helix::ams::lane_sources(f.lane());
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBCu);
    CHECK(helix::ams::resolve(sources).color_rgb == 0xBCBCBCu);
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
