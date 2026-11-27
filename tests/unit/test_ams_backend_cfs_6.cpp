// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 174 to 206 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly [1464],
// [1199], [1710], [general], [filament_slot_override], and more

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/cfs_test_fixtures.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/force_bypass_controls_guard.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/toast_log.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system/telemetry_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;
using helix::printer::AmsBackendCfs;
using helix::printer::CfsMaterialDb;
using helix::test::make_cfs_status_json;
using helix::test::make_unit_box_explicit;

using helix::test::CfsAutoRefillHelper;
using helix::test::CfsCalibHelper;
using helix::test::CfsInsertRuleRig;
using helix::test::CfsK1RemapHelper;
using helix::test::CfsK2CalibHelper;
using helix::test::CfsLifecycleHelper;
using helix::test::CfsLiveAckBackend;
using helix::test::CfsOverrideRig;
using helix::test::CfsRemapHelper;
using helix::test::CfsTmpCacheDir;
using helix::test::check_not_single_spool;
using helix::test::configfile_queries;
using helix::test::drain_calib_queue;
using helix::test::feed_toolhead_filament;
using helix::test::find_action;
using helix::test::FlatFailDispatchCfs;
using helix::test::HeldAckCfs;
using helix::test::json;
using helix::test::k2_plus_startup_box;
using helix::test::k2_plus_t1_connected;
using helix::test::kEditedRemainingG;
using helix::test::link_lane_four_to_spool_137;
using helix::test::MacroCacheScope;
using helix::test::make_boot_box_all_units_down;
using helix::test::make_cfs_notification;
using helix::test::make_empty_bus_box;
using helix::test::make_flat_box;
using helix::test::make_flat_fork_box;
using helix::test::make_fork_box_with_boxes;
using helix::test::make_gapped_fork_box;
using helix::test::make_multi_unit_box;
using helix::test::make_runout_box;
using helix::test::make_runout_removed_box;
using helix::test::make_runout_seated_box;
using helix::test::make_single_unit_box;
using helix::test::make_unbranded_bay_box;
using helix::test::make_unit_up_delta;
using helix::test::no_unit_settings;
using helix::test::paused_ctx;
using helix::test::per_slot_box;
using helix::test::per_slot_unit;
using helix::test::registered_unit_settings;
using helix::test::script_has_command;
using helix::test::SyncCfs;
using helix::test::toolhead_sensor_frame;
using helix::test::UnansweredClientCfs;
using helix::test::UnhomedCfsBackend;
using json = nlohmann::json;
using helix::test::GcodeRecordingApi;

// A placeholder's bays belong to a box that is not there: nothing reaches them
// and nothing counts them (#1464).
TEST_CASE("CFS: an absent box's bays refuse every operation", "[ams][cfs][1464]") {
    SECTION("fork {1,3}: load, edit and identity write are refused") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 3})));

        CHECK(backend.load_filament(5).result == AmsResult::INVALID_SLOT);
        CHECK(backend.change_tool(5).result == AmsResult::INVALID_SLOT);
        CHECK(backend.dispatched.empty());

        SlotInfo edit;
        edit.material = "PETG";
        edit.color_rgb = 0x0A2989;
        CHECK(helix::test::apply_edit(backend, 5, edit).result == AmsResult::INVALID_SLOT);
        CHECK_FALSE(CfsTestAccess::get_override(backend, 5).has_value());

        backend.push(5, "PETG", "eSUN", "", 0x0A2989);
        CHECK(backend.captured.empty());

        const auto info = backend.get_system_info();
        CHECK(info.present_slot_count() == 8);
        CHECK(backend.get_slot_info(5).status == SlotStatus::EMPTY);
        CHECK(backend.get_slot_info(5).mapped_tool == -1);
    }

    SECTION("stock T1+T3: tool assignment and RFID never touch T2's bays") {
        json box = make_multi_unit_box(3);
        box["T2"] = json{{"state", "None"}};
        helix::test::StartedBackend<CfsRemapHelper> backend;
        helix::ams::FilamentSlotOverride kept;
        kept.spool_name = "T2A spool";
        CfsTestAccess::seed_override(backend, 4, kept);
        backend.fire_status(make_cfs_notification(box));
        backend.fire_status(make_cfs_notification(box));

        CHECK(backend.set_tool_mapping(0, 5).result == AmsResult::INVALID_SLOT);
        CHECK(backend.captured.empty());
        CHECK_FALSE(CfsTestAccess::last_rfid_uid(backend, 5).has_value());
        REQUIRE(CfsTestAccess::get_override(backend, 4).has_value());
        CHECK(CfsTestAccess::get_override(backend, 4)->spool_name == "T2A spool");
    }
}

// A stock box frame is a delta: the units it omits are unchanged, not gone.
// Only a frame reporting a unit disconnected takes it off the bus (#1464).
TEST_CASE("CFS stock: a delta frame naming one unit leaves the others as they were",
          "[ams][cfs][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json full = make_multi_unit_box(3);
    full["T1"]["vender"] = json::array({"Creality", "none", "none", "none"});
    full["T1"]["remain_len"] = json::array({"300", "-1", "-1", "-1"});
    full["T1"]["color_value"] = json::array({"0FF5500", "-1", "-1", "-1"});
    backend.fire_status(make_cfs_notification(full));
    REQUIRE(backend.get_system_info().units.size() == 3);

    json delta = json::object();
    delta["T3"] = full["T3"];
    delta["T3"]["filament"] = "B";
    backend.fire_status(make_cfs_notification(delta));

    const auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 3);
    for (const auto& unit : info.units) {
        INFO("unit " << unit.unit_index);
        CHECK_FALSE(unit.absent);
    }
    CHECK(info.present_slot_count() == 12);
    CHECK(backend.get_slot_info(0).color_rgb == 0xFF5500u);
    CHECK(info.current_slot == 9);

    SECTION("a frame reporting a unit disconnected does take it off") {
        json gone = json::object();
        gone["T2"] = json{{"state", "None"}};
        backend.fire_status(make_cfs_notification(gone));
        CHECK(backend.get_system_info().slot_absent(5));
        CHECK(backend.get_slot_info(0).color_rgb == 0xFF5500u);
    }
}

// The merged stock state is a picture of one session: a restart starts over,
// so a unit its first frame omits does not come back from before (#1464).
TEST_CASE("CFS stock: a restart does not resurrect a unit from the previous session",
          "[ams][cfs][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_multi_unit_box(3)));
    REQUIRE(backend.get_system_info().units.size() == 3);

    CfsTestAccess::call_on_started(backend);
    backend.fire_status(make_cfs_notification(make_multi_unit_box(1)));
    CHECK(backend.get_system_info().units.size() == 1);
    CHECK(backend.get_system_info().total_slots == 4);
}

// A stock tool whose bay sits in a box off the bus is refused, and so is
// clearing that bay's spool (#1464).
TEST_CASE("CFS stock: tool change and Clear Spool refuse an absent bay", "[ams][cfs][1464]") {
    json box = make_multi_unit_box(3);
    box["T2"] = json{{"state", "None"}};
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(box));

    CHECK(backend.change_tool(5).result == AmsResult::INVALID_SLOT);
    CHECK(backend.dispatched.empty());

    helix::ams::FilamentSlotOverride kept;
    kept.spool_name = "T2B spool";
    CfsTestAccess::seed_override(backend, 5, kept);
    helix::AmsBackend& base = backend;
    base.clear_slot_override(5);
    REQUIRE(CfsTestAccess::get_override(backend, 5).has_value());
    CHECK(CfsTestAccess::get_override(backend, 5)->spool_name == "T2B spool");
}

// The all-units slot row keeps an absent box's bays in place but disabled, the
// card's treatment: dimmed, and a tap reaches nothing (#1464).
TEST_CASE_METHOD(LVGLUITestFixture, "CFS: the all-units slot row disables an absent unit's bays",
                 "[ams][cfs][1464]") {
    ui_spool_canvas_register();
    ui_ams_slot_register();
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(true);
    auto owned = std::make_unique<helix::test::WiredBackend<CfsRemapHelper>>();
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    REQUIRE(backend->start().success());
    json box = make_multi_unit_box(3);
    box["T2"] = json{{"state", "None"}};
    backend->fire_status(make_cfs_notification(box));

    lv_obj_t* area = lv_obj_create(test_screen());
    lv_obj_set_size(area, 800, 200);
    AmsDetailWidgets w;
    w.slot_grid = lv_obj_create(area);
    helix::ui::WidgetRef slots[16];
    const auto result =
        ams_detail_create_slots(w, slots, 16, /*unit_index=*/-1, [](lv_event_t*) {}, nullptr);
    REQUIRE(result.slot_count == 12);
    auto check_disabled = [&](bool box2_absent) {
        for (int bay = 0; bay < 12; ++bay) {
            INFO("bay " << bay);
            REQUIRE(slots[bay] != nullptr);
            CHECK(lv_obj_has_state(slots[bay], LV_STATE_DISABLED) ==
                  (box2_absent && bay >= 4 && bay < 8));
        }
    };
    check_disabled(true);

    // Same span either way, so no rebuild: a refresh follows the box.
    json back = make_multi_unit_box(3);
    backend->fire_status(make_cfs_notification(back));
    helix::ui::ams_detail_sync_slot_states(slots, result.slot_count);
    check_disabled(false);

    backend->fire_status(make_cfs_notification(box));
    helix::ui::ams_detail_sync_slot_states(slots, result.slot_count);
    check_disabled(true);

    int count = result.slot_count;
    ams_detail_destroy_slots(w, slots, count);
    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    ams.deinit_subjects();
}

// Tapping a bay of a box that is not on the bus opens no menu (#1464).
TEST_CASE_METHOD(LVGLUITestFixture, "CFS: an absent bay's tap opens no context menu",
                 "[ams][cfs][context_menu][1464]") {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    helix::test::RegisteredWired<CfsRemapHelper> backend;
    json box = make_multi_unit_box(3);
    box["T2"] = json{{"state", "None"}};
    backend->fire_status(make_cfs_notification(box));

    helix::ui::SlotMenuHost host;
    host.parent_screen = test_screen();
    helix::ui::AmsContextMenu menu;
    helix::ui::open_slot_context_menu(menu, host, 5, test_screen());
    CHECK_FALSE(menu.is_visible());
    helix::ui::open_slot_context_menu(menu, host, 8, test_screen());
    CHECK(menu.is_visible());
    menu.hide();
}

// The last box dropping leaves no placeholder: its bays fall outside the span,
// and what was kept for them waits there unchanged until it returns.
TEST_CASE("CFS fork: a trailing box dropping leaves its state untouched",
          "[ams][cfs][fork][lane][1464]") {
    helix::test::RegisteredWired<CfsRemapHelper> backend;
    backend->fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));
    helix::ams::FilamentSlotOverride box3a;
    box3a.spool_name = "Box 3 A spool";
    box3a.spoolman_id = 108;
    CfsTestAccess::seed_override(*backend, 8, box3a);
    const auto baseline = CfsTestAccess::last_rfid_uid(*backend, 8);

    backend->fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2})));
    CHECK(backend->get_system_info().total_slots == 8);
    CHECK(backend->get_system_info().present_slot_count() == 8);
    CHECK(backend->load_filament(8).result == AmsResult::INVALID_SLOT);
    REQUIRE(CfsTestAccess::get_override(*backend, 8).has_value());
    CHECK(CfsTestAccess::get_override(*backend, 8)->spool_name == "Box 3 A spool");
    CHECK(CfsTestAccess::last_rfid_uid(*backend, 8) == baseline);
    const auto lane = helix::ams::lane_sources(backend.lane(8));
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->product_name == std::optional<std::string>("Box 3 A"));

    backend->fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));
    REQUIRE(CfsTestAccess::get_override(*backend, 8).has_value());
    CHECK(CfsTestAccess::get_override(*backend, 8)->spool_name == "Box 3 A spool");
}

// Stock numbers bays by box address too: with T2 off the bus, T3's bays are
// global 8-11 and must stay reachable (#1464).
TEST_CASE("CFS stock: a unit behind a disconnected one is still addressable", "[ams][cfs][1464]") {
    json box = make_multi_unit_box(3);
    box["T2"] = json{{"state", "None"}};
    box["T3"]["filament"] = "B";

    const auto info = AmsBackendCfs::parse_box_status(box);
    REQUIRE(info.get_slot_global(8) != nullptr);
    CHECK(info.current_slot == 9);
    CHECK(info.total_slots >= 12);

    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(box));
    CHECK(backend.load_filament(5).result == AmsResult::INVALID_SLOT);
    CHECK(backend.dispatched.empty());
    REQUIRE(backend.load_filament(11).result == AmsResult::SUCCESS);
    REQUIRE(backend.dispatched.size() == 1);
    CHECK(backend.dispatched[0].find("TNN=T3D") != std::string::npos);
}

// The shared state layer publishes per-slot subjects over 0..total_slots, so
// T3 behind a missing T2 is only visible when total_slots spans it (#1464).
TEST_CASE_METHOD(LVGLTestFixture, "CFS stock: AmsState publishes a unit behind a missing one",
                 "[ams][cfs][1464]") {
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    get_printer_state().init_subjects(false);
    ams.init_subjects(true);

    auto owned = std::make_unique<helix::test::WiredBackend<CfsRemapHelper>>();
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    REQUIRE(backend->start().success());

    json box = make_multi_unit_box(3);
    box["T2"] = json{{"state", "None"}};
    box["T3"]["vender"] = json::array({"Creality", "Creality", "none", "none"});
    box["T3"]["remain_len"] = json::array({"300", "300", "-1", "-1"});
    box["T3"]["color_value"] = json::array({"0FFFFFF", "0FF5500", "-1", "-1"});
    backend->fire_status(make_cfs_notification(box));
    ams.sync_from_backend();

    CHECK(lv_subject_get_int(ams.get_slot_count_subject()) == 12);
    CHECK(lv_subject_get_int(ams.get_slot_color_subject(9)) == 0xFF5500);
    CHECK(backend->get_system_info().present_slot_count() == 8);

    // The unit card binds this to show the missing box disabled and labelled.
    const auto absent = [](int unit) {
        lv_subject_t* subject =
            lv_xml_get_subject(nullptr, helix::AmsState::unit_absent_subject_name(unit).c_str());
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    };
    CHECK(absent(0) == 0);
    CHECK(absent(1) == 1);
    CHECK(absent(2) == 0);

    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    ams.deinit_subjects();
}

TEST_CASE("CFS: a failed pre-op G28 does not send the envelope unwind", "[ams][cfs][homing]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::CREALITY_K1};
    helix::PrinterState state;
    GcodeRecordingApi api{client, state};
    api.fail = {"G28"}; // Klipper's homing failure

    UnhomedCfsBackend backend{&api, &client};

    CfsTestAccess::dispatch_action_script(backend, "CR_BOX_LOAD TNN=0");
    // token.defer() can queue from inside a drain, so drain until quiet or the
    // callback is left behind and the isolation-leak gate names this test.
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // The G28 went out and failed, so the payload never shipped.
    REQUIRE(api.contains("G28"));
    REQUIRE_FALSE(api.contains("CR_BOX_LOAD"));

    // Nothing was saved, so nothing may be restored. Sending the restore anyway
    // draws a Klipper rejection that surfaces as a second error toast beside the
    // homing failure the user actually needs to read.
    CHECK_FALSE(api.contains("RESTORE_GCODE_STATE"));
    CHECK_FALSE(api.contains("BOX_RESTORE_FAN"));
}

TEST_CASE("CFS: a failed payload still sends the envelope unwind", "[ams][cfs][homing]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::CREALITY_K1};
    helix::PrinterState state;
    GcodeRecordingApi api{client, state};
    // G28 succeeds; the body is what fails

    UnhomedCfsBackend backend{&api, &client};

    // The mock acks the payload, so drive the failure through the same callback
    // Klipper's rejection would reach.
    CfsTestAccess::dispatch_action_script(backend, "CR_BOX_LOAD TNN=0");
    // token.defer() can queue from inside a drain, so drain until quiet or the
    // callback is left behind and the isolation-leak gate names this test.
    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(api.contains("G28"));
    REQUIRE(api.contains("CR_BOX_LOAD"));
}

// Own-write echo suppression (prestonbrown/helixscreen#1633) and the insert
// rule (prestonbrown/helixscreen#1710)

TEST_CASE("CFS untagged insert offers Clear (#1710)", "[ams][cfs][1710]") {
    // A stock bay whose tag was read keeps the arrays latched to that spool,
    // so an insert's first frame restates the PULLED spool and says nothing
    // about the new one. The edge opens a wait for the RFID probe's answer;
    // three frames of silence are No evidence under the slot spec's insert
    // rule: keep everything and ask the user whether it is the same spool.
    CfsTmpCacheDir tmp("cfs_insert_notice");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::reset_insert_offers_for_test();
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Boot with a tagged PETG spool seated: the first occupancy observation is
    // the session's baseline, never an insert edge.
    backend.fire_status(make_cfs_notification(
        make_single_unit_box({"100003", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"})));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // The lane carries a user assignment for the spool; that is what the
    // notice asks about. Made through the edit path so it declares its colour,
    // which is what lets it survive the bay reading EMPTY below.
    SlotInfo edit = backend.get_slot_info(0);
    edit.color_rgb = 0x7EC8E3;
    helix::test::edit_slot_as_user(backend, 0, edit);

    // Pulling the spool is not an insert. The tag's material and colour stay
    // latched in the arrays; only the occupancy fields drop.
    json pulled = make_single_unit_box({"100003", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"});
    pulled["T1"]["vender"][0] = "none";
    pulled["T1"]["remain_len"][0] = "-1";
    backend.fire_status(make_cfs_notification(pulled));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // A spool goes in that the reader cannot name: the frame restates the
    // latched material and colour and reports the bay occupied again. The box
    // is idle so the probe runs and genuinely says nothing.
    json inserted =
        make_single_unit_box({"100003", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"});
    inserted["filament_useup"] = 0;

    SECTION("the edge frame is never judged, silence then asks") {
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());

        // Two more frames still restating the latched spool stay quiet...
        backend.fire_status(make_cfs_notification(inserted));
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());

        // ...the third is the probe's wait spent: the reader never said
        // anything about the inserted spool.
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::INFO);

        // The wait is spent; restating occupancy is not a new edge.
        toasts.clear();
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());
    }

    SECTION("the probe's answer decides") {
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
        CHECK(toasts.empty());

        // The probe reads a tag: a different spool's PLA and white. The frame
        // states something other than the latched spool, so it is judged.
        json answer =
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"});
        backend.fire_status(make_cfs_notification(answer));
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
        CHECK(toasts.empty());
    }

    SECTION("the bay emptying ends the wait silently") {
        backend.fire_status(make_cfs_notification(inserted));
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());

        json gone = json(inserted);
        gone["T1"]["vender"][0] = "none";
        gone["T1"]["remain_len"][0] = "-1";
        backend.fire_status(make_cfs_notification(gone));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());

        // A re-insert opens a fresh wait rather than resuming a spent one.
        backend.fire_status(make_cfs_notification(inserted));
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());
    }

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE("CFS a re-read identical tag stays silent (#1710)", "[ams][cfs][1710]") {
    // The stock arrays latch the pulled spool's colour and material, and a
    // re-inserted tagged spool re-reads IDENTICAL values, so the probe's
    // answer never states anything different from the latch. What separates
    // a completed read from the latch is `vender`: "unknown" while the seated
    // tag is unread, a real vendor name once the probe has read it. A
    // completed read restating the before-insert evidence is the same spool:
    // keep everything and say nothing.
    CfsInsertRuleRig rig;
    auto& backend = rig.backend;

    // The same spool goes back in: the edge frame restates the latched values
    // with the tag still unread.
    backend.fire_status(make_cfs_notification(rig.reinserted));
    rig.drain();
    CHECK(rig.toasts.empty());

    // The probe completes on later frames: vender resolves to the tag's
    // vendor while material and colour repeat the latched pair exactly.
    json reread = json(rig.reinserted);
    reread["T1"]["vender"][0] = "Creality";
    for (int i = 0; i < 4; ++i) {
        backend.fire_status(make_cfs_notification(reread));
        rig.drain();
        CHECK(rig.toasts.empty());
    }
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("CFS a deferred probe holds the insert verdict until it runs (#1710)",
          "[ams][cfs][1710]") {
    // Mid-operation the box answers an RFID refresh with busy, so the insert
    // probe parks in deferred_probes_ until an idle poll. The pending
    // insert's quiet countdown must not expire while the probe still owes
    // its answer: the verdict waits, and the released probe's answer decides.
    CfsInsertRuleRig rig;
    auto& backend = rig.backend;

    // The insert lands while the box reports its runout latch: the probe is
    // deferred on the very frame that opens the pending insert.
    json busy = json(rig.reinserted);
    busy["filament_useup"] = 1;
    backend.fire_status(make_cfs_notification(busy));
    rig.drain();
    CHECK(rig.toasts.empty());

    // Busy frame after busy frame restates the latched spool. The countdown
    // holds rather than expiring into a notice the probe has not weighed in
    // on yet.
    for (int i = 0; i < 6; ++i) {
        backend.fire_status(make_cfs_notification(busy));
        rig.drain();
        CHECK(rig.toasts.empty());
    }
    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());

    // The box goes idle: the deferred probe dispatches on this poll.
    json idle = json(busy);
    idle["filament_useup"] = 0;
    backend.fire_status(make_cfs_notification(idle));
    rig.drain();
    CHECK(rig.toasts.empty());

    // The probe's answer lands on a later frame: a different spool's PLA and
    // white, so the held verdict clears what described the pulled one.
    json answer = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"0FFFFFF", "-1", "-1", "-1"});
    answer["filament_useup"] = 0;
    backend.fire_status(make_cfs_notification(answer));
    rig.drain();
    CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(rig.toasts.empty());
}

TEST_CASE("CFS flat insert edge verdicts (#1710)", "[ams][cfs][1710]") {
    // The fork reports each seated spool's identity fresh in every frame, so
    // the edge itself is the reading, minus the fields that restate our own
    // pushed label. Registered, not bare: lane_id() names a position only once
    // AmsState has stamped the backend index, and an unregistered backend's
    // edit files its declaration on no lane at all.
    helix::test::RegisteredWired<CfsRemapHelper> backend_reg;
    auto& backend = *backend_reg;

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::reset_insert_offers_for_test();
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    backend.fire_status(make_cfs_notification(make_flat_fork_box()));

    // A user assignment for the seated spool, made the way the application
    // makes one: a colour the frame did not report, declared through the edit
    // path so it survives the bay reading EMPTY below.
    SlotInfo edit = backend.get_slot_info(0);
    edit.color_rgb = 0x7EC8E3;
    helix::test::edit_slot_as_user(backend, 0, edit);

    json pulled = make_flat_fork_box();
    pulled["slots"][0]["present"] = false;
    pulled["slots"][0]["loaded"] = false;
    pulled["slots"][0]["material"] = "None";
    pulled["slots"][0]["brand"] = "None";
    pulled["slots"][0]["name"] = "None";
    pulled["slots"][0]["color"] = "None";
    pulled["loaded_slot"] = -1;
    pulled["loaded_mask"] = 0;
    pulled["runout"] = nullptr;
    backend.fire_status(make_cfs_notification(pulled));
    // A deliberate user assignment survives an empty bay, so the assignment
    // the verdicts below act on is still standing at the edge.
    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    SECTION("a different spool drops what described the old one") {
        json inserted = make_flat_fork_box();
        inserted["slots"][0]["material"] = "PETG";
        inserted["slots"][0]["color"] = "#0A2989";
        inserted["slots"][0]["brand"] = "Jayo";
        inserted["slots"][0]["name"] = "Blue PETG";
        backend.fire_status(make_cfs_notification(inserted));
        CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
        // The changed fingerprint clears through the hardware-event path as
        // well; the assertion pins the end state the spec names.
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());
    }

    SECTION("a colour-only swap still drops the assignment") {
        // Same material, different colour: the least swap there is. The edit
        // declared only its colour, so the label discounts nothing the frame
        // states here and the colour difference alone is a different spool.
        // The changed fingerprint clears through the hardware-event path as
        // well; the assertions pin the end state, silently, the spec names.
        json inserted = make_flat_fork_box();
        inserted["slots"][0]["color"] = "#0A2989";
        backend.fire_status(make_cfs_notification(inserted));
        CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());
    }

    SECTION("the same spool keeps everything silently") {
        // Slot 2 carries no user label, so its re-inserted identity is read
        // whole: both fields read and equal is the same spool, which drops
        // nothing and asks nothing. Slot 0 stays out throughout so its own
        // pending label cannot add a second verdict here.
        json pulled2 = json(pulled);
        pulled2["slots"][2]["present"] = false;
        pulled2["slots"][2]["material"] = "None";
        pulled2["slots"][2]["brand"] = "None";
        pulled2["slots"][2]["name"] = "None";
        pulled2["slots"][2]["color"] = "None";
        backend.fire_status(make_cfs_notification(pulled2));
        REQUIRE(backend.get_slot_info(2).material.empty());

        // The same spool goes back in: the fork states its identity fresh.
        backend.fire_status(make_cfs_notification(pulled));
        REQUIRE(backend.get_slot_info(2).material == "PETG");
        CHECK(backend.get_slot_info(2).color_rgb == 0x0A2989u);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(toasts.empty());
    }

    SECTION("no evidence asks instead of clearing") {
        json inserted = make_flat_fork_box();
        inserted["slots"][0]["material"] = "None";
        inserted["slots"][0]["brand"] = "None";
        inserted["slots"][0]["name"] = "None";
        inserted["slots"][0]["color"] = "None";
        backend.fire_status(make_cfs_notification(inserted));
        CHECK(CfsTestAccess::get_override(backend, 0).has_value());
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::INFO);
    }

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE("CFS flat fingerprint upgrade folds stored composites (#1710)", "[ams][cfs][1710]") {
    // The fold itself: only the legacy four-field shape moves, and anything
    // else - the current shape, the stock composite, a value no build of ours
    // wrote - passes through untouched.
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("PLA|eSUN|Black PLA|111111") ==
          "PLA|111111");
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("PLA|") == "PLA|");
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("PLA|111111") == "PLA|111111");
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("101001|0FF5500") == "101001|0FF5500");
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("").empty());
    CHECK(AmsBackendCfs::normalize_legacy_flat_fingerprint("a|b|c|d|e") == "a|b|c|d|e");

    // The upgrade itself: a fork override persisted by a build whose
    // fingerprint counted brand and the product line, with the box reporting
    // the same spool under a different brand and product line, must not read
    // as a swap at the first poll.
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;

    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.color_rgb = 0x111111;
    ovr.color_set = true;
    ovr.fingerprint = "PLA|eSUN|Black PLA|111111";
    mock_printer.client.mock_db_set("lane_data", "lane1", helix::ams::to_lane_data_record(0, ovr));

    CfsTestAccess::call_on_started(backend);
    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
    const auto baseline = CfsTestAccess::last_rfid_uid(backend, 0);
    REQUIRE(baseline.has_value());
    CHECK(*baseline == "PLA|111111");

    // Brand and the product line are presentation, not identity: the same
    // material and colour keep the override.
    json same_spool = make_flat_fork_box();
    same_spool["slots"][0]["brand"] = "Polymaker";
    same_spool["slots"][0]["name"] = "PolyLite PLA";
    backend.fire_status(make_cfs_notification(same_spool));
    // The write it made re-reads the records through the update queue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());

    // A colour change is a different spool under the two-field composite.
    json swapped = json(same_spool);
    swapped["slots"][0]["color"] = "#0A2989";
    backend.fire_status(make_cfs_notification(swapped));
    // The write it made re-reads the records through the update queue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
    // The last session's poll reads (records and the bypass key) answer
    // through the queue after its backend is gone; settle them here.
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

TEST_CASE("CFS clear_slot_override writes nothing to the box", "[ams][cfs][fork]") {
    // The lane half of Clear Spool: the box's own clear goes out with the
    // cleared edit before it ("Clear Spool writes the box's clear once").
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_single_unit_box(
        {"101001", "101001", "101001", "101001"}, {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"})));
    helix::AmsBackend& base = backend;
    base.clear_slot_override(2);
    CHECK(backend.captured.empty());

    backend.fire_status(make_cfs_notification(make_flat_fork_box()));
    base.clear_slot_override(2);
    CHECK(backend.captured.empty());
}

TEST_CASE("CFS tool-table remap is the routing an unremapped print runs with",
          "[ams][cfs][remap][routing]") {
    // The box routes T1B to bay T1D (and T1D to T1B). A print using tool 1 must
    // seed bay 3, so print start sends no BOX_MODIFY_TN undoing it.
    helix::test::RegisteredWired<CfsRemapHelper> reg;
    auto& helper = *reg;
    auto status = make_cfs_status_json();
    status["box"]["map"] = {{"T1A", "T1A"}, {"T1B", "T1D"}, {"T1C", "T1C"}, {"T1D", "T1B"}};
    json notification = {{"method", "notify_status_update"}, {"params", json::array({status, 0})}};
    helper.fire_status(notification);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(helper.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    const auto live = helper.get_tool_mapping();
    REQUIRE(live.size() == 4);
    REQUIRE(live[1] == 3);
    CHECK(helper.firmware_default_routing().head(1) == 3);
    CHECK(helper.firmware_default_routing().head(3) == 1);

    GcodeToolInfo t1{};
    t1.tool_index = 1;
    t1.color_rgb = 0x808080;
    t1.color_known = false;
    auto& ams = AmsState::instance();
    const auto seed = ams.seed_tool_mappings({t1}, ams.collect_available_slots(), false);
    REQUIRE(seed.size() == 1);
    CHECK(seed[0].mapped_slot == live[1]);
}

TEST_CASE("CFS routing ignores our own unconfirmed tool-table write",
          "[ams][cfs][remap][routing]") {
    // BOX_MODIFY_TN updates the local table the moment it is sent. Until a box
    // frame carries a map, that table is our intent, not what the printer routes
    // by, so an unremapped print must not be seeded from it.
    helix::test::StartedBackend<CfsRemapHelper> helper;
    auto status = make_cfs_status_json();
    status["box"].erase("map");
    json notification = {{"method", "notify_status_update"}, {"params", json::array({status, 0})}};
    helper.fire_status(notification);
    REQUIRE(helper.firmware_tool_mapping_generation() == 0);

    REQUIRE(helper.set_tool_mapping(1, 3).result == AmsResult::SUCCESS);
    REQUIRE(helper.get_tool_mapping().size() > 1);
    REQUIRE(helper.get_tool_mapping()[1] == 3);

    CHECK(helper.firmware_default_routing().head(1) == 1);

    SECTION("a box frame that reports the map makes it the routing") {
        status["box"]["map"] = {{"T1A", "T1A"}, {"T1B", "T1D"}, {"T1C", "T1C"}, {"T1D", "T1B"}};
        json frame = {{"method", "notify_status_update"}, {"params", json::array({status, 0})}};
        helper.fire_status(frame);
        REQUIRE(helper.firmware_tool_mapping_generation() > 0);
        CHECK(helper.firmware_default_routing().head(1) == 3);
    }
}

TEST_CASE("the K1-family CFS upgrade costs 5 mm of printable Y", "[ams][cfs]") {
    CHECK(cfs_k1_upgrade_y_shrink_mm(true) == 5.0);
    CHECK(cfs_k1_upgrade_y_shrink_mm(false) == 0.0);
}

TEST_CASE("CFS backend reports the K1-family upgrade's printable-Y shrink (#968)", "[ams][cfs]") {
    auto* config = Config::get_instance();
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C printer type -> 5 mm") {
        config->set<std::string>(type_path, "Creality K1C");
        AmsBackendCfs backend(nullptr, nullptr);
        CHECK(backend.printable_y_shrink_mm() == 5.0);
    }

    SECTION("K1 Max printer type -> 5 mm") {
        config->set<std::string>(type_path, "Creality K1 Max");
        AmsBackendCfs backend(nullptr, nullptr);
        CHECK(backend.printable_y_shrink_mm() == 5.0);
    }

    SECTION("K2 Plus printer type -> none") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        AmsBackendCfs backend(nullptr, nullptr);
        CHECK(backend.printable_y_shrink_mm() == 0.0);
    }

    config->set<std::string>(type_path, saved);
}

TEST_CASE_METHOD(LVGLTestFixture, "the print-start fit check reads the CFS printable-Y shrink",
                 "[ams][cfs][print_start]") {
    auto* config = Config::get_instance();
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");
    config->set<std::string>(type_path, "Creality K1C");

    SECTION("a CFS backend takes 5 mm") {
        helix::test::RegisteredBackend<AmsBackendCfs> backend(nullptr, nullptr);
        CHECK(AmsState::instance().printable_y_shrink_mm() == 5.0);
    }
    SECTION("a system that takes no bed area answers none") {
        helix::test::RegisteredBackend<AmsBackendMock> backend(4);
        CHECK(AmsState::instance().printable_y_shrink_mm() == 0.0);
    }

    config->set<std::string>(type_path, saved);
}

// The parse stamps LOADED on the seated bay

TEST_CASE("CFS parse stamps LOADED on the lane the toolhead is fed from", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;

    // Letter alone is a lane selection, not a seated lane — the toolhead switch
    // is the only thing that says filament actually arrived.
    cfs.fire_status(make_cfs_notification(per_slot_box("B")));

    SECTION("letter without the toolhead switch stamps nothing") {
        REQUIRE(cfs.get_current_slot() == 1);
        REQUIRE_FALSE(cfs.is_filament_loaded());

        for (int i = 0; i < 4; ++i) {
            CHECK(cfs.get_slot_info(i).status != SlotStatus::LOADED);
            CHECK_FALSE(cfs.slot_is_actively_loaded(i));
            CHECK_FALSE(cfs.can_unload_from_toolhead(i));
        }
    }

    SECTION("letter plus the toolhead switch stamps exactly that bay") {
        cfs.fire_status(toolhead_sensor_frame(true));

        CHECK(cfs.get_slot_info(1).status == SlotStatus::LOADED);
        CHECK(cfs.slot_is_actively_loaded(1));
        CHECK(cfs.can_unload_from_toolhead(1));
        CHECK(cfs.get_slot_filament_segment(1) == PathSegment::NOZZLE);

        // The three idle bays keep their parsed status and stay unloaded.
        CHECK(cfs.get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK(cfs.get_slot_info(2).status == SlotStatus::EMPTY);
        CHECK(cfs.get_slot_info(3).status == SlotStatus::EMPTY);
        for (int i : {0, 2, 3}) {
            CHECK_FALSE(cfs.slot_is_actively_loaded(i));
            CHECK_FALSE(cfs.can_unload_from_toolhead(i));
        }
    }

    SECTION("a box frame arriving after the sensor re-derives the stamp") {
        cfs.fire_status(toolhead_sensor_frame(true));
        REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

        // Slots are rebuilt wholesale on every box frame, so the stamp has to
        // be re-applied afterwards or it is lost on the next notification.
        cfs.fire_status(make_cfs_notification(per_slot_box("B")));
        CHECK(cfs.get_slot_info(1).status == SlotStatus::LOADED);
        CHECK(cfs.can_unload_from_toolhead(1));
    }
}

TEST_CASE("CFS LOADED stamp follows the active lane across a toolchange", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    cfs.fire_status(make_cfs_notification(per_slot_box("A")));
    cfs.fire_status(toolhead_sensor_frame(true));
    REQUIRE(cfs.get_slot_info(0).status == SlotStatus::LOADED);

    // Bay 1 becomes the engaged lane. Exactly one bay may be LOADED — a stale
    // stamp on bay 0 would light two active-lane highlights at once.
    cfs.fire_status(make_cfs_notification(per_slot_box("B")));

    CHECK(cfs.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(cfs.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK(cfs.slot_is_actively_loaded(1));
    CHECK_FALSE(cfs.slot_is_actively_loaded(0));
}

TEST_CASE("CFS LOADED stamp honours the per-unit global-index offset", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;

    // T2's lane A is global slot 4, not 0 — the offset is (n-1)*4.
    json box{{"filament", 0}, {"T1", per_slot_unit("None")}, {"T2", per_slot_unit("A")}};
    cfs.fire_status(make_cfs_notification(box));
    cfs.fire_status(toolhead_sensor_frame(true));

    REQUIRE(cfs.get_current_slot() == 4);
    CHECK(cfs.get_slot_info(4).status == SlotStatus::LOADED);
    CHECK(cfs.slot_is_actively_loaded(4));
    CHECK_FALSE(cfs.slot_is_actively_loaded(0));
    CHECK(cfs.get_slot_info(0).status == SlotStatus::AVAILABLE);
}

// Un-stamping restores what firmware said, not a guess

TEST_CASE("CFS clears the LOADED stamp when the toolhead switch opens", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    cfs.fire_status(make_cfs_notification(per_slot_box("B")));
    cfs.fire_status(toolhead_sensor_frame(true));
    REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

    // Sensor-only frame: the slot vector is NOT rebuilt, so the stamp has to be
    // undone in place or the bay stays LOADED forever after an unload.
    cfs.fire_status(toolhead_sensor_frame(false));

    CHECK(cfs.get_slot_info(1).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(cfs.slot_is_actively_loaded(1));
    CHECK_FALSE(cfs.can_unload_from_toolhead(1));
}

TEST_CASE("CFS un-stamping restores EMPTY rather than inventing a spool", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;

    // Bay 2 reads EMPTY (no RFID vendor, no remaining length) while the unit
    // still names lane C and the toolhead switch is closed — the spool was
    // pulled out with filament still threaded to the nozzle. The stamp is
    // deliberately applied over EMPTY here: filament IS at the toolhead and
    // the user must still be able to unload it.
    cfs.fire_status(make_cfs_notification(per_slot_box("C")));
    cfs.fire_status(toolhead_sensor_frame(true));
    REQUIRE(cfs.get_current_slot() == 2);
    REQUIRE(cfs.get_slot_info(2).status == SlotStatus::LOADED);
    REQUIRE(cfs.can_unload_from_toolhead(2));

    // Once the toolhead clears, the bay must fall back to EMPTY — demoting to
    // AVAILABLE would fabricate a spool in a bay firmware called empty.
    cfs.fire_status(toolhead_sensor_frame(false));
    CHECK(cfs.get_slot_info(2).status == SlotStatus::EMPTY);
    CHECK_FALSE(cfs.get_slot_info(2).is_present());
}

TEST_CASE("CFS drops the LOADED stamp when no unit names an active lane", "[ams][cfs][1199]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    cfs.fire_status(make_cfs_notification(per_slot_box("B")));
    cfs.fire_status(toolhead_sensor_frame(true));
    REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

    // T1.filament goes "None" — parse_box_status leaves current_slot at -1 and
    // handle_status clears the aggregate. The switch is still closed
    // (filament in the tube), but no bay owns it, so no bay may claim LOADED.
    cfs.fire_status(make_cfs_notification(per_slot_box("None")));

    REQUIRE(cfs.get_current_slot() == -1);
    REQUIRE(cfs.is_filament_loaded());
    for (int i = 0; i < 4; ++i) {
        CHECK(cfs.get_slot_info(i).status != SlotStatus::LOADED);
        CHECK_FALSE(cfs.slot_is_actively_loaded(i));
    }
}

TEST_CASE("CFS RFID fingerprint change clears override (hardware swap detected)",
          "[ams][cfs][filament_slot_override]") {
    CfsTmpCacheDir tmp("task14_uid_swap_clears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed override AND the corresponding DB entry so we can verify
    // clear_async deletes it on swap.
    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: material=101001, color=0FF5500 establishes the baseline.
    // No clear.
    json box1 = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box1));

    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");
    REQUIRE(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Second parse: DIFFERENT fingerprint on slot 0 (material=102001, new
    // color) — physical swap detected.
    //
    // Sequence inside handle_status for this slot:
    //   1. check_hardware_event_clear fires clear_override_locked, which
    //      erases the user-set override (brand, spool_name, spoolman_id,
    //      material, color) AND deletes the lane_data record.
    //   2. mirror_firmware_to_lane_data is SKIPPED on this parse. A clear_async
    //      DELETE and a save_async POST against the same lane_data key are
    //      independently ordered, so firing both in one pass races — a DELETE
    //      landing second would drop the record we just published. The
    //      republish happens on the next parse instead.
    json box2 = make_single_unit_box({"102001", "101001", "101001", "101001"},
                                     {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"});
    backend.fire_status(make_cfs_notification(box2));

    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "102001|000FF00");

    // The swap parse leaves nothing behind: override erased, record deleted.
    CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Next parse (same new spool, unchanged fingerprint) republishes firmware
    // truth for the NEW spool.
    backend.fire_status(make_cfs_notification(box2));

    // Override now holds the auto-mirrored firmware values (no user metadata):
    // new color, no spool_name / spoolman_id / brand from the wiped override.
    auto post_swap = CfsTestAccess::get_override(backend, 0);
    REQUIRE(post_swap.has_value());
    CHECK(post_swap->color_rgb == 0x00FF00u);
    CHECK(post_swap->spool_name.empty());
    CHECK(post_swap->spoolman_id == 0);
    CHECK(post_swap->brand.empty());

    // Orca sees the new spool's color, not stale user data.
    auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#00FF00");
    CHECK_FALSE(stored.contains("spool_name"));
    CHECK(stored["spool_id"].is_null());
    CHECK(stored["vendor_name"].is_null());

    // Override-exclusive fields reset on the live slot. Firmware-populated
    // fields (brand from material DB, color_rgb) stay — CFS firmware owns
    // those and they should reflect the new spool.
    auto info = backend.get_slot_info(0);
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.remaining_weight_g == -1.0f);
    // color_rgb was re-parsed from firmware this pass — should reflect new
    // spool's color (0x00FF00), not the old override (0xFF5500).
    CHECK(info.color_rgb == 0x00FF00u);
}

TEST_CASE("CFS runout invalidates the exhausted lane's remembered Spoolman link",
          "[ams][cfs][filament_slot_override][1390]") {
    CfsOverrideRig rig("cfs_runout_strips_link");

    // Lane 4 feeding on a healthy spool: the latch is observed CLEAR (0).
    rig.poll(make_runout_seated_box(/*useup=*/0, /*active=*/"D"));

    link_lane_four_to_spool_137(*rig.backend);
    REQUIRE(rig.client.mock_db_get("lane_data", "lane4").value("spool_id", 0) == 137);

    // The spool runs out mid-print: latch rises 0 -> 1 while bay D is still
    // the seated, active lane. This edge is what captures the lane.
    rig.poll(make_runout_seated_box(/*useup=*/1, /*active=*/"D"));

    // The user pulls the exhausted spool. Bay D reads EMPTY.
    rig.poll(make_runout_removed_box(/*useup=*/1, /*active=*/"None"));

    auto ovr = CfsTestAccess::get_override(*rig.backend, 3);
    REQUIRE(ovr.has_value());
    SECTION("the Spoolman handle is dropped from the override") {
        CHECK(ovr->spoolman_id == 0);
        CHECK(ovr->spoolman_filament_id == 0);
        CHECK(ovr->spoolman_vendor_id == 0);
    }
    SECTION("identity survives: only the handle is gone") {
        CHECK(ovr->material == "ASA-CF");
        CHECK(ovr->brand == "Elegoo");
        CHECK(ovr->spool_name == "Black ASA");
        // Remembered, not claimed. Linking a spool carries the server's colour
        // and material in without a person choosing either, so the record keeps
        // them without outranking a machine that later states its own.
    }
    SECTION("the live slot shows the drop immediately") {
        auto info = rig.backend->get_slot_info(3);
        CHECK(info.spoolman_id == 0);
        CHECK(info.spoolman_filament_id == 0);
        CHECK(info.spoolman_vendor_id == 0);
        CHECK(info.material == "ASA-CF");
        CHECK(info.status == SlotStatus::EMPTY);
    }
    SECTION("the persisted lane_data record keeps identity, loses the id") {
        auto stored = rig.client.mock_db_get("lane_data", "lane4");
        REQUIRE(!stored.is_null());
        CHECK(stored["spool_id"].is_null());
        CHECK(stored.value("helix_spoolman_filament_id", 0) == 0);
        CHECK(stored.value("spoolman_vendor_id", 0) == 0);
        CHECK(stored["vendor_name"] == "Elegoo");
        CHECK(stored["filament_type"] == "ASA-CF");
        CHECK(stored["name"] == "Black ASA");
    }
}

TEST_CASE("CFS: a weights-only edit is cleared when its bay reads EMPTY",
          "[ams][cfs][filament_slot_override]") {
    CfsOverrideRig rig("cfs_weights_only_empty_bay");
    rig.poll(make_unbranded_bay_box("46"));
    REQUIRE(rig.backend->get_slot_info(0).status == SlotStatus::AVAILABLE);

    SlotInfo edit = rig.backend->get_slot_info(0);
    edit.remaining_weight_g = kEditedRemainingG;
    helix::test::edit_slot_as_user(*rig.backend, 0, edit);

    // The edit stored a record that declares the weight and nothing else, so
    // neither lock nor any identity field can speak for it.
    const auto staged = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(staged.has_value());
    REQUIRE(staged->remaining_weight_g == kEditedRemainingG);
    REQUIRE(staged->material.empty());
    REQUIRE(staged->brand.empty());
    REQUIRE(staged->spool_name.empty());
    REQUIRE(staged->spoolman_id == 0);
    REQUIRE(staged->spoolman_vendor_id == 0);
    REQUIRE(staged->catalog_id.empty());
    REQUIRE(staged->product_name.empty());

    const helix::ams::LaneSources before = helix::ams::lane_sources(rig.registration->lane(0));
    REQUIRE(before.pending.has_value());
    REQUIRE(before.pending->remaining_weight_g == kEditedRemainingG);
    REQUIRE_FALSE(before.pending->color_rgb.has_value());
    REQUIRE_FALSE(before.pending->material.has_value());
    REQUIRE_FALSE(before.pending->brand.has_value());

    rig.poll(make_unbranded_bay_box("-1"));
    REQUIRE(rig.backend->get_slot_info(0).status == SlotStatus::EMPTY);

    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    const helix::ams::LaneSources after = helix::ams::lane_sources(rig.registration->lane(0));
    CHECK_FALSE(after.pending.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "CFS reads the chute wiper and purge position from the configfile each Klipper "
                 "start",
                 "[ams][cfs][chute][2007][purge_chute]") {
    get_printer_state().init_subjects(false); // the purge reads its homed axes
    helix::test::StartedBackend<UnansweredClientCfs> cfs;
    auto& client = cfs.wire().fake_client();
    auto answer_configfile = [&client](const json& settings) {
        const auto it =
            std::find_if(client.rpc_calls.rbegin(), client.rpc_calls.rend(), [](const auto& c) {
                return c.params.contains("objects") && c.params["objects"].contains("configfile");
            });
        REQUIRE(it != client.rpc_calls.rend());
        auto cb = it->success_cb;
        cb(json{{"result", {{"status", {{"configfile", {{"settings", settings}}}}}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    CHECK_FALSE(cfs.chute_wiper());
    // BOX_GO_TO_EXTRUDE_POS is an absolute XY move, so the chute purge needs X and Y homed.
    const IPrintRouting& routing = cfs;
    get_printer_state().update_from_status(json{{"toolhead", {{"homed_axes", "xy"}}}});
    CHECK(routing.manual_purge_gcode(50, 600).empty());

    answer_configfile(json::parse(R"({"box": {"clean_left_pos_x": 153.5, "has_extrude_pos": 1,
        "clean_left_pos_y": 225.3, "clean_right_pos_x": 178.0, "clean_right_pos_y": 225.3}})"));
    CHECK(routing.manual_purge_gcode(50, 600) == AmsBackendCfs::chute_purge_gcode(50, 600));
    CHECK(AmsBackendCfs::chute_purge_gcode(50, 600).find(
              "BOX_GO_TO_EXTRUDE_POS\nM83\nG1 E50 F600") != std::string::npos);
    get_printer_state().update_from_status(json{{"toolhead", {{"homed_axes", "xz"}}}});
    CHECK(routing.manual_purge_gcode(50, 600).empty());
    get_printer_state().update_from_status(json{{"toolhead", {{"homed_axes", "xy"}}}});
    const auto wiper = cfs.chute_wiper();
    REQUIRE(wiper);
    CHECK(wiper->x_min == Catch::Approx(153.5));
    CHECK(wiper->x_max == Catch::Approx(178.0));
    CHECK(wiper->y == Catch::Approx(225.3));

    cfs.on_klipper_restarted();
    answer_configfile(no_unit_settings());
    CHECK_FALSE(cfs.chute_wiper());
    CHECK(routing.manual_purge_gcode(50, 600).empty());
    get_printer_state().update_from_status(json{{"toolhead", {{"homed_axes", ""}}}});
}

TEST_CASE("CFS runout backup: another loaded spool of the same group, or none",
          "[ams][cfs][endless_spool][lane_runout]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    SECTION("stock: the group's other loaded spool") {
        json box = make_runout_box(0);
        box["same_material"] =
            json::array({json::array({"101001", "01A1A1A", json::array({"T1B", "T1C"}), "PLA"}),
                         json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"})});
        backend.fire_status(make_cfs_notification(box));
        CHECK(backend.runout_backup_for(1) == std::optional<int>(2));
        CHECK(backend.runout_backup_for(2) == std::optional<int>(1));
        // A singleton group, and an ungrouped slot: nothing to refill from.
        CHECK(backend.runout_backup_for(0) == std::optional<int>(-1));
        CHECK(backend.runout_backup_for(3) == std::optional<int>(-1));
    }
    SECTION("no grouping data: the box cannot say in advance") {
        backend.fire_status(make_cfs_notification(make_runout_box(0)));
        CHECK_FALSE(backend.runout_backup_for(1).has_value());
    }
}
