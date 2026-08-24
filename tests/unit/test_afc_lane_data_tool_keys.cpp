// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_data_tool_keys.cpp
 * @brief parse_lane_data() under AFC's laneN → T(n) key change (AFC-Klipper-Add-On #832).
 *
 * Virtual tools let one lane answer to several T(n) macros, so a lane-name key
 * can no longer name the record: dev/1.3 keys each record by the T(n) mapping
 * itself (AFC_lane.py send_lane_data, "key": T<n>), one record per mapped tool,
 * and the record carries no lane identity inside. Old firmware keeps the
 * lane-name keys.
 *
 * The join is therefore tool → slot, through the same mapping the status path
 * builds from lane "map"/"current_map". These tests pin the three ways the DB
 * path must not silently lose the payload:
 *   - T(n) records apply once a tool mapping exists (even non-identity ones)
 *   - a T(n)-only payload never bootstraps the slot registry (keys are tools)
 *   - a payload that arrives before any mapping is parked and replayed when
 *     the mapping lands — query_lane_data() is one-shot and never retried
 */

#include "ams_backend_afc.h"
#include "ams_types.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

/// A dev-shaped record: keyed by T(n), lane identity only implicit in the key.
nlohmann::json tool_record(const char* color, const char* material, const char* name,
                           const char* vendor, int spool_id) {
    return {{"color", color},
            {"material", material},
            {"name", name},
            {"vendor_name", vendor},
            {"spool_id", spool_id}};
}

constexpr uint32_t kDefaultColor = AMS_DEFAULT_SLOT_COLOR;

/// Namespace scope, not anonymous: ams_backend_afc.h befriends this by name.
/// Slots known (post-discovery), mapping-free unless a stepper delta says
/// otherwise. Pass an empty vector for the pre-discovery registry.
class AfcLaneDataToolKeyHelper : public AmsBackendAfc {
  public:
    explicit AfcLaneDataToolKeyHelper(const std::vector<std::string>& names)
        : AmsBackendAfc(nullptr, nullptr) {
        if (!names.empty()) {
            initialize_slots(names);
        }
    }

    void feed_lane_data(const nlohmann::json& lane_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_lane_data(lane_data);
    }

    /// Drive the live status path — the only source of tool mappings.
    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    [[nodiscard]] int slot_count() const {
        return slots_.slot_count();
    }

    [[nodiscard]] std::string lane_name(int slot_index) const {
        return slots_.name_of(slot_index);
    }

    [[nodiscard]] uint32_t color(int slot_index) const {
        return get_slot_info(slot_index).color_rgb;
    }

    [[nodiscard]] std::string material(int slot_index) const {
        return get_slot_info(slot_index).material;
    }

    [[nodiscard]] std::string spool_name(int slot_index) const {
        return get_slot_info(slot_index).spool_name;
    }

    [[nodiscard]] std::string brand(int slot_index) const {
        return get_slot_info(slot_index).brand;
    }

    [[nodiscard]] int spoolman_id(int slot_index) const {
        return get_slot_info(slot_index).spoolman_id;
    }
};

// ============================================================================
// T(n) keys resolve through the tool mapping
// ============================================================================

TEST_CASE("T-keyed lane_data applies through the tool mapping", "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({"lane1", "lane2"});

    // Non-identity numbering on purpose: T5 → lane2 proves the join goes
    // through the mapping, not through index arithmetic.
    helper.feed_stepper("lane1", {{"map", "T0"}});
    helper.feed_stepper("lane2", {{"map", "T5"}});

    helper.feed_lane_data({
        {"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
        {"T5", tool_record("#00FF00", "PETG", "Green PETG", "Prusament", 9)},
    });

    CHECK(helper.color(0) == 0xFF0000);
    CHECK(helper.material(0) == "PLA");
    CHECK(helper.spool_name(0) == "Red PLA");
    CHECK(helper.brand(0) == "Overture");
    CHECK(helper.spoolman_id(0) == 7);

    CHECK(helper.color(1) == 0x00FF00);
    CHECK(helper.material(1) == "PETG");
    CHECK(helper.brand(1) == "Prusament");
}

TEST_CASE("multi-mapped lane receives its records from every T key",
          "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({"lane1", "lane2"});

    // One lane, two tools — the virtual-tools shape (#605). current_map picks
    // T16 as the active one; both records describe the same spool upstream.
    helper.feed_stepper("lane1",
                        {{"map", nlohmann::json::array({"T16", "T17"})}, {"current_map", "T16"}});
    helper.feed_stepper("lane2", {{"map", "T1"}});

    helper.feed_lane_data({
        {"T16", tool_record("#0000FF", "ASA", "Blue ASA", "Polymaker", 3)},
        {"T17", tool_record("#0000FF", "ASA", "Blue ASA", "Polymaker", 3)},
        {"T1", tool_record("#FFFF00", "TPU", "Yellow TPU", "Sunlu", 4)},
    });

    CHECK(helper.color(0) == 0x0000FF);
    CHECK(helper.material(0) == "ASA");
    CHECK(helper.brand(0) == "Polymaker");
    CHECK(helper.color(1) == 0xFFFF00);
}

TEST_CASE("lane-name keys still apply without any tool mapping", "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({"lane1", "lane2"});

    // Old firmware: keys are lane names and arrive before any mapping exists.
    helper.feed_lane_data({
        {"lane1", {{"color", "#FF0000"}, {"material", "PLA"}}},
    });

    CHECK(helper.color(0) == 0xFF0000);
    CHECK(helper.material(0) == "PLA");
}

// ============================================================================
// Registry safety: T(n) keys are tools, never lanes
// ============================================================================

TEST_CASE("a T-only payload never bootstraps the slot registry", "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({});

    helper.feed_lane_data({
        {"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
        {"T1", tool_record("#00FF00", "PETG", "Green PETG", "Prusament", 9)},
    });

    // Slots are named by lanes (discovery's job). "T0"/"T1" slots here would
    // poison the registry for the whole session — lane data never resolves to
    // them and the status path only iterates slots that exist.
    CHECK(helper.slot_count() == 0);
}

// ============================================================================
// Ordering: the one-shot DB query can land before the first mapping
// ============================================================================

TEST_CASE("T-keyed payload arriving before the mapping is parked and replayed",
          "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({"lane1", "lane2"});

    // DB reply wins the race: no lane has a map yet.
    helper.feed_lane_data({
        {"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
    });
    CHECK(helper.color(0) == kDefaultColor); // parked, not dropped

    // The subscription catches up and names lane1's tool.
    helper.feed_stepper("lane1", {{"map", "T0"}});

    // query_lane_data() is one-shot and never retried, so the parked payload
    // must have been replayed against the new mapping.
    CHECK(helper.color(0) == 0xFF0000);
    CHECK(helper.spool_name(0) == "Red PLA");
    CHECK(helper.brand(0) == "Overture");
}

TEST_CASE("parked payload with no matching mapping stays parked", "[afc][lane_data][tool_keys]") {
    AfcLaneDataToolKeyHelper helper({"lane1", "lane2"});

    helper.feed_lane_data({
        {"T3", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
    });

    // A mapping for a DIFFERENT tool arrives; T3 belongs to nobody.
    helper.feed_stepper("lane2", {{"map", "T1"}});

    CHECK(helper.color(0) == kDefaultColor);
    CHECK(helper.color(1) == kDefaultColor);
    CHECK(helper.spool_name(1).empty());
}
