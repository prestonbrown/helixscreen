// SPDX-License-Identifier: GPL-3.0-or-later

// AmsBackendMultiAce: a Snapmaker U1 with multiACE's ACE units upstream of its
// heads. Status frames come from tests/fixtures/multiace/ (the engine's
// get_status() shape); commands are captured at execute_gcode().

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_multiace.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "multiace_status.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/registered_backend.h"
#include "ui/ams_drawing_utils.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

std::string fixture_path(const std::string& name) {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/multiace/" + name;
}

json ace_frame(const std::string& fixture) {
    std::ifstream f(fixture_path(fixture));
    INFO("fixture missing or unreadable: " << fixture_path(fixture));
    REQUIRE(f.is_open());
    json j;
    f >> j;
    return json{{"ace", j}};
}

json feed_frame(int head, const std::string& state, bool detected = true) {
    const char* key = head <= 1 ? "filament_feed left" : "filament_feed right";
    return json{
        {key,
         {{"extruder" + std::to_string(head),
           {{"filament_detected", detected}, {"channel_state", state}, {"channel_error", "ok"}}}}}};
}

PrinterDiscovery discover(std::vector<std::string> objects) {
    PrinterDiscovery hw;
    hw.parse_objects(json(objects));
    return hw;
}

const std::vector<std::string> kU1Objects{"filament_detect", "extruder",   "extruder1",
                                          "extruder2",       "extruder3",  "print_task_config",
                                          "toolhead",        "toolchanger"};

class CapturingMultiAce : public AmsBackendMultiAce {
  public:
    explicit CapturingMultiAce(IMoonrakerAPI* api = nullptr,
                               helix::IMoonrakerClient* client = nullptr)
        : AmsBackendMultiAce(api, client) {
        running_.store(true);
    }

    std::vector<std::string> sent;

    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool) override {
        sent.push_back(gcode);
        last_ok = std::move(on_complete);
        last_error = std::move(on_error);
        return AmsErrorHelper::success();
    }

    /// The firmware's answer to the last command, delivered as the base
    /// delivers it: on the main thread through the update queue.
    void answer_ok() {
        REQUIRE(last_ok);
        last_ok();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    void answer_error(const std::string& message) {
        REQUIRE(last_error);
        MoonrakerError err;
        err.type = MoonrakerErrorType::JSON_RPC_ERROR;
        err.message = message;
        last_error(err);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    helix::SharedCallback<void()> last_ok;
    helix::SharedCallback<void(const MoonrakerError&)> last_error;

    void close_pending_window() {
        pending_load_grace_ = std::chrono::milliseconds{0};
    }

    /// Every kEventError payload the backend raises.
    std::vector<std::string> errors;
    void record_errors() {
        set_event_callback([this](const std::string& ev, const std::string& data) {
            if (ev == kEventError) {
                errors.push_back(data);
            }
        });
    }

    void frame(const json& status) {
        SnapmakerTestAccess::handle_status(*this, status);
    }
};

struct MultiAceFixture : public HelixTestFixture {
    ~MultiAceFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// The real on_started(), where the backend acquires its loaded override
/// store. As in test_ams_override_store_lifecycle.cpp: start() is final on
/// AmsSubscriptionBackend and drags in subscription setup a store round-trip
/// has no use for.
class StoreBackedMultiAce : public CapturingMultiAce {
  public:
    explicit StoreBackedMultiAce(IMoonrakerAPI* api) : CapturingMultiAce(api, nullptr) {}
    using AmsBackendMultiAce::on_started;
};

/// Per-test read-cache sandbox, so a neighbouring test's stored records are
/// not reachable from this one (the cache file serves every backend in the
/// process). Same idiom as test_ams_override_store_lifecycle.cpp.
struct MultiAceOverrideCacheDir {
    std::filesystem::path path;
    std::filesystem::path previous;
    explicit MultiAceOverrideCacheDir(const std::string& suffix) {
        previous = helix::ams::detail::slot_override_cache_dir_ref();
        path = std::filesystem::temp_directory_path() /
               ("multiace_override_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        helix::ams::detail::slot_override_cache_dir_ref() = path;
    }
    ~MultiAceOverrideCacheDir() {
        helix::ams::detail::slot_override_cache_dir_ref() = previous;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

/// A live print state for the gates that read it.
struct MultiAcePrintFixture : public LVGLTestFixture {
    MultiAcePrintFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
    }
    MoonrakerClientMock client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
};

} // namespace

// ============================================================================
// Detection and construction
// ============================================================================

TEST_CASE_METHOD(MultiAcePrintFixture, "factory builds MultiAce only for a multiACE U1",
                 "[multiace]") {
    const PrinterDiscovery plain = discover(kU1Objects);
    auto with_ace = kU1Objects;
    with_ace.push_back("ace");
    const PrinterDiscovery modded = discover(with_ace);
    REQUIRE(modded.mmu_type() == AmsType::SNAPMAKER);

    // Told apart by behaviour: only the multiACE variant shows the ACE units.
    auto units_after_frame = [this](const PrinterDiscovery* hw) {
        auto backend = AmsBackend::create(AmsType::SNAPMAKER, api.get(), &client, hw);
        REQUIRE(backend);
        REQUIRE(backend->get_type() == AmsType::SNAPMAKER);
        auto& snap = static_cast<AmsBackendSnapmaker&>(*backend);
        SnapmakerTestAccess::handle_status(snap, ace_frame("head_mode_4ace.json"));
        return backend->get_system_info().units.size();
    };
    CHECK(units_after_frame(&modded) == 5);
    CHECK(units_after_frame(&plain) == 1);
    CHECK(units_after_frame(nullptr) == 1);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(MultiAcePrintFixture, "AmsState builds the multiACE backend from discovery",
                 "[multiace][ams]") {
    auto with_ace = kU1Objects;
    with_ace.push_back("ace");
    const PrinterDiscovery modded = discover(with_ace);

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.init_backends_from_hardware(modded, api.get(), &client);
    REQUIRE(ams.backend_count() == 1);

    // Only the multiACE variant shows the ACE units.
    auto& snap = static_cast<AmsBackendSnapmaker&>(*ams.get_backend(0));
    SnapmakerTestAccess::handle_status(snap, ace_frame("head_mode_4ace.json"));
    CHECK(snap.get_system_info().units.size() == 5);

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiAcePrintFixture, "a 4-ACE rig gives every bay a slot subject",
                 "[multiace][ams]") {
    auto with_ace = kU1Objects;
    with_ace.push_back("ace");
    const PrinterDiscovery modded = discover(with_ace);

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.init_backends_from_hardware(modded, api.get(), &client);
    REQUIRE(ams.backend_count() == 1);

    auto& snap = static_cast<AmsBackendSnapmaker&>(*ams.get_backend(0));
    SnapmakerTestAccess::handle_status(snap, ace_frame("head_mode_4ace.json"));
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // ACE 4's bays are global slots 16-19, past the old 16-slot ceiling; the
    // fixture fills bay 1 with a red PLA and reports bay 3 empty.
    const int first = multiace::bay_global(3, 0);
    for (int s = 0; s < 4; ++s) {
        REQUIRE(ams.get_slot_color_subject(first + s) != nullptr);
        REQUIRE(ams.get_slot_status_subject(first + s) != nullptr);
    }
    CHECK(lv_subject_get_int(ams.get_slot_color_subject(first)) == 0xFF0000);
    CHECK(lv_subject_get_int(ams.get_slot_status_subject(first + 2)) ==
          static_cast<int>(SlotStatus::EMPTY));

    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    ams.deinit_subjects();
}

TEST_CASE("multiACE subscribes the ace object only beside a U1", "[multiace]") {
    auto with_ace = kU1Objects;
    with_ace.push_back("ace");
    CHECK(AmsBackendSnapmaker::required_status_objects(discover(with_ace)).contains("ace"));
    CHECK_FALSE(AmsBackendSnapmaker::required_status_objects(discover(kU1Objects)).contains("ace"));
}

// ============================================================================
// Units and bays
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "head mode shows each ACE as a hub into its head", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    const AmsSystemInfo info = b.get_system_info();

    REQUIRE(info.units.size() == 5);
    CHECK(info.total_slots == 20);
    CHECK(info.units[0].slot_count == 4);
    for (int k = 0; k < 4; ++k) {
        const AmsUnit& u = info.units[static_cast<size_t>(k + 1)];
        CHECK(u.unit_index == k + 1);
        CHECK(u.name == "ACE " + std::to_string(k + 1));
        CHECK(u.first_slot_global_index == multiace::bay_global(k, 0));
        CHECK(u.topology == PathTopology::HUB);
        CHECK(b.get_unit_topology(k + 1) == PathTopology::HUB);
        CHECK(u.hub_tool_label == k); // fixture wires head h to ACE h
        REQUIRE(u.slots.size() == 4);
        for (const SlotInfo& s : u.slots) {
            CHECK(s.mapped_tool == k);
            CHECK(s.extruder_name == (k == 0 ? "extruder" : "extruder" + std::to_string(k)));
        }
    }
    CHECK(b.get_unit_topology(0) == PathTopology::PARALLEL);

    // Head 0 is routed from ACE 1 bay 1 and head 2 from ACE 3 bay 4.
    CHECK(b.get_slot_info(multiace::bay_global(0, 0)).status == SlotStatus::LOADED);
    CHECK(b.get_slot_info(multiace::bay_global(2, 3)).status == SlotStatus::LOADED);
    CHECK(b.get_slot_info(multiace::bay_global(1, 0)).status == SlotStatus::AVAILABLE);
    CHECK(b.get_slot_info(multiace::bay_global(1, 2)).status == SlotStatus::EMPTY);
    const SlotInfo red = b.get_slot_info(multiace::bay_global(1, 0));
    CHECK(red.material == "PLA");
    CHECK(red.brand == "Anycubic");
    CHECK(red.color_rgb == 0xFF0000);
    CHECK(info.units[1].environment.has_value());
    CHECK(info.units[1].environment->humidity_pct == 31.5f);

    CHECK(b.slot_is_actively_loaded(multiace::bay_global(0, 0)));
    CHECK_FALSE(b.slot_is_actively_loaded(multiace::bay_global(0, 1)));
}

TEST_CASE_METHOD(MultiAceFixture, "an unwired head-mode ACE unit feeds no nozzle", "[multiace]") {
    CapturingMultiAce b;
    json frame = ace_frame("head_mode_4ace.json");
    // Head 4 becomes a side-feeder head, so no head names ACE 4.
    frame["ace"]["head_feeder"]["3"] = true;
    frame["ace"]["head_ace"]["3"] = -1;
    b.frame(frame);

    const AmsSystemInfo info = b.get_system_info();
    REQUIRE(info.units.size() == 5);
    REQUIRE(info.units[4].hub_tool_label == -1);
    for (const SlotInfo& s : info.units[4].slots) {
        CHECK(s.mapped_tool == -1);
        CHECK(s.extruder_name.empty());
    }

    const ams_draw::SystemToolLayout layout = ams_draw::compute_system_tool_layout(info, &b);
    CHECK(layout.units[4].tool_count == 0);
    CHECK(layout.units[4].hub_group == -1);
    // The four heads and nothing else: no phantom fifth nozzle.
    CHECK(layout.total_physical_tools == 4);
}

TEST_CASE_METHOD(MultiAceFixture, "units rebuild on mode flip", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    REQUIRE(b.get_system_info().units[1].topology == PathTopology::HUB);
    REQUIRE(b.get_slot_info(multiace::bay_global(1, 2)).mapped_tool == 1);

    b.frame(json{{"ace", {{"mode", "multi"}}}});
    const AmsSystemInfo multi = b.get_system_info();
    REQUIRE(multi.units.size() == 5);
    CHECK(multi.units[1].topology == PathTopology::PARALLEL);
    CHECK(multi.units[2].hub_tool_label == -1);
    // Multi mode: bay s feeds head s on every unit.
    CHECK(b.get_slot_info(multiace::bay_global(1, 2)).mapped_tool == 2);
    CHECK(b.get_slot_info(multiace::bay_global(3, 0)).mapped_tool == 0);

    b.frame(json{{"ace", {{"mode", "normal"}}}});
    CHECK(b.get_system_info().units.size() == 1);
    CHECK(b.get_system_info().total_slots == 4);
}

TEST_CASE_METHOD(MultiAceFixture, "api_version 2 and normal mode behave as plain U1",
                 "[multiace]") {
    SECTION("api_version 2") {
        CapturingMultiAce b;
        json frame = ace_frame("head_mode_4ace.json");
        frame["ace"]["api_version"] = 2;
        b.frame(frame);
        CHECK(b.get_system_info().units.size() == 1);
        CHECK(b.slot_has_prep_sensor(0));
        REQUIRE(b.load_filament(1).success());
        REQUIRE(b.sent.size() == 1);
        CHECK(b.sent[0] == "AUTO_FEEDING EXTRUDER=1 LOAD=1");
    }
    SECTION("normal mode") {
        CapturingMultiAce b;
        b.frame(ace_frame("normal_mode.json"));
        CHECK(b.get_system_info().units.size() == 1);
        REQUIRE(b.unload_filament(1).success());
        REQUIRE(b.sent.size() == 1);
        CHECK(b.sent[0] == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1");
    }
    SECTION("no frame yet") {
        CapturingMultiAce b;
        CHECK(b.get_system_info().units.size() == 1);
        CHECK_FALSE(b.get_dryer_info(1).supported);
    }
}

// ============================================================================
// The heads an ACE feeds
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "ACE gate is not head port presence", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));
    b.frame(feed_frame(0, "preload_finish", true));
    b.frame(feed_frame(2, "preload_finish", true));

    // Head 0 is ACE-fed: its channel's filament_detected is the ACE's gate.
    CHECK_FALSE(b.slot_has_prep_sensor(0));
    CHECK_FALSE(b.slot_insert_presence(0).has_value());
    // Head 2 loads from its own side feeder.
    CHECK(b.slot_has_prep_sensor(2));
    CHECK(b.slot_insert_presence(2) == std::optional<bool>{true});
}

TEST_CASE_METHOD(MultiAceFixture, "an ACE-fed head's gate edge is not a spool insert",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));
    for (int head : {0, 2}) {
        b.frame(feed_frame(head, "wait_insert", false));
        b.frame(feed_frame(head, "wait_insert", true));
    }

    // Head 0's gate rises when the ACE feeds it; no spool went into the head.
    CHECK(b.slot_insert_identity(0) == InsertIdentity::NotRead);
    // Head 2 loads from its own feeder, where the same edge is an insert.
    CHECK(b.slot_insert_identity(2) == InsertIdentity::Pending);
}

TEST_CASE_METHOD(MultiAceFixture, "ACE head unload ends at preload_finish", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));

    SECTION("ACE head") {
        b.frame(feed_frame(0, "load_finish"));
        REQUIRE(b.slot_has_filament_at_toolhead(0));
        b.frame(feed_frame(0, "unload_prepare"));
        REQUIRE(b.get_current_action() == AmsAction::UNLOADING);
        b.frame(feed_frame(0, "unload_doing"));
        b.frame(feed_frame(0, "preload_finish"));
        CHECK(b.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(b.slot_has_filament_at_toolhead(0));
    }
    SECTION("feeder head still waits for unload_finish") {
        b.frame(feed_frame(2, "unload_doing"));
        REQUIRE(b.get_current_action() == AmsAction::UNLOADING);
        b.frame(feed_frame(2, "preload_finish"));
        CHECK(b.get_system_info().action == AmsAction::UNLOADING);
        b.frame(feed_frame(2, "unload_finish"));
        CHECK(b.get_current_action() == AmsAction::IDLE);
    }
    SECTION("an ACE head resting at preload_finish ends nothing") {
        b.frame(feed_frame(0, "load_feeding"));
        REQUIRE(b.get_current_action() == AmsAction::LOADING);
        b.frame(feed_frame(0, "preload_finish"));
        CHECK(b.get_system_info().action == AmsAction::LOADING);
    }
}

// ============================================================================
// Commands
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "load bay emits ACE and SLOT always", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));

    // Head 1 holds nothing; ACE 2 is wired to it.
    REQUIRE(b.load_filament(multiace::bay_global(1, 1)).success());
    REQUIRE(b.sent.size() == 1);
    CHECK(b.sent[0] == "ACE_LOAD_HEAD HEAD=1 ACE=1 SLOT=1");

    SECTION("an empty bay is refused") {
        b.sent.clear();
        CHECK_FALSE(b.load_filament(multiace::bay_global(1, 2)).success());
        CHECK(b.sent.empty());
    }
    SECTION("a bay no ACE-fed head is wired to is refused") {
        b.frame(json{{"ace", {{"head_feeder", {{"3", true}}}}}});
        b.sent.clear();
        CHECK_FALSE(b.load_filament(multiace::bay_global(3, 0)).success());
        CHECK(b.sent.empty());
    }
}

TEST_CASE_METHOD(MultiAceFixture, "load into an occupied head unloads first", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"toolhead", {{"extruder", "extruder"}}}});
    const AmsSystemInfo info = b.get_system_info();
    // A HUB unit with a head picked: the base rule would ask the screen to swap.
    REQUIRE(info.current_slot == 0);
    CHECK_FALSE(b.needs_unload_before_load(info, multiace::bay_global(0, 1)));

    // Head 0 is routed from ACE 1 bay 1; loading bay 2 unloads it first, alone.
    REQUIRE(b.load_filament(multiace::bay_global(0, 1)).success());
    REQUIRE(b.sent.size() == 1);
    CHECK(b.sent[0] == "ACE_UNLOAD_HEAD HEAD=0");

    SECTION("the load goes once a frame shows the head empty") {
        b.answer_ok();
        CHECK(b.sent.size() == 1);
        b.frame(feed_frame(0, "unload_doing"));
        CHECK(b.sent.size() == 1);
        json emptied = {{"ace", {{"head_source", {{"2", {{"ace_index", 2}, {"slot", 3}}}}}}}};
        emptied.update(feed_frame(0, "preload_finish"));
        b.frame(emptied);
        REQUIRE(b.sent.size() == 2);
        CHECK(b.sent[1] == "ACE_LOAD_HEAD HEAD=0 ACE=0 SLOT=1");
        // Sent once, not on every later frame.
        b.frame(feed_frame(0, "preload_finish"));
        CHECK(b.sent.size() == 2);
    }
    SECTION("a Klipper restart drops the load") {
        b.answer_ok();
        b.on_klipper_restarted();
        json emptied = {{"ace", {{"head_source", {{"2", {{"ace_index", 2}, {"slot", 3}}}}}}}};
        emptied.update(feed_frame(0, "preload_finish"));
        b.frame(emptied);
        CHECK(b.sent.size() == 1);
    }
    SECTION("an unload that answers OK while head_source survives loads nothing") {
        b.answer_ok();
        b.frame(feed_frame(0, "preload_finish"));
        b.frame(json{{"ace", {{"swap_phase", "idle"}}}});
        CHECK(b.sent.size() == 1);
    }
    SECTION("a cleared head_source with the head still loaded loads nothing") {
        b.frame(feed_frame(0, "load_finish"));
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        CHECK(b.sent.size() == 1);
    }
    SECTION("a failed unload drops the load") {
        b.answer_error("unload failed");
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        CHECK(b.sent.size() == 1);
    }
    SECTION("another filament op drops the load") {
        REQUIRE(b.unload_filament(1).success());
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        REQUIRE(b.sent.size() == 2);
        CHECK(b.sent[1] == "ACE_UNLOAD_HEAD HEAD=1");
    }
    SECTION("a tool change drops the load") {
        REQUIRE(b.change_tool(1).success());
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        REQUIRE(b.sent.size() == 2);
        CHECK(b.sent[1] == "T1");
    }
    SECTION("the routed bay of a loaded head is already loaded") {
        b.frame(feed_frame(0, "load_finish"));
        b.sent.clear();
        CHECK_FALSE(b.load_filament(multiace::bay_global(0, 0)).success());
        CHECK(b.sent.empty());
    }
}

TEST_CASE_METHOD(MultiAceFixture, "a refused op leaves a pending bay load standing", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"toolhead", {{"extruder", "extruder"}}}});
    b.record_errors();

    // Head 0 is loaded and routed from ACE 1 bay 1: loading bay 2 defers
    // behind the head's unload.
    REQUIRE(b.load_filament(multiace::bay_global(0, 1)).success());
    REQUIRE(b.sent.size() == 1);
    CHECK(b.sent[0] == "ACE_UNLOAD_HEAD HEAD=0");
    b.answer_ok();

    // A tap on an empty bay is refused - and must not take the deferred load
    // with it: the refusal changed nothing.
    CHECK_FALSE(b.load_filament(multiace::bay_global(0, 2)).success());
    CHECK(b.sent.size() == 1);
    CHECK(b.errors.empty());

    // The head empties and the deferred load still goes.
    json emptied = {{"ace", {{"head_source", json::object()}}}};
    emptied.update(feed_frame(0, "preload_finish"));
    b.frame(emptied);
    REQUIRE(b.sent.size() == 2);
    CHECK(b.sent[1] == "ACE_LOAD_HEAD HEAD=0 ACE=0 SLOT=1");
}

TEST_CASE_METHOD(MultiAcePrintFixture, "bay swap refused while paused", "[multiace]") {
    CapturingMultiAce b(api.get(), &client);
    b.frame(ace_frame("head_mode_4ace.json"));
    helix::test::set_wire_state(state, PrintJobState::PAUSED);

    CHECK_FALSE(b.load_filament(multiace::bay_global(0, 1)).success());
    CHECK(b.sent.empty());
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(MultiAceFixture, "every head goes through the engine while it is active",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));

    // Heads 2 and 3 load from their side feeders, but the engine owns the
    // feed state machine for them too.
    REQUIRE(b.load_filament(2).success());
    REQUIRE(b.unload_filament(3).success());
    REQUIRE(b.load_filament(1).success());
    REQUIRE(b.unload_filament(0).success());
    REQUIRE(b.sent.size() == 4);
    CHECK(b.sent[0] == "ACE_LOAD_HEAD HEAD=2");
    CHECK(b.sent[1] == "ACE_UNLOAD_HEAD HEAD=3");
    // Head mode: the engine picks the wired unit and the head's last bay.
    CHECK(b.sent[2] == "ACE_LOAD_HEAD HEAD=1");
    CHECK(b.sent[3] == "ACE_UNLOAD_HEAD HEAD=0");
}

TEST_CASE_METHOD(MultiAceFixture, "an unload with no head to name refuses under the engine",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    // No current slot and no head named: nothing resolves, and the stock leaf
    // macro must not run under the engine's shadowed feed files.
    const AmsError err = b.unload_filament(-1);
    CHECK_FALSE(err.success());
    CHECK(err.result == AmsResult::WRONG_STATE);
    CHECK(err.user_msg == "Nothing loaded");
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "multi mode always names the bay of a head load", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("multi_mode_2ace.json"));

    // Head 1 was last fed from ACE 2 bay 2.
    REQUIRE(b.load_filament(1).success());
    // Head 3 is manual: the engine loads it by hand-feed rules, no bay.
    REQUIRE(b.load_filament(3).success());
    REQUIRE(b.sent.size() == 2);
    CHECK(b.sent[0] == "ACE_LOAD_HEAD HEAD=1 ACE=1 SLOT=1");
    CHECK(b.sent[1] == "ACE_LOAD_HEAD HEAD=3");

    // Head 0 has no recorded bay, and a bare load would take the active unit's.
    b.sent.clear();
    const AmsError refused = b.load_filament(0);
    CHECK_FALSE(refused.success());
    CHECK(refused.user_msg == "Select a filament slot to load");
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "a routed bay unloads and selects through its head",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));

    REQUIRE(b.unload_filament(multiace::bay_global(2, 3)).success());
    REQUIRE(b.select_slot(multiace::bay_global(2, 3)).success());
    REQUIRE(b.sent.size() == 2);
    CHECK(b.sent[0] == "ACE_UNLOAD_HEAD HEAD=2");
    CHECK(b.sent[1] == "T2");

    b.sent.clear();
    CHECK_FALSE(b.unload_filament(multiace::bay_global(2, 0)).success());
    CHECK_FALSE(b.select_slot(multiace::bay_global(2, 0)).success());
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "bay path segments", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(feed_frame(0, "load_finish"));

    CHECK(b.get_slot_filament_segment(multiace::bay_global(0, 0)) == PathSegment::NOZZLE);
    CHECK(b.get_slot_filament_segment(multiace::bay_global(0, 1)) == PathSegment::PREP);
    CHECK(b.get_slot_filament_segment(multiace::bay_global(0, 2)) == PathSegment::NONE);
    CHECK(b.can_unload_from_toolhead(multiace::bay_global(0, 0)));
    CHECK_FALSE(b.can_unload_from_toolhead(multiace::bay_global(0, 1)));
}

TEST_CASE_METHOD(MultiAceFixture, "batch ops with ACE-fed heads", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));
    b.frame(feed_frame(0, "load_finish"));
    b.frame(feed_frame(2, "load_finish"));

    SECTION("batch load into an ACE head is not supported yet") {
        CHECK(b.load_filament_batch({0, 2}).result == AmsResult::NOT_SUPPORTED);
        CHECK(b.sent.empty());
    }
    SECTION("unloading every loaded head is the engine's unload-all") {
        REQUIRE(b.unload_filament_batch({2, 0}).success());
        REQUIRE(b.sent.size() == 1);
        CHECK(b.sent[0] == "ACE_UNLOAD_ALL_HEADS");
    }
    SECTION("a partial selection unloads head by head") {
        b.frame(feed_frame(1, "load_finish"));
        REQUIRE(b.unload_filament_batch({0, 2}).success());
        REQUIRE(b.sent.size() == 1);
        CHECK(b.sent[0] == "ACE_UNLOAD_HEAD HEAD=0\nACE_UNLOAD_HEAD HEAD=2");
    }
}

TEST_CASE_METHOD(MultiAceFixture, "an ACE-fed head is not offered as a batch load target",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));

    // Heads 0 and 1 are ACE-fed: the backend refuses a batch load into them,
    // so the picker must not read them as eligible.
    CHECK(b.slot_op_eligibility(0, /*load=*/true) ==
          AmsBackend::FilamentOpEligibility::NotSupported);
    CHECK(b.slot_op_eligibility(1, /*load=*/true) ==
          AmsBackend::FilamentOpEligibility::NotSupported);
    // A feeder head keeps the channel-state answer, and a bay stays Busy.
    json feeder_ready = feed_frame(2, "wait_insert");
    feeder_ready["filament_feed right"]["extruder2"]["module_exist"] = true;
    b.frame(feeder_ready);
    b.frame(json{{"filament_motion_sensor e2_filament", {{"enabled", true}}}});
    CHECK(b.slot_op_eligibility(2, /*load=*/true) == AmsBackend::FilamentOpEligibility::Eligible);
    CHECK(b.slot_op_eligibility(multiace::bay_global(0, 0), /*load=*/true) ==
          AmsBackend::FilamentOpEligibility::Busy);
}

TEST_CASE_METHOD(MultiAceFixture, "a batch unload names its own refusal", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));

    // An unrouted bay in an unload batch: the refusal must say unload, not
    // the load-side wording.
    const AmsError err = b.unload_filament_batch({multiace::bay_global(0, 2)});
    CHECK(err.result == AmsResult::NOT_SUPPORTED);
    CHECK(err.technical_msg.find("unload") != std::string::npos);
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "batch load runs the single-load checks per head", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_2ace_2feeder.json"));

    SECTION("feeder heads go through the engine") {
        REQUIRE(b.load_filament_batch({3, 2}).success());
        REQUIRE(b.sent.size() == 1);
        CHECK(b.sent[0] == "ACE_LOAD_HEAD HEAD=3\nACE_LOAD_HEAD HEAD=2");
    }
    SECTION("a loaded head refuses the whole batch") {
        b.frame(feed_frame(2, "load_finish"));
        CHECK_FALSE(b.load_filament_batch({3, 2}).success());
        CHECK(b.sent.empty());
    }
    SECTION("a feeder out of automatic mode refuses the whole batch") {
        b.frame(json{{"filament_feed right",
                      {{"extruder3",
                        {{"filament_detected", true},
                         {"channel_state", "wait_insert"},
                         {"channel_error", "ok"},
                         {"module_exist", false}}}}}});
        CHECK_FALSE(b.load_filament_batch({2, 3}).success());
        CHECK_FALSE(b.load_filament(3).success());
        CHECK(b.sent.empty());
    }
}

// ============================================================================
// Dryer
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "dry targets the unit", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));

    CHECK_FALSE(b.get_dryer_info(0).supported);
    const DryerInfo idle = b.get_dryer_info(2);
    CHECK(idle.supported);
    CHECK(idle.max_temp_c == 55.0f);
    CHECK(b.get_dryer_info(4).active);

    REQUIRE(b.start_drying(45.0f, 240, -1, 2).success());
    REQUIRE(b.stop_drying(2).success());
    REQUIRE(b.sent.size() == 2);
    CHECK(b.sent[0] == "ACE_DRY ACE=1 TEMP=45 DURATION=240");
    CHECK(b.sent[1] == "ACE_STOP_DRYING ACE=1");

    b.sent.clear();
    // The ACE Pro's range is shared with the ACE backend.
    CHECK_FALSE(b.start_drying(70.0f, 240, -1, 2).success());
    CHECK_FALSE(b.start_drying(45.0f, 240, -1, 0).success());
    CHECK(b.sent.empty());

    // Unit 4 is drying at 45C with 180 of 240 minutes left: a temperature
    // change restarts on what is left.
    REQUIRE(b.update_drying(50.0f, -1, -1, 4).success());
    REQUIRE(b.sent.size() == 2);
    CHECK(b.sent[0] == "ACE_STOP_DRYING ACE=3");
    CHECK(b.sent[1] == "ACE_DRY ACE=3 TEMP=50 DURATION=180");
}

// ============================================================================
// Errors
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "coded 525 error files under its head", "[multiace]") {
    CapturingMultiAce b;
    const std::string line = "!! 0003-0525-0002-0207 [multiACE] head 3 is wired to ACE 3 (one ACE "
                             "per head) - refusing load from ACE 1.";

    ClassifyContext idle;
    const auto ev = b.classify_error(line, idle);
    REQUIRE(ev);
    CHECK(ev->code == "0003-0525-0002-0207");
    CHECK(ev->detail.find("Toolhead 3: [multiACE] head 3 is wired") == 0);
    CHECK(ev->raw_detail == line.substr(3));
    CHECK(ev->command_refusal);

    ClassifyContext printing;
    printing.is_printing = true;
    const auto during = b.classify_error(line, printing);
    REQUIRE(during);
    CHECK_FALSE(during->command_refusal);

    // Index 0 is also the engine's "no head": a fault that names none gets no
    // toolhead label.
    const auto headless =
        b.classify_error("!! 0003-0525-0000-0200 [multiACE] HEAD must be 0-3", idle);
    REQUIRE(headless);
    CHECK(headless->detail == "[multiACE] HEAD must be 0-3");
    const auto head_one = b.classify_error(
        "!! 0003-0525-0000-0203 [multiACE] head 1 has filament of ACE 1 / Slot 2 STAGED", idle);
    REQUIRE(head_one);
    CHECK(head_one->detail.find("Toolhead 1: ") == 0);
    const auto head_ten = b.classify_error("!! 0003-0525-0000-0203 head 10 x", idle);
    REQUIRE(head_ten);
    CHECK(head_ten->detail == "head 10 x");

    // Not multiACE's: another module id, or a code outside its band.
    CHECK_FALSE(b.classify_error("!! 0003-0523-0002-0207 runout", idle));
    CHECK_FALSE(b.classify_error("!! 0003-0525-0002-0100 something", idle));
    CHECK_FALSE(b.classify_error("// 0003-0525-0002-0207 echo", idle));
}

TEST_CASE_METHOD(MultiAceFixture, "a load the firmware fails marks its head unconfirmed",
                 "[multiace][resume][load_unconfirmed]") {
    CapturingMultiAce b;

    SECTION("a head the engine does not feed") {
        b.frame(ace_frame("multi_mode_2ace.json"));
        REQUIRE(b.load_filament(3).success());
        REQUIRE(b.sent.back() == "ACE_LOAD_HEAD HEAD=3");
        b.answer_error("load failed");
        CHECK(SnapmakerTestAccess::load_unconfirmed(b, 3));
    }
    SECTION("a head load from its recorded bay in multi mode") {
        b.frame(ace_frame("multi_mode_2ace.json"));
        REQUIRE(b.load_filament(1).success());
        REQUIRE(b.sent.back() == "ACE_LOAD_HEAD HEAD=1 ACE=1 SLOT=1");
        b.answer_error("load failed");
        CHECK(SnapmakerTestAccess::load_unconfirmed(b, 1));
    }
    SECTION("a bay loaded into an empty head") {
        b.frame(ace_frame("head_mode_4ace.json"));
        REQUIRE(b.load_filament(multiace::bay_global(1, 1)).success());
        REQUIRE(b.sent.back() == "ACE_LOAD_HEAD HEAD=1 ACE=1 SLOT=1");
        b.answer_error("load failed");
        CHECK(SnapmakerTestAccess::load_unconfirmed(b, 1));
    }
    SECTION("a bay load that waited for its head to empty") {
        b.frame(ace_frame("head_mode_4ace.json"));
        b.frame(json{{"toolhead", {{"extruder", "extruder"}}}});
        REQUIRE(b.load_filament(multiace::bay_global(0, 1)).success());
        REQUIRE(b.sent.back() == "ACE_UNLOAD_HEAD HEAD=0");
        b.answer_ok();
        json emptied = {{"ace", {{"head_source", {{"2", {{"ace_index", 2}, {"slot", 3}}}}}}}};
        emptied.update(feed_frame(0, "preload_finish"));
        b.frame(emptied);
        REQUIRE(b.sent.back() == "ACE_LOAD_HEAD HEAD=0 ACE=0 SLOT=1");
        CHECK_FALSE(SnapmakerTestAccess::load_unconfirmed(b, 0));
        b.answer_error("load failed");
        CHECK(SnapmakerTestAccess::load_unconfirmed(b, 0));
    }
}

TEST_CASE_METHOD(MultiAceFixture, "a failed head load surfaces once", "[multiace]") {
    CapturingMultiAce b;
    b.record_errors();
    b.frame(ace_frame("head_mode_4ace.json"));
    REQUIRE(b.errors.empty());

    // The engine's head_source says the load into head 1 failed.
    b.frame(
        json{{"ace",
              {{"head_source", {{"1", {{"ace_index", 0}, {"slot", 1}, {"load_failed", true}}}}}}}});
    REQUIRE(b.errors.size() == 1);
    CHECK(b.errors[0].find("Toolhead 2") != std::string::npos);

    // Frames restating the standing failure do not repeat it, and a source
    // that clears arms the edge again.
    b.frame(
        json{{"ace",
              {{"head_source", {{"1", {{"ace_index", 0}, {"slot", 1}, {"load_failed", true}}}}}}}});
    CHECK(b.errors.size() == 1);
    b.frame(json{{"ace", {{"head_source", json::object()}}}});
    b.frame(
        json{{"ace",
              {{"head_source", {{"1", {{"ace_index", 0}, {"slot", 1}, {"load_failed", true}}}}}}}});
    CHECK(b.errors.size() == 2);
}

namespace {
/// The app paused on a recoverable cause, as prepare_for_resume classifies it.
void pause_app_print() {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                               {"print_stats", {{"state", "paused"}}},
                               {"virtual_sdcard", {{"is_active", false}}}});
}

/// Head @p head's feed channel as an automatic feeder reports it.
json fed_head(int head, const std::string& state) {
    json frame = feed_frame(head, state);
    auto& channel = frame.begin().value()["extruder" + std::to_string(head)];
    channel["module_exist"] = true;
    channel["disable_auto"] = false;
    return frame;
}

AmsError resume_head(CapturingMultiAce& b, int head) {
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    bool fired = false;
    b.prepare_for_resume(head, [&](const AmsError& err) {
        fired = true;
        captured = err;
    });
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(fired);
    return captured;
}
} // namespace

TEST_CASE_METHOD(MultiAceFixture, "Resume proceeds from a loaded ACE head",
                 "[multiace][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_app_print();
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"print_stats", {{"state", "paused"}}}});
    // An ACE-fed head the engine loaded ends at load_finish like a stock head.
    b.frame(fed_head(1, "preload_finish"));
    b.frame(fed_head(1, "load_finish"));
    b.sent.clear();

    CHECK(resume_head(b, 1).success());
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "a failed ACE load holds Resume until the head loads",
                 "[multiace][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_app_print();
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"print_stats", {{"state", "paused"}}}});
    // The channel still reads the last good load when the engine reports the
    // failed one, so only the engine's load_failed holds Resume here.
    b.frame(fed_head(1, "preload_finish"));
    b.frame(fed_head(1, "load_finish"));
    b.frame(
        json{{"ace",
              {{"head_source", {{"1", {{"ace_index", 0}, {"slot", 1}, {"load_failed", true}}}}}}}});
    b.sent.clear();

    // The engine owns the feed, so Resume sends no stock AUTO_FEEDING; it
    // refuses and sends the user to the filament panel.
    const AmsError held = resume_head(b, 1);
    CHECK_FALSE(held.success());
    CHECK(held.user_msg.find("Toolhead 2 (T1) didn't load") == 0);
    CHECK(held.suggestion == "Load it in the filament panel, then tap Resume.");
    CHECK(b.sent.empty());

    // The engine's next load reaching load_finish releases it.
    b.frame(fed_head(1, "preload_finish"));
    b.frame(fed_head(1, "load_finish"));
    CHECK(resume_head(b, 1).success());
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "an ACE head whose feeder failed holds Resume without a refeed",
                 "[multiace][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_app_print();
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"print_stats", {{"state", "paused"}}}});
    b.frame(fed_head(1, "load_fail"));
    b.sent.clear();

    const AmsError held = resume_head(b, 1);
    CHECK_FALSE(held.success());
    CHECK(held.suggestion == "Load it in the filament panel, then tap Resume.");
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "a runout on an engine-fed head refuses Resume without a refeed",
                 "[multiace][resume][load_unconfirmed]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_app_print();
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    b.frame(json{{"print_stats", {{"state", "paused"}}}});
    SnapmakerTestAccess::set_sensor_present(b, 1, false);
    b.sent.clear();

    const AmsError held = resume_head(b, 1);
    CHECK_FALSE(held.success());
    CHECK(held.user_msg == "Toolhead 2 (T1): Filament runout");
    CHECK(held.suggestion == "Load it in the filament panel, then tap Resume.");
    CHECK(b.sent.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "only the four heads are tools", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    REQUIRE(b.get_system_info().total_slots == 20);

    CHECK_FALSE(b.change_tool(8).success());
    CHECK_FALSE(b.change_tool(4).success());
    CHECK(b.sent.empty());
    REQUIRE(b.change_tool(3).success());
    REQUIRE(b.sent.size() == 1);
    CHECK(b.sent[0] == "T3");
}

TEST_CASE_METHOD(MultiAceFixture, "a stock U1 keeps a PREP sensor on every head",
                 "[multiace][snapmaker]") {
    AmsBackendSnapmaker stock(nullptr, nullptr);
    for (int h = 0; h < 4; ++h) {
        CHECK(stock.slot_has_prep_sensor(h));
    }
    CHECK_FALSE(stock.slot_has_prep_sensor(4));
    CHECK_FALSE(stock.slot_has_prep_sensor(-1));
}

TEST_CASE_METHOD(MultiAceFixture, "a dropped bay load says so", "[multiace]") {
    CapturingMultiAce b;
    b.record_errors();
    b.frame(ace_frame("head_mode_4ace.json"));
    REQUIRE(b.load_filament(multiace::bay_global(0, 1)).success());
    REQUIRE(b.sent.size() == 1);

    SECTION("the head still holds filament when the window closes") {
        b.close_pending_window();
        b.answer_ok();
        b.frame(feed_frame(0, "preload_finish"));
        REQUIRE(b.errors.size() == 1);
        CHECK(b.errors[0].find("ACE 1 bay 2 was not loaded") == 0);
        // Dropped for good: a later empty frame sends nothing.
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        CHECK(b.sent.size() == 1);
    }
    SECTION("a late empty frame after the window closed loads nothing") {
        b.close_pending_window();
        b.answer_ok();
        // The first frame after the answer already shows the head empty, but
        // the window is shut.
        json emptied = {{"ace", {{"head_source", json::object()}}}};
        emptied.update(feed_frame(0, "preload_finish"));
        b.frame(emptied);
        CHECK(b.sent.size() == 1);
        CHECK(b.errors.size() == 1);
    }
    SECTION("the engine stops driving the units") {
        b.frame(json{{"ace", {{"mode", "normal"}}}});
        REQUIRE(b.errors.size() == 1);
        b.frame(json{{"ace", {{"mode", "head"}, {"head_source", json::object()}}}});
        CHECK(b.sent.size() == 1);
    }
}

TEST_CASE_METHOD(MultiAcePrintFixture, "a pending bay load yields to a print", "[multiace]") {
    CapturingMultiAce b(api.get(), &client);
    b.record_errors();
    b.frame(ace_frame("head_mode_4ace.json"));
    REQUIRE(b.load_filament(multiace::bay_global(0, 1)).success());
    REQUIRE(b.sent.size() == 1);
    b.answer_ok();

    SECTION("the printer reports the print") {
        helix::test::set_wire_state(state, PrintJobState::PAUSED);
    }
    SECTION("a print start this screen is preparing, not yet on the wire") {
        state.print_state().set_print_start_state(PrintStartPhase::HOMING, "", 0);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        REQUIRE(state.print_state().get_print_lifecycle() == PrintState::Preparing);
    }
    b.frame(json{{"ace", {{"head_source", json::object()}}}});
    CHECK(b.sent.size() == 1);
    CHECK(b.errors.size() == 1);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE_METHOD(MultiAceFixture, "a loaded head with no recorded source refuses a bay swap",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    // Head 1 holds filament the engine has no head_source for.
    b.frame(feed_frame(1, "load_finish"));

    const AmsError refused = b.load_filament(multiace::bay_global(1, 0));
    CHECK_FALSE(refused.success());
    CHECK(refused.user_msg == "Toolhead 2: Unload from toolhead first");
    CHECK(b.sent.empty());
}

// ============================================================================
// Spool identity: upstream bays, numbering, captions
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "a routed head's spool lives in its bay", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));

    // Head 0 is routed from ACE 1 bay 1, head 2 from ACE 3 bay 4; heads 1 and
    // 3 and every bay hold a spool of their own (or none).
    CHECK(b.upstream_slot(0) == std::optional<int>{multiace::bay_global(0, 0)});
    CHECK(b.upstream_slot(2) == std::optional<int>{multiace::bay_global(2, 3)});
    CHECK_FALSE(b.upstream_slot(1).has_value());
    CHECK_FALSE(b.upstream_slot(3).has_value());
    CHECK_FALSE(b.upstream_slot(multiace::bay_global(0, 0)).has_value());
    CHECK_FALSE(b.upstream_slot(-1).has_value());

    // Without the engine contract there are no bays to be upstream.
    CapturingMultiAce plain;
    CHECK_FALSE(plain.upstream_slot(0).has_value());
}

TEST_CASE_METHOD(MultiAceFixture, "a head's consumption lands on the bay feeding it",
                 "[multiace][filament_tracker]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    const AmsSystemInfo info = b.get_system_info();
    REQUIRE(info.total_slots == 20);

    // Head 0 is routed from ACE 1 bay 1 and head 2 from ACE 3 bay 4: what
    // those nozzles extrude comes off the bays' spools.
    CHECK(b.slot_for_extruder(0) == std::optional<int>{multiace::bay_global(0, 0)});
    CHECK(b.slot_for_extruder(2) == std::optional<int>{multiace::bay_global(2, 3)});
    for (int head = 0; head < 4; ++head) {
        CHECK(b.slot_for_extruder(head) == b.head_source_slot(head, info));
    }
    // The U1 has four extruders however many bays the ACEs add.
    for (int e = 4; e < info.total_slots; ++e) {
        CHECK_FALSE(b.slot_for_extruder(e).has_value());
    }

    // Without the engine contract each head feeds itself.
    CapturingMultiAce plain;
    for (int head = 0; head < 4; ++head) {
        CHECK(plain.slot_for_extruder(head) == std::optional<int>{head});
    }
    CHECK_FALSE(plain.slot_for_extruder(4).has_value());
}

TEST_CASE_METHOD(MultiAceFixture, "spool numbering counts a routed bay once", "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));

    // A routed head wears its bay's number.
    CHECK(b.spool_display_number(0) == b.spool_display_number(multiace::bay_global(0, 0)));
    CHECK(b.spool_display_number(2) == b.spool_display_number(multiace::bay_global(2, 3)));

    // Lanes that hold their own spool number 1..N with no gaps or repeats.
    std::set<int> numbers;
    for (int gi = 0; gi < b.get_system_info().total_slots; ++gi) {
        if (b.upstream_slot(gi).has_value()) {
            continue;
        }
        INFO("slot " << gi);
        CHECK(numbers.insert(b.spool_display_number(gi)).second);
    }
    CHECK(numbers == std::set<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18});

    // A backend with no shared spools keeps the plain 1-based lane numbers.
    CapturingMultiAce plain;
    for (int gi = 0; gi < plain.get_system_info().total_slots; ++gi) {
        CHECK(plain.spool_display_number(gi) == gi + 1);
    }
}

TEST_CASE_METHOD(MultiAceFixture, "an ACE-fed head's upstream caption names its unit and bay",
                 "[multiace]") {
    CapturingMultiAce b;
    b.frame(ace_frame("head_mode_4ace.json"));
    const AmsSystemInfo info = b.get_system_info();

    CHECK(helix::ui::upstream_lane_caption(info, b, 0) ==
          std::string("ACE 1 ") + helix::ui::kUnitSeparator + " 1");
    CHECK(helix::ui::upstream_lane_caption(info, b, 2) ==
          std::string("ACE 3 ") + helix::ui::kUnitSeparator + " 4");
    // An unrouted head and a bay have no upstream to name.
    CHECK(helix::ui::upstream_lane_caption(info, b, 1).empty());
    CHECK(helix::ui::upstream_lane_caption(info, b, multiace::bay_global(0, 0)).empty());
}

TEST_CASE_METHOD(MultiAceFixture, "a route naming an absent unit reads as unrouted", "[multiace]") {
    CapturingMultiAce b;
    json frame = ace_frame("head_mode_4ace.json");
    // Head 2's source names ACE 3, but only ACE 1 and ACE 2 answer.
    json kept = json::array();
    for (const auto& u : frame["ace"]["aces"]) {
        if (u["idx"] <= 1) {
            kept.push_back(u);
        }
    }
    frame["ace"]["aces"] = kept;
    frame["ace"]["head_ace"] = {{"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}};
    frame["ace"]["head_source"] = {{"0", {{"ace_index", 0}, {"slot", 0}}},
                                   {"2", {{"ace_index", 2}, {"slot", 0}}}};
    b.frame(frame);

    const AmsSystemInfo info = b.get_system_info();
    REQUIRE(info.units.size() == 3);
    // Head 0's route names a shown unit; head 2's does not.
    CHECK(b.upstream_slot(0) == std::optional<int>{multiace::bay_global(0, 0)});
    CHECK(b.upstream_slot(2) == std::nullopt);
    CHECK(helix::ui::upstream_lane_caption(info, b, 2).empty());
    // The absent unit's index range is not numbered as spools: ACE 2 bay 1 is
    // the 8th spool-owning lane, not the 9th.
    CHECK(b.spool_display_number(multiace::bay_global(1, 0)) == 8);
}

TEST_CASE_METHOD(MultiAceFixture, "spool numbers skip no index the engine left out", "[multiace]") {
    CapturingMultiAce b;
    json frame = ace_frame("head_mode_4ace.json");
    // The engine reports ACE 1 and ACE 3 only: the ACE 2 index range is a
    // gap, not four invisible spools.
    json kept = json::array();
    for (const auto& u : frame["ace"]["aces"]) {
        if (u["idx"] != 1) {
            kept.push_back(u);
        }
    }
    frame["ace"]["aces"] = kept;
    frame["ace"]["head_ace"] = {{"0", 0}, {"1", 0}, {"2", 2}, {"3", 2}};
    frame["ace"]["head_source"] = json{{"0", {{"ace_index", 0}, {"slot", 0}}}};
    b.frame(frame);

    const AmsSystemInfo info = b.get_system_info();
    REQUIRE(info.units.size() == 4); // the heads plus the three units shown
    REQUIRE(info.total_slots == multiace::bay_global(3, 3) + 1);

    // Every lane a unit covers, numbered: a routed head wears its bay's
    // number, so the DISTINCT numbers over the covered lanes run 1..N with
    // no skip where the missing unit's index range sits.
    std::vector<int> numbers;
    for (int gi = 0; gi < info.total_slots; ++gi) {
        if (info.get_slot_global(gi)) {
            numbers.push_back(b.spool_display_number(gi));
        }
    }
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    std::vector<int> expected(numbers.size());
    std::iota(expected.begin(), expected.end(), 1);
    CHECK(numbers == expected);
}

// ============================================================================
// Bay identity editing
// ============================================================================

TEST_CASE_METHOD(MultiAceFixture, "a bay edit lands and survives the engine's frames",
                 "[multiace][ams]") {
    helix::test::RegisteredBackend<CapturingMultiAce> reg;
    CapturingMultiAce& b = *reg;
    b.frame(ace_frame("head_mode_4ace.json"));

    // ACE 2 bay 1 feeds head 1, which nothing is routed to: an edit there is
    // identity for a spool sitting in the box, not for a printing lane.
    const int bay = multiace::bay_global(1, 0);
    REQUIRE(b.get_slot_info(bay).material == "PLA");
    REQUIRE(b.get_slot_info(bay).color_rgb == 0xFF0000);

    SlotInfo edit = b.get_slot_info(bay);
    edit.material = "PETG";
    edit.brand = "Polymaker";
    edit.color_rgb = 0x1E5AA8;
    edit.spool_name = "Blue PETG";
    REQUIRE(b.commit_user_edit(bay, b.get_slot_info(bay), edit).success());

    const SlotInfo edited = b.get_slot_info(bay);
    CHECK(edited.material == "PETG");
    CHECK(edited.brand == "Polymaker");
    CHECK(edited.color_rgb == 0x1E5AA8u);
    CHECK(edited.spool_name == "Blue PETG");

    SECTION("a frame restating the engine's table does not take the bay back") {
        b.frame(ace_frame("head_mode_4ace.json"));
        const SlotInfo reframed = b.get_slot_info(bay);
        CHECK(reframed.material == "PETG");
        CHECK(reframed.brand == "Polymaker");
        CHECK(reframed.color_rgb == 0x1E5AA8u);
    }
    SECTION("a topology-only frame rebuilds the units and keeps the edit") {
        b.frame(json{{"ace", {{"head_source", json::object()}}}});
        CHECK(b.get_slot_info(bay).material == "PETG");
        CHECK(b.get_slot_info(bay).color_rgb == 0x1E5AA8u);
    }

    // The head lane the bay feeds keeps the U1's own identity: a bay edit
    // writes the bay, and nothing else.
    CHECK(b.get_slot_info(1).material.empty());
    CHECK(b.get_slot_info(1).color_rgb != edited.color_rgb);
}

TEST_CASE_METHOD(MultiAceFixture, "a bay edit clears when a different spool is detected",
                 "[multiace][ams]") {
    helix::test::RegisteredBackend<CapturingMultiAce> reg;
    CapturingMultiAce& b = *reg;
    b.frame(ace_frame("head_mode_4ace.json"));

    const int bay = multiace::bay_global(1, 0);
    SlotInfo edit = b.get_slot_info(bay);
    edit.material = "PETG";
    edit.brand = "Polymaker";
    edit.color_rgb = 0x1E5AA8;
    edit.spool_name = "Blue PETG";
    REQUIRE(b.commit_user_edit(bay, b.get_slot_info(bay), edit).success());
    REQUIRE(b.get_slot_info(bay).material == "PETG");

    // The engine restating its table is the same spool: the edit stands.
    b.frame(ace_frame("head_mode_4ace.json"));
    CHECK(b.get_slot_info(bay).material == "PETG");

    SECTION("an empty bay states nothing; the edit waits for evidence") {
        json emptied = ace_frame("head_mode_4ace.json");
        emptied["ace"]["aces"][1]["slots"][0]["status"] = "empty";
        emptied["ace"]["aces"][1]["slots"][0]["material"] = "";
        emptied["ace"]["aces"][1]["slots"][0]["color"] = json::array({0, 0, 0});
        b.frame(emptied);
        CHECK(b.get_slot_info(bay).material == "PETG");
    }

    // A different spool in the bay clears the edit and yields to the engine.
    json respooled = ace_frame("head_mode_4ace.json");
    respooled["ace"]["aces"][1]["slots"][0]["material"] = "PLA";
    respooled["ace"]["aces"][1]["slots"][0]["brand"] = "Anycubic";
    respooled["ace"]["aces"][1]["slots"][0]["color"] = json::array({0, 128, 0});
    b.frame(respooled);
    const SlotInfo after = b.get_slot_info(bay);
    CHECK(after.material == "PLA");
    CHECK(after.color_rgb == 0x008000u);
    CHECK(after.brand == "Anycubic");
    CHECK(after.spool_name.empty());
}

TEST_CASE_METHOD(MultiAceFixture, "bay edits refuse slots the backend does not show",
                 "[multiace][ams]") {
    SlotInfo edit;
    edit.material = "PETG";
    edit.color_rgb = 0x1E5AA8;

    SECTION("no engine contract: the bays do not exist") {
        helix::test::RegisteredBackend<CapturingMultiAce> reg;
        CapturingMultiAce& b = *reg;
        CHECK_FALSE(helix::test::apply_edit(b, multiace::bay_global(0, 0), edit).success());
    }
    SECTION("an index past the last bay") {
        helix::test::RegisteredBackend<CapturingMultiAce> reg;
        CapturingMultiAce& b = *reg;
        b.frame(ace_frame("head_mode_4ace.json"));
        REQUIRE(b.get_system_info().total_slots == 20);
        CHECK_FALSE(helix::test::apply_edit(b, 20, edit).success());
        CHECK_FALSE(helix::test::apply_edit(b, -1, edit).success());
    }
}

TEST_CASE_METHOD(MultiAceFixture, "clearing a bay spool drops the edit and yields to the engine",
                 "[multiace][ams]") {
    MultiAceOverrideCacheDir cache("clear");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<StoreBackedMultiAce> reg(&api);
    StoreBackedMultiAce& b = *reg;
    b.on_started();
    b.frame(ace_frame("head_mode_4ace.json"));

    const int bay = multiace::bay_global(1, 0);
    SlotInfo edit = b.get_slot_info(bay);
    // Unlinked: an edit that links a spool declares the binding alone and the
    // server's record supplies the identity, which is not this test's subject.
    edit.material = "PETG";
    edit.brand = "Polymaker";
    edit.color_rgb = 0x1E5AA8;
    edit.spool_name = "Blue PETG";
    REQUIRE(b.commit_user_edit(bay, b.get_slot_info(bay), edit).success());
    REQUIRE(b.get_slot_info(bay).material == "PETG");

    // The clear the context menu runs: the commit clears what an edit states,
    // then the override record and the lane's own records go with it. A bay
    // whose engine states identity reads the engine's word again - the same
    // clear semantics a head with a tag has, where the tag's own statement
    // survives the clear of everything the user laid over it.
    SlotInfo original = b.get_slot_info(bay);
    SlotInfo cleared = original;
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.brand.clear();
    cleared.spool_name.clear();
    cleared.clear_spoolman_link();
    REQUIRE(b.commit_user_edit(bay, original, cleared).success());
    b.clear_slot_override(bay);

    const SlotInfo after = b.get_slot_info(bay);
    CHECK(after.material == "PLA");
    CHECK(after.brand == "Anycubic");
    // The fields only the user's record supplied stay gone.
    CHECK(after.spool_name.empty());
    CHECK(after.spoolman_id == 0);

    // The frame path agrees: the engine's table is the bay's machine reading.
    // As on a head, the colour the user picked sits on the slot until this
    // frame's repaint resolves the lane the clear left.
    b.frame(ace_frame("head_mode_4ace.json"));
    const SlotInfo reframed = b.get_slot_info(bay);
    CHECK(reframed.material == "PLA");
    CHECK(reframed.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(MultiAceFixture, "a bay edit survives a restart and outranks the engine's table",
                 "[multiace][ams][filament_slot_override]") {
    MultiAceOverrideCacheDir cache("restart");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // Session 1: the user names the spool in ACE 2 bay 1.
    {
        helix::test::RegisteredBackend<StoreBackedMultiAce> reg(&api);
        StoreBackedMultiAce& b = *reg;
        b.on_started();
        b.frame(ace_frame("head_mode_4ace.json"));
        SlotInfo edit = b.get_slot_info(multiace::bay_global(1, 0));
        edit.material = "PETG";
        edit.brand = "Polymaker";
        edit.color_rgb = 0x1E5AA8;
        REQUIRE(b.commit_user_edit(multiace::bay_global(1, 0),
                                   b.get_slot_info(multiace::bay_global(1, 0)), edit)
                    .success());
    }

    // Session 2: the store loads, the engine restates its own table, and the
    // stored edit still wins.
    {
        helix::test::RegisteredBackend<StoreBackedMultiAce> reg(&api);
        StoreBackedMultiAce& b = *reg;
        b.on_started();
        b.frame(ace_frame("head_mode_4ace.json"));

        const SlotInfo slot = b.get_slot_info(multiace::bay_global(1, 0));
        CHECK(slot.material == "PETG");
        CHECK(slot.brand == "Polymaker");
        CHECK(slot.color_rgb == 0x1E5AA8u);
        // A bay nobody edited keeps the engine's word: ACE 1 bay 2 is the
        // engine's Generic blue PETG, not the edit's Polymaker shade.
        const SlotInfo untouched = b.get_slot_info(multiace::bay_global(0, 1));
        CHECK(untouched.brand == "Generic");
        CHECK(untouched.color_rgb == 0x0000FFu);
    }
}
