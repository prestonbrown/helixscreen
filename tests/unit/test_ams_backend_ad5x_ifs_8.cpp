// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 265 to 293 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs][lane_truth], [ad5x_ifs][1199], [ad5x][ad5x_ifs][lane_data][1632], [ad5x][routing],
// [ad5x_ifs][1710], and more

#include "ui_insert_notice.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/unique_temp_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::kAmsDefaultSlotColor;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::StepOperationType;

using helix::test::Ad5xHomingGuardBackend;
using helix::test::Ad5xHomingGuardFixture;
using helix::test::Ad5xIfsTmpCacheDir;
using helix::test::Ad5xIfsTmpJsonFile;
using helix::test::Ad5xRunoutFixture;
using helix::test::Ad5xRunoutOpBackend;
using helix::test::Ad5xRunoutOpFixture;
using helix::test::boot_from_lane_data;
using helix::test::drain_queue;
using helix::test::firmware_states_slot0;
using helix::test::FrameRig;
using helix::test::GcodeCapturingBackend;
using helix::test::identity_tool_map;
using helix::test::IfsLaneTruth;
using helix::test::json;
using helix::test::kZmodMaterials;
using helix::test::kZmodPalette;
using helix::test::lane_record_reread_lands;
using helix::test::less_waste_vars;
using helix::test::lesswaste_frame;
using helix::test::lesswaste_ready;
using helix::test::LiveAckAd5xIfsBackend;
using helix::test::macro_changes_state;
using helix::test::macro_error;
using helix::test::make_auto_mirror_override;
using helix::test::make_extruder;
using helix::test::make_head_sensor;
using helix::test::make_ifs_frame;
using helix::test::make_ifs_vars_macro;
using helix::test::make_locked_override;
using helix::test::make_motion_sensor;
using helix::test::make_port_sensor;
using helix::test::make_routing_backend;
using helix::test::make_save_variables;
using helix::test::make_zcolor;
using helix::test::make_zmod_head_sensor;
using helix::test::make_zmod_motion_sensor;
using helix::test::material_button;
using helix::test::outside_lane_record;
using helix::test::outside_tool_change_zcolor;
using helix::test::palette_button;
using helix::test::plugin_rig_frame;
using helix::test::QuietRunningAd5xIfs;
using helix::test::RegisteredAd5xIfs;
using helix::test::RunningAd5xIfs;
using helix::test::seat_slot0;
using helix::test::seat_unrecorded_zmod;
using helix::test::seed_standard_colors;
using helix::test::slot_row;
using helix::test::standard_variables;
using helix::test::TestableAd5xIfsBackend;
using helix::test::unload_routed_to_toolhead;
using helix::test::WiredAd5xIfs;
using helix::test::wrap_notification;
using helix::test::zmod_change_frame;
using json = nlohmann::json;

// The runout detail is assembled from several translatable pieces. Each piece
// is a whole sentence with the variable part as `{}`, so translators can reorder
// it; a subject glued on in C++ plus a fragment starting mid-clause ("is
// installed but ...", "matches.") cannot be reordered and stays untranslated.
//
// These assertions pin the assembled ENGLISH only. They deliberately do not
// claim to catch a relapse into concatenation: `who + " " + lv_tr("will switch
// ...")` produces a byte-identical English string, so no assertion on the
// output can tell the two apart (verified by mutating it back -- this test
// still passed). What differs is the CATALOG, and that is guarded elsewhere:
// re-splitting the sentence makes the whole-sentence key obsolete and the
// fragments new, which the translation coverage gate
// (qc_translation_coverage in scripts/quality-checks.sh) fails on.
TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS runout detail reads as whole sentences",
                 "[ams][ad5x_ifs][runout][i18n]") {
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    const auto contains = [](const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    };

    SECTION("stock zMod names Infinite Spool Mode as the subject") {
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/false, std::nullopt);
        const std::string detail = Ad5xIfsTestAccess::runout_detail(backend);

        // Subject adjacent to its verb is exactly what concatenation could not
        // guarantee once a translator moved either one.
        REQUIRE(contains(detail, "Infinite Spool Mode will switch to a slot"));
        REQUIRE(contains(detail, "No slot currently matches."));
    }

    SECTION("a matching backup slot is named in one sentence, not three pieces") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/false, std::nullopt);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend), "Slot 3 matches."));
    }

    SECTION("an unreadable plugin setting names the plugin as the subject") {
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/true, std::nullopt);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend),
                         "bambufy is installed, but its Endless Spool setting could not be read"));
    }

    SECTION("backup switching turned off names the plugin as the subject") {
        Ad5xIfsTestAccess::set_var_prefix(backend, "lessWaste");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/true, false);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend),
                         "lessWaste is installed but its Endless Spool setting is turned off"));
    }
}

// Endless spool: the shared abstraction over the plugin state

TEST_CASE("AD5X IFS endless spool capabilities", "[ams][ad5x_ifs][endless_spool][1250]") {
    using namespace helix::printer;

    SECTION("stock zMod: Available + FirmwareManaged, ANALOG_PRUTOK always-on") {
        // The matrix a source read of zmod_ifs.py:cmd_ANALOG_PRUTOK +
        // ad5x_display_off.cfg:39-44 establishes (corroborated on-device). Stock zMod
        // has its own switchover — "Infinite Spool Mode" — with no toggle. That is
        // Available/FirmwareManaged/provider="zmod", not RequiresPlugin/PluginMissing.
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.availability == EndlessSpoolAvailability::Available);
        REQUIRE(caps.availability != EndlessSpoolAvailability::RequiresPlugin);
        REQUIRE(caps.availability != EndlessSpoolAvailability::Unsupported);
        REQUIRE(caps.enabled == EndlessSpoolEnabled::On);
        REQUIRE(caps.enabled != EndlessSpoolEnabled::Off);
        REQUIRE(caps.editability == EndlessSpoolEditability::ReadOnly);
        REQUIRE(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
        REQUIRE(caps.provider == "zmod");
        REQUIRE(caps.available());
        REQUIRE_FALSE(caps.editable());
    }

    SECTION("plugin installed but variable_backup unreadable: Unknown, never Off") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.availability == EndlessSpoolAvailability::Available);
        REQUIRE(caps.enabled == EndlessSpoolEnabled::Unknown);
        REQUIRE(caps.enabled != EndlessSpoolEnabled::Off);
        REQUIRE(caps.provider == "lessWaste");
    }

    SECTION("variable_backup on/off maps to On/Off, and names the provider") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));
        REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
        REQUIRE(backend.get_endless_spool_capabilities().provider == "bambufy");

        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 0}}));
        REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);
    }

    SECTION("read-only even with a plugin, and writes are refused") {
        // `backup` is never written today and write_ifs_var() rides the _IFS_VARS
        // unknown-command latch, so an editable toggle would silently stop working.
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.editability == EndlessSpoolEditability::ReadOnly);
        REQUIRE(caps.restriction == EndlessSpoolRestriction::PluginReadOnly);

        auto result = backend.set_endless_spool_backup(0, 1);
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
        REQUIRE(result.user_msg ==
                endless_spool_restriction_text(EndlessSpoolRestriction::PluginReadOnly));
        REQUIRE_FALSE(backend.reset_endless_spool().success());
    }

    SECTION("no per-slot relation: the firmware computes the match at runout time") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        backend.fire_status(make_ifs_vars_macro(json{{"variable_backup", 1}}));
        seed_standard_colors(backend);

        REQUIRE(backend.get_endless_spool_config().empty());
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS overrides the eligibility rule with type+colour+presence",
                 "[ams][ad5x_ifs][endless_spool][eligibility][1250]") {
    // Lane 0: PLA / FF0000, present.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    SECTION("the generic material-compatibility default would say yes; AD5X says no") {
        // Same material, different colour. filament::are_materials_compatible()
        // (the AmsBackend default) accepts this; AD5X must not, because the
        // firmware's own switchover is colour-matched too.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "00FF00");

        REQUIRE(filament::materials_compatible("PLA", "PLA"));
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }

    SECTION("an absent port is not eligible even on an exact match") {
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }

    SECTION("exact type + colour + presence is eligible, case-insensitively") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "pla");
        Ad5xIfsTestAccess::set_color(backend, 2, "ff0000");
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Eligible);
        // And it is the same rule the firmware-match scan uses.
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == 2);
    }

    SECTION("self and out-of-range are never eligible") {
        using helix::printer::BackupEligibility;
        REQUIRE(backend.endless_spool_backup_eligibility(0, 0) == BackupEligibility::Incompatible);
        REQUIRE(backend.endless_spool_backup_eligibility(0, AmsBackendAd5xIfs::kDefaultPorts) ==
                BackupEligibility::Incompatible);
        REQUIRE(backend.endless_spool_backup_eligibility(-1, 0) == BackupEligibility::Incompatible);
    }

    SECTION("a filled grade is refused outright, never softened") {
        // The base rule would answer GradeDiffers here and let the user proceed.
        // AD5X must not: its firmware matches the type string exactly, so a
        // PLA-CF lane will simply never be selected for a PLA runout, and
        // offering it as a choosable option would promise a swap that cannot
        // happen. A backend's verdict is about what its firmware will do.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA-CF");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }
}

// Firmware default routing
//
// The AD5X is the one backend whose default map is neither identity nor a
// bounded-head table: lessWaste publishes an arbitrary 16-entry tool -> port
// map that the user can rearrange. Ports are 1-BASED and 5 is the unmapped
// sentinel, so the conversion to 0-based heads is the obvious place to get an
// off-by-one, which is why it is pinned with a deliberately non-identity map.

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X publishes its tool->port map as the firmware default routing",
                 "[ams][ad5x][routing]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto vars = standard_variables();
    // T0->port3, T1->port1, T2->port4, T3->port2, everything else unmapped (5).
    vars["less_waste_tools"] = json::array({3, 1, 4, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    backend.fire_status(make_save_variables(vars));

    auto routing = backend.firmware_default_routing();

    CHECK(routing.head(0) == 2);   // port 3 -> head 2
    CHECK(routing.head(1) == 0);   // port 1 -> head 0
    CHECK(routing.head(2) == 3);   // port 4 -> head 3
    CHECK(routing.head(3) == 1);   // port 2 -> head 1
    CHECK(routing.head(4) == -1);  // sentinel 5 = unmapped, NOT port 4
    CHECK(routing.head(15) == -1); // last table entry, still unmapped
    CHECK(routing.head(16) == -1); // past the table
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X without IFS vars falls back to lane-per-tool",
                 "[ams][ad5x][routing]") {
    // Native zMod never populates tool_map_ (has_ifs_vars_ stays false). Answering
    // an all-unmapped table there would strand every tool as AUTO, so the backend
    // must fall back rather than publish a table it does not have.

    auto routing = backend.firmware_default_routing();

    CHECK(routing.head(0) == 0);
    CHECK(routing.head(3) == 3);
}

TEST_CASE("clearing an AD5X port takes the linked spool's brand off it",
          "[ams][ad5x_ifs][lane][1672]") {
    // The clear the context menu performs: ui_ams_detail blanks the identity
    // fields, drops the Spoolman handles, and commits that as a user edit.
    //
    // apply_user_edit() paints the lane onto the port while it runs, and at
    // that moment the lane still carries the record for the spool being
    // unlinked - commit_user_edit() drops it only once apply_user_edit() has
    // returned. Without settle_port_locked() holding the caller's identity
    // across that paint, the outgoing spool's brand lands back on a port the
    // user just cleared, and no later paint can retire it: nothing states a
    // brand any more, and apply_resolved() leaves an unobserved field standing.
    Ad5xIfsTmpCacheDir tmp("clear_takes_the_brand");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SpoolInfo spool;
    spool.id = 1;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PETG";
    spool.material = "PETG";
    spool.color_hex = "FF00FF";
    helix::test::spool_states(backend, 0, spool);
    backend.repaint_slot_from_lane(0);

    const SlotInfo linked = backend.get_slot_info(0);
    REQUIRE(linked.brand == "Polymaker");
    REQUIRE(linked.spoolman_id == 1);

    // Field for field what ui_ams_detail.cpp#handle_menu_action does on
    // CLEAR_SPOOL. The default colour and the multi-colour list matter to the
    // shape of the record this leaves behind: declares_color() is what
    // classify_declaration() keys on, so a clear that left a declarable colour
    // standing would file the user's word where a real one files a memory.
    SlotInfo cleared = linked;
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.multi_color_hexes.clear();
    cleared.brand.clear();
    cleared.catalog_id.clear();
    cleared.product_name.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;
    helix::test::edit_slot_as_user(backend, 0, cleared);

    // The brand is the assertion this test exists for: nothing states one any
    // more, and apply_resolved() cannot retire a field no source observes, so
    // whatever stands here after the commit is what the port will show until
    // some source speaks. spoolman_id rides along as the binding half.
    const SlotInfo after = backend.get_slot_info(0);
    CHECK(after.brand.empty());
    CHECK(after.spoolman_id == 0);
}

TEST_CASE("an AD5X slot edit paints the edit's values, not the pre-edit ones",
          "[ams][ad5x_ifs][lane][1647]") {
    // The lane already carries a person's earlier edit when the next one lands,
    // so any paint that resolves before the new declaration is filed lays the
    // earlier values back over the new ones.
    Ad5xIfsTmpCacheDir tmp("edit_paints_edit");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo first = backend.get_slot_info(0);
    first.material = "PLA";
    first.color_rgb = 0xFF0000;
    first.color_name = "Old Red";
    first.brand = "OldBrand";
    helix::test::edit_slot_as_user(backend, 0, first);
    REQUIRE(backend.get_slot_info(0).brand == "OldBrand");
    REQUIRE(backend.get_slot_info(0).color_name == "Old Red");

    SlotInfo second = backend.get_slot_info(0);
    second.material = "PETG";
    second.color_rgb = 0x0000FF;
    second.color_name = "New Blue";
    second.brand = "NewBrand";
    helix::test::edit_slot_as_user(backend, 0, second);

    const SlotInfo painted = backend.get_slot_info(0);
    CHECK(painted.brand == "NewBrand");
    CHECK(painted.color_name == "New Blue");
    CHECK((painted.color_rgb & 0xFFFFFF) == 0x0000FF);
    CHECK(painted.material == "PETG");

    helix::ui::UpdateQueue::instance().drain();
}

// Own-write echo suppression (prestonbrown/helixscreen#1633) and the insert
// rule (prestonbrown/helixscreen#1710)

TEST_CASE("AD5X IFS echo guard refuses to arm without a presence boundary (#1633)",
          "[ams][ad5x_ifs][1633]") {
    // Native ZMOD reports no per-port presence and no IFS_STATUS Ports: no
    // signal on that machine can name the spool a write was made against, so
    // arming would withhold readings until the next edit. The guard must
    // refuse, which leaves the echo filing as VendorCache: the lesser harm,
    // and the design's explicit call for this hardware.
    Ad5xIfsTmpCacheDir tmp("ifs_echo_no_boundary");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));
    // The write must succeed: a failed dispatch drops the staging through the
    // matched abandon and the echo would file for the wrong reason. The local
    // write path pins the write, so only the arm refusal can explain filing.
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend,
                                                      (tmp.path / "Adventurer5M.json").string());

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");
    const auto lane = backend.lane_id(0);

    SlotInfo edit;
    edit.color_rgb = 0x7EC8E3;
    edit.material = "PLA";
    helix::test::edit_slot_as_user(backend, 0, edit);
    REQUIRE(std::filesystem::exists(tmp.path / "Adventurer5M.json"));
    helix::ui::UpdateQueue::instance().drain();

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#7EC8E3", "ffmType1": "PLA"}
    })");
    auto vendor = helix::ams::lane_sources(lane).firmware;
    REQUIRE(vendor.has_value());
    CHECK(vendor->color_rgb == 0x7EC8E3u);
    CHECK(vendor->material == "PLA");
}

TEST_CASE("AD5X IFS insert with no tag evidence offers Clear (#1710)", "[ams][ad5x_ifs][1710]") {
    // The IFS reads nothing off a spool: its colour and material are its own
    // memory of the last write. Every insert is therefore No evidence under
    // the slot spec's insert rule, so everything is kept and the user is
    // asked whether it is the same spool.
    Ad5xIfsTmpCacheDir tmp("ifs_insert_notice");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::reset_insert_offers_for_test();
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Boot with a spool seated: the first presence observation is the
    // session's baseline, never an insert edge, so nothing is asked.
    backend.fire_status(make_port_sensor(1, true));
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // Pulling the spool is not an insert either.
    backend.fire_status(make_port_sensor(1, false));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // A spool going in IS the edge, and it carries no evidence.
    backend.fire_status(make_port_sensor(1, true));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);

    // Re-stating presence with no removal in between is not a new edge.
    toasts.clear();
    backend.fire_status(make_port_sensor(1, true));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE("AD5X JSON-inferred presence is not an insert edge (#1710)", "[ams][ad5x_ifs][1710]") {
    // Pre-SILENT zmod has no silk-truth query, so Adventurer5M.json's colour
    // is the only presence source. The file latches identity across an eject
    // and our own edit writes it, so a rising edge read out of the file is as
    // likely to be the write coming back as a spool going in, and the insert
    // notice would land on the user's own fresh edit, where Clear wipes it.
    Ad5xIfsTmpCacheDir tmp("ifs_json_presence_no_notice");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend,
                                                      (tmp.path / "Adventurer5M.json").string());
    // No per-port sensors, no IFS_STATUS Ports, GET_ZCOLOR SILENT refused:
    // the file is the only presence source this session has.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::reset_insert_offers_for_test();
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Boot with the lane empty: the first file reading is the session's
    // baseline, never an edge.
    const char* empty_lane = R"({
        "FFMInfo": {"ffmColor1": "", "ffmType1": ""}
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, empty_lane);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // The user assigns an identity to the empty lane.
    SlotInfo edit;
    edit.color_rgb = 0x7EC8E3;
    edit.material = "PLA";
    helix::test::edit_slot_as_user(backend, 0, edit);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // A poll still carrying the pre-edit file reads the lane as ejected (empty
    // colour while IDLE); the next poll returns the write and presence rises
    // again. Neither file reading is a spool going in: no notice either time.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, empty_lane);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#7EC8E3", "ffmType1": "PLA"}
    })");
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // A sensor-backed edge still names the physical occupant and asks.
    backend.fire_status(make_port_sensor(1, false));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());
    backend.fire_status(make_port_sensor(1, true));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);

    helix::ui::set_test_toast_hook(nullptr);
}

// A Z-Mod change that aborts mid-chain (INSERT_PRUTOK_IFS raising on a jam)
// never reaches END_CHANGE_FILAMENT, so last_data.channel never returns to 99.
TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS gives up a Z-Mod change that never ends",
                 "[ams][ad5x_ifs][runout][1714]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    // The head runs out, the sensor pauses the job, and ANALOG_PRUTOK starts
    // the change to a matching spool, which then jams.
    set_print_state(helix::PrintJobState::PRINTING);
    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));
    set_print_state(helix::PrintJobState::PAUSED);
    backend.fire_status(zmod_change_frame(1));
    REQUIRE(backend.get_current_action() == AmsAction::SELECTING);
    Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(backend.get_system_info().is_busy());

    // Past the swap budget the action goes back to IDLE, not ERROR.
    Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(181));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK_FALSE(backend.get_system_info().is_busy());
    CHECK(backend.can_cancel_operation());

    // The runout prompt, the AD5X's only runout surface, can raise again.
    CHECK(Ad5xIfsTestAccess::evaluate_runout(backend));
    CHECK(backend.runout_active());

    // The user resumes; the fault clears.
    set_print_state(helix::PrintJobState::PRINTING);
    Ad5xIfsTestAccess::evaluate_runout(backend);
    REQUIRE(backend.get_current_action() == AmsAction::IDLE);

    // The stale channel is ignored until it reads idle; the next change counts.
    backend.fire_status(zmod_change_frame(1));
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    backend.fire_status(zmod_change_frame(99));
    backend.fire_status(zmod_change_frame(2));
    CHECK(backend.get_current_action() == AmsAction::SELECTING);
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS gives up a Z-Mod change when the job ends",
                 "[ams][ad5x_ifs][1714]") {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    auto& backend = *backend_reg;

    set_print_state(helix::PrintJobState::PRINTING);
    backend.fire_status(zmod_change_frame(1));
    REQUIRE(backend.get_current_action() == AmsAction::SELECTING);

    set_print_state(helix::PrintJobState::CANCELLED);
    Ad5xIfsTestAccess::run_action_timeout(backend);
    CHECK(backend.get_current_action() == AmsAction::IDLE);
}

TEST_CASE("AD5X IFS a spool linked over an outside tool's lane keeps its name through a firmware "
          "change",
          "[ams][ad5x][ad5x_ifs][lane_data][1632]") {
    // Linking replaces the outside statement, so the firmware change that
    // follows has no statement to release and the spool's name stands.
    Ad5xIfsTmpCacheDir tmp("outside_then_linked");
    MockPrinter mock_printer;
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    outside_lane_record(0, "#FF0000", "PLA", "Red PLA", 0));

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&mock_printer.api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    boot_from_lane_data(backend, mock_printer, tmp.path);
    firmware_states_slot0(backend, "FF0000", "PLA");

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite PLA";
    spool.material = "PLA";
    spool.color_hex = "FF0000";
    SlotInfo link = backend.get_slot_info(0);
    link.spoolman_id = 42;
    helix::test::edit_slot_as_user(backend, 0, link);
    helix::test::spool_states(backend, 0, spool);
    backend.repaint_slot_from_lane(0);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(backend.get_slot_info(0).spoolman_id == 42);
    {
        const auto linked = helix::ams::lane_sources(backend_reg.lane(0));
        REQUIRE(linked.spoolman.has_value());
        REQUIRE((!linked.pending.has_value() || (!linked.pending->color_rgb.has_value() &&
                                                 !linked.pending->material.has_value())));
    }

    outside_tool_change_zcolor(backend);

    const auto lane = helix::ams::lane_sources(backend_reg.lane(0));
    REQUIRE(lane.spoolman.has_value());
    CHECK(lane.spoolman->spool_name == std::string("PolyLite PLA"));
    CHECK(backend.get_slot_info(0).spoolman_id == 42);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("AD5X IFS boot reads the binding an outside tool left in lane_data",
          "[ams][ad5x][ad5x_ifs][lane_data][1632]") {
    Ad5xIfsTmpCacheDir tmp("outside_binding_at_boot");
    MockPrinter mock_printer;

    SECTION("rebound to another spool") {
        mock_printer.client.mock_db_set("lane_data", "lane1",
                                        outside_lane_record(0, "#0055FF", "PETG", "Blue PETG", 77));
        helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&mock_printer.api, nullptr);
        AmsBackendAd5xIfs& backend = *backend_reg;
        boot_from_lane_data(backend, mock_printer, tmp.path);
        firmware_states_slot0(backend, "0055FF", "PETG");
        const SlotInfo info = backend.get_slot_info(0);
        CHECK(info.spoolman_id == 77);
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
    }
    SECTION("unbound") {
        mock_printer.client.mock_db_set("lane_data", "lane1",
                                        outside_lane_record(0, "#0055FF", "PETG", "Blue PETG", 0));
        helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&mock_printer.api, nullptr);
        AmsBackendAd5xIfs& backend = *backend_reg;
        boot_from_lane_data(backend, mock_printer, tmp.path);
        firmware_states_slot0(backend, "0055FF", "PETG");
        const SlotInfo info = backend.get_slot_info(0);
        CHECK(info.spoolman_id == 0);
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
        CHECK(info.spool_name == "Blue PETG");
    }
    helix::ui::UpdateQueue::instance().drain();
}

// The parse contract the authority flag depends on

TEST_CASE("AD5X IFS stamps LOADED on the seated lane", "[ams][ad5x_ifs][1199]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> ifs;
    lesswaste_ready(ifs);
    seat_slot0(ifs);

    REQUIRE(ifs.get_system_info().current_slot == 0);
    REQUIRE(ifs.is_filament_loaded());

    SECTION("the seated lane is LOADED and every other lane is not") {
        CHECK(ifs.get_slot_info(0).status == SlotStatus::LOADED);
        CHECK(ifs.slot_is_actively_loaded(0));

        // Slot 2 has a spool but is not at the head — AVAILABLE, never LOADED.
        CHECK(ifs.get_slot_info(2).status == SlotStatus::AVAILABLE);
        CHECK_FALSE(ifs.slot_is_actively_loaded(2));
        // Slot 1 has no spool at all.
        CHECK(ifs.get_slot_info(1).status == SlotStatus::EMPTY);
        CHECK_FALSE(ifs.slot_is_actively_loaded(1));
        CHECK_FALSE(ifs.slot_is_actively_loaded(3));
    }

    SECTION("an empty head clears it even while the lane keeps its spool") {
        ifs.fire_status(make_head_sensor(false));

        REQUIRE_FALSE(ifs.is_filament_loaded());
        CHECK(ifs.get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK_FALSE(ifs.slot_is_actively_loaded(0));
    }

    SECTION("out-of-range slots are false, not a crash") {
        CHECK_FALSE(ifs.slot_is_actively_loaded(-1));
        CHECK_FALSE(ifs.slot_is_actively_loaded(99));
    }
}

// The runout the per-slot rule must survive (#995)

TEST_CASE("AD5X IFS keeps the seated lane loaded through a port-sensor runout",
          "[ams][ad5x_ifs][1199][995]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> ifs;
    lesswaste_ready(ifs);
    seat_slot0(ifs);
    REQUIRE(ifs.get_slot_info(0).status == SlotStatus::LOADED);

    // Runout on hardware WITH per-port sensors: the spool leaves the lane
    // sensor, but the filament it already fed is still at the extruder, so the
    // head switch stays tripped and the firmware still names slot 0 active.
    ifs.fire_status(make_port_sensor(1, false));

    REQUIRE(ifs.get_system_info().current_slot == 0);
    REQUIRE(ifs.is_filament_loaded()); // the aggregate pair still says loaded

    // The head sensor alone decides: reading the port sensor here would report
    // the lane unloaded and blank the highlight mid-recovery.
    CHECK(ifs.get_slot_info(0).status == SlotStatus::LOADED);
    CHECK(ifs.slot_is_actively_loaded(0));

    // The #995 unload gate and the highlight agree.
    CHECK(ifs.can_unload_from_toolhead(0));

    // The runout does not spill onto any other lane.
    CHECK_FALSE(ifs.slot_is_actively_loaded(1));
    CHECK_FALSE(ifs.slot_is_actively_loaded(2));
}

// Hardware without per-port sensors is unaffected

TEST_CASE("AD5X IFS marks the seated lane loaded with no port sensors at all",
          "[ams][ad5x_ifs][1199]") {
    // The ZMOD-only variant publishes no _ifs_port_sensor_* objects, so
    // port_presence_ stays false for every lane; the rule keys on the head
    // sensor, so the seated lane is still LOADED.
    helix::test::StartedBackend<AmsBackendAd5xIfs> ifs;
    lesswaste_ready(ifs);

    json frame = lesswaste_frame(less_waste_vars(1)); // T1 -> port 2 -> slot 1
    frame.update(make_head_sensor(true));
    ifs.fire_status(frame);

    REQUIRE(ifs.get_system_info().current_slot == 1);
    CHECK(ifs.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(ifs.slot_is_actively_loaded(1));
    CHECK(ifs.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK_FALSE(ifs.slot_is_actively_loaded(0));
}

// A toolchange moves the stamp with the firmware's active-lane pointer

TEST_CASE("AD5X IFS moves the LOADED stamp on a toolchange", "[ams][ad5x_ifs][1199]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> ifs;
    lesswaste_ready(ifs);
    seat_slot0(ifs);
    REQUIRE(ifs.slot_is_actively_loaded(0));

    // T2 becomes active (-> port 3 -> slot 2); slot 2's spool is already there.
    ifs.fire_status(lesswaste_frame(less_waste_vars(2)));

    REQUIRE(ifs.get_system_info().current_slot == 2);
    CHECK(ifs.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(ifs.slot_is_actively_loaded(2));
    // The lane we left must not keep a stale LOADED — two highlighted lanes is
    // exactly the failure the per-slot rule cannot self-correct.
    CHECK(ifs.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(ifs.slot_is_actively_loaded(0));
}

// Purge over the bin: zMod's stock _G28 + _GOTO_TRASH ahead of a purge

TEST_CASE("AD5X IFS purge-bin gcode by macro set and job state",
          "[ams][ad5x_ifs][ad5x_bin_purge]") {
    struct Row {
        std::vector<std::string> macros;
        const char* idle;   // preamble with the machine free
        const char* held;   // preamble while a job holds it: never _G28
        const char* runout; // the runout recovery Purge
    };
    const char* bare_move = "M83\nG1 E50 F600";
    const Row rows[] = {
        {{"_G28", "_GOTO_TRASH", "PURGE_FILAMENT"},
         "_G28\n_GOTO_TRASH",
         "_GOTO_TRASH",
         "_GOTO_TRASH\nPURGE_FILAMENT"},
        {{"_G28", "_GOTO_TRASH"}, "_G28\n_GOTO_TRASH", "_GOTO_TRASH", bare_move},
        {{"_GOTO_TRASH", "PURGE_FILAMENT"}, "", "", bare_move},
        {{"_G28", "PURGE_FILAMENT"}, "", "", bare_move},
        {{"IFS_PURGE", "IFS_LOAD"}, "", "", bare_move}, // Forge-X
    };
    for (const auto& row : rows) {
        json objects = json::array({"save_variables"});
        for (const auto& m : row.macros) {
            objects.push_back("gcode_macro " + m);
        }
        helix::PrinterDiscovery hw;
        hw.parse_objects(objects);
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        const helix::IPrintRouting& base = backend;
        CHECK(base.purge_preamble_gcode(false).empty()); // nothing until discovery
        backend.set_discovery(hw);
        INFO(objects.dump());
        CHECK(base.purge_preamble_gcode(false) == row.idle);
        CHECK(base.purge_preamble_gcode(true) == row.held);
        CHECK(AmsBackendAd5xIfs::runout_purge_gcode(AmsBackendAd5xIfs::bin_purge_macros(hw)) ==
              row.runout);
    }
}

TEST_CASE("AD5X IFS writes the plugin's table before Z-Mod's AFC lane map",
          "[ams][ad5x_ifs][tool_mapping][2033]") {
    MockPrinter mock_printer;
    GcodeCapturingBackend backend(&mock_printer.api, nullptr);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"zmod", "zmod_color", "zmod_ifs", "AFC", "AFC_IFS IFS", "AFC_lane lane3"}));
    backend.set_discovery(hw);
    // The lessWaste plugin's macro confirmed, then its table.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.feed(make_save_variables(standard_variables()));
    backend.captured_gcodes.clear();

    // The table that routes the print goes first, so a refused SET_MAP (it
    // only mirrors the pick for the AFC panels) can never cost it its write.
    REQUIRE(backend.set_tool_mapping(0, 2).success());
    CAPTURE(backend.captured_gcodes);
    REQUIRE(backend.captured_gcodes.size() == 2);
    CHECK(backend.captured_gcodes[0].rfind("_IFS_VARS tools=", 0) == 0);
    CHECK(backend.captured_gcodes[1] == "SET_MAP LANE=lane3 MAP=T0");
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("AD5X IFS update_slot_weight preserves identity and does not write Adventurer5M.json",
          "[ams][ad5x_ifs][filament_slot_override][981]") {
    Ad5xIfsTmpCacheDir tmp("weight_only_preserves_identity");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    seed_standard_colors(backend); // firmware truth: slot 0 = PLA / #FF0000
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // A prior in-app edit: slot 0 holds PETG / #ABCDEF with a weight.
    helix::ams::FilamentSlotOverride locked;
    locked.material = "PETG";
    locked.color_rgb = 0xABCDEF;
    locked.color_set = true;
    locked.remaining_weight_g = 100.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, locked);

    // A sentinel Adventurer5M.json at the resolved local path. update_slot_weight
    // must leave it byte-for-byte untouched (apply_user_edit would rewrite it).
    const std::string json_path = (tmp.path / "Adventurer5M.json").string();
    const std::string sentinel = "{\"FFMInfo\":{\"ffmType1\":\"PETG\",\"sentinel\":true}}";
    { std::ofstream(json_path) << sentinel; }
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);

    backend.update_slot_weight(0, /*remaining=*/42.0f, /*total=*/-1.0f, /*persist=*/true);

    // Weight updated; identity untouched.
    auto ovr = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(ovr.has_value());
    CHECK(ovr->remaining_weight_g == 42.0f);
    CHECK(ovr->material == "PETG");
    CHECK(ovr->color_rgb == 0xABCDEFu);

    // Adventurer5M.json was NOT rewritten (#981).
    std::ifstream check(json_path);
    std::string after((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
    CHECK(after == sentinel);

    // get_slot_info weight reflects the in-memory update. (Material identity is
    // asserted on the override above; get_slot_info doesn't re-run
    // apply_overrides here, so it still shows firmware-level state — not what
    // this test is about.)
    CHECK(backend.get_slot_info(0).remaining_weight_g == 42.0f);
}

TEST_CASE("an AD5X linked lane's record keeps the firmware's material; firmware gets a valid one",
          "[ams][ad5x_ifs][filament_slot_override][1653]") {
    Ad5xIfsTmpCacheDir tmp("linked_material_spelling");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::inject_override_store(
        backend, std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs"));

    const std::string json_path = (tmp.path / "Adventurer5M.json").string();
    { std::ofstream(json_path) << R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})"; }
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);

    helix::ams::FilamentSlotOverride linked;
    linked.spoolman_id = 42;
    linked.brand = "Polymaker";
    linked.material = "SILK";
    Ad5xIfsTestAccess::seed_override(backend, 0, linked);

    // Spoolman spells the linked spool's material in a way the AD5X
    // whitelist does not.
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite Silk";
    spool.material = "Silk PLA";
    spool.color_hex = "FF0000";
    helix::test::spool_states(backend, 0, spool);
    backend.repaint_slot_from_lane(0);

    // A colour-only edit that keeps the spool.
    SlotInfo edit = backend.get_slot_info(0);
    INFO("shown material " << edit.material);
    edit.color_rgb = 0x1E5AA8;
    helix::test::edit_slot_as_user(backend, 0, edit);

    const auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->material == "SILK");
    std::ifstream written_file(json_path);
    const nlohmann::json written = nlohmann::json::parse(written_file, nullptr, false);
    REQUIRE(written.is_object());
    CHECK(written["FFMInfo"]["ffmType1"] == "SILK");
}

TEST_CASE("AD5X IFS debounce workers end with the backend", "[ams][ad5x][ad5x_ifs][threading]") {
    // Each debounce holds a fast-lane worker for its window. A backend torn
    // down mid-window has nothing left for that worker to do, so the worker
    // has to give the lane back at once rather than sleep the window out.
    using Clock = std::chrono::steady_clock;
    auto& fast = helix::http::HttpExecutor::fast();
    const auto settle_deadline = Clock::now() + std::chrono::seconds(3);
    while (fast.inflight() != 0 && Clock::now() < settle_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fast.inflight() == 0);

    // Each bound sits well under its window, so a worker that sleeps the
    // window out fails it while a loaded shard still has room to clear one
    // that wakes on teardown.
    int bound_ms = 0;
    MockPrinter mock_printer;
    {
        helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&mock_printer.api, nullptr);
        AmsBackendAd5xIfs& backend = *backend_reg;
        SECTION("Adventurer5M.json re-read") {
            bound_ms = 1000; // of a 2s window
            Ad5xIfsTestAccess::schedule_json_reread(backend);
        }
        SECTION("GET_ZCOLOR query") {
            bound_ms = 400; // of a 500ms window
            Ad5xIfsTestAccess::schedule_zcolor_query(backend);
        }
        SECTION("GET_ZCOLOR response collection") {
            bound_ms = 400; // of a 500ms window
            // The window starts in execute_gcode's success callback, which
            // may arrive on another thread after the call returns.
            Ad5xIfsTestAccess::query_zcolor_silent(backend);
            const auto submitted_deadline = Clock::now() + std::chrono::seconds(2);
            while (fast.inflight() == 0 && Clock::now() < submitted_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        REQUIRE(fast.inflight() > 0);
    }

    const auto destroyed = Clock::now();
    const auto deadline = destroyed + std::chrono::seconds(3);
    while (fast.inflight() != 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - destroyed);
    CHECK(fast.inflight() == 0);
    CHECK(waited.count() < bound_ms);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("AD5X IFS lane truth: an edit's colour and type go to the firmware, the rest to "
          "lane_data",
          "[ams][ad5x_ifs][lane_truth]") {
    IfsLaneTruth h("lane_truth_edit");

    SlotInfo edit = h.backend.get_slot_info(0);
    edit.brand = "Polymaker";
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00;
    edit.remaining_weight_g = 500.0f;
    edit.nozzle_temp_min = 240;
    edit.bed_temp = 70;
    helix::test::edit_slot_as_user(h.backend, 0, edit);

    const nlohmann::json written = h.file();
    REQUIRE(written.is_object());
    CHECK(written["FFMInfo"]["ffmColor1"] == "#00FF00");
    CHECK(written["FFMInfo"]["ffmType1"] == "PETG");

    // The record never states a colour the firmware has not taken.
    nlohmann::json rec = h.record();
    REQUIRE(rec.is_object());
    CHECK(rec["vendor_name"] == "Polymaker");
    CHECK(rec["color"].is_null());
    CHECK(rec["material"].is_null());
    CHECK(rec["nozzle_temp"] == 240);
    CHECK(rec["bed_temp"] == 70);

    // Until the firmware echoes it, the lane shows the edit.
    CHECK(h.pending());
    CHECK(h.backend.get_slot_info(0).color_rgb == 0x00FF00u);

    h.firmware("#00FF00", "PETG");
    IfsLaneTruth::drain();
    CHECK_FALSE(h.pending());
    rec = h.record();
    CHECK(rec["color"] == "#00FF00");
    CHECK(rec["material"] == "PETG");
    CHECK(rec["vendor_name"] == "Polymaker");
    const SlotInfo shown = h.backend.get_slot_info(0);
    CHECK(shown.color_rgb == 0x00FF00u);
    CHECK(shown.material == "PETG");
    CHECK(shown.brand == "Polymaker");
}

TEST_CASE("AD5X IFS lane truth: a refused firmware write leaves the record without the colour",
          "[ams][ad5x_ifs][lane_truth]") {
    IfsLaneTruth h("lane_truth_refused");
    Ad5xIfsTestAccess::set_local_adventurer_json_path(h.backend,
                                                      (h.tmp.path / "no/such/dir/A.json").string());

    SlotInfo edit = h.backend.get_slot_info(0);
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00;
    helix::test::edit_slot_as_user(h.backend, 0, edit);

    CHECK_FALSE(h.pending());
    const nlohmann::json rec = h.record();
    if (rec.is_object()) {
        CHECK(rec["color"].is_null());
        CHECK(rec["material"].is_null());
    }
    const SlotInfo shown = h.backend.get_slot_info(0);
    CHECK(shown.color_rgb == 0xFF0000u);
    CHECK(shown.material == "PLA");
}

TEST_CASE("AD5X IFS lane truth: a CHANGE_ZCOLOR from the LCD beats the record",
          "[ams][ad5x_ifs][lane_truth]") {
    IfsLaneTruth h("lane_truth_lcd");
    helix::ams::FilamentSlotOverride stored;
    stored.brand = "Polymaker";
    stored.material = "PLA";
    stored.color_rgb = 0xFF0000;
    stored.color_set = true;
    Ad5xIfsTestAccess::seed_override(h.backend, 0, stored);

    Ad5xIfsTestAccess::on_gcode_response_line(h.backend,
                                              "CHANGE_ZCOLOR SLOT=1 HEX=0000FF TYPE=ABS");

    const SlotInfo shown = h.backend.get_slot_info(0);
    CHECK(shown.color_rgb == 0x0000FFu);
    CHECK(shown.material == "ABS");
    CHECK(shown.brand == "Polymaker");
    const nlohmann::json rec = h.record();
    REQUIRE(rec.is_object());
    CHECK(rec["color"] == "#0000FF");
    CHECK(rec["material"] == "ABS");
    CHECK(rec["vendor_name"] == "Polymaker");
}

TEST_CASE("AD5X IFS lane truth: a type the firmware spells its own way settles on its echo",
          "[ams][ad5x_ifs][lane_truth]") {
    IfsLaneTruth h("lane_truth_spelling");

    SlotInfo edit = h.backend.get_slot_info(0);
    edit.material = "Silk PLA";
    helix::test::edit_slot_as_user(h.backend, 0, edit);

    const auto pending = helix::ams::lane_sources(h.backend.lane_id(0)).pending;
    REQUIRE(pending.has_value());
    CHECK(pending->material == std::optional<std::string>("SILK"));

    h.firmware("#FF0000", "SILK");
    CHECK_FALSE(h.pending());
    CHECK(h.backend.get_slot_info(0).material == "SILK");
}

TEST_CASE("AD5X IFS lane truth: a firmware change before the first record read keeps the record",
          "[ams][ad5x_ifs][lane_truth]") {
    const nlohmann::json stored = {
        {"lane", "0"}, {"vendor_name", "Polymaker"}, {"material", "PLA"}, {"color", "#FF0000"}};
    IfsLaneTruth h("lane_truth_unread", stored, /*settle=*/false);

    Ad5xIfsTestAccess::set_has_ifs_vars(h.backend, true);

    // The read is still out: a firmware change must not save a bare record.
    h.firmware("#0000FF", "PLA");
    CHECK(h.record()["vendor_name"] == "Polymaker");

    // Once the read lands, the record takes the change it missed, and the
    // plugin's save_variables follow at the next file parse.
    IfsLaneTruth::drain();
    nlohmann::json rec = h.record();
    CHECK(rec["vendor_name"] == "Polymaker");
    CHECK(rec["color"] == "#0000FF");
    h.printer.client.clear_gcode_script_history();
    h.firmware("#0000FF", "PLA");
    bool pushed = false;
    for (const auto& g : h.printer.client.gcode_script_history()) {
        pushed = pushed || g.rfind("_IFS_VARS colors=", 0) == 0;
    }
    CHECK(pushed);

    h.firmware("#00FF00", "PLA");
    rec = h.record();
    CHECK(rec["vendor_name"] == "Polymaker");
    CHECK(rec["color"] == "#00FF00");
    IfsLaneTruth::drain();
}

TEST_CASE("AD5X IFS with AFC off takes the print's map but sends no SET_MAP",
          "[ams][ad5x_ifs][tool_mapping][2033]") {
    MockPrinter mock_printer;
    GcodeCapturingBackend backend(&mock_printer.api, nullptr);
    helix::PrinterDiscovery hw;
    // Z-Mod's Moonraker says its print start reads `tools`; AFC=0 publishes no
    // AFC objects and defines no SET_MAP.
    hw.set_server_takes_print_start_tools(true);
    hw.parse_objects(nlohmann::json::array({"zmod", "zmod_color", "zmod_ifs"}));
    REQUIRE_FALSE(hw.has_zmod_afc());
    backend.set_discovery(hw);

    CHECK(backend.remap_ready());
    CHECK(backend.print_start_params({{0, 2}, {1, 0}}) ==
          helix::PrintStartParams{{"tools", "3,1"}});

    REQUIRE(backend.set_tool_mapping(0, 2).success());
    CHECK(backend.captured_gcodes.empty());
}
