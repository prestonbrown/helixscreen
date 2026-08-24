// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_ace_per_slot_loaded.cpp
 * @brief ACE per-slot load authority (#1199).
 *
 * ACE's only SlotStatus::LOADED write used to live in load_filament()'s
 * gcode-success callback, and the next status frame unconditionally overwrote
 * it — so no ACE slot was ever LOADED for longer than one poll. Since ACE is
 * HUB topology, the inherited can_unload_from_toolhead() (which keys on that
 * status) was effectively always false and the AMS panel never offered Unload.
 *
 * The fix is NOT to remap the firmware's per-slot "loaded" string. That string
 * only exists in the community ValgACE dialect, where it sits in the same
 * enumeration as "available" and "ready" — a slot-local spool state, the same
 * trap as AFC's lane status "Loaded" meaning loaded-to-hub. Native Anycubic
 * GoKlipper has no per-slot "loaded" at all and answers the seated question
 * with the separate top-level `current_filament`. So slot_status_from_string()
 * is left alone, and instead every parse path that resolves the seated slot —
 * the ValgACE "loaded" scan, `loaded_slot`, and `current_filament` — now stamps
 * SlotStatus::LOADED on the slot it arbitrated to.
 */

#include "ams_backend_ace.h"
#include "ams_types.h"

#include <json.hpp> // nlohmann/json from libhv
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/// Drives the production paths: the WebSocket object path via
/// handle_status_update(), and the REST fallback via the two poll parsers.
class AcePerSlotLoadedHelper : public AmsBackendAce {
  public:
    AcePerSlotLoadedHelper() : AmsBackendAce(nullptr, nullptr) {}

    /// Wrap an ACE object in a notify_status_update envelope under `key`
    /// ("filament_hub" for native GoKlipper, "ace" for community ValgACE).
    void feed(const std::string& key, const json& ace_obj) {
        json notification;
        notification["params"] = json::array({json{{key, ace_obj}}, 0.0});
        handle_status_update(notification);
    }

    bool feed_rest_status(const json& data) {
        return parse_status_response(data);
    }

    bool feed_rest_slots(const json& data) {
        return parse_slots_response(data);
    }

    void force_aggregate(int slot, bool loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.current_slot = slot;
        system_info_.filament_loaded = loaded;
    }
};

json native_slot(int index, const std::string& status) {
    return json{{"index", index},
                {"status", status},
                {"sku", ""},
                {"type", "PLA"},
                {"color", json::array({255, 0, 0})}};
}

/// Native Anycubic GoKlipper `filament_hub`: per-slot vocabulary is
/// empty/ready/preload/running/runout, and `current_filament` ("<unit>-<local>")
/// is the only field that names the seated slot.
json native_hub(const std::string& current_filament) {
    return json{
        {"model", "ACE Pro"},
        {"firmware", "1.0.0"},
        {"status", "ready"},
        {"slots", json::array({native_slot(0, "empty"), native_slot(1, "ready"),
                               native_slot(2, "ready"), native_slot(3, "empty")})},
        {"current_filament", current_filament},
    };
}

json valgace_slot(const std::string& status) {
    return json{{"status", status}, {"type", "PLA"}, {"color", json::array({0, 255, 0})}};
}

/// Community ValgACE `ace`: no top-level seated field at all — the per-slot
/// "loaded" string is the only seated signal it publishes.
json valgace(const std::vector<std::string>& statuses) {
    json slots = json::array();
    for (const auto& s : statuses) {
        slots.push_back(valgace_slot(s));
    }
    return json{{"model", "ACE Pro"}, {"firmware", "valg"}, {"status", "ready"}, {"slots", slots}};
}

} // namespace

// ============================================================================
// The authority seam
// ============================================================================

TEST_CASE("ACE claims per-slot load authority", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;
    CHECK(ace.has_per_slot_loaded_authority());
}

// ============================================================================
// Native GoKlipper: current_filament is the seated signal
// ============================================================================

TEST_CASE("ACE stamps LOADED on the slot current_filament names", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;
    ace.feed("filament_hub", native_hub("0-1"));

    REQUIRE(ace.get_current_slot() == 1);
    REQUIRE(ace.is_filament_loaded());

    CHECK(ace.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(ace.slot_is_actively_loaded(1));
    CHECK(ace.can_unload_from_toolhead(1));

    // Slot 2 reports the same firmware string ("ready") but is not seated.
    CHECK(ace.get_slot_info(2).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(ace.slot_is_actively_loaded(2));
    CHECK_FALSE(ace.can_unload_from_toolhead(2));

    CHECK(ace.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK_FALSE(ace.slot_is_actively_loaded(0));
}

TEST_CASE("ACE LOADED stamp survives a status frame and follows a toolchange", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;
    ace.feed("filament_hub", native_hub("0-1"));
    REQUIRE(ace.get_slot_info(1).status == SlotStatus::LOADED);

    // The parse rewrites every slot's status from the firmware vocabulary on
    // each frame; the stamp has to be re-derived afterwards or it lasts exactly
    // one poll (the pre-#1199 behaviour).
    ace.feed("filament_hub", native_hub("0-1"));
    CHECK(ace.get_slot_info(1).status == SlotStatus::LOADED);

    ace.feed("filament_hub", native_hub("0-2"));
    CHECK(ace.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(ace.get_slot_info(1).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(ace.slot_is_actively_loaded(1));
}

// ============================================================================
// ValgACE: the per-slot "loaded" string, arbitrated
// ============================================================================

TEST_CASE("ACE maps ValgACE's seated slot to LOADED without remapping the vocabulary",
          "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;

    SECTION("the slot the aggregate arbitrates to becomes LOADED") {
        ace.feed("ace", valgace({"available", "loaded", "empty", "ready"}));

        REQUIRE(ace.get_current_slot() == 1);
        CHECK(ace.get_slot_info(1).status == SlotStatus::LOADED);
        CHECK(ace.can_unload_from_toolhead(1));

        CHECK(ace.get_slot_info(0).status == SlotStatus::AVAILABLE);
        CHECK(ace.get_slot_info(2).status == SlotStatus::EMPTY);
        CHECK(ace.get_slot_info(3).status == SlotStatus::AVAILABLE);
    }

    SECTION("only one slot may be seated even if firmware says 'loaded' twice") {
        // The aggregate scan takes the first "loaded" and stops. A HUB backend
        // has one toolhead, so a second LOADED stamp would light two
        // active-lane highlights at once.
        ace.feed("ace", valgace({"loaded", "loaded", "empty", "empty"}));

        REQUIRE(ace.get_current_slot() == 0);
        CHECK(ace.get_slot_info(0).status == SlotStatus::LOADED);
        CHECK(ace.get_slot_info(1).status == SlotStatus::AVAILABLE);
        CHECK_FALSE(ace.slot_is_actively_loaded(1));
    }

    SECTION("the shared vocabulary map is untouched — 'loaded' alone is AVAILABLE") {
        // Guard against "fixing" this by remapping the string: the seated slot
        // is chosen by arbitration, not by the per-slot token. Slot 1 says
        // "loaded" but slot 0's earlier "loaded" won the toolhead.
        ace.feed("ace", valgace({"loaded", "loaded", "empty", "empty"}));
        CHECK(ace.get_slot_info(1).status == SlotStatus::AVAILABLE);
        CHECK(ace.get_slot_info(1).is_present());
    }
}

TEST_CASE("ACE clears the LOADED stamp when nothing is seated", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;
    ace.feed("ace", valgace({"available", "loaded", "empty", "ready"}));
    REQUIRE(ace.get_slot_info(1).status == SlotStatus::LOADED);

    // Every slot drops back to a spool-present state and the aggregate is
    // cleared explicitly via loaded_slot: -1.
    json unloaded = valgace({"available", "available", "empty", "ready"});
    unloaded["loaded_slot"] = -1;
    ace.feed("ace", unloaded);

    REQUIRE_FALSE(ace.is_filament_loaded());
    for (int i = 0; i < 4; ++i) {
        CHECK(ace.get_slot_info(i).status != SlotStatus::LOADED);
        CHECK_FALSE(ace.slot_is_actively_loaded(i));
        CHECK_FALSE(ace.can_unload_from_toolhead(i));
    }
}

// ============================================================================
// REST fallback: /status and /slots are separate polls
// ============================================================================

TEST_CASE("ACE REST /slots poll does not wipe the seated stamp from /status", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;

    json slots{{"slots", json::array({json{{"status", "ready"}, {"material", "PLA"}},
                                      json{{"status", "ready"}, {"material", "PETG"}},
                                      json{{"status", "empty"}}})}};

    ace.feed_rest_slots(slots);
    ace.feed_rest_status(json{{"loaded_slot", 1}});

    REQUIRE(ace.get_current_slot() == 1);
    CHECK(ace.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(ace.can_unload_from_toolhead(1));

    // /slots polls on its own cadence and carries no seated field. Re-parsing
    // it must not demote the seated slot back to AVAILABLE.
    ace.feed_rest_slots(slots);
    CHECK(ace.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(ace.can_unload_from_toolhead(1));

    // ...and must not report a change every poll either, which would emit a
    // STATE_CHANGED event twice a second forever.
    CHECK_FALSE(ace.feed_rest_slots(slots));

    // Clearing the seated slot releases the stamp.
    ace.feed_rest_status(json{{"loaded_slot", -1}});
    CHECK(ace.get_slot_info(1).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(ace.slot_is_actively_loaded(1));
}

// ============================================================================
// Equivalence with the aggregate rule it replaces
// ============================================================================

TEST_CASE("ACE per-slot rule agrees with the aggregate pair it derives from", "[ams][ace][1199]") {
    AcePerSlotLoadedHelper ace;
    ace.feed("filament_hub", native_hub(""));

    for (int seated : {-1, 0, 1, 2, 3}) {
        for (bool loaded : {false, true}) {
            ace.force_aggregate(seated, loaded);
            // A frame that carries no seated field re-derives the stamp from
            // whatever the aggregate currently holds.
            ace.feed_rest_slots(json{
                {"slots", json::array({json{{"status", "ready"}}, json{{"status", "ready"}},
                                       json{{"status", "ready"}}, json{{"status", "ready"}}})}});

            for (int i = 0; i < 4; ++i) {
                const bool aggregate_answer = (i == seated) && loaded;
                INFO("seated=" << seated << " loaded=" << loaded << " slot=" << i);
                CHECK(ace.slot_is_actively_loaded(i) == aggregate_answer);
            }
        }
    }
}
