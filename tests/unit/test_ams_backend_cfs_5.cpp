// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 134 to 173 of test_ams_backend_cfs.cpp before it was split, in its order. Mostly
// [bypass][1904][1933], [endless_spool], [bypass][1904], [fork][1464], [bypass], and more

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

TEST_CASE("CFS endless spool: nothing is claimed before the first box frame",
          "[ams][cfs][endless_spool]") {
    // endless_spool_enabled starts false because a bool has to start somewhere,
    // not because the firmware said so. Answering Off from it puts "will not
    // switch spools on runout" on screen with no data behind it, and the frame
    // that lands a couple of seconds later then contradicts it — the line
    // appears and vanishes. Unknown says we do not know, NotReady says why.
    helix::test::StartedBackend<CfsRemapHelper> backend;

    auto caps = backend.get_endless_spool_capabilities();
    CHECK(caps.enabled == EndlessSpoolEnabled::Unknown);
    CHECK(caps.restriction == EndlessSpoolRestriction::NotReady);
    CHECK(caps.editability == EndlessSpoolEditability::ReadOnly);
    CHECK_FALSE(caps.editable());

    // The first real frame answers for real.
    backend.fire_status(make_cfs_notification(make_runout_box(0)));
    auto answered = backend.get_endless_spool_capabilities();
    CHECK(answered.enabled == EndlessSpoolEnabled::On);
    CHECK(answered.restriction == EndlessSpoolRestriction::FirmwareManaged);
}

TEST_CASE("CFS endless spool: the boot frame's auto_refill survives units coming up later",
          "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    SECTION("auto_refill=1 reads On once the unit connects") {
        const json full = make_runout_box(0);
        backend.fire_status(make_cfs_notification(make_boot_box_all_units_down(full)));
        backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));

        CHECK(backend.get_system_info().endless_spool_enabled);
        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
    }

    SECTION("auto_refill=0 reads Off once the unit connects") {
        json full = make_runout_box(0);
        full["auto_refill"] = 0;
        backend.fire_status(make_cfs_notification(make_boot_box_all_units_down(full)));
        backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));

        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);
    }

    SECTION("with no frame ever carrying auto_refill, a connected unit is still Unknown") {
        json full = make_runout_box(0);
        full.erase("auto_refill");
        backend.fire_status(make_cfs_notification(make_boot_box_all_units_down(full)));
        backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));

        const auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.enabled == EndlessSpoolEnabled::Unknown);
        CHECK(caps.restriction == EndlessSpoolRestriction::NotReady);
    }

    SECTION("a later delta that omits auto_refill keeps the value") {
        const json full = make_runout_box(0);
        backend.fire_status(make_cfs_notification(full));
        backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));

        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
    }
}

TEST_CASE("CFS endless spool: boot-frame grouping and tool map survive units coming up later",
          "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json full = make_runout_box(0);
    // Every group a singleton, so the grouping is what turns On into
    // OnWithoutBackup.
    full["same_material"] =
        json::array({json::array({"101001", "0FFFFFF", json::array({"T1A"}), "PLA"}),
                     json::array({"101001", "01A1A1A", json::array({"T1B"}), "PLA"})});
    full["map"]["T1A"] = "T1B";

    backend.fire_status(make_cfs_notification(make_boot_box_all_units_down(full)));
    backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));

    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::OnWithoutBackup);
    const auto map = backend.get_tool_mapping();
    REQUIRE_FALSE(map.empty());
    CHECK(map[0] == 1);
}

TEST_CASE("CFS Endless Spool switch follows the box-reported state", "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    SECTION("before any frame the switch is not offered") {
        CHECK_FALSE(backend.get_endless_spool_capabilities().switchable);
        CHECK_FALSE(backend.set_endless_spool_enabled(true).success());
    }

    SECTION("auto_refill=1 reports on, switchable") {
        backend.fire_status(make_cfs_notification(make_runout_box(0)));
        const auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.switchable);
        CHECK(caps.enabled == EndlessSpoolEnabled::On);
    }

    SECTION("auto_refill=0 reports off, switchable") {
        json box = make_runout_box(0);
        box["auto_refill"] = 0;
        backend.fire_status(make_cfs_notification(box));
        const auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.switchable);
        CHECK(caps.enabled == EndlessSpoolEnabled::Off);
    }

    SECTION("the old per-backend toggle action is gone") {
        CHECK_FALSE(find_action(backend, "toggle_auto_refill").has_value());
    }
}

TEST_CASE("CFS Endless Spool switch sends the switch's value", "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsAutoRefillHelper> backend;

    SECTION("switch on while the box reports on sends ENABLE=1") {
        backend.fire_status(make_cfs_notification(make_runout_box(0)));
        backend.captured.clear();
        REQUIRE(backend.set_endless_spool_enabled(true).success());
        CHECK(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=1"});
        drain_calib_queue();
    }

    SECTION("switch off while the box reports off sends ENABLE=0") {
        json box = make_runout_box(0);
        box["auto_refill"] = 0;
        backend.fire_status(make_cfs_notification(box));
        backend.captured.clear();
        REQUIRE(backend.set_endless_spool_enabled(false).success());
        CHECK(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=0"});
        drain_calib_queue();
    }
}

TEST_CASE("CFS Endless Spool switch: the sent state is what the status and switch show",
          "[ams][cfs][endless_spool]") {
    // The box resends auto_refill only when it changes, so nothing else will
    // tell the status line about a send.
    helix::test::StartedBackend<CfsAutoRefillHelper> backend;
    const json full = make_runout_box(0);
    backend.fire_status(make_cfs_notification(full));
    REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);

    REQUIRE(backend.set_endless_spool_enabled(false).success());
    drain_calib_queue();
    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);

    // A unit delta afterwards must not resurrect the pre-send value.
    backend.fire_status(make_cfs_notification(make_unit_up_delta(full)));
    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);

    // The box's own report still wins over the optimistic value.
    backend.fire_status(make_cfs_notification(json{{"auto_refill", 1}}));
    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
}

TEST_CASE("CFS Endless Spool switch: a send Klipper does not complete changes nothing",
          "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsAutoRefillHelper> backend;
    backend.reject = true;
    backend.fire_status(make_cfs_notification(make_runout_box(0)));
    REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);

    REQUIRE(backend.set_endless_spool_enabled(false).success());
    drain_calib_queue();
    REQUIRE(backend.captured.back() == "BOX_ENABLE_AUTO_REFILL ENABLE=0");

    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
}

TEST_CASE("CFS Endless Spool switch speaks the box module's dialect",
          "[ams][cfs][flat][endless_spool]") {
    helix::test::StartedBackend<CfsAutoRefillHelper> backend;

    SECTION("the identified fork sends its runout-swap setter") {
        backend.fire_status(make_cfs_notification(make_flat_fork_box()));
        backend.captured.clear();
        REQUIRE(backend.set_endless_spool_enabled(false).success());
        CHECK(backend.captured == std::vector<std::string>{"_BOX_SET_RUNOUT_SWAP ENABLE=0"});
        drain_calib_queue();
        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);
    }

    SECTION("an unidentified flat module is refused and sent nothing") {
        json box = make_flat_fork_box();
        box.erase("api_version");
        backend.fire_status(make_cfs_notification(box));
        backend.captured.clear();
        CHECK_FALSE(backend.get_endless_spool_capabilities().switchable);
        CHECK_FALSE(backend.set_endless_spool_enabled(true).success());
        CHECK(backend.captured.empty());
    }
}

TEST_CASE("CFS endless spool: a backend restart forgets the enable bit",
          "[ams][cfs][endless_spool]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_runout_box(0)));
    REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);

    CfsTestAccess::call_on_started(backend);
    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Unknown);
}

// Bypass declaration lifetime — what actually means "the CFS took the feed back"
//
// Field evidence, K2 Plus 2026-08-18. Bypass was declared (BOX_ENABLE_CFS_PRINT
// ENABLE=0) at 23:13 and restored cleanly across a restart at 00:34 with the box
// still reporting enable=0. By the next restart `enable` had returned to 1 with
// no host command in between: no BOX_ENABLE_CFS_PRINT anywhere in klippy.log, no
// disable_bypass() in ours, and the prints in that window were plain Moonraker
// start_print calls from Fluidd, which runs nothing vendor-specific. The box
// re-arms itself.
//
// Dropping the declaration on `enable` alone therefore threw bypass away on the
// first full frame after every restart (partial frames omit the field, so it only
// ever bit at startup) while the external spool was still feeding the nozzle.

TEST_CASE("CFS drops the bypass declaration once a bay is actually loaded", "[ams][cfs][bypass]") {
    // The real drift case the guard exists for: the CFS is feeding again, so the
    // declaration is stale and must not permit a later re-derivation.
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::set_bypass_declared(cfs, true);

    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    box["enable"] = 1;
    box["T1"]["filament"] = "A"; // bay 1 slot A is the active lane
    cfs.fire_status(make_cfs_notification(box));

    CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
}

TEST_CASE("CFS drops the bypass declaration for a loaded bay even while stood down",
          "[ams][cfs][bypass]") {
    // enable=0 with a bay loaded: the box is not participating in prints but a
    // lane is threaded and named active. Gating the drop on enable==1 as well
    // would leave the declaration latched with the CFS holding the path.
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::set_bypass_declared(cfs, true);

    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    box["enable"] = 0;
    box["T1"]["filament"] = "A";
    cfs.fire_status(make_cfs_notification(box));

    CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
}

TEST_CASE("CFS with no unit on the bus feeds only the external spool", "[ams][cfs][bypass][1904]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::answer_unit_registration(cfs, json::object());

    SECTION("before any box frame it claims nothing") {
        CHECK_FALSE(cfs.get_system_info().external_spool_only);
        CHECK_FALSE(cfs.is_bypass_active());
    }

    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    REQUIRE(cfs.get_system_info().units.empty());

    SECTION("an empty bus is external-spool-only, bypass with no declaration") {
        const auto info = cfs.get_system_info();
        CHECK(info.external_spool_only);
        CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
        CHECK(cfs.is_bypass_active());
        CHECK_FALSE(cfs.requires_slot_selection_for_load());
        // Nothing to switch between: the toggle has no other side.
        CHECK_FALSE(info.supports_bypass);
    }

    SECTION("the toolhead sensor decides whether the spool is loaded") {
        CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);
        cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
        CHECK(cfs.get_system_info().current_slot == helix::ui::kExternalSpoolSlot);

        CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/false);
        cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
        CHECK(cfs.get_system_info().current_slot == -1);
    }

    SECTION("unload of the loaded spool runs the external-spool script") {
        CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);
        cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
        REQUIRE(cfs.unload_filament(helix::ui::kExternalSpoolSlot).result == AmsResult::SUCCESS);
        REQUIRE(cfs.dispatched.size() == 1);
        CHECK(cfs.dispatched[0] ==
              AmsBackendCfs::bypass_unload_gcode(CfsTestAccess::macro_variant(cfs),
                                                 /*has_quit_material=*/false));
    }

    SECTION("a unit coming online ends it, loaded spool included") {
        CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);
        cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
        REQUIRE(cfs.get_system_info().current_slot == helix::ui::kExternalSpoolSlot);

        json box =
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
        cfs.fire_status(make_cfs_notification(box));
        const auto info = cfs.get_system_info();
        CHECK_FALSE(info.external_spool_only);
        CHECK_FALSE(cfs.is_bypass_active());
        CHECK(info.supports_bypass);
        CHECK(info.current_slot != helix::ui::kExternalSpoolSlot);
    }
}

TEST_CASE("CFS boot frame with no unit answered yet is not an empty bus",
          "[ams][cfs][bypass][1904][1933]") {
    // A K2 with a CFS boots with its first full frame naming every address
    // "None" (the units have not answered the RS-485 poll), while the box
    // reports itself connected. The unit arrives later as a delta.
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::answer_unit_registration(cfs, json::object());
    CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);

    json boot = make_empty_bus_box();
    boot["state"] = "connect";
    cfs.fire_status(make_cfs_notification(boot));
    REQUIRE(cfs.get_system_info().units.empty());

    auto check_never_single_spool = [&] {
        const auto info = cfs.get_system_info();
        CHECK_FALSE(info.external_spool_only);
        CHECK(info.presents_filament_system());
        CHECK_FALSE(cfs.is_bypass_active());
        CHECK(info.current_slot != helix::ui::kExternalSpoolSlot);
    };
    check_never_single_spool();

    // A frame carrying only the toolhead sensor re-derives the loaded path.
    cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
    check_never_single_spool();

    SECTION("the unit answering keeps it a filament system") {
        json full =
            make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
        cfs.fire_status(make_cfs_notification(json{{"T1", full["T1"]}}));
        REQUIRE_FALSE(cfs.get_system_info().units.empty());
        check_never_single_spool();
    }

    SECTION("a box state without a value decides nothing") {
        json bare = make_empty_bus_box();
        bare.erase("state");
        helix::test::StartedBackend<CfsRemapHelper> fresh;
        fresh.fire_status(make_cfs_notification(bare));
        CHECK_FALSE(fresh.get_system_info().external_spool_only);
    }

    SECTION("the box reporting disconnect settles it as an empty bus") {
        cfs.fire_status(make_cfs_notification(json{{"state", "disconnect"}}));
        CHECK(cfs.get_system_info().external_spool_only);
        CHECK(cfs.is_bypass_active());

        // And back: the box reconnecting with no unit yet is undecided again.
        cfs.fire_status(make_cfs_notification(json{{"state", "connect"}}));
        CHECK_FALSE(cfs.get_system_info().external_spool_only);
    }
}

TEST_CASE("CFS with a registered unit waits out the boot-time empty bus",
          "[ams][cfs][bypass][1904][1933]") {
    // The measured K2 Plus cold boot: box "None" at Klipper startup and ready,
    // "disconnect" about 7 s after ready, T1 answering at about 38 s, then the
    // box "connect".
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::answer_unit_registration(cfs, registered_unit_settings());

    cfs.fire_status(make_cfs_notification(k2_plus_startup_box()));
    feed_toolhead_filament(cfs);
    check_not_single_spool(cfs);

    cfs.fire_status(make_cfs_notification(json{{"state", "disconnect"}}));
    feed_toolhead_filament(cfs);
    check_not_single_spool(cfs);

    cfs.fire_status(make_cfs_notification(json{{"T1", k2_plus_t1_connected()}}));
    REQUIRE_FALSE(cfs.get_system_info().units.empty());
    check_not_single_spool(cfs);

    cfs.fire_status(make_cfs_notification(json{{"state", "connect"}}));
    check_not_single_spool(cfs);

    SECTION("the settle time passing after the unit answered changes nothing") {
        CfsTestAccess::expire_unit_settle(cfs);
        check_not_single_spool(cfs);
    }
}

TEST_CASE("CFS with no registered unit is single-spool when the box reports disconnect",
          "[ams][cfs][bypass][1904][1933]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;

    SECTION("an empty address table") {
        CfsTestAccess::answer_unit_registration(cfs, no_unit_settings());
    }
    SECTION("no address table at all") {
        CfsTestAccess::answer_unit_registration(cfs, json::object());
    }

    cfs.fire_status(make_cfs_notification(k2_plus_startup_box()));
    CHECK_FALSE(cfs.get_system_info().external_spool_only);

    cfs.fire_status(make_cfs_notification(json{{"state", "disconnect"}}));
    feed_toolhead_filament(cfs);
    const auto info = cfs.get_system_info();
    CHECK(info.external_spool_only);
    CHECK_FALSE(info.supports_bypass);
    CHECK(cfs.is_bypass_active());
    CHECK(info.current_slot == helix::ui::kExternalSpoolSlot);
}

TEST_CASE("CFS with a registered unit that never answers is single-spool after the settle time",
          "[ams][cfs][bypass][1904][1933]") {
    // A CFS unplugged since printer.cfg last saved it still leaves its entry.
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::answer_unit_registration(cfs, registered_unit_settings());
    cfs.fire_status(make_cfs_notification(k2_plus_startup_box()));
    cfs.fire_status(make_cfs_notification(json{{"state", "disconnect"}}));
    feed_toolhead_filament(cfs);
    check_not_single_spool(cfs);

    CfsTestAccess::expire_unit_settle(cfs);
    const auto info = cfs.get_system_info();
    CHECK(info.external_spool_only);
    CHECK_FALSE(info.supports_bypass);
    CHECK(cfs.is_bypass_active());
    CHECK(info.current_slot == helix::ui::kExternalSpoolSlot);

    SECTION("a unit answering late still ends it") {
        cfs.fire_status(make_cfs_notification(json{{"T1", k2_plus_t1_connected()}}));
        check_not_single_spool(cfs);
        CHECK(cfs.get_system_info().supports_bypass);
    }
}

TEST_CASE("CFS decides nothing on an empty bus until printer.cfg has answered",
          "[ams][cfs][bypass][1904][1933]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::await_unit_registration(cfs);
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    feed_toolhead_filament(cfs);
    check_not_single_spool(cfs);

    SECTION("an answer of no unit settles it at once") {
        CfsTestAccess::answer_unit_registration(cfs, no_unit_settings());
        CHECK(cfs.get_system_info().external_spool_only);
        CHECK(cfs.is_bypass_active());
    }
    SECTION("an answer naming a unit keeps waiting") {
        CfsTestAccess::answer_unit_registration(cfs, registered_unit_settings());
        check_not_single_spool(cfs);
    }
    SECTION("no answer at all still ends at the settle time") {
        CfsTestAccess::expire_unit_settle(cfs);
        CHECK(cfs.get_system_info().external_spool_only);
    }
}

TEST_CASE("CFS with no registered unit stays single-spool across Refresh RFID and Comm test",
          "[ams][cfs][bypass][1904][1933]") {
    // Both actions re-query the box; neither is a new session, so neither may
    // reopen the settle window or fetch the configfile again.
    helix::test::StartedBackend<UnansweredClientCfs> cfs;
    auto& client = cfs.wire().fake_client();

    REQUIRE(configfile_queries(client) == 1);
    CfsTestAccess::answer_unit_registration(cfs, no_unit_settings());
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    REQUIRE(cfs.get_system_info().external_spool_only);

    for (const char* action : {"refresh_rfid", "comm_test"}) {
        CAPTURE(action);
        REQUIRE(cfs.execute_device_action(action, std::any{}).result == AmsResult::SUCCESS);
        // The re-query's answer arrives as a full box frame.
        cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
        CHECK(cfs.get_system_info().external_spool_only);
    }
    CHECK(configfile_queries(client) == 1);
}

TEST_CASE("CFS with no registered unit stays single-spool across a Klipper restart",
          "[ams][cfs][bypass][1904][1933]") {
    helix::test::StartedBackend<UnansweredClientCfs> cfs;
    auto& client = cfs.wire().fake_client();
    CfsTestAccess::answer_unit_registration(cfs, no_unit_settings());
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    feed_toolhead_filament(cfs);
    REQUIRE(cfs.get_system_info().external_spool_only);
    REQUIRE(cfs.get_system_info().current_slot == helix::ui::kExternalSpoolSlot);

    auto check_single_spool = [&cfs] {
        const auto info = cfs.get_system_info();
        CHECK(info.external_spool_only);
        CHECK_FALSE(info.supports_bypass);
        CHECK(info.current_slot == helix::ui::kExternalSpoolSlot);
    };

    // The configfile is asked again, and the frames of a Klipper start arrive
    // before it answers: the box "None", then "disconnect".
    cfs.on_klipper_restarted();
    CHECK(configfile_queries(client) == 2);
    cfs.fire_status(make_cfs_notification(k2_plus_startup_box()));
    feed_toolhead_filament(cfs);
    check_single_spool();
    cfs.fire_status(make_cfs_notification(json{{"state", "disconnect"}}));
    check_single_spool();

    CfsTestAccess::answer_unit_registration(cfs, no_unit_settings());
    check_single_spool();
}

TEST_CASE("CFS with a registered unit gets a new settle window on a Klipper restart",
          "[ams][cfs][bypass][1904][1933]") {
    helix::test::StartedBackend<UnansweredClientCfs> cfs;
    auto& client = cfs.wire().fake_client();
    CfsTestAccess::answer_unit_registration(cfs, registered_unit_settings());
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    CfsTestAccess::expire_unit_settle(cfs);
    REQUIRE(cfs.get_system_info().external_spool_only);

    // A SAVE_CONFIG restart may come with the unit plugged back in.
    cfs.on_klipper_restarted();
    CHECK(configfile_queries(client) == 2);
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    CHECK_FALSE(cfs.get_system_info().external_spool_only);

    SECTION("the unit answering ends the wait") {
        cfs.fire_status(make_cfs_notification(json{{"T1", k2_plus_t1_connected()}}));
        check_not_single_spool(cfs);
    }
    SECTION("the new window running out ends it single-spool again") {
        CfsTestAccess::expire_unit_settle(cfs);
        CHECK(cfs.get_system_info().external_spool_only);
    }
}

TEST_CASE("CFS keeps a unit it has seen when a later frame drops it", "[ams][cfs][bypass][1904]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    cfs.fire_status(make_cfs_notification(box));
    REQUIRE_FALSE(cfs.get_system_info().units.empty());

    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    CHECK_FALSE(cfs.get_system_info().external_spool_only);
    CHECK_FALSE(cfs.is_bypass_active());
}

TEST_CASE("CFS flat schema is never external-spool-only", "[ams][cfs][bypass][1904]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    json box = make_flat_fork_box();
    box["slots"] = json::array();
    cfs.fire_status(make_cfs_notification(box));
    REQUIRE(CfsTestAccess::schema(cfs) == CfsSchema::Flat);
    CHECK_FALSE(cfs.get_system_info().external_spool_only);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "CFS bypass setters refuse an empty bus, forced controls included",
                 "[ams][cfs][bypass][1904]") {
    helix::test::ForceBypassControlsGuard force;
    REQUIRE(SettingsManager::instance().get_ams_force_bypass_controls());
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    CfsTestAccess::answer_unit_registration(cfs, json::object());
    cfs.fire_status(make_cfs_notification(make_empty_bus_box()));
    REQUIRE(cfs.get_system_info().external_spool_only);
    CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);
    cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
    REQUIRE(cfs.get_system_info().current_slot == helix::ui::kExternalSpoolSlot);

    CHECK(cfs.enable_bypass().result == AmsResult::NOT_SUPPORTED);
    CHECK(cfs.disable_bypass().result == AmsResult::NOT_SUPPORTED);
    CHECK(cfs.captured.empty());
    CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
}

TEST_CASE("CFS with a unit on the bus is not external-spool-only", "[ams][cfs][bypass][1904]") {
    helix::test::StartedBackend<CfsRemapHelper> cfs;
    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    // A box stood down by the bypass declaration still has its unit.
    box["enable"] = 0;
    cfs.fire_status(make_cfs_notification(box));
    CHECK_FALSE(cfs.get_system_info().external_spool_only);
    CHECK_FALSE(cfs.is_bypass_active());
}

TEST_CASE("CFS flat: the runout-swap plan is not a runout event", "[ams][cfs][flat][1390]") {
    // The plan rides every frame once a lane is loaded and swapping is on, so
    // treating it as an event arms an episode against the lane that just
    // loaded successfully. That episode then strips the lane's remembered
    // Spoolman link the first time the bay reads empty — silent, permanent
    // data loss for a fork user who also runs Spoolman.
    helix::test::StartedBackend<CfsRemapHelper> backend;

    // Mid-load: gate already fed, nothing loaded yet, so no chain is published.
    json loading = make_flat_fork_box();
    loading["runout"] = nullptr;
    loading["loaded_slot"] = -1;
    loading["loaded_mask"] = 0;
    loading["slots"][0]["loaded"] = false;
    backend.fire_status(make_cfs_notification(loading));
    REQUIRE_FALSE(backend.get_system_info().filament_runout);

    // Bay 0 remembers a Spoolman link. Seeded directly: the strip reads the
    // override, and nothing about this case depends on how it got there.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.brand = "eSUN";
    ovr.spool_name = "Black PLA";
    ovr.spoolman_id = 137;
    ovr.spoolman_vendor_id = 21;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Load completes: bay 0 feeding, gate reading true, chain published.
    backend.fire_status(make_cfs_notification(make_flat_fork_box()));
    CHECK_FALSE(backend.get_system_info().filament_runout);

    // The user pulls bay 0's spool later. With an episode armed against that
    // lane, this is the poll that drops its Spoolman link for good.
    json pulled = make_flat_fork_box();
    pulled["slots"][0]["present"] = false;
    pulled["slots"][0]["loaded"] = false;
    pulled["loaded_slot"] = -1;
    pulled["loaded_mask"] = 0;
    backend.fire_status(make_cfs_notification(pulled));

    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    auto after = CfsTestAccess::get_override(backend, 0);
    REQUIRE(after.has_value());
    CHECK(after->spoolman_id == 137);
    CHECK(after->spoolman_vendor_id == 21);
}

TEST_CASE("CFS flat: a gate reading the frame did not publish leaves the latch",
          "[ams][cfs][flat]") {
    SECTION("a null reading does not invent a runout on a healthy box") {
        // Klipper publishes a sensor field as null until its first read; the
        // sibling filament_sensor_error ships that way on a healthy box. Null
        // is "no reading", which is not the same answer as false.
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_flat_fork_box()));
        REQUIRE_FALSE(backend.get_system_info().filament_runout);

        json unread = make_flat_fork_box();
        unread["filament_detected"] = nullptr;
        REQUIRE_NOTHROW(backend.fire_status(make_cfs_notification(unread)));
        CHECK_FALSE(backend.get_system_info().filament_runout);
    }

    SECTION("a null reading does not clear a latched runout") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json empty_gate = make_flat_fork_box();
        empty_gate["filament_detected"] = false;
        empty_gate["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(empty_gate));
        REQUIRE(backend.get_system_info().filament_runout);

        json unread = make_flat_fork_box();
        unread["filament_detected"] = nullptr;
        unread["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(unread));
        CHECK(backend.get_system_info().filament_runout);
    }

    SECTION("a delta that omits the key entirely leaves the latch") {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        json empty_gate = make_flat_fork_box();
        empty_gate["filament_detected"] = false;
        empty_gate["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(empty_gate));
        REQUIRE(backend.get_system_info().filament_runout);

        json delta = make_flat_fork_box();
        delta.erase("filament_detected");
        delta["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(delta));
        CHECK(backend.get_system_info().filament_runout);
    }
}

TEST_CASE("CFS flat: a slot delta does not clear the endless-spool enable bit",
          "[ams][cfs][flat][endless_spool]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;

    backend.fire_status(make_cfs_notification(make_flat_fork_box()));
    REQUIRE(backend.get_system_info().endless_spool_enabled);
    REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);

    // What Moonraker actually pushes when one bay changes: the slots array and
    // nothing else. runout_swap_enabled did not change, so it is not resent —
    // and a flat frame always carries slots, so this branch always runs.
    json delta = make_flat_fork_box();
    delta.erase("runout_swap_enabled");
    delta["slots"][2]["present"] = false;
    backend.fire_status(make_cfs_notification(delta));

    CHECK(backend.get_system_info().endless_spool_enabled);
    CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
}

// The fork's `runout` object is box.py's swap plan: `loaded_slot` and `chain`,
// the other present slots with identical material and colour in slot order.
// A runout swaps to chain[0], so that edge is the per-slot relation the AMS
// context menu's backup row reads (#1464).
TEST_CASE("CFS flat: runout.chain is the loaded slot's backup edge",
          "[ams][cfs][flat][endless_spool][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_flat_fork_box()));

    SECTION("the chain head backs the loaded slot, and the row has a relation to show") {
        const auto caps = backend.get_endless_spool_capabilities();
        const auto cfg = backend.get_endless_spool_config();
        // decide_show_backup_row's inputs: a read-only backend shows the row
        // only when it is available AND reports a relation.
        CHECK(caps.available());
        CHECK_FALSE(cfg.empty());
        CHECK(caps.enabled == EndlessSpoolEnabled::On);
        CHECK(endless_spool_backup_for(cfg, 0) == 1);
        CHECK(endless_spool_backup_for(cfg, 1) == -1);
        CHECK(endless_spool_backup_for(cfg, 2) == -1);
    }

    SECTION("a frame carrying only runout reads its bays from the frames before it") {
        backend.fire_status(make_cfs_notification(
            json{{"runout", {{"chain", json::array({3})}, {"loaded_slot", 0}}}}));
        CHECK(endless_spool_backup_for(backend.get_endless_spool_config(), 0) == 3);
    }

    SECTION("a delta that omits runout keeps the parsed chain") {
        json delta = make_flat_fork_box();
        delta.erase("runout");
        delta["slots"][3]["present"] = false;
        backend.fire_status(make_cfs_notification(delta));
        CHECK(endless_spool_backup_for(backend.get_endless_spool_config(), 0) == 1);
    }

    SECTION("runout null (nothing loaded) clears the relation, not a negative") {
        json idle = make_flat_fork_box();
        idle["runout"] = nullptr;
        idle["loaded_slot"] = -1;
        backend.fire_status(make_cfs_notification(idle));
        CHECK(backend.get_endless_spool_config().empty());
        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
    }

    SECTION("an empty chain means the loaded spool has no backup") {
        json lonely = make_flat_fork_box();
        lonely["runout"] = json{{"chain", json::array()}, {"loaded_slot", 0}};
        backend.fire_status(make_cfs_notification(lonely));
        CHECK(backend.get_endless_spool_config().empty());
        CHECK(backend.get_endless_spool_capabilities().enabled ==
              EndlessSpoolEnabled::OnWithoutBackup);
    }

    SECTION("only the chain head is a backup; the tail is not a succession") {
        json multi = make_flat_fork_box();
        multi["runout"] = json{{"chain", json::array({0, 3})}, {"loaded_slot", 2}};
        backend.fire_status(make_cfs_notification(multi));
        const auto cfg = backend.get_endless_spool_config();
        CHECK(endless_spool_backup_for(cfg, 2) == 0);
        CHECK(endless_spool_backup_for(cfg, 0) == -1);
        CHECK(endless_spool_backup_for(cfg, 3) == -1);
    }

    SECTION("swap disabled: the plan is published but no backup is claimed") {
        json off = make_flat_fork_box();
        off["runout_swap_enabled"] = false;
        backend.fire_status(make_cfs_notification(off));
        CHECK(backend.get_endless_spool_config().empty());
    }

    SECTION("a chain head that names no other bay states nothing, not a negative") {
        // Only an empty chain is the firmware saying "no backup". A head we
        // cannot map is no statement at all: no relation, and plain On.
        for (const json& chain : {json::array({9}), json::array({0}), json::array({"1"})}) {
            json bad = make_flat_fork_box();
            bad["runout"] = json{{"chain", chain}, {"loaded_slot", 0}};
            REQUIRE_NOTHROW(backend.fire_status(make_cfs_notification(bad)));
            CHECK(backend.get_endless_spool_config().empty());
            CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
        }
    }

    SECTION("a stock frame drops the fork-era chain") {
        backend.fire_status(make_cfs_notification(make_runout_box(0)));
        CHECK(backend.get_endless_spool_config().empty());
    }
}

TEST_CASE("CFS flat: payload slot indices are the bays across a box gap",
          "[ams][cfs][flat][endless_spool][1464]") {
    const json box = make_gapped_fork_box();
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(box));

    CHECK(backend.get_system_info().current_slot == 6);
    const auto cfg = backend.get_endless_spool_config();
    CHECK(endless_spool_backup_for(cfg, 6) == 8);
    CHECK(endless_spool_backup_for(cfg, 2) == -1);

    SECTION("the external entry still reads as bypass") {
        json bypass = box;
        bypass["loaded_slot"] = 12;
        bypass["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(bypass));
        CHECK(backend.get_system_info().current_slot == -2);
    }
}

// Commands go the other way: box.py registers T<n> and takes SLOT= in its own
// global numbering, which is the bay's own index, so bay 6 is sent as 6.
TEST_CASE("CFS fork: commands name the firmware slot, not the bay position",
          "[ams][cfs][fork][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_gapped_fork_box()));

    SECTION("load") {
        REQUIRE(backend.load_filament(7).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched == std::vector<std::string>{"T7"});
    }

    SECTION("tool change with a spool loaded swaps through T<n>") {
        REQUIRE(backend.change_tool(8).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched == std::vector<std::string>{"T8"});
    }

    SECTION("slot identity write") {
        backend.push(6, "PETG", "eSUN", "", 0x0A2989);
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0].rfind("_BOX_SLOT_SET SLOT=6 ", 0) == 0);
    }

    SECTION("slot clear") {
        backend.push(6, "", "", "", kAmsDefaultSlotColor);
        CHECK(backend.captured == std::vector<std::string>{"_BOX_SLOT_CLEAR SLOT=6"});
    }

    // The box's NAME is the product the bay holds, the same field it reports
    // back as the product, so a catalog pick writes the product's name.
    SECTION("slot identity write names the product") {
        SlotInfo info;
        info.material = "PLA";
        info.brand = "Creality";
        info.color_rgb = 0xD74242;
        info.product_name = "Hyper PLA";
        REQUIRE(backend.push_info(6, info).success());
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0].find(R"( NAME="Hyper PLA" )") != std::string::npos);
    }
    SECTION("with no product the spool's own name is written") {
        SlotInfo info;
        info.material = "PLA";
        info.brand = "Creality";
        info.color_rgb = 0xD74242;
        info.spool_name = "Shelf 3 red";
        REQUIRE(backend.push_info(6, info).success());
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0].find(R"( NAME="Shelf 3 red" )") != std::string::npos);
    }
}

// The fork's slots[] states what the box holds, set by _BOX_SLOT_SET or read
// off a tag, and says nothing about which: none of it is badged as a tag read.
TEST_CASE("CFS fork: slot data is not a tag reading", "[ams][cfs][fork][tag_link]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_flat_fork_box()));
    REQUIRE(backend.get_slot_info(0).material == "PLA");
    CHECK_FALSE(backend.tag_reading(0));
    CHECK_FALSE(backend.tag_reading(2));
}

// The fork's box.py registers NOZZLE_CLEAN and no error-clear, error-resume
// or tag re-read command (Jacob10383/k2-plus-custom-firmware extras/box.py).
TEST_CASE("CFS fork: buttons send only commands the fork registers", "[ams][cfs][fork]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_flat_fork_box()));
    backend.captured.clear();

    SECTION("Reset sends no stock error clear") {
        REQUIRE(backend.reset().success());
        CHECK(backend.captured.empty());
    }
    SECTION("Recover resumes through RESUME") {
        REQUIRE(backend.recover().success());
        CHECK(backend.captured == std::vector<std::string>{"RESUME"});
    }
    SECTION("Clean Nozzle sends the fork's NOZZLE_CLEAN") {
        REQUIRE(find_action(backend, "nozzle_clean").has_value());
        REQUIRE(backend.execute_device_action("nozzle_clean", {}).success());
        CHECK(backend.captured == std::vector<std::string>{"NOZZLE_CLEAN"});
    }
    SECTION("Refresh RFID is not offered") {
        CHECK_FALSE(find_action(backend, "refresh_rfid").has_value());
    }
}

// Firmware with print mapping registers BOX_SELECT_SLOT, which always names a
// slot. T<n> names the file's tool during a mapped print, so a manual load sent
// as T<n> mid-print would feed whatever slot that tool maps to.
TEST_CASE("CFS fork: manual loads select the slot when the firmware has print mapping",
          "[ams][cfs][fork][select_slot]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json box = make_gapped_fork_box();

    SECTION("revision 2") {
        box["api_revision"] = 2;
        backend.fire_status(make_cfs_notification(box));

        SECTION("load") {
            REQUIRE(backend.load_filament(7).result == AmsResult::SUCCESS);
            CHECK(backend.dispatched == std::vector<std::string>{"BOX_SELECT_SLOT SLOT=7"});
        }
        SECTION("swap") {
            REQUIRE(backend.change_tool(8).result == AmsResult::SUCCESS);
            CHECK(backend.dispatched == std::vector<std::string>{"BOX_SELECT_SLOT SLOT=8"});
        }
        SECTION("external spool") {
            box["loaded_slot"] = -1;
            box["runout"] = nullptr;
            backend.fire_status(make_cfs_notification(box));
            REQUIRE(backend.enable_bypass().result == AmsResult::SUCCESS);
            CHECK(backend.dispatched == std::vector<std::string>{"BOX_SELECT_SLOT SLOT=12"});
        }
    }

    SECTION("revision 1 with print_mapping_version 1") {
        box["print_mapping_version"] = 1;
        backend.fire_status(make_cfs_notification(box));
        REQUIRE(backend.load_filament(7).result == AmsResult::SUCCESS);
        CHECK(backend.dispatched == std::vector<std::string>{"BOX_SELECT_SLOT SLOT=7"});
    }

    SECTION("revision 1 without print mapping keeps T<n>") {
        backend.fire_status(make_cfs_notification(box));
        REQUIRE(backend.load_filament(7).result == AmsResult::SUCCESS);
        CHECK(backend.dispatched == std::vector<std::string>{"T7"});
    }

    SECTION("revision 1 without print mapping keeps T<external>") {
        box["loaded_slot"] = -1;
        box["runout"] = nullptr;
        backend.fire_status(make_cfs_notification(box));
        REQUIRE(backend.enable_bypass().result == AmsResult::SUCCESS);
        CHECK(backend.dispatched == std::vector<std::string>{"T12"});
    }

    SECTION("a delta without the version fields keeps the capability") {
        box["api_revision"] = 2;
        backend.fire_status(make_cfs_notification(box));
        backend.fire_status(make_cfs_notification(json{{"temp_c", 25}}));
        REQUIRE(backend.load_filament(7).result == AmsResult::SUCCESS);
        CHECK(backend.dispatched == std::vector<std::string>{"BOX_SELECT_SLOT SLOT=7"});
    }

    SECTION("stock dialect is unchanged") {
        CHECK(AmsBackendCfs::load_gcode(7, CfsMacroVariant::K2).find("BOX_SELECT_SLOT") ==
              std::string::npos);
        CHECK(AmsBackendCfs::swap_gcode(7, CfsMacroVariant::K1).find("BOX_SELECT_SLOT") ==
              std::string::npos);
    }
}

// BOX_PRINT_MAP sets the tool->slot map for the next print only, so the Fork
// remap is a pre-print send. Its MAP= is BOX_PRINT_START's: file tool, then
// global slot, which is our bay index.
TEST_CASE("CFS fork: print mapping goes out as BOX_PRINT_MAP before the print",
          "[ams][cfs][fork][print_map]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    json box = make_gapped_fork_box();

    SECTION("revision 4 sends the map") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));

        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::PrePrintSend);
        CHECK(backend.requires_preprint_send());
        CHECK(helix::printer::can_remap(backend));
        CHECK_FALSE(helix::printer::remap_is_persistent(backend.get_remap_strategy()));

        CHECK(backend.build_preprint_gcode({0, 1}, {{0, 5}, {1, 6}}) ==
              "BOX_PRINT_MAP MAP=0:5,1:6");

        // The format, apart from which slots this box set can feed.
        CHECK(AmsBackendCfs::print_map_gcode({0, 1}, {{0, 5}, {1, 2}}) ==
              "BOX_PRINT_MAP MAP=0:5,1:2");
        // A used tool the user left alone is still named, at its own slot, so
        // the map says what the whole print means.
        CHECK(AmsBackendCfs::print_map_gcode({0, 1, 2}, {{0, 5}}) ==
              "BOX_PRINT_MAP MAP=0:5,1:1,2:2");
        // Tool order is the file's, not the remap's insertion order.
        CHECK(AmsBackendCfs::print_map_gcode({}, {{3, 9}, {1, 6}}) == "BOX_PRINT_MAP MAP=1:6,3:9");
    }

    SECTION("no remap still sends the identity map for the file's tools") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        CHECK(AmsBackendCfs::print_map_gcode({0, 1}, {}) == "BOX_PRINT_MAP MAP=0:0,1:1");
        CHECK(backend.build_preprint_gcode({0, 1}, {}) == "BOX_PRINT_MAP MAP=0:0,1:1");
        CHECK(backend.build_preprint_gcode({0, 1}, {{0, 0}, {1, 1}}) ==
              "BOX_PRINT_MAP MAP=0:0,1:1");
    }

    SECTION("a start with nothing to map clears any pending map") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        CHECK(AmsBackendCfs::print_map_gcode({}, {}).empty());
        CHECK(backend.build_preprint_gcode({}, {}) == "BOX_PRINT_MAP MAP=");
        CHECK(backend.build_preprint_gcode({}, {{2, 2}}) == "BOX_PRINT_MAP MAP=");
    }

    SECTION("the map names only the file's tools") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        CHECK(AmsBackendCfs::print_map_gcode({0}, {{0, 2}, {3, 1}}) == "BOX_PRINT_MAP MAP=0:2");
        // A tool the file does not use cannot refuse the print either: slot 1
        // sits in a box this chain does not have.
        CHECK(backend.check_preprint_remap({0}, {{0, 9}, {3, 1}}).success());
        CHECK(backend.build_preprint_gcode({0}, {{0, 9}, {3, 1}}) == "BOX_PRINT_MAP MAP=0:9");
    }

    SECTION("an unremapped start after a remapped one overwrites the pending map") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        REQUIRE(backend.build_preprint_gcode({0, 1}, {{0, 9}, {1, 10}}) ==
                "BOX_PRINT_MAP MAP=0:9,1:10");
        CHECK(backend.build_preprint_gcode({0, 1}, {}) == "BOX_PRINT_MAP MAP=0:0,1:1");
    }

    SECTION("a remap to a slot that cannot feed sends only the clear and says why") {
        box["api_revision"] = 4;
        box["boxes"] = json::array(
            {json{{"address", 2}, {"online", false}}, json{{"address", 3}, {"online", true}}});
        backend.fire_status(make_cfs_notification(box));

        // Box 2 (slots 4-7) is not answering.
        auto err = backend.check_preprint_remap({0}, {{0, 5}});
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.slot_index == 5);
        CHECK(err.user_msg.find("Disconnected") != std::string::npos);
        CHECK(backend.build_preprint_gcode({0}, {{0, 5}}) == "BOX_PRINT_MAP MAP=");

        // Box 1 (slots 0-3) is not on the bus at all.
        err = backend.check_preprint_remap({0}, {{0, 1}});
        CHECK(err.result == AmsResult::INVALID_SLOT);
        CHECK(err.slot_index == 1);
        CHECK(backend.build_preprint_gcode({0}, {{0, 1}}) == "BOX_PRINT_MAP MAP=");

        // One bad target refuses the whole map, not just its own entry.
        CHECK(backend.build_preprint_gcode({0, 1}, {{0, 9}, {1, 5}}) == "BOX_PRINT_MAP MAP=");

        // Box 3 answers.
        CHECK(backend.check_preprint_remap({0}, {{0, 9}}).success());
        CHECK(backend.build_preprint_gcode({0}, {{0, 9}}) == "BOX_PRINT_MAP MAP=0:9");
    }

    SECTION("the pre-print send is not a BOX_MODIFY_TN table write") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        CHECK(backend.can_set_tool_mapping(0, 5).result == AmsResult::NOT_SUPPORTED);
        CHECK(backend.set_tool_mapping(0, 5).result == AmsResult::NOT_SUPPORTED);
        CHECK(backend.captured.empty());
    }

    SECTION("revision 3 is not trusted with BOX_PRINT_MAP") {
        box["api_revision"] = 3;
        backend.fire_status(make_cfs_notification(box));
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::None);
        CHECK_FALSE(backend.requires_preprint_send());
        CHECK(backend.build_preprint_gcode({0, 1}, {{0, 5}, {1, 6}}).empty());
        CHECK(backend.build_preprint_gcode({}, {}).empty());
    }

    SECTION("revision 2 has no BOX_PRINT_MAP: no remap, no pre-print send") {
        box["api_revision"] = 2;
        backend.fire_status(make_cfs_notification(box));
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::None);
        CHECK_FALSE(backend.requires_preprint_send());
        CHECK(backend.build_preprint_gcode({0, 1}, {{0, 5}, {1, 2}}).empty());
        CHECK(backend.build_preprint_gcode({0, 1}, {}).empty());
        CHECK(backend.build_preprint_gcode({}, {}).empty());
    }

    SECTION("revision 1 has none either") {
        backend.fire_status(make_cfs_notification(box));
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::None);
        CHECK_FALSE(backend.requires_preprint_send());
    }

    SECTION("a box that turns stock drops it") {
        box["api_revision"] = 4;
        backend.fire_status(make_cfs_notification(box));
        REQUIRE(backend.requires_preprint_send());
        backend.fire_status(make_cfs_notification(make_cfs_status_json()["box"]));
        CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
        CHECK_FALSE(backend.requires_preprint_send());
    }
}

TEST_CASE("CFS stock keeps its native remap and no pre-print send", "[ams][cfs][print_map]") {
    CfsRemapHelper backend;
    CHECK(backend.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    CHECK_FALSE(backend.requires_preprint_send());
    CHECK(backend.build_preprint_gcode({0, 1}, {{0, 5}, {1, 2}}).empty());
    CHECK(backend.build_preprint_gcode({0, 1}, {}).empty());
}

// A bay in a box missing from the chain is refused, not sent: box.py registers
// no T<n> for a slot it has not found.
TEST_CASE("CFS fork: a bay no box reported is refused", "[ams][cfs][fork][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_gapped_fork_box()));

    SECTION("load") {
        const auto err = backend.load_filament(2);
        CHECK(err.result == AmsResult::INVALID_SLOT);
        CHECK(backend.dispatched.empty());
    }

    SECTION("tool change") {
        const auto err = backend.change_tool(1);
        CHECK(err.result == AmsResult::INVALID_SLOT);
        CHECK(err.technical_msg == "Slot 1 not known to the box firmware");
        CHECK(backend.dispatched.empty());
    }

    SECTION("slot clear") {
        helix::AmsBackend& base = backend;
        base.clear_slot_override(2);
        CHECK(backend.captured.empty());
    }
}

// box.py registers no BOX_MODIFY_TN, so a remap on Fork would error and move
// nothing. The UI asks can_remap() before offering the pick.
TEST_CASE("CFS fork: tool remapping is not offered", "[ams][cfs][fork][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> stock;
    stock.fire_status(make_cfs_notification(make_multi_unit_box(1)));
    CHECK(helix::printer::can_remap(stock));

    helix::test::StartedBackend<CfsRemapHelper> fork;
    fork.fire_status(make_cfs_notification(make_flat_fork_box()));
    CHECK_FALSE(helix::printer::can_remap(fork));
    CHECK(fork.set_tool_mapping(0, 1).result == AmsResult::NOT_SUPPORTED);
    CHECK(fork.captured.empty());
}

// Per-bay state is keyed by the firmware slot, so a box leaving the chain does
// not hand its bays' state to the box behind it, and a box coming back finds
// its own (#1464).
TEST_CASE("CFS fork: per-bay state stays on its physical slot when a box drops",
          "[ams][cfs][fork][1464]") {
    helix::test::StartedBackend<CfsRemapHelper> backend;
    backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));

    SECTION("a Spoolman-linked override survives its box dropping out") {
        helix::ams::FilamentSlotOverride box2a;
        box2a.spool_name = "Box 2 A spool";
        box2a.spoolman_id = 104;
        CfsTestAccess::seed_override(backend, 4, box2a);
        helix::ams::FilamentSlotOverride box3a;
        box3a.spool_name = "Box 3 A spool";
        box3a.spoolman_id = 108;
        CfsTestAccess::seed_override(backend, 8, box3a);

        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 3})));
        REQUIRE(CfsTestAccess::get_override(backend, 4).has_value());
        CHECK(CfsTestAccess::get_override(backend, 4)->spool_name == "Box 2 A spool");
        REQUIRE(CfsTestAccess::get_override(backend, 8).has_value());
        CHECK(CfsTestAccess::get_override(backend, 8)->spool_name == "Box 3 A spool");

        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));
        REQUIRE(CfsTestAccess::get_override(backend, 4).has_value());
        CHECK(CfsTestAccess::get_override(backend, 4)->spool_name == "Box 2 A spool");
        CHECK(CfsTestAccess::get_override(backend, 8)->spool_name == "Box 3 A spool");
    }

    SECTION("an unlinked override is not read as a spool swap") {
        helix::ams::FilamentSlotOverride box2a;
        box2a.spool_name = "Box 2 A spool";
        CfsTestAccess::seed_override(backend, 4, box2a);

        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 3})));
        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));
        REQUIRE(CfsTestAccess::get_override(backend, 4).has_value());
        CHECK(CfsTestAccess::get_override(backend, 4)->spool_name == "Box 2 A spool");
    }

    SECTION("bays report the firmware's slot numbers and its tools") {
        backend.fire_status(
            make_cfs_notification(make_fork_box_with_boxes({1, 3}, /*loaded_slot=*/9)));
        const auto info = backend.get_system_info();
        CHECK(info.current_slot == 9);
        CHECK(info.current_tool == 9);
        for (int bay : {0, 3, 8, 11}) {
            INFO("bay " << bay);
            CHECK(backend.get_slot_info(bay).color_rgb == 0x101010u * (bay + 1));
            CHECK(backend.get_slot_info(bay).mapped_tool == bay);
        }
        REQUIRE(info.tool_to_slot_map.size() > 8);
        CHECK(info.tool_to_slot_map[8] == 8);
    }

    SECTION("a bay in the gap is refused, not sent") {
        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 3})));
        CHECK(backend.load_filament(5).result == AmsResult::INVALID_SLOT);
        CHECK(backend.dispatched.empty());
        REQUIRE(backend.load_filament(9).result == AmsResult::SUCCESS);
        CHECK(backend.dispatched == std::vector<std::string>{"T9"});
    }
}

// The RFID fingerprint of each bay is read under the firmware's slot number, so
// a box leaving the head of the chain does not hand the next box's tags to its
// bays, where they would read as a spool swap and clear its overrides (#1464).
TEST_CASE("CFS fork: RFID fingerprints stay on their bays when a box drops",
          "[ams][cfs][fork][1464]") {
    auto run = [](const std::vector<int>& full, const std::vector<int>& dropped) {
        helix::test::StartedBackend<CfsRemapHelper> backend;
        backend.fire_status(make_cfs_notification(make_fork_box_with_boxes(full)));
        for (int bay = 4; bay < 8; ++bay) {
            helix::ams::FilamentSlotOverride kept;
            kept.spool_name = "Box 2 bay " + std::to_string(bay);
            CfsTestAccess::seed_override(backend, bay, kept);
        }
        std::map<int, std::optional<std::string>> baselines;
        for (int bay = 0; bay < 4 * full.back(); ++bay) {
            baselines[bay] = CfsTestAccess::last_rfid_uid(backend, bay);
        }
        REQUIRE(baselines[8].has_value());

        for (const auto& boxes : {dropped, full}) {
            backend.fire_status(make_cfs_notification(make_fork_box_with_boxes(boxes)));
            for (int bay = 4; bay < 8; ++bay) {
                INFO("bay " << bay);
                REQUIRE(CfsTestAccess::get_override(backend, bay).has_value());
                CHECK(CfsTestAccess::get_override(backend, bay)->spool_name ==
                      "Box 2 bay " + std::to_string(bay));
            }
            for (const auto& [bay, uid] : baselines) {
                INFO("bay " << bay);
                CHECK(CfsTestAccess::last_rfid_uid(backend, bay) == uid);
            }
        }
    };

    SECTION("{1,2,3} -> {2,3} -> {1,2,3}") {
        run({1, 2, 3}, {2, 3});
    }
    SECTION("{1,2,3,4} -> {1,3,4} -> {1,2,3,4}") {
        run({1, 2, 3, 4}, {1, 3, 4});
    }
}

// The lane store is keyed by the same index, so what the box last said about a
// bay stays on that bay's lane while its box is gone, and the shared views
// built from it neither show nor offer the missing box's bays as real ones.
TEST_CASE("CFS fork: lane records stay on their bays when a box drops",
          "[ams][cfs][fork][lane][1464]") {
    helix::test::RegisteredWired<CfsRemapHelper> backend;
    backend->fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 2, 3})));
    backend->fire_status(make_cfs_notification(make_fork_box_with_boxes({1, 3})));

    const auto box2a = helix::ams::lane_sources(backend.lane(4));
    REQUIRE(box2a.firmware.has_value());
    CHECK(box2a.firmware->product_name == std::optional<std::string>("Box 2 A"));
    CHECK(box2a.firmware->spoolman_id == std::optional<int>(104));
    const auto box3a = helix::ams::lane_sources(backend.lane(8));
    REQUIRE(box3a.firmware.has_value());
    CHECK(box3a.firmware->product_name == std::optional<std::string>("Box 3 A"));

    SECTION("the missing box's bays are not offered as lanes") {
        const auto slots = helix::AmsState::instance().collect_available_slots();
        CHECK(slots.size() == 8);
        for (const auto& slot : slots) {
            INFO("slot " << slot.slot_index);
            CHECK((slot.slot_index < 4 || slot.slot_index >= 8));
        }
    }

    SECTION("counts shown to the user are the bays that exist") {
        const auto info = backend->get_system_info();
        CHECK(info.total_slots == 12);
        CHECK(info.present_slot_count() == 8);
        CHECK(info.slot_absent(5));
        CHECK_FALSE(info.slot_exists(5));
        CHECK(info.slot_exists(9));
    }
}
