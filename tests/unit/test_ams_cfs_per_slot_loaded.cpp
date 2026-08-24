// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_cfs_per_slot_loaded.cpp
 * @brief CFS per-slot load authority (#1199).
 *
 * CFS parse used to write only AVAILABLE/EMPTY, so SlotStatus::LOADED never
 * appeared on a CFS slot at all. Two things fell out of that: the
 * `case SlotStatus::LOADED` arm of get_slot_filament_segment() was dead code,
 * and the inherited can_unload_from_toolhead() — which keys on that status for
 * a HUB backend — returned false for *every* CFS slot, so the AMS panel never
 * offered Unload on a loaded bay.
 *
 * The seated slot is the intersection of two signals that arrive on separate
 * Moonraker frames: the per-unit `T{n}.filament` letter names the lane, and
 * `filament_switch_sensor filament_sensor.filament_detected` says whether
 * anything actually reached the toolhead. handle_status_update now derives
 * SlotStatus::LOADED from that pair on every frame, so the per-slot status and
 * the aggregate can no longer disagree.
 *
 * Fixture values follow the K2 Plus box shape documented in
 * CREALITY_K2_SUPPORT.md: per-unit `vender`/`remain_len`/`material_type`/
 * `color_value` arrays of 4, and the active-lane letter in `T{n}.filament`.
 */

#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "config.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

namespace {

/// Feeds the production Moonraker status path (handle_status_update ->
/// parse_box_status), so the parse chain under test is the real one.
class CfsPerSlotLoadedHelper : public AmsBackendCfs {
  public:
    CfsPerSlotLoadedHelper() : AmsBackendCfs(nullptr, nullptr) {}

    void feed(const json& params_inner) {
        json notification;
        notification["params"] = json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    void feed_box(const json& box) {
        feed(json{{"box", box}});
    }

    /// The toolhead switch — sole writer of the aggregate filament_loaded.
    void feed_toolhead_sensor(bool detected) {
        feed(json{{"filament_switch_sensor filament_sensor", {{"filament_detected", detected}}}});
    }

    /// The aggregate pair the legacy rule read. Set directly so a test can
    /// stage a state without a frame that happens to produce it.
    void force_aggregate(int slot, bool loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.current_slot = slot;
        system_info_.filament_loaded = loaded;
    }
};

/// One connected CFS unit. `active_letter` is the T{n}.filament value —
/// "A".."D" for the lane this unit has engaged, "None" when it has none.
/// Bays 0 and 1 carry tagged spools; bays 2 and 3 are empty.
json make_unit(const std::string& active_letter) {
    return json{
        {"state", "1"},
        {"version", "1.0.0"},
        {"sn", "CFS-TEST"},
        {"vender", json::array({"Creality", "Creality", "none", "none"})},
        {"remain_len", json::array({"240", "240", "-1", "-1"})},
        {"material_type", json::array({"101001", "101001", "-1", "-1"})},
        {"color_value", json::array({"FF0000", "00FF00", "-1", "-1"})},
        {"filament", active_letter},
    };
}

/// Single-unit box frame with T1 engaged on `active_letter`.
json make_box(const std::string& active_letter) {
    return json{{"filament", 0}, {"T1", make_unit(active_letter)}};
}

} // namespace

// ============================================================================
// The authority seam
// ============================================================================

TEST_CASE("CFS claims per-slot load authority", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;
    CHECK(cfs.has_per_slot_loaded_authority());
}

// ============================================================================
// The parse stamps LOADED on the seated bay
// ============================================================================

TEST_CASE("CFS parse stamps LOADED on the lane the toolhead is fed from", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;

    // Letter alone is a lane selection, not a seated lane — the toolhead switch
    // is the only thing that says filament actually arrived.
    cfs.feed_box(make_box("B"));

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
        cfs.feed_toolhead_sensor(true);

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
        cfs.feed_toolhead_sensor(true);
        REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

        // Slots are rebuilt wholesale on every box frame, so the stamp has to
        // be re-applied afterwards or it is lost on the next notification.
        cfs.feed_box(make_box("B"));
        CHECK(cfs.get_slot_info(1).status == SlotStatus::LOADED);
        CHECK(cfs.can_unload_from_toolhead(1));
    }
}

TEST_CASE("CFS LOADED stamp follows the active lane across a toolchange", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;
    cfs.feed_box(make_box("A"));
    cfs.feed_toolhead_sensor(true);
    REQUIRE(cfs.get_slot_info(0).status == SlotStatus::LOADED);

    // Bay 1 becomes the engaged lane. Exactly one bay may be LOADED — a stale
    // stamp on bay 0 would light two active-lane highlights at once.
    cfs.feed_box(make_box("B"));

    CHECK(cfs.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(cfs.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK(cfs.slot_is_actively_loaded(1));
    CHECK_FALSE(cfs.slot_is_actively_loaded(0));
}

TEST_CASE("CFS LOADED stamp honours the per-unit global-index offset", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;

    // T2's lane A is global slot 4, not 0 — the offset is (n-1)*4.
    json box{{"filament", 0}, {"T1", make_unit("None")}, {"T2", make_unit("A")}};
    cfs.feed_box(box);
    cfs.feed_toolhead_sensor(true);

    REQUIRE(cfs.get_current_slot() == 4);
    CHECK(cfs.get_slot_info(4).status == SlotStatus::LOADED);
    CHECK(cfs.slot_is_actively_loaded(4));
    CHECK_FALSE(cfs.slot_is_actively_loaded(0));
    CHECK(cfs.get_slot_info(0).status == SlotStatus::AVAILABLE);
}

// ============================================================================
// Un-stamping restores what firmware said, not a guess
// ============================================================================

TEST_CASE("CFS clears the LOADED stamp when the toolhead switch opens", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;
    cfs.feed_box(make_box("B"));
    cfs.feed_toolhead_sensor(true);
    REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

    // Sensor-only frame: the slot vector is NOT rebuilt, so the stamp has to be
    // undone in place or the bay stays LOADED forever after an unload.
    cfs.feed_toolhead_sensor(false);

    CHECK(cfs.get_slot_info(1).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(cfs.slot_is_actively_loaded(1));
    CHECK_FALSE(cfs.can_unload_from_toolhead(1));
}

TEST_CASE("CFS un-stamping restores EMPTY rather than inventing a spool", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;

    // Bay 2 reads EMPTY (no RFID vendor, no remaining length) while the unit
    // still names lane C and the toolhead switch is closed — the spool was
    // pulled out with filament still threaded to the nozzle. The stamp is
    // deliberately applied over EMPTY here: filament IS at the toolhead and
    // the user must still be able to unload it.
    cfs.feed_box(make_box("C"));
    cfs.feed_toolhead_sensor(true);
    REQUIRE(cfs.get_current_slot() == 2);
    REQUIRE(cfs.get_slot_info(2).status == SlotStatus::LOADED);
    REQUIRE(cfs.can_unload_from_toolhead(2));

    // Once the toolhead clears, the bay must fall back to EMPTY — demoting to
    // AVAILABLE would fabricate a spool in a bay firmware called empty.
    cfs.feed_toolhead_sensor(false);
    CHECK(cfs.get_slot_info(2).status == SlotStatus::EMPTY);
    CHECK_FALSE(cfs.get_slot_info(2).is_present());
}

TEST_CASE("CFS drops the LOADED stamp when no unit names an active lane", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;
    cfs.feed_box(make_box("B"));
    cfs.feed_toolhead_sensor(true);
    REQUIRE(cfs.get_slot_info(1).status == SlotStatus::LOADED);

    // T1.filament goes "None" — parse_box_status leaves current_slot at -1 and
    // handle_status_update clears the aggregate. The switch is still closed
    // (filament in the tube), but no bay owns it, so no bay may claim LOADED.
    cfs.feed_box(make_box("None"));

    REQUIRE(cfs.get_current_slot() == -1);
    REQUIRE(cfs.is_filament_loaded());
    for (int i = 0; i < 4; ++i) {
        CHECK(cfs.get_slot_info(i).status != SlotStatus::LOADED);
        CHECK_FALSE(cfs.slot_is_actively_loaded(i));
    }
}

// ============================================================================
// Equivalence with the aggregate rule it replaces
// ============================================================================

TEST_CASE("CFS per-slot rule agrees with the aggregate pair it derives from", "[ams][cfs][1199]") {
    CfsPerSlotLoadedHelper cfs;
    cfs.feed_box(make_box("None"));

    // Opting a backend in must not silently narrow the predicate: for every
    // (current_slot, filament_loaded) pair CFS can report, the per-slot answer
    // has to match what the legacy aggregate rule would have said.
    for (int seated : {-1, 0, 1, 2, 3}) {
        for (bool loaded : {false, true}) {
            cfs.force_aggregate(seated, loaded);
            cfs.feed_toolhead_sensor(loaded); // re-derives without changing it

            for (int i = 0; i < 4; ++i) {
                const bool aggregate_answer = (i == seated) && loaded;
                INFO("seated=" << seated << " loaded=" << loaded << " slot=" << i);
                CHECK(cfs.slot_is_actively_loaded(i) == aggregate_answer);
            }
        }
    }
}
