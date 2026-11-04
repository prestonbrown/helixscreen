// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_per_slot_loaded.cpp
 * @brief Generic klipper-toolchanger per-slot load gating (#1199).
 *
 * The last backend of the #1199 sweep, and the only PathTopology::PARALLEL one.
 * It stays on the aggregate rule, for a reason the other backends never had:
 * this backend has no filament signal at all. get_slot_filament_segment()
 * returns NOZZLE unconditionally, no per-tool switch is read, and
 * is_filament_loaded() is nothing more than `tool_number >= 0`. The only fact
 * the parse can state is WHICH TOOL IS ON THE CARRIAGE, and that is
 * single-valued — exactly what the aggregate current_slot + filament_loaded pair
 * encodes, assigned verbatim from klipper-toolchanger's own
 * `toolchanger.tool_number`.
 *
 * Two things this pins down:
 *
 *  - `tool <name>.mounted` is NOT that authority. It arrives on a different
 *    Moonraker object, and an all-tools-mounted payload is a shape we already
 *    emit ourselves (moonraker_client_mock_objects.cpp gives every `tool T<n>`
 *    `mounted: true`). The old parse wrote `mounted ? LOADED : AVAILABLE`
 *    straight into slot.status from that object alone, so such a payload marked
 *    every tool LOADED — the state that would make an opt-in report every tool
 *    as the active one. The stamp is now derived from the carriage tool on both
 *    parse paths, so the two writers cannot disagree.
 *
 *  - The base PARALLEL arm of can_unload_from_toolhead() keys on is_present(),
 *    and a toolchanger slot is never EMPTY or UNKNOWN — it is a physical
 *    toolhead. So it read true for every tool forever, which through
 *    AmsContextMenu::decide_can_load()'s `!toolhead_unload` factor left Load
 *    permanently disabled on every tool, and through decide_unload_mode()
 *    offered Unload (`UNSELECT_TOOL T=<n>`) on tools parked in their docks.
 *    Unmounting is only meaningful for the tool actually on the carriage.
 */

#include "ams_backend_toolchanger.h"
#include "ams_types.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

/// Drives the real backend's protected notify_status_update handler.
class ToolChangerLoadHelper : public helix::AmsBackendToolChanger {
  public:
    explicit ToolChangerLoadHelper(int tool_count)
        : helix::AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(tool_count));
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
    }

    /// Feed one notify_status_update params object.
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    /// The all-tools-mounted shape our own mock emits.
    static json every_tool_mounted(int tool_count, bool mounted = true) {
        json status = json::object();
        for (int i = 0; i < tool_count; ++i) {
            status["tool T" + std::to_string(i)] = json{{"mounted", mounted}, {"active", false}};
        }
        return status;
    }
};

} // namespace

// ============================================================================
// The authority seam — deliberately NOT claimed
// ============================================================================

TEST_CASE("Toolchanger stays on the aggregate load rule", "[ams][toolchanger][1199]") {
    // Not a style choice: there is no per-tool filament signal for a per-slot
    // rule to be authoritative about. See the file comment.
    ToolChangerLoadHelper backend(4);
    CHECK_FALSE(backend.has_per_slot_loaded_authority());
}

// ============================================================================
// toolchanger.tool_number is the one authority
// ============================================================================

TEST_CASE("Toolchanger reports only the carriage tool actively loaded",
          "[ams][toolchanger][1199]") {
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"status", "ready"}, {"tool_number", 2}}}});

    REQUIRE(backend.get_current_slot() == 2);
    REQUIRE(backend.is_filament_loaded());

    CHECK(backend.slot_is_actively_loaded(2));
    CHECK_FALSE(backend.slot_is_actively_loaded(0));
    CHECK_FALSE(backend.slot_is_actively_loaded(1));
    CHECK_FALSE(backend.slot_is_actively_loaded(3));

    // The per-slot stamp agrees with the aggregate, so a future opt-in cannot
    // silently blank the active-tool highlight.
    CHECK(backend.get_slot_info(2).status == helix::SlotStatus::LOADED);
    CHECK(backend.get_slot_info(0).status == helix::SlotStatus::AVAILABLE);
    CHECK(backend.get_slot_info(1).status == helix::SlotStatus::AVAILABLE);
    CHECK(backend.get_slot_info(3).status == helix::SlotStatus::AVAILABLE);
}

TEST_CASE("Toolchanger with no tool on the carriage reports nothing loaded",
          "[ams][toolchanger][1199]") {
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 1}}}});
    REQUIRE(backend.slot_is_actively_loaded(1));

    // -1 is klipper-toolchanger's "all tools docked".
    backend.feed(json{{"toolchanger", {{"tool_number", -1}}}});

    CHECK_FALSE(backend.is_filament_loaded());
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK_FALSE(backend.slot_is_actively_loaded(i));
        CHECK(backend.get_slot_info(i).status == helix::SlotStatus::AVAILABLE);
    }
}

TEST_CASE("Toolchanger moves the load stamp on a tool change", "[ams][toolchanger][1199]") {
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 0}}}});
    REQUIRE(backend.slot_is_actively_loaded(0));
    REQUIRE(backend.get_slot_info(0).status == helix::SlotStatus::LOADED);

    backend.feed(json{{"toolchanger", {{"tool_number", 3}}}});

    CHECK(backend.slot_is_actively_loaded(3));
    CHECK(backend.get_slot_info(3).status == helix::SlotStatus::LOADED);
    CHECK_FALSE(backend.slot_is_actively_loaded(0));
    CHECK(backend.get_slot_info(0).status == helix::SlotStatus::AVAILABLE);
}

TEST_CASE("Toolchanger out-of-range slots are false, not a crash", "[ams][toolchanger][1199]") {
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 1}}}});

    CHECK_FALSE(backend.slot_is_actively_loaded(-1));
    CHECK_FALSE(backend.slot_is_actively_loaded(99));
    CHECK_FALSE(backend.can_unload_from_toolhead(-1));
    CHECK_FALSE(backend.can_unload_from_toolhead(99));
}

// ============================================================================
// `mounted` is not that authority
// ============================================================================

TEST_CASE("Toolchanger with mounted true on every tool still names one loaded",
          "[ams][toolchanger][1199]") {
    // The literal payload moonraker_client_mock_objects.cpp emits. The old parse
    // wrote LOADED into all four slots from this alone.
    ToolChangerLoadHelper backend(4);
    json status = ToolChangerLoadHelper::every_tool_mounted(4);
    status["toolchanger"] = json{{"status", "ready"}, {"tool_number", 1}};
    backend.feed(status);

    CHECK(backend.slot_is_actively_loaded(1));
    CHECK(backend.get_slot_info(1).status == helix::SlotStatus::LOADED);
    for (int i : {0, 2, 3}) {
        CAPTURE(i);
        CHECK_FALSE(backend.slot_is_actively_loaded(i));
        CHECK(backend.get_slot_info(i).status == helix::SlotStatus::AVAILABLE);
        CHECK_FALSE(backend.can_unload_from_toolhead(i));
    }
}

TEST_CASE("Toolchanger tool objects alone never name a loaded tool", "[ams][toolchanger][1199]") {
    // A `tool T<n>` delta with no toolchanger frame behind it. mounted lives on
    // a separate object and cannot be promoted to seating authority on its own.
    ToolChangerLoadHelper backend(4);
    backend.feed(ToolChangerLoadHelper::every_tool_mounted(4));

    CHECK_FALSE(backend.is_filament_loaded());
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK_FALSE(backend.slot_is_actively_loaded(i));
        CHECK(backend.get_slot_info(i).status == helix::SlotStatus::AVAILABLE);
    }
}

TEST_CASE("Toolchanger keeps the carriage tool loaded across a tool-only delta",
          "[ams][toolchanger][1199]") {
    // toolchanger and `tool T<n>` arrive in independent deltas — the staleness
    // hole that bit Happy Hare (599c13365). A tool-only frame must not demote
    // the tool the carriage frame named.
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 2}}}});
    REQUIRE(backend.get_slot_info(2).status == helix::SlotStatus::LOADED);

    backend.feed(ToolChangerLoadHelper::every_tool_mounted(4, /*mounted=*/false));

    CHECK(backend.get_slot_info(2).status == helix::SlotStatus::LOADED);
    CHECK(backend.slot_is_actively_loaded(2));
    CHECK(backend.can_unload_from_toolhead(2));
}

// ============================================================================
// Unmount is offered for the carriage tool only
// ============================================================================

TEST_CASE("Toolchanger offers Unload only for the tool on the carriage",
          "[ams][toolchanger][1199]") {
    // can_unload_from_toolhead() gates the context menu's Unload AND, inverted
    // through decide_can_load()'s !toolhead_unload factor, its Load. The base
    // PARALLEL arm returns is_present(), which every toolchanger slot satisfies
    // forever, so Load was disabled on all four tools.
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 2}}}});

    CHECK(backend.can_unload_from_toolhead(2));
    CHECK_FALSE(backend.can_unload_from_toolhead(0));
    CHECK_FALSE(backend.can_unload_from_toolhead(1));
    CHECK_FALSE(backend.can_unload_from_toolhead(3));

    // Every slot is still is_present() — the state the base rule read.
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(backend.get_slot_info(i).is_present());
    }
}

TEST_CASE("Toolchanger offers Unload on no tool when all are docked", "[ams][toolchanger][1199]") {
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", -1}}}});

    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK_FALSE(backend.can_unload_from_toolhead(i));
    }
}

TEST_CASE("Toolchanger unmount is a toolhead unload with no cold alternative",
          "[ams][toolchanger][1199]") {
    // slot_unloads_to_toolhead() stays on the base rule: unload_filament() sends
    // UNSELECT_TOOL, a real toolhead operation, never a cold lane eject. With no
    // eject and no lane recovery, decide_unload_mode() lands on Unavailable for a
    // docked tool — the right answer, since there is nothing to unmount.
    ToolChangerLoadHelper backend(4);
    backend.feed(json{{"toolchanger", {{"tool_number", 2}}}});

    CHECK(backend.slot_unloads_to_toolhead(2, backend.can_unload_from_toolhead(2)));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, backend.can_unload_from_toolhead(0)));

    CHECK_FALSE(backend.supports_lane_eject());
    CHECK_FALSE(backend.supports_force_eject());
}
