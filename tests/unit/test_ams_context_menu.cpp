// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_context_menu.h"
#include "ui_ams_lane_settings.h"
#include "ui_button.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "helix/ui/text_metrics.h"
#if HELIX_HAS_PLUGINS
#include "../test_helpers/plugin_host_test_support.h"
#endif
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_execute.h"
#include "filament_op_slot_resolver.h"
#include "filament_variants.h"
#include "plugin_menu.h"
#include "printer_discovery.h"
#include "theme_manager.h"
#include "toolchanger_addon.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;
using namespace helix::ui;

// Forwards the private static predicates (friend access).
class AmsContextMenuTestAccess {
  public:
    static bool should_show_clear_spool(const SlotInfo& slot) {
        return AmsContextMenu::should_show_clear_spool(slot);
    }

    using UnloadMode = AmsContextMenu::UnloadMode;

    static UnloadMode decide_unload_mode(bool toolhead_unload, bool can_recover,
                                         bool recovery_attributed, bool supports_eject,
                                         bool slot_has_filament, bool supports_force_eject,
                                         bool slot_empty) {
        return AmsContextMenu::decide_unload_mode(toolhead_unload, can_recover, recovery_attributed,
                                                  supports_eject, slot_has_filament,
                                                  supports_force_eject, slot_empty);
    }

    static bool decide_can_load(bool system_busy, bool toolhead_unload,
                                std::optional<bool> slot_has_filament, bool print_active) {
        return AmsContextMenu::decide_can_load(system_busy, toolhead_unload, slot_has_filament,
                                               print_active);
    }

    static bool decide_unload_enabled(bool system_busy, UnloadMode mode, bool print_active,
                                      bool cold_ops_print_gated = false) {
        return AmsContextMenu::decide_unload_enabled(system_busy, mode, print_active,
                                                     cold_ops_print_gated);
    }

    using SlotOpDecision = AmsContextMenu::SlotOpDecision;

    static SlotOpDecision decide_slot_ops(const AmsBackend* backend, int slot_index,
                                          bool pending_is_loaded, bool system_busy,
                                          bool print_blocks_op, bool toolhead_unaccounted) {
        return AmsContextMenu::decide_slot_ops(backend, slot_index, pending_is_loaded, system_busy,
                                               print_blocks_op, toolhead_unaccounted);
    }
};

// "Clear Spool" was revealed only when `!slot_has_filament`, so it vanished the
// moment a new spool went into the lane — precisely when a stale assignment is
// most harmful, because that is when the wrong metadata gets printed with and
// when an edit will aim a Spoolman write at the previous spool.
//
// Stale metadata on an EMPTY lane is cosmetic. Stale metadata on a LOADED lane
// is the actual failure. Presence must not gate the affordance.
TEST_CASE("AmsContextMenu::should_show_clear_spool ignores whether filament is present",
          "[ams][context_menu]") {
    SECTION("assigned AND loaded still offers the clear — the regression") {
        SlotInfo slot;
        slot.status = SlotStatus::LOADED;
        slot.spoolman_id = 86;
        slot.material = "ASA";
        CHECK(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }

    SECTION("assigned and empty offers the clear") {
        SlotInfo slot;
        slot.status = SlotStatus::EMPTY;
        slot.spoolman_id = 86;
        slot.material = "ASA";
        CHECK(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }

    SECTION("material-only assignment counts, with no Spoolman link") {
        SlotInfo slot;
        slot.status = SlotStatus::LOADED;
        slot.spoolman_id = 0;
        slot.material = "PLA";
        CHECK(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }

    SECTION("spoolman-link-only assignment counts, with no material") {
        SlotInfo slot;
        slot.status = SlotStatus::AVAILABLE;
        slot.spoolman_id = 86;
        slot.material.clear();
        CHECK(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }

    SECTION("nothing assigned offers nothing to clear") {
        SlotInfo slot;
        slot.status = SlotStatus::AVAILABLE;
        slot.spoolman_id = 0;
        slot.material.clear();
        CHECK_FALSE(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }

    SECTION("empty and unassigned offers nothing to clear") {
        SlotInfo slot;
        slot.status = SlotStatus::EMPTY;
        slot.spoolman_id = 0;
        slot.material.clear();
        CHECK_FALSE(AmsContextMenuTestAccess::should_show_clear_spool(slot));
    }
}

// AmsContextMenu::decide_unload_mode() — Unload button operation selection.
//
// The BoxTurtle hub sensor is shared across every lane on a unit, so
// can_recover_lane_position() can read true for EVERY lane at once when AFC
// names no active lane. A prior version of this chain checked RecoverPosition
// unconditionally before Eject, which meant one unattributed stranded lane
// hid Eject from every seated lane sharing its hub — the bug this ruling
// fixes. lane_recovery_is_attributed() breaks the tie: attributed recovery
// outranks Eject, unattributed recovery defers to it.
using UnloadMode = AmsContextMenuTestAccess::UnloadMode;

TEST_CASE("AmsContextMenu::decide_unload_mode toolhead-loaded wins over everything",
          "[ams][context_menu]") {
    // Even if the backend also claims recovery is possible and attributed, a
    // slot that unloads via the heated toolhead path must take Unload.
    auto mode = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/true, /*can_recover=*/true, /*recovery_attributed=*/true,
        /*supports_eject=*/true, /*slot_has_filament=*/true, /*supports_force_eject=*/true,
        /*slot_empty=*/false);
    CHECK(mode == UnloadMode::Unload);
}

TEST_CASE("AmsContextMenu::decide_unload_mode attributed strand outranks Eject",
          "[ams][context_menu]") {
    // AFC named this exact lane as active (lane_recovery_is_attributed==true).
    // Even though the lane also has filament present (would otherwise take
    // Eject), the confident diagnosis wins.
    auto mode = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/false, /*can_recover=*/true, /*recovery_attributed=*/true,
        /*supports_eject=*/true, /*slot_has_filament=*/true, /*supports_force_eject=*/false,
        /*slot_empty=*/false);
    CHECK(mode == UnloadMode::RecoverPosition);
}

TEST_CASE(
    "AmsContextMenu::decide_unload_mode unattributed strand does not take Eject from a seated lane",
    "[ams][context_menu]") {
    // This is the regression the ruling fixes: an unattributed hub-wide trigger
    // (can_recover=true, recovery_attributed=false) must NOT preempt Eject on a
    // lane that is simply seated (slot_has_filament=true).
    auto mode = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/false, /*can_recover=*/true, /*recovery_attributed=*/false,
        /*supports_eject=*/true, /*slot_has_filament=*/true, /*supports_force_eject=*/false,
        /*slot_empty=*/false);
    CHECK(mode == UnloadMode::Eject);
}

TEST_CASE("AmsContextMenu::decide_unload_mode unattributed lane with nothing ejectable still gets "
          "Recover",
          "[ams][context_menu]") {
    // No filament present to eject (slot_has_filament=false), so Eject is not an
    // option regardless of attribution — the unattributed Recover arm is the
    // last resort that still offers a way out for a lane with no other option.
    auto mode = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/false, /*can_recover=*/true, /*recovery_attributed=*/false,
        /*supports_eject=*/true, /*slot_has_filament=*/false, /*supports_force_eject=*/false,
        /*slot_empty=*/true);
    CHECK(mode == UnloadMode::RecoverPosition);
}

TEST_CASE("AmsContextMenu::decide_unload_mode falls through to ForceEject and Unavailable",
          "[ams][context_menu]") {
    // No toolhead unload, no recovery possible at all, no eject support: an
    // empty lane with force-eject support gets ForceEject...
    auto force_eject = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/false, /*can_recover=*/false, /*recovery_attributed=*/false,
        /*supports_eject=*/false, /*slot_has_filament=*/false, /*supports_force_eject=*/true,
        /*slot_empty=*/true);
    CHECK(force_eject == UnloadMode::ForceEject);

    // ...and with nothing at all supported, there is genuinely nothing to do.
    auto unavailable = AmsContextMenuTestAccess::decide_unload_mode(
        /*toolhead_unload=*/false, /*can_recover=*/false, /*recovery_attributed=*/false,
        /*supports_eject=*/false, /*slot_has_filament=*/false, /*supports_force_eject=*/false,
        /*slot_empty=*/true);
    CHECK(unavailable == UnloadMode::Unavailable);
}

// decide_can_load() must agree with AmsSubscriptionBackend::refuse_if_printing()
// in BOTH directions.
//
// Offering what the backend refuses is bundle JX2FVRB9: a runout-paused AD5X user
// tapped Load, following Klipper's own "load it and press RESUME" instruction,
// and got "Cannot run filament operation while printing".
//
// Refusing what the backend now ACCEPTS is the other half, and the reason this
// test changed shape: refuse_if_printing() no longer blocks a PAUSED print on a
// backend whose filament macro does not home itself. Pause-then-swap is the
// runout / colour-change recovery workflow on AFC, Happy Hare, CFS, ACE, QIDI,
// toolchangers and Snapmaker; only AD5X IFS still refuses it, because
// `_IFS_REMOVE_CURRENT_PRUTOK` runs a buried `_G28` that probes a loadcell-Z
// nozzle into the part (bundle XWPBR2DX).
//
// The parameter carries print_blocks_filament_op()'s answer, not the raw
// print_active subject — the tests below drive it through that predicate rather
// than hand-writing booleans, so a change to the rule shows up here.
//
// Mutation check: drop the `!print_blocks_op` term from decide_can_load() and
// "Load is refused while PRINTING" fails; make print_blocks_filament_op() ignore
// backend_self_homes and both PAUSED sections fail.
TEST_CASE("AmsContextMenu::decide_can_load agrees with the backend print guard",
          "[ams][context_menu][print_guard]") {
    using helix::ui::print_blocks_filament_op;

    auto can_load = [](bool printing, bool paused, bool self_homes) {
        return AmsContextMenuTestAccess::decide_can_load(
            /*system_busy=*/false, /*toolhead_unload=*/false, /*slot_has_filament=*/true,
            print_blocks_filament_op(helix::test::lifecycle_from_bools(printing, paused),
                                     self_homes));
    };

    SECTION("Load is offered for a filled, non-seated lane when no print is running") {
        CHECK(can_load(/*printing=*/false, /*paused=*/false, /*self_homes=*/false));
        CHECK(can_load(/*printing=*/false, /*paused=*/false, /*self_homes=*/true));
    }

    SECTION("Load is refused while PRINTING, on every backend") {
        CHECK_FALSE(can_load(/*printing=*/true, /*paused=*/false, /*self_homes=*/false));
        CHECK_FALSE(can_load(/*printing=*/true, /*paused=*/false, /*self_homes=*/true));
    }

    SECTION("Load is refused while PREPARING, on every backend") {
        // The window the bool pair could not express: a host-side pre-start
        // block reads (printing=false, paused=false) off print_stats, which is
        // indistinguishable from idle, so Load was offered while the pre-start
        // G-code homed and probed. Reached by naming the enum, not through the
        // bool shim, which deliberately cannot produce Preparing.
        CHECK_FALSE(AmsContextMenuTestAccess::decide_can_load(
            /*system_busy=*/false, /*toolhead_unload=*/false, /*slot_has_filament=*/true,
            print_blocks_filament_op(PrintState::Preparing, /*self_homes=*/false)));
        CHECK_FALSE(AmsContextMenuTestAccess::decide_can_load(
            /*system_busy=*/false, /*toolhead_unload=*/false, /*slot_has_filament=*/true,
            print_blocks_filament_op(PrintState::Preparing, /*self_homes=*/true)));
    }

    SECTION("Load is OFFERED on a paused print when the backend does not self-home") {
        // AFC / Happy Hare / CFS / ACE / QIDI / toolchanger / Snapmaker. This is
        // the recovery Klipper asks for; greying it made HelixScreen the only
        // surface that could not perform it.
        CHECK(can_load(/*printing=*/false, /*paused=*/true, /*self_homes=*/false));
    }

    SECTION("Load is still refused on a paused print when the backend self-homes") {
        // AD5X IFS only.
        CHECK_FALSE(can_load(/*printing=*/false, /*paused=*/true, /*self_homes=*/true));
    }

    SECTION("The pre-existing terms still hold") {
        CHECK_FALSE(AmsContextMenuTestAccess::decide_can_load(true, false, true, false)); // busy
        CHECK_FALSE(AmsContextMenuTestAccess::decide_can_load(false, true, true, false)); // seated
        CHECK_FALSE(AmsContextMenuTestAccess::decide_can_load(false, false, false, false)); // empty
    }

    SECTION("An UNKNOWN-presence lane is not treated as empty") {
        // SlotStatus::UNKNOWN means the backend publishes no per-lane presence,
        // not "the lane is empty" — slot_presence() reports it unanswerable and
        // Load stays reachable so the backend gets to refuse if it really is.
        CHECK(AmsContextMenuTestAccess::decide_can_load(false, false, std::nullopt, false));
    }
}

// Only the heated toolhead unload is subject to the print gate at all. The cold
// lane ops leave the toolhead parked where the print left it and the backend
// permits them via check_preconditions(false) — which never consults print state
// — so blocking the whole button would strand filament a paused user could
// legitimately eject.
//
// The print term itself is print_blocks_filament_op()'s answer, so PAUSED now
// also stops blocking the heated Unload on every backend but AD5X. That is the
// live Discord report: "Unload failed: Cannot run filament operation while
// printing", raised while merely PAUSED.
//
// Mutation check: delete the cold-lane arm and "Cold lane ops stay available"
// fails; make print_blocks_filament_op() ignore backend_self_homes and the
// paused sections fail.
TEST_CASE("AmsContextMenu::decide_unload_enabled blocks only the toolhead unload mid-print",
          "[ams][context_menu][print_guard]") {
    using helix::ui::print_blocks_filament_op;

    auto unload_enabled = [](UnloadMode mode, bool printing, bool paused, bool self_homes,
                             bool cold_ops_print_gated = false) {
        return AmsContextMenuTestAccess::decide_unload_enabled(
            /*system_busy=*/false, mode,
            print_blocks_filament_op(helix::test::lifecycle_from_bools(printing, paused),
                                     self_homes),
            cold_ops_print_gated);
    };

    SECTION("Toolhead unload is refused while PRINTING, on every backend") {
        CHECK(unload_enabled(UnloadMode::Unload, false, false, false));
        CHECK_FALSE(unload_enabled(UnloadMode::Unload, /*printing=*/true, false, false));
        CHECK_FALSE(unload_enabled(UnloadMode::Unload, /*printing=*/true, false, true));
    }

    SECTION("Toolhead unload is OFFERED on a paused print unless the backend self-homes") {
        CHECK(unload_enabled(UnloadMode::Unload, false, /*paused=*/true, /*self_homes=*/false));
        CHECK_FALSE(
            unload_enabled(UnloadMode::Unload, false, /*paused=*/true, /*self_homes=*/true));
    }

    SECTION("Toolhead unload is refused while PREPARING") {
        CHECK_FALSE(AmsContextMenuTestAccess::decide_unload_enabled(
            /*system_busy=*/false, UnloadMode::Unload,
            print_blocks_filament_op(PrintState::Preparing, /*self_homes=*/false),
            /*cold_ops_print_gated=*/false));
    }

    SECTION("Cold lane ops stay available mid-print, even on a self-homing backend") {
        for (auto mode : {UnloadMode::Eject, UnloadMode::RecoverPosition, UnloadMode::ForceEject}) {
            CHECK(unload_enabled(mode, /*printing=*/true, false, /*self_homes=*/false));
            CHECK(unload_enabled(mode, /*printing=*/true, false, /*self_homes=*/true));
        }
    }

    // AFC's cmd_LANE_UNLOAD opens with its own is_printing() check, so the cold
    // exemption above would offer a button the firmware discards without moving
    // anything. The exemption is withdrawn per backend, not globally: the section
    // above must keep passing for everyone else.
    SECTION("Cold lane ops grey out while PRINTING when the firmware refuses them too") {
        for (auto mode : {UnloadMode::Eject, UnloadMode::RecoverPosition, UnloadMode::ForceEject}) {
            CHECK_FALSE(unload_enabled(mode, /*printing=*/true, false, /*self_homes=*/false,
                                       /*cold_ops_print_gated=*/true));
        }
    }

    // AFC's is_printing() is `state == "printing"` exactly, so a PAUSED job still
    // reaches the firmware. Greying it would break the pause-then-clear-a-strand
    // recovery this whole gate was narrowed to preserve.
    SECTION("A gated backend still offers cold lane ops while PAUSED") {
        for (auto mode : {UnloadMode::Eject, UnloadMode::RecoverPosition, UnloadMode::ForceEject}) {
            CHECK(unload_enabled(mode, false, /*paused=*/true, /*self_homes=*/false,
                                 /*cold_ops_print_gated=*/true));
        }
    }

    SECTION("Busy and Unavailable still win over everything") {
        CHECK_FALSE(
            AmsContextMenuTestAccess::decide_unload_enabled(true, UnloadMode::Eject, false));
        CHECK_FALSE(
            AmsContextMenuTestAccess::decide_unload_enabled(false, UnloadMode::Unavailable, false));
    }
}

// ============================================================================
// A backend that withdraws its unmount offer has to be able to make it stick.
//
// AmsContextMenu composes the loaded signal as an OR
// (`src/ui/ui_ams_context_menu.cpp#AmsContextMenu::decide_slot_ops`): the
// snapshot taken when the menu opens (can_unload_from_toolhead) OR the live pair
// (slot_is_actively_loaded || slot_has_filament_at_toolhead). A rule that lives
// in only one of those three is put back by the other two, so a withdrawal that
// reads correct in the backend never reaches the button. On a tool changer the
// button dispatches UNSELECT_TOOL, which moves a physical machine.
// ============================================================================

namespace {

/// Real tool-changer backend with the MedusaHC dock sensors attached, driven by
/// raw Moonraker frames. Nothing here is a stand-in: the accessors below are the
/// ones the menu calls.
class DockSensorToolChanger : public AmsBackendToolChanger {
  public:
    DockSensorToolChanger() : AmsBackendToolChanger(nullptr, nullptr) {
        set_discovered_tools({"T0", "T1", "T2", "T3"});
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array(
            {"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "pin_watch io"}));
        set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(hw));
    }

    void feed(const nlohmann::json& status) {
        handle_status_update(nlohmann::json{{"method", "notify_status_update"},
                                            {"params", nlohmann::json::array({status, 0.0})}});
    }
};

/// The AMS panels' call site, so a case supplies the inputs the UI supplies:
/// both panels snapshot AmsBackend::can_unload_from_toolhead() for the slot and
/// hand that to the menu as `pending_is_loaded`. The decision itself is
/// production's - this only wires the arguments.
[[nodiscard]] AmsContextMenuTestAccess::SlotOpDecision ops_for(const AmsBackend& backend, int slot,
                                                               bool print_blocks_op = false) {
    return AmsContextMenuTestAccess::decide_slot_ops(
        &backend, slot, backend.can_unload_from_toolhead(slot), backend.get_system_info().is_busy(),
        print_blocks_op, backend.toolhead_filament_unaccounted().value_or(false));
}

} // namespace

// A U1 head whose toolhead switch reads filament while channel_state says
// preload_finish: filament parked short of the nozzle. The menu offers the
// heated Unload for it and keeps Load, since the head is not loaded.
TEST_CASE("Snapmaker lane menu offers Unload and Load for filament parked in the toolhead",
          "[ams][context_menu][snapmaker][parked]") {
    using UnloadMode = AmsContextMenuTestAccess::UnloadMode;
    helix::test::RegisteredBackend<helix::AmsBackendSnapmaker> reg(nullptr, nullptr);
    auto& backend = *reg;
    auto feed = [&](const char* channel_state, bool switch_detected) {
        helix::SnapmakerTestAccess::handle_status(
            backend,
            nlohmann::json{
                {"toolhead", {{"extruder", "extruder3"}}},
                {"filament_feed right",
                 {{"extruder3", {{"filament_detected", true}, {"channel_state", channel_state}}}}},
                {"filament_motion_sensor e3_filament", {{"filament_detected", switch_detected}}}});
    };

    SECTION("parked") {
        feed("preload_finish", true);
        const auto d = ops_for(backend, 3);
        CHECK(d.unload_mode == UnloadMode::Unload);
        CHECK(d.unload_enabled);
        CHECK(d.can_load);
    }
    SECTION("switch clear: nothing to unload") {
        feed("preload_finish", false);
        const auto d = ops_for(backend, 3);
        CHECK(d.unload_mode == UnloadMode::Unavailable);
        CHECK_FALSE(d.unload_enabled);
        CHECK(d.can_load);
    }
    SECTION("loaded to the nozzle") {
        feed("load_finish", true);
        const auto d = ops_for(backend, 3);
        CHECK(d.unload_mode == UnloadMode::Unload);
        CHECK(d.unload_enabled);
        CHECK_FALSE(d.can_load);
    }
}

TEST_CASE("Lane menus withdraw Unload while the toolhead is unaccounted",
          "[ams][context_menu][1324]") {
    // Filament at the toolhead with no lane claiming it: the lane the menu is
    // open on may not be the seated one, so neither the heated unload nor a
    // cold eject is offered on any lane. The computed mode is left alone - a
    // relabel to Eject would cold-retract the lane that is secretly seated -
    // only the enablement withdraws, and the sidebar's active-head Unload
    // covers the state instead.
    using UnloadMode = AmsContextMenuTestAccess::UnloadMode;

    AmsBackendMock loaded(4);
    REQUIRE(loaded.start());
    // Slot 0 is the mock's default loaded lane.
    REQUIRE(loaded.can_unload_from_toolhead(0));
    CHECK(ops_for(loaded, 0).unload_enabled);
    loaded.stop();

    AmsBackendMock unaccounted(4);
    unaccounted.set_initial_state_scenario("unaccounted");
    REQUIRE(unaccounted.start());
    REQUIRE(unaccounted.toolhead_filament_unaccounted().value_or(false));
    const auto d = ops_for(unaccounted, 0);
    CHECK(d.unload_mode == UnloadMode::Unload);
    CHECK_FALSE(d.unload_enabled);
    unaccounted.stop();
}

TEST_CASE("A dock-sensor fault disables Unmount and says why", "[ams][context_menu][toolchanger]") {
    using UnloadMode = AmsContextMenuTestAccess::UnloadMode;

    DockSensorToolChanger backend;
    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});

    // The settled machine: tool 1 on the carriage, Unmount offered for it.
    REQUIRE(ops_for(backend, 1).is_loaded);

    // -2 is the sensors saying they cannot tell which tool is mounted. The parse
    // deliberately HOLDS the last known tool, so everything derived from
    // current_slot still names a perfectly plausible tool 1.
    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});
    REQUIRE(backend.get_current_slot() == 1);
    REQUIRE(backend.is_filament_loaded());

    // ERROR is excluded from is_busy() (AmsSystemInfo::is_busy), so the busy
    // term cannot be what greys the button — the loaded signal has to.
    REQUIRE_FALSE(backend.get_system_info().is_busy());
    // Nothing is loaded and this backend has no cold lane op to fall back on, so
    // the Unload button has no operation at all and is disabled.
    const auto faulted = ops_for(backend, 1);
    REQUIRE_FALSE(faulted.is_loaded);
    CHECK(faulted.unload_mode == UnloadMode::Unavailable);
    CHECK_FALSE(faulted.unload_enabled);

    // The hint is gated on !unload_enabled, so both halves live or die together:
    // a still-enabled button also never explains itself.
    CHECK_FALSE(backend.unload_blocked_reason(1).empty());
}

TEST_CASE("The dock-sensor withdrawal is not a permanent blanking",
          "[ams][context_menu][toolchanger]") {
    // The fault must withdraw the offer and then give it back — a latch that
    // never clears would strand the user with no way to unmount at all.
    DockSensorToolChanger backend;
    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}}}});
    REQUIRE(ops_for(backend, 2).is_loaded);

    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});
    REQUIRE_FALSE(ops_for(backend, 2).is_loaded);

    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 2}}}});
    CHECK(ops_for(backend, 2).is_loaded);
    CHECK(backend.unload_blocked_reason(2).empty());

    // And the docked tools were never a target, fault or no fault.
    for (int slot : {0, 1, 3}) {
        CAPTURE(slot);
        CHECK_FALSE(ops_for(backend, slot).is_loaded);
    }
}

TEST_CASE("A mount-to-feed tool changer offers the heated Unload on docked tools",
          "[ams][context_menu][toolchanger][mount_feed]") {
    using UnloadMode = AmsContextMenuTestAccess::UnloadMode;
    DockSensorToolChanger backend;
    backend.set_mounts_to_feed(true);

    SECTION("nothing on the carriage") {
        backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", -1}}}});
        for (int slot : {0, 1, 2, 3}) {
            CAPTURE(slot);
            const auto d = ops_for(backend, slot);
            CHECK(d.unload_mode == UnloadMode::Unload);
            CHECK(d.unload_enabled);
            CHECK(d.can_load);
            CHECK(helix::ui::read_unload_target_loaded(&backend, backend.get_system_info(), slot));
        }
    }
    SECTION("another tool mounted") {
        backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        const auto docked = ops_for(backend, 2);
        CHECK(docked.unload_mode == UnloadMode::Unload);
        CHECK(docked.unload_enabled);
        CHECK(ops_for(backend, 1).unload_enabled);
    }
    SECTION("a dock-sensor fault withdraws it") {
        backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", -2}}}});
        for (int slot : {0, 1, 2, 3}) {
            CAPTURE(slot);
            CHECK_FALSE(ops_for(backend, slot).unload_enabled);
        }
    }
    SECTION("without mount-to-feed a docked tool is still no target") {
        backend.set_mounts_to_feed(false);
        backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
        CHECK_FALSE(backend.slot_filament_parked_in_toolhead(2));
        CHECK(ops_for(backend, 2).unload_mode == UnloadMode::Unavailable);
    }
}

// ============================================================================
// On a tool changer with no feed, filament is by hand and the menu mounts
// ============================================================================

TEST_CASE("A no-feed tool changer's menu offers Mount and Park, never a filament op",
          "[ams][context_menu][toolchanger]") {
    // Each tool carries its own filament permanently, so presence is true for
    // all four at once and decides nothing. Which tool is on the carriage
    // decides Mount and Park; nothing decides Load or Unload, which are by hand.
    DockSensorToolChanger backend;
    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 1}}}});
    REQUIRE(backend.get_current_slot() == 1);
    REQUIRE_FALSE(backend.get_system_info().is_busy());

    const auto view = helix::ui::read_filament_head_view(&backend, get_printer_state());
    REQUIRE(view.heads.size() == 4);
    CHECK(view.mounted_head == 1);

    const auto mounted = helix::ui::decide_slot_actions(view, 1);
    CHECK_FALSE(mounted.load.enabled);
    CHECK_FALSE(mounted.load.reason.empty());
    CHECK_FALSE(mounted.unload.enabled);
    CHECK(mounted.mount.visible);
    CHECK_FALSE(mounted.mount.enabled);
    CHECK(mounted.park.visible == backend.supports_tool_park());

    for (int slot : {0, 2, 3}) {
        CAPTURE(slot);
        const auto docked = helix::ui::decide_slot_actions(view, slot);
        CHECK_FALSE(docked.load.enabled);
        CHECK(docked.mount.visible);
        CHECK(docked.mount.enabled);
        CHECK_FALSE(docked.park.visible);
    }

    // A swap hands Mount over: the departed tool becomes mountable.
    backend.feed(nlohmann::json{{"medusahc", {{"operation", "idle"}, {"current_tool", 3}}}});
    const auto after = helix::ui::read_filament_head_view(&backend, get_printer_state());
    CHECK(helix::ui::decide_slot_actions(after, 1).mount.enabled);
    CHECK_FALSE(helix::ui::decide_slot_actions(after, 3).mount.enabled);
}

// ============================================================================
// on_created: the header names the position in the backend's own word
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: the slot header uses the backend's own word for a position",
                 "[ui][ams][context_menu]") {
    // AmsBackendMock reports Happy Hare, whose noun is Gate. A header reading
    // "Slot 3" on this backend is the English word for something the firmware,
    // the macros and every other surface call a gate.
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(backend->lane_noun() == LaneNoun::Gate);

    // ams_context_menu.xml is registered lazily by AmsPanel/AmsOverviewPanel
    // rather than by helix::register_xml_components(), so the fixture's
    // "every component" registration does not cover it.
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));

    lv_obj_t* header = lv_obj_find_by_name(test_screen(), "slot_header");
    REQUIRE(header != nullptr);
    CHECK(std::string(lv_label_get_text(header)) == "Gate 3");
}

// ============================================================================
// on_created: Clear Spool greys out with a reason while its lane feeds the job
// ============================================================================

namespace {

/// Pin the lifecycle through the wire driver for one case and restore a
/// machine-free state on scope exit, even when an assertion throws: the
/// lifecycle subject outlives every test in this binary, and a latched
/// PRINTING changes what any later test's Clear Spool is allowed to do.
class ScopedWireState {
  public:
    ScopedWireState(PrinterState& st, helix::PrintJobState s) : st_(st) {
        helix::test::set_wire_state(st_, s);
    }
    ~ScopedWireState() {
        helix::test::set_wire_state(st_, helix::PrintJobState::STANDBY);
    }

  private:
    PrinterState& st_;
};

} // namespace

TEST_CASE("AmsContextMenu::decide_offers_pa_calibration needs the filament at an idle nozzle",
          "[ams][context_menu][pa_calibration]") {
    CHECK(AmsContextMenu::decide_offers_pa_calibration(true, true, false, false));
    CHECK_FALSE(AmsContextMenu::decide_offers_pa_calibration(false, true, false, false));
    CHECK_FALSE(AmsContextMenu::decide_offers_pa_calibration(true, false, false, false));
    CHECK_FALSE(AmsContextMenu::decide_offers_pa_calibration(true, true, true, false));
    CHECK_FALSE(AmsContextMenu::decide_offers_pa_calibration(true, true, false, true));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: Calibrate PA shows on the loaded lane of a measuring printer",
                 "[ui][ams][context_menu][pa_calibration]") {
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(backend->slot_is_actively_loaded(0));
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    ScopedWireState idle(state(), helix::PrintJobState::STANDBY);

    struct Hardware {
        explicit Hardware(std::vector<std::string> objects) {
            helix::PrinterDiscovery hw;
            hw.parse_objects(objects);
            hw.set_printer_objects(objects);
            get_printer_state().set_hardware(hw);
        }
        ~Hardware() {
            get_printer_state().set_hardware(helix::PrinterDiscovery{});
        }
    };
    const auto offered = [&](int slot, bool loaded) {
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), slot, test_screen(), loaded, backend.get()));
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_pa_calibrate");
        REQUIRE(btn != nullptr);
        return !lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);
    };

    SECTION("a printer that measures") {
        Hardware hw({"extruder", "prtouch_v3", "helix_pa_gauge"});
        CHECK(offered(0, true));
        CHECK_FALSE(offered(1, false));
    }
    SECTION("a printer that cannot") {
        Hardware hw({"extruder", "prtouch_v3"});
        CHECK_FALSE(offered(0, true));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: Clear Spool greys out with a reason while its lane feeds the job",
                 "[ui][ams][context_menu][1661]") {
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(backend->lane_noun() == LaneNoun::Gate);
    // create_mock()'s seed, restated for a directly-constructed mock: slot 0 is
    // the loaded, current lane, the one a job draws from.
    REQUIRE(backend->slot_is_actively_loaded(0));

    // ams_context_menu.xml is registered lazily by AmsPanel/AmsOverviewPanel
    // rather than by helix::register_xml_components(), so the fixture's
    // "every component" registration does not cover it.
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    SECTION("mid-print, on the lane the job is drawing from") {
        ScopedWireState printing(state(), helix::PrintJobState::PRINTING);

        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/0, test_screen(),
                                      /*is_loaded=*/true, backend.get()));

        lv_subject_t* can_clear = lv_xml_get_subject(nullptr, "ams_slot_can_clear");
        REQUIRE(can_clear != nullptr);
        CHECK(lv_subject_get_int(can_clear) == 0);

        // The reason is on screen in the backend's own word for the position.
        lv_subject_t* hint = lv_xml_get_subject(nullptr, "ams_slot_clear_hint");
        REQUIRE(hint != nullptr);
        const std::string hint_text = lv_subject_get_string(hint);
        CHECK(hint_text.find("Gate 1") != std::string::npos);
        CHECK(hint_text.find("feeding the current print") != std::string::npos);

        lv_subject_t* hint_visible = lv_xml_get_subject(nullptr, "ams_slot_clear_hint_visible");
        REQUIRE(hint_visible != nullptr);
        CHECK(lv_subject_get_int(hint_visible) == 1);

        // The bound button itself: revealed (the slot carries a spool) but
        // disabled, so the finger cannot fire a clear the dispatch would refuse.
        lv_obj_t* btn_clear = lv_obj_find_by_name(test_screen(), "btn_clear_spool");
        REQUIRE(btn_clear != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn_clear, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_state(btn_clear, LV_STATE_DISABLED));
    }

    SECTION("same lane, machine free") {
        ScopedWireState idle(state(), helix::PrintJobState::STANDBY);

        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/0, test_screen(),
                                      /*is_loaded=*/true, backend.get()));

        lv_subject_t* can_clear = lv_xml_get_subject(nullptr, "ams_slot_can_clear");
        REQUIRE(can_clear != nullptr);
        CHECK(lv_subject_get_int(can_clear) == 1);

        lv_subject_t* hint_visible = lv_xml_get_subject(nullptr, "ams_slot_clear_hint_visible");
        REQUIRE(hint_visible != nullptr);
        CHECK(lv_subject_get_int(hint_visible) == 0);

        lv_obj_t* btn_clear = lv_obj_find_by_name(test_screen(), "btn_clear_spool");
        REQUIRE(btn_clear != nullptr);
        CHECK_FALSE(lv_obj_has_state(btn_clear, LV_STATE_DISABLED));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: an external menu resets a lane's Clear refusal",
                 "[ui][ams][context_menu][1661]") {
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(backend->slot_is_actively_loaded(0));

    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    // Stage the refusal: a lane menu rendered mid-print on the feeding lane
    // leaves can_clear=0 and the hint published. The subjects are static and
    // outlive the menu, so this is the state the bypass menu inherits.
    {
        ScopedWireState printing(state(), helix::PrintJobState::PRINTING);
        AmsContextMenu lane_menu;
        REQUIRE(lane_menu.show_near_widget(test_screen(), /*slot_index=*/0, test_screen(),
                                           /*is_loaded=*/true, backend.get()));
    }
    lv_subject_t* can_clear = lv_xml_get_subject(nullptr, "ams_slot_can_clear");
    REQUIRE(can_clear != nullptr);
    REQUIRE(lv_subject_get_int(can_clear) == 0); // the refusal is staged

    AmsContextMenu ext_menu;
    REQUIRE(ext_menu.show_for_external_spool(test_screen(), test_screen()));

    // The bypass spool's own Clear erases its assignment, not a lane's
    // metadata, so the lane's refusal must not render under it.
    CHECK(lv_subject_get_int(can_clear) == 1);

    lv_subject_t* hint = lv_xml_get_subject(nullptr, "ams_slot_clear_hint");
    REQUIRE(hint != nullptr);
    CHECK(std::string(lv_subject_get_string(hint)).empty());

    lv_subject_t* hint_visible = lv_xml_get_subject(nullptr, "ams_slot_clear_hint_visible");
    REQUIRE(hint_visible != nullptr);
    CHECK(lv_subject_get_int(hint_visible) == 0);

    // The fresh XML tree binds from the subjects at creation, so a stale
    // refusal would draw the bypass menu's own Clear disabled.
    lv_obj_t* btn_clear = lv_obj_find_by_name(test_screen(), "btn_clear_spool");
    REQUIRE(btn_clear != nullptr);
    CHECK_FALSE(lv_obj_has_state(btn_clear, LV_STATE_DISABLED));
}

// ============================================================================
// on_created: Preload follows the backend capability and the print gate
// ============================================================================

namespace {
/// A mock whose "filament at the toolhead" answer the test sets.
class PreloadMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    bool loaded = false;
    [[nodiscard]] bool is_filament_loaded() const override {
        return loaded;
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: Preload shows for a preloading backend and greys when blocked",
                 "[ui][ams][context_menu][preload]") {
    auto backend = std::make_unique<PreloadMock>(4);
    REQUIRE(backend->supports_lane_preload());

    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    SECTION("machine free, nothing loaded: shown and enabled") {
        ScopedWireState idle(state(), helix::PrintJobState::STANDBY);
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                      /*is_loaded=*/false, backend.get()));
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_preload");
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
    }

    SECTION("filament loaded: shown but disabled") {
        ScopedWireState idle(state(), helix::PrintJobState::STANDBY);
        backend->loaded = true;
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                      /*is_loaded=*/false, backend.get()));
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_preload");
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
    }

    SECTION("mid-print: shown but disabled") {
        ScopedWireState printing(state(), helix::PrintJobState::PRINTING);
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                      /*is_loaded=*/false, backend.get()));
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_preload");
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
    }

    SECTION("external spool: hidden, even after a lane menu showed it") {
        {
            AmsContextMenu lane_menu;
            REQUIRE(lane_menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                               /*is_loaded=*/false, backend.get()));
        }
        AmsContextMenu ext_menu;
        REQUIRE(ext_menu.show_for_external_spool(test_screen(), test_screen()));
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_preload");
        REQUIRE(btn != nullptr);
        CHECK(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }
}

// ============================================================================
// A Z-Mod Creator 5 with the afclite shim: Load is a feed
// ============================================================================

TEST_CASE("Load stays offered on the mounted head when Load is a feed",
          "[ams][context_menu][toolchanger][zmod][feed]") {
    nlohmann::json objects = nlohmann::json::array(
        {"zmod", "zmod_color", "save_variables", "zmod_afclite", "AFC", "AFC_unit C5",
         "AFC_lane E0", "AFC_lane E1", "AFC_lane E2", "AFC_lane E3"});
    for (int i = 1; i <= 4; ++i) {
        objects.push_back("gcode_button extruder_pos" + std::to_string(i));
        objects.push_back("gcode_button extruder_grab" + std::to_string(i));
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);

    helix::test::ToolChangerHelper feeding(4);
    feeding.set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(hw));
    feeding.set_tool_feed(helix::toolchanger_addon::resolve_tool_feed(hw));
    feeding.feed(nlohmann::json{{"zmod_color", {{"active_tool_id", 1}}}});
    REQUIRE(feeding.get_current_slot() == 1);

    CHECK(ops_for(feeding, 1).can_load);
    CHECK(ops_for(feeding, 0).can_load);

    // The same carriage reading on a changer whose Load is a mount refuses the
    // mounted tool.
    helix::test::ToolChangerHelper mounting(4);
    mounting.set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(hw));
    mounting.feed(nlohmann::json{{"zmod_color", {{"active_tool_id", 1}}}});
    CHECK_FALSE(ops_for(mounting, 1).can_load);
}

// ============================================================================
// on_created: the eject refusal is the backend's to explain
// ============================================================================

namespace {
class EjectReasonBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    std::string reason;
    [[nodiscard]] std::string eject_unavailable_reason(int /*slot_index*/) const override {
        return reason;
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "the context menu shows a backend's eject reason",
                 "[ui][ams][context_menu]") {
    auto backend = std::make_unique<EjectReasonBackend>(4);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    SECTION("a reason reaches the hint") {
        backend->reason = "x";
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                      /*is_loaded=*/false, backend.get()));

        lv_subject_t* hint = lv_xml_get_subject(nullptr, "ams_slot_eject_hint");
        REQUIRE(hint != nullptr);
        CHECK(std::string(lv_subject_get_string(hint)) == "x");
        lv_subject_t* visible = lv_xml_get_subject(nullptr, "ams_slot_eject_hint_visible");
        REQUIRE(visible != nullptr);
        CHECK(lv_subject_get_int(visible) == 1);

        lv_obj_t* label = lv_obj_find_by_name(test_screen(), "eject_force_move_hint");
        REQUIRE(label != nullptr);
        CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("no reason hides the hint") {
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                      /*is_loaded=*/false, backend.get()));

        lv_subject_t* visible = lv_xml_get_subject(nullptr, "ams_slot_eject_hint_visible");
        REQUIRE(visible != nullptr);
        CHECK(lv_subject_get_int(visible) == 0);

        lv_obj_t* label = lv_obj_find_by_name(test_screen(), "eject_force_move_hint");
        REQUIRE(label != nullptr);
        CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    }
}

// ============================================================================
// Plugin items for the slot
// ============================================================================

#if HELIX_HAS_PLUGINS
namespace {

/// One enabled plugin declaring a filament_slot item whose handler records its context.
struct SlotMenuPlugin {
    helix::plugin::test::TempDir tmp;
    std::unique_ptr<helix::plugin::test::HostRig> rig;

    SlotMenuPlugin() {
        std::filesystem::create_directories(tmp.path / "slot-plug");
        std::ofstream(tmp.path / "slot-plug" / "manifest.json")
            << R"({"id":"slot-plug","name":"Slot Plug","version":"1.0.0","menu_items":[
                 {"id":"look","menu":"filament_slot","label":"Look up","handler":"look"}]})";
        std::ofstream(tmp.path / "slot-plug" / "main.lua") << R"(
            helix.ui.on("look", function(ctx)
                seen = string.format("%s|%s|%s|%s", tostring(ctx.unit), tostring(ctx.slot),
                                     tostring(ctx.label), tostring(ctx.material))
            end)
        )";
        rig = std::make_unique<helix::plugin::test::HostRig>(
            helix::plugin::test::enabled("slot-plug", {}));
        rig->host->load_from(tmp.path.string());
    }

    std::string seen() {
        lua_State* L = rig->host->runtime("slot-plug")->state();
        lua_getglobal(L, "seen");
        std::string s = lua_isnil(L, -1) ? "nil" : lua_tostring(L, -1);
        lua_pop(L, 1);
        return s;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: plugin rows get their own column",
                 "[ui][ams][context_menu][plugin]") {
    SlotMenuPlugin plugin;
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);

    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    lv_obj_t* col = lv_obj_find_by_name(test_screen(), "col_plugins");
    REQUIRE(col);
    CHECK(lv_obj_get_parent(col) == lv_obj_find_by_name(test_screen(), "menu_columns"));
    CHECK_FALSE(lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* heading = lv_obj_find_by_name(col, "heading_text");
    REQUIRE(heading);
    CHECK(std::string(lv_label_get_text(heading)) == "Slot Plug");
    lv_obj_t* button = lv_obj_find_by_name(col, "item_button");
    REQUIRE(button);
    lv_obj_t* label = ui_button_get_label(button);
    REQUIRE(label);
    CHECK(std::string(lv_label_get_text(label)) == "Look up");

    const std::string material = backend->get_slot_info(2).material;
    lv_obj_send_event(lv_obj_get_parent(button), LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(menu.is_visible());
    CHECK(plugin.seen() == "0|2|Gate 3|" + (material.empty() ? std::string("nil") : material));
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: the plugin column fits a typical label at 800x480",
                 "[ui][ams][context_menu][plugin]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);

    helix::plugin::test::TempDir tmp;
    std::filesystem::create_directories(tmp.path / "inspect");
    std::ofstream(tmp.path / "inspect" / "manifest.json")
        << R"({"id":"inspect","name":"Slot Demo","version":"1.0.0","menu_items":[
             {"id":"i","menu":"filament_slot","label":"Inspect this slot","icon":"tune",
              "handler":"go"}]})";
    std::ofstream(tmp.path / "inspect" / "main.lua") << R"(helix.ui.on("go", function() end))";
    helix::plugin::test::HostRig rig(helix::plugin::test::enabled("inspect", {}));
    rig.host->load_from(tmp.path.string());

    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    lv_obj_t* col = lv_obj_find_by_name(test_screen(), "col_plugins");
    REQUIRE(col);
    lv_obj_update_layout(lv_obj_get_screen(col));
    lv_obj_t* label = ui_button_get_label(lv_obj_find_by_name(col, "item_button"));
    REQUIRE(label);
    const int32_t need =
        helix::ui::text_width("Inspect this slot", lv_obj_get_style_text_font(label, LV_PART_MAIN),
                              lv_obj_get_style_text_letter_space(label, LV_PART_MAIN));
    CAPTURE(lv_obj_get_width(col), lv_obj_get_width(label), need);
    CHECK(lv_obj_get_width(label) >= need);
    lv_obj_t* card = lv_obj_find_by_name(test_screen(), "context_menu");
    REQUIRE(card);
    CHECK(lv_obj_get_width(card) <= 800);
    menu.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: the plugin column hides without plugin items",
                 "[ui][ams][context_menu][plugin]") {
    auto backend = std::make_unique<AmsBackendMock>(4);
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    lv_obj_t* col = lv_obj_find_by_name(test_screen(), "col_plugins");
    REQUIRE(col);
    CHECK(lv_obj_get_child_count(col) == 0);
    CHECK(lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN));
    menu.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: the external spool takes no plugin rows",
                 "[ui][ams][context_menu][plugin]") {
    SlotMenuPlugin plugin;
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    AmsContextMenu menu;
    REQUIRE(menu.show_for_external_spool(test_screen(), test_screen()));
    lv_obj_t* col = lv_obj_find_by_name(test_screen(), "col_plugins");
    REQUIRE(col);
    CHECK(lv_obj_get_child_count(col) == 0);
    CHECK(lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN));
    menu.hide();
    process_lvgl(20);
}
#endif // HELIX_HAS_PLUGINS

// ============================================================================
// Edit Spool
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: Edit Spool opens the editor",
                 "[ui][ams][context_menu][edit_spool]") {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    auto backend = std::make_unique<AmsBackendMock>(4);

    AmsContextMenu menu;
    std::optional<AmsContextMenu::MenuAction> fired;
    menu.set_action_callback(
        [&](AmsContextMenu::MenuAction action, int /*slot*/) { fired = action; });
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/1, test_screen(),
                                  /*is_loaded=*/false, backend.get()));

    lv_obj_t* btn = lv_obj_find_by_name(test_screen(), "btn_edit");
    REQUIRE(btn != nullptr);
    lv_obj_t* label = ui_button_get_label(btn);
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Edit Spool");

    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    UpdateQueue::instance().drain();
    REQUIRE(fired.has_value());
    CHECK(*fired == AmsContextMenu::MenuAction::EDIT);
}

// ============================================================================
// A lane menu is actions only, and never scrolls
// ============================================================================

namespace {

/// A Happy Hare lane whose menu offers everything it can: Check Slot, Preload,
/// an editable Endless Spool and a writable tool map. With Spoolman on and the
/// mock's seeded material, Select Spool, Scan QR and Clear Spool show too.
std::unique_ptr<AmsBackendMock> fullest_lane_backend() {
    auto backend = std::make_unique<AmsBackendMock>(4);
    backend->set_operation_delay(0);
    backend->set_endless_spool_supported(true);
    backend->set_endless_spool_editable(true);
    backend->set_remap_strategy(AmsBackend::RemapStrategy::Native);
    backend->set_remap_ready(true);
    return backend;
}

/// Spoolman availability for one case, put back on scope exit.
class ScopedSpoolman {
  public:
    ScopedSpoolman() : subj_(lv_xml_get_subject(nullptr, "printer_has_spoolman")) {
        REQUIRE(subj_ != nullptr);
        was_ = lv_subject_get_int(subj_);
        lv_subject_set_int(subj_, 1);
    }
    ~ScopedSpoolman() {
        lv_subject_set_int(subj_, was_);
    }

  private:
    lv_subject_t* subj_;
    int32_t was_ = 0;
};

struct MenuFit {
    int32_t card_h = 0;
    int32_t available_h = 0;
    int32_t scroll_bottom = 0;
    int32_t scroll_right = 0;
    lv_area_t card{};
    std::string layout; ///< each shown column's width and height, for the failure message
};

/// Open the fullest lane menu on slot 2 at @p w x @p h and measure its card.
MenuFit measure_lane_menu(LVGLUITestFixture& fx, int32_t w, int32_t h, bool with_plugins) {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, w, h);
    theme_manager_refresh_layout_constants(disp);

    auto backend = fullest_lane_backend();
    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(fx.test_screen(), /*slot_index=*/2, fx.test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    lv_obj_t* card = lv_obj_find_by_name(fx.test_screen(), "context_menu");
    REQUIRE(card != nullptr);
    lv_obj_update_layout(lv_obj_get_screen(card));
    // Every action this backend offers is on the card being measured.
    for (const char* name : {"btn_gate_check", "btn_preload", "btn_spoolman", "btn_clear_spool"}) {
        INFO(name);
        lv_obj_t* btn = lv_obj_find_by_name(card, name);
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_t* plugins = lv_obj_find_by_name(card, "col_plugins");
    REQUIRE(plugins != nullptr);
    CHECK(lv_obj_has_flag(plugins, LV_OBJ_FLAG_HIDDEN) == !with_plugins);

    MenuFit fit;
    lv_obj_t* columns = lv_obj_find_by_name(card, "menu_columns");
    REQUIRE(columns != nullptr);
    for (uint32_t i = 0; i < lv_obj_get_child_count(columns); ++i) {
        lv_obj_t* col = lv_obj_get_child(columns, static_cast<int32_t>(i));
        if (!lv_obj_has_flag(col, LV_OBJ_FLAG_HIDDEN)) {
            fit.layout += std::string(lv_obj_get_name(col)) + " " +
                          std::to_string(lv_obj_get_width(col)) + "x" +
                          std::to_string(lv_obj_get_height(col)) + "; ";
        }
    }
    fit.card_h = lv_obj_get_height(card);
    fit.available_h =
        lv_obj_get_height(lv_obj_get_parent(card)) - 2 * theme_manager_get_spacing("space_md");
    fit.scroll_bottom = lv_obj_get_scroll_bottom(card);
    fit.scroll_right = lv_obj_get_scroll_right(card);
    lv_obj_get_coords(card, &fit.card);
    menu.hide();
    fx.process_lvgl(20);
    return fit;
}

void check_fits(const MenuFit& fit, int32_t w, int32_t h) {
    CAPTURE(w, h, fit.card_h, fit.available_h, fit.scroll_bottom, fit.scroll_right, fit.layout);
    CHECK(fit.card_h <= fit.available_h);
    CHECK(fit.scroll_bottom <= 0);
    CHECK(fit.scroll_right <= 0);
    CHECK(fit.card.x1 >= 0);
    CHECK(fit.card.y1 >= 0);
    CHECK(fit.card.x2 < w);
    CHECK(fit.card.y2 < h);
}

// The fixture's theme keeps the 800x480 body font at every size, so its rows
// measure wider than a device's. The landscape screens absorb that; 320x480 does
// not, so its flow is pinned with measured sizes in decide_column_flow's test.
constexpr std::pair<int32_t, int32_t> kMenuSizes[] = {{800, 480}, {480, 320}, {480, 272}};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: a lane menu is actions only",
                 "[ui][ams][context_menu][slim]") {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    ScopedSpoolman spoolman;
    auto backend = fullest_lane_backend();
    // The backend offers everything the menu's old rows were for: slot select,
    // a writable tool map and an editable backup.
    REQUIRE(backend->supports_gate_select());
    REQUIRE(lane_settings::show_tool_row(backend.get()));
    REQUIRE(lane_settings::show_backup_row(backend->get_endless_spool_capabilities(),
                                           backend->get_endless_spool_config(), 2));

    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    for (const char* name :
         {"btn_gate_select", "dropdown_divider", "tool_dropdown_row", "tool_dropdown",
          "backup_dropdown_row", "backup_dropdown", "endless_status"}) {
        INFO(name);
        CHECK(lv_obj_find_by_name(test_screen(), name) == nullptr);
    }
    // Check Slot and Preload stay: they act on the lane the menu is open on.
    for (const char* name : {"btn_gate_check", "btn_preload"}) {
        INFO(name);
        lv_obj_t* btn = lv_obj_find_by_name(test_screen(), name);
        REQUIRE(btn != nullptr);
        CHECK_FALSE(lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN));
    }
    menu.hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsContextMenu: a lane menu fits without scrolling",
                 "[ui][ams][context_menu][slim]") {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    ScopedSpoolman spoolman;
    for (const auto& [w, h] : kMenuSizes) {
        check_fits(measure_lane_menu(*this, w, h, /*with_plugins=*/false), w, h);
    }
    theme_manager_refresh_layout_constants(lv_display_get_default());
}

#if HELIX_HAS_PLUGINS
TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: a lane menu with plugin items at the cap fits without scrolling",
                 "[ui][ams][context_menu][slim][plugin]") {
    // Two plugins with more filament_slot items between them than the cap
    // allows, so the plugin column holds its most.
    helix::plugin::test::TempDir tmp;
    std::string four;
    for (int i = 0; i < 4; ++i)
        four += std::string(i ? "," : "") + R"({"id":"i)" + std::to_string(i) +
                R"(","menu":"filament_slot","label":"Slot action )" + std::to_string(i) +
                R"(","handler":"go"})";
    for (const char* id : {"fit-a", "fit-b"}) {
        std::filesystem::create_directories(tmp.path / id);
        std::ofstream(tmp.path / id / "manifest.json")
            << R"({"id":")" << id << R"(","name":")" << id
            << R"(","version":"1.0.0","menu_items":[)" << four << "]}";
        std::ofstream(tmp.path / id / "main.lua") << R"(helix.ui.on("go", function() end))";
    }
    helix::plugin::test::HostRig rig(
        helix::plugin::test::enabled_all({{"fit-a", {}}, {"fit-b", {}}}));
    rig.host->load_from(tmp.path.string());
    REQUIRE(helix::plugin_menu::any_declared(helix::plugin_menu::MenuKind::FilamentSlot));

    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    ScopedSpoolman spoolman;
    for (const auto& [w, h] : kMenuSizes) {
        check_fits(measure_lane_menu(*this, w, h, /*with_plugins=*/true), w, h);
    }
    theme_manager_refresh_layout_constants(lv_display_get_default());
}
#endif // HELIX_HAS_PLUGINS

#if HELIX_HAS_PLUGINS
namespace {

/// One enabled plugin with two filament_slot items, the first long enough to
/// ellipsize in a narrow column.
struct SlotDemoPlugin {
    helix::plugin::test::TempDir tmp;
    std::unique_ptr<helix::plugin::test::HostRig> rig;

    SlotDemoPlugin() {
        std::filesystem::create_directories(tmp.path / "slot-demo");
        std::ofstream(tmp.path / "slot-demo" / "manifest.json")
            << R"({"id":"slot-demo","name":"Slot Demo","version":"1.0.0","menu_items":[
                 {"id":"i","menu":"filament_slot","label":"Inspect this slot","icon":"tune",
                  "handler":"go"},
                 {"id":"t","menu":"filament_slot","label":"Tag slot","handler":"go"}]})";
        std::ofstream(tmp.path / "slot-demo" / "main.lua")
            << R"(helix.ui.on("go", function() end))";
        rig = std::make_unique<helix::plugin::test::HostRig>(
            helix::plugin::test::enabled("slot-demo", {}));
        rig->host->load_from(tmp.path.string());
    }
};

int32_t screen_top(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.y1;
}

/// The column's visible children that are not its heading, in order.
std::vector<lv_obj_t*> column_rows(lv_obj_t* col) {
    std::vector<lv_obj_t*> rows;
    for (uint32_t i = 0; i < lv_obj_get_child_count(col); ++i) {
        lv_obj_t* child = lv_obj_get_child(col, static_cast<int32_t>(i));
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            rows.push_back(child);
        }
    }
    return rows;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: the plugin column lines up with the action columns",
                 "[ui][ams][context_menu][slim][plugin]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    SlotDemoPlugin plugin;
    ScopedSpoolman spoolman;
    auto backend = fullest_lane_backend();
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") == LV_RESULT_OK);
    AmsContextMenu menu;
    REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                  /*is_loaded=*/false, backend.get()));
    lv_obj_t* spool = lv_obj_find_by_name(test_screen(), "col_spool");
    lv_obj_t* plugins = lv_obj_find_by_name(test_screen(), "col_plugins");
    REQUIRE(spool);
    REQUIRE(plugins);
    lv_obj_update_layout(lv_obj_get_screen(plugins));
    REQUIRE(screen_top(plugins) == screen_top(spool)); // side by side

    // The same heading: its label and its rule at the same heights.
    lv_obj_t* spool_heading = lv_obj_find_by_name(spool, "col_heading");
    lv_obj_t* plugin_heading = lv_obj_get_child(plugins, 0);
    REQUIRE(spool_heading);
    CHECK(screen_top(lv_obj_find_by_name(plugin_heading, "heading_text")) ==
          screen_top(lv_obj_find_by_name(spool_heading, "heading_text")));
    REQUIRE(lv_obj_get_child_count(plugin_heading) == 2);
    CHECK(screen_top(lv_obj_get_child(plugin_heading, 1)) ==
          screen_top(lv_obj_get_child(spool_heading, 1)));
    CHECK(lv_obj_get_height(lv_obj_get_child(plugin_heading, 1)) == 1);

    // Row for row: same tops, same height.
    const auto spool_rows = column_rows(spool);
    const auto plugin_rows = column_rows(plugins);
    REQUIRE(plugin_rows.size() == 2);
    REQUIRE(spool_rows.size() >= 2);
    for (size_t i = 0; i < plugin_rows.size(); ++i) {
        INFO("row " << i);
        CHECK(screen_top(plugin_rows[i]) == screen_top(spool_rows[i]));
        CHECK(lv_obj_get_height(plugin_rows[i]) == lv_obj_get_height(spool_rows[i]));
    }
    menu.hide();
    process_lvgl(20);
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsContextMenu: a plugin column wrapped below the others takes the card's width",
                 "[ui][ams][context_menu][slim][plugin]") {
    lv_display_t* disp = lv_display_get_default();
    {
        ScopedResolution res(disp, 320, 480);
        theme_manager_refresh_layout_constants(disp);
        SlotDemoPlugin plugin;
        ScopedSpoolman spoolman;
        auto backend = fullest_lane_backend();
        REQUIRE(lv_xml_register_component_from_file("A:ui_xml/ams_context_menu.xml") ==
                LV_RESULT_OK);
        AmsContextMenu menu;
        REQUIRE(menu.show_near_widget(test_screen(), /*slot_index=*/2, test_screen(),
                                      /*is_loaded=*/false, backend.get()));
        lv_obj_t* columns = lv_obj_find_by_name(test_screen(), "menu_columns");
        lv_obj_t* filament = lv_obj_find_by_name(test_screen(), "col_filament");
        lv_obj_t* plugins = lv_obj_find_by_name(test_screen(), "col_plugins");
        REQUIRE(columns);
        REQUIRE(filament);
        REQUIRE(plugins);
        lv_obj_update_layout(lv_obj_get_screen(columns));
        REQUIRE(lv_obj_get_style_flex_flow(columns, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP);
        REQUIRE(screen_top(plugins) > screen_top(filament));

        CHECK(lv_obj_get_width(plugins) == lv_obj_get_content_width(columns));
        lv_obj_t* label = ui_button_get_label(lv_obj_find_by_name(plugins, "item_button"));
        REQUIRE(label);
        const int32_t need = helix::ui::text_width(
            "Inspect this slot", lv_obj_get_style_text_font(label, LV_PART_MAIN),
            lv_obj_get_style_text_letter_space(label, LV_PART_MAIN));
        CAPTURE(lv_obj_get_width(plugins), lv_obj_get_width(label), need);
        CHECK(lv_obj_get_width(label) >= need);
        // Its text starts beside the icon, as the action rows' do, not centred
        // in the wider row.
        CHECK(lv_obj_get_style_text_align(label, LV_PART_MAIN) == LV_TEXT_ALIGN_LEFT);
        menu.hide();
        process_lvgl(20);
    }
    theme_manager_refresh_layout_constants(disp);
}
#endif // HELIX_HAS_PLUGINS
