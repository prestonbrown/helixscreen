// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_backend_traits.cpp
 * @brief Each AmsBackendMock persona answers the constant capability questions
 *        exactly as the real backend it stands in for.
 *
 * The mock is what every `--test` run drives. Its personas take their traits
 * from the real backends' kTraits, so a capability a real backend changes
 * reaches the mock with no second edit. These cases fail the moment the two
 * disagree on any field.
 */

#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_openams.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "test_helpers/qidi_box_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The mock's traits with the fields it answers from itself copied from @p real,
/// so the comparison covers every other field. Environment data is faked for
/// every persona (HELIX_MOCK_AMS_ENV); the mock has no firmware store, so spool
/// persistence is always ToolState's job; consumption tracking is a test hook.
BackendTraits comparable(const AmsBackendMock& mock, const BackendTraits& real) {
    BackendTraits t = mock.traits();
    t.has_environment_sensors = real.has_environment_sensors;
    t.has_firmware_spool_persistence = real.has_firmware_spool_persistence;
    t.tracks_consumption_natively = real.tracks_consumption_natively;
    return t;
}

} // namespace

TEST_CASE("Mock personas answer their real backend's traits", "[ams][mock][traits]") {
    SECTION("Happy Hare (default persona)") {
        AmsBackendMock mock(4);
        CHECK(comparable(mock, AmsBackendHappyHare::kTraits) == AmsBackendHappyHare::kTraits);
    }
    SECTION("AFC") {
        AmsBackendMock mock(4);
        mock.set_afc_mode(true);
        CHECK(comparable(mock, AmsBackendAfc::kTraits) == AmsBackendAfc::kTraits);
    }
    SECTION("Tool changer") {
        AmsBackendMock mock(4);
        mock.set_tool_changer_mode(true);
        CHECK(comparable(mock, AmsBackendToolChanger::kTraits) == AmsBackendToolChanger::kTraits);
    }
    SECTION("AD5X IFS") {
        AmsBackendMock mock(4);
        mock.set_ifs_mode(true);
        CHECK(comparable(mock, AmsBackendAd5xIfs::kTraits) == AmsBackendAd5xIfs::kTraits);
    }
    SECTION("Snapmaker U1") {
        AmsBackendMock mock(4);
        mock.set_snapmaker_mode(true);
        CHECK(comparable(mock, AmsBackendSnapmaker::kTraits) == AmsBackendSnapmaker::kTraits);
    }
}

TEST_CASE("Mock personas answer whether the carriage can be empty as their real backend does",
          "[ams][mock][traits][tool_switcher][1784]") {
    SECTION("Snapmaker U1") {
        AmsBackendMock mock(4);
        mock.set_snapmaker_mode(true);
        AmsBackendSnapmaker real(nullptr, nullptr);
        CHECK(mock.carriage_can_be_empty() == real.carriage_can_be_empty());
    }
    SECTION("Tool changer") {
        AmsBackendMock mock(4);
        mock.set_tool_changer_mode(true);
        AmsBackendToolChanger real(nullptr, nullptr);
        CHECK(mock.carriage_can_be_empty() == real.carriage_can_be_empty());
    }
    SECTION("AFC") {
        AmsBackendMock mock(4);
        mock.set_afc_mode(true);
        AmsBackendAfc real(nullptr, nullptr);
        CHECK(mock.carriage_can_be_empty() == real.carriage_can_be_empty());
    }
}

TEST_CASE("Mock environment sensors follow the configured environment mode",
          "[ams][mock][traits]") {
    AmsBackendMock mock(4);
    mock.set_environment_mode("passive");
    CHECK(mock.has_environment_sensors());
    mock.set_environment_mode("none");
    CHECK_FALSE(mock.has_environment_sensors());
}

TEST_CASE("A backend that sets no traits answers the base defaults", "[ams][traits]") {
    const BackendTraits t{};
    CHECK(t.has_physical_tray);
    CHECK(t.slot_status_tracks_filament);
    CHECK(t.allows_implicit_chaining);
    CHECK_FALSE(t.supports_auto_heat_on_load);
    CHECK_FALSE(t.supports_auto_heat_on_unload);
    CHECK_FALSE(t.holds_optimistic_action);
    CHECK_FALSE(t.unloads_stand_alone);
    CHECK_FALSE(t.has_per_slot_loaded_authority);
}

TEST_CASE("Real kTraits keep the answers the UI branches on", "[ams][traits]") {
    // The persona cases above compare the mock with these structs, so they
    // cannot catch a wrong value here. These pin the ones a lane-per-tool
    // system must keep.
    CHECK(AmsBackendHappyHare::kTraits.has_physical_tray);
    CHECK_FALSE(AmsBackendHappyHare::kTraits.recovers_filament_on_resume);
    CHECK_FALSE(AmsBackendHappyHare::kTraits.should_suppress_idle_runout_modal);
    CHECK(AmsBackendHappyHare::kTraits.supports_reset);
    CHECK_FALSE(AmsBackendHappyHare::kTraits.allows_implicit_chaining);
    CHECK_FALSE(AmsBackendSnapmaker::kTraits.has_physical_tray);
    // Reset is offered only where it clears something the backend keeps.
    CHECK_FALSE(AmsBackendSnapmaker::kTraits.supports_reset);
    CHECK_FALSE(AmsBackendToolChanger::kTraits.supports_reset);
    // Only a backend silent until its firmware starts holds the UI's action.
    CHECK(AmsBackendHappyHare::kTraits.holds_optimistic_action);
    CHECK_FALSE(AmsBackendAfc::kTraits.holds_optimistic_action);
    CHECK(AmsBackendSnapmaker::kTraits.holds_optimistic_action);
    // A tool changer's dock is the first half of a swap; a U1 unload is whole.
    CHECK(AmsBackendSnapmaker::kTraits.unloads_stand_alone);
    CHECK_FALSE(AmsBackendToolChanger::kTraits.unloads_stand_alone);
    CHECK_FALSE(AmsBackendHappyHare::kTraits.unloads_stand_alone);
    CHECK_FALSE(AmsBackendAfc::kTraits.unloads_stand_alone);
    // A tool changer's spools hang outside the machine: no case to draw.
    CHECK_FALSE(AmsBackendToolChanger::kTraits.has_physical_tray);
    // Console-equipped firmwares send one command per user action.
    CHECK_FALSE(AmsBackendAfc::kTraits.allows_implicit_chaining);
    // Backends whose firmware names the seated lane per slot; Happy Hare and the
    // tool changer keep the aggregate (current_slot, filament_loaded) rule.
    CHECK(AmsBackendQidi::kTraits.has_per_slot_loaded_authority);
    CHECK(helix::printer::AmsBackendCfs::kTraits.has_per_slot_loaded_authority);
    CHECK(AmsBackendAce::kTraits.has_per_slot_loaded_authority);
    CHECK(AmsBackendAd5xIfs::kTraits.has_per_slot_loaded_authority);
    CHECK_FALSE(AmsBackendHappyHare::kTraits.has_per_slot_loaded_authority);
    // Firmware that stores the spool id per lane; elsewhere ToolState persists
    // the assignment in the Moonraker DB.
    CHECK(AmsBackendHappyHare::kTraits.has_firmware_spool_persistence);
    CHECK_FALSE(AmsBackendToolChanger::kTraits.has_firmware_spool_persistence);
    CHECK_FALSE(AmsBackendAd5xIfs::kTraits.has_firmware_spool_persistence);
    CHECK(AmsBackendAfc::kTraits.printer_reports_spool_ids);
    CHECK(AmsBackendHappyHare::kTraits.printer_reports_spool_ids);
    CHECK_FALSE(BackendTraits{}.printer_reports_spool_ids);
}

TEST_CASE("A U1 owns a spool per tool and has no bypass", "[ams][traits][spoolman]") {
    // The Spoolman active-spool sync assigns per tool only where this holds.
    AmsBackendSnapmaker u1(nullptr, nullptr);
    CHECK(u1.supports_per_tool_spool_assignment());
    CHECK_FALSE(u1.is_bypass_active());
}

TEST_CASE("The mock leaves spool persistence to ToolState in every persona",
          "[ams][mock][traits]") {
    AmsBackendMock mock(4);
    mock.set_afc_mode(true);
    REQUIRE(AmsBackendAfc::kTraits.has_firmware_spool_persistence);
    CHECK_FALSE(mock.has_firmware_spool_persistence());
}

TEST_CASE("Mock AFC load leaves only the new lane loaded", "[ams][mock][traits]") {
    AmsBackendMock mock(4);
    mock.set_afc_mode(true);
    mock.set_operation_delay(0);
    REQUIRE(mock.start());
    REQUIRE(mock.has_per_slot_loaded_authority());

    const int prior = mock.get_current_slot();
    REQUIRE(prior >= 0);
    REQUIRE(mock.slot_is_actively_loaded(prior));

    int target = -1;
    for (int i = 0; i < mock.get_system_info().total_slots; ++i) {
        if (i != prior && mock.get_slot_info(i).status == SlotStatus::AVAILABLE) {
            target = i;
            break;
        }
    }
    REQUIRE(target >= 0);

    REQUIRE(mock.load_filament(target).success());
    mock.wait_for_operation_thread();

    CHECK(mock.slot_is_actively_loaded(target));
    CHECK_FALSE(mock.slot_is_actively_loaded(prior));
    mock.stop();
}

// ============================================================================
// Runout ownership
// ============================================================================

namespace {

helix::ClassifyContext paused_job() {
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    ctx.is_printing = true;
    return ctx;
}

} // namespace

TEST_CASE("runout ownership traits match the table", "[ams][traits][runout_traits]") {
    struct Row {
        const char* name;
        BackendTraits traits;
        bool raises_runout_fault;
        bool raises_runout_fault_during_job;
        bool unloads_to_hub_between_prints;
    };
    const Row rows[] = {
        {"Happy Hare", AmsBackendHappyHare::kTraits, true, true, false},
        {"AFC", AmsBackendAfc::kTraits, true, true, false},
        {"AD5X IFS", AmsBackendAd5xIfs::kTraits, true, false, true},
        {"CFS", helix::printer::AmsBackendCfs::kTraits, true, true, false},
        {"ACE", AmsBackendAce::kTraits, false, false, false},
        {"QIDI Box", AmsBackendQidi::kTraits, false, false, false},
        {"OpenAMS", AmsBackendOpenAms::kTraits, false, false, false},
        {"Tool changer", AmsBackendToolChanger::kTraits, false, false, false},
        {"Snapmaker", AmsBackendSnapmaker::kTraits, false, false, false},
    };
    for (const Row& row : rows) {
        INFO(row.name);
        CHECK(row.traits.raises_runout_fault == row.raises_runout_fault);
        CHECK(row.traits.raises_runout_fault_during_job == row.raises_runout_fault_during_job);
        CHECK(row.traits.unloads_to_hub_between_prints == row.unloads_to_hub_between_prints);
    }
}

// The backends that raise a runout fault pin that trait beside the runout fixture that proves
// they raise it (the [runout_traits] cases in their own test files). These are the other side:
// a backend with no runout fault of its own must produce no error event for a runout, or the
// generic modal would be silenced with nothing put in its place.
TEST_CASE("runout_fault_traits_match_the_error_hooks", "[ams][traits][runout_traits]") {
    const std::string runout_line = "!! Filament runout";

    SECTION("ACE has no error hook") {
        AmsBackendAce backend(nullptr, nullptr);
        CHECK_FALSE(backend.classify_error(runout_line, paused_job()).has_value());
        CHECK_FALSE(backend.current_error().has_value());
        CHECK_FALSE(backend.traits().raises_runout_fault);
    }

    SECTION("OpenAMS raises faults of the manager, not a runout") {
        AmsBackendOpenAms backend(nullptr, nullptr);
        CHECK_FALSE(backend.classify_error(runout_line, paused_job()).has_value());
        CHECK_FALSE(backend.current_error().has_value());
        CHECK_FALSE(backend.traits().raises_runout_fault);
    }

    SECTION("Snapmaker classifies only the missing-filament-type refusal") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        CHECK_FALSE(backend.classify_error(runout_line, paused_job()).has_value());
        CHECK_FALSE(backend.current_error().has_value());
        CHECK_FALSE(backend.traits().raises_runout_fault);
    }

    SECTION("a tool changer raises dock-sensor faults, not a runout") {
        AmsBackendToolChanger backend(nullptr, nullptr);
        CHECK_FALSE(backend.classify_error(runout_line, paused_job()).has_value());
        CHECK_FALSE(backend.current_error().has_value());
        CHECK_FALSE(backend.traits().raises_runout_fault);
    }

    SECTION("QIDI Box reports a blocked slot with a dismiss-only dialog") {
        AmsBackendQidi backend(nullptr, nullptr);
        CHECK_FALSE(backend.classify_error(runout_line, paused_job()).has_value());
        CHECK_FALSE(backend.current_error().has_value());
        QidiBoxTestAccess::parse_vars(
            backend,
            json{{"enable_box", 1}, {"slot0", 1}, {"slot1", -3}, {"slot2", 1}, {"slot3", 1}});
        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        // Nothing to resume or reload from: the generic runout modal is the surface
        // that offers the user a way forward.
        REQUIRE(ev->recovery_actions.size() == 1);
        CHECK(ev->recovery_actions[0].gcode.empty());
        CHECK_FALSE(backend.traits().raises_runout_fault);
    }
}
