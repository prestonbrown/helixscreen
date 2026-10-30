// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_data_tool_keys.cpp
 * @brief Applying lane_data under AFC's laneN → T(n) key change (AFC-Klipper-Add-On #832).
 *
 * Virtual tools let one lane answer to several T(n) macros, so dev/1.3 keys
 * each record by the T(n) mapping itself, one record per mapped tool, with no
 * lane identity inside. The join is tool → slot, through the mapping the status
 * path builds from lane "map"/"current_map". The DB path must not lose the
 * payload:
 *   - T(n) records apply once a tool mapping exists (even non-identity ones)
 *   - a T(n)-only payload never bootstraps the slot registry (keys are tools)
 *   - a payload that arrives before any mapping is parked and replayed when
 *     the mapping lands — query_lane_data() is one-shot and never retried
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "test_helpers/afc_lane_feed.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::feed_lane_data;
using helix::test::feed_lane_status;

namespace {

/// A dev-shaped record: keyed by T(n), lane identity only implicit in the key.
nlohmann::json tool_record(const char* color, const char* material, const char* name,
                           const char* vendor, int spool_id) {
    return {{"color", color},
            {"material", material},
            {"name", name},
            {"vendor_name", vendor},
            {"spool_id", spool_id}};
}

constexpr uint32_t kDefaultColor = kAmsDefaultSlotColor;

} // namespace

TEST_CASE("T-keyed lane_data applies through the tool mapping", "[afc][lane_data][tool_keys]") {
    AmsBackendAfc afc(nullptr, nullptr);
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    // Non-identity numbering on purpose: T5 → lane2 proves the join goes
    // through the mapping, not through index arithmetic.
    feed_lane_status(afc, "lane1", {{"map", "T0"}});
    feed_lane_status(afc, "lane2", {{"map", "T5"}});

    feed_lane_data(afc, {
                            {"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
                            {"T5", tool_record("#00FF00", "PETG", "Green PETG", "Prusament", 9)},
                        });

    CHECK(afc.get_slot_info(0).color_rgb == 0xFF0000);
    CHECK(afc.get_slot_info(0).spool_name == "Red PLA");
    CHECK(afc.get_slot_info(0).brand == "Overture");
    CHECK(afc.get_slot_info(0).spoolman_id == 7);
    CHECK(afc.get_slot_info(1).color_rgb == 0x00FF00);
    CHECK(afc.get_slot_info(1).material == "PETG");
}

TEST_CASE("multi-mapped lane receives its records from every T key",
          "[afc][lane_data][tool_keys]") {
    AmsBackendAfc afc(nullptr, nullptr);
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    // One lane, two tools — the virtual-tools shape (#605). current_map picks
    // T16 as the active one; both records describe the same spool upstream.
    feed_lane_status(afc, "lane1",
                     {{"map", nlohmann::json::array({"T16", "T17"})}, {"current_map", "T16"}});
    feed_lane_status(afc, "lane2", {{"map", "T1"}});

    feed_lane_data(afc, {
                            {"T16", tool_record("#0000FF", "ASA", "Blue ASA", "Polymaker", 3)},
                            {"T17", tool_record("#0000FF", "ASA", "Blue ASA", "Polymaker", 3)},
                            {"T1", tool_record("#FFFF00", "TPU", "Yellow TPU", "Sunlu", 4)},
                        });

    CHECK(afc.get_slot_info(0).color_rgb == 0x0000FF);
    CHECK(afc.get_slot_info(0).brand == "Polymaker");
    CHECK(afc.get_slot_info(1).color_rgb == 0xFFFF00);
}

TEST_CASE("lane-name keys still apply without any tool mapping", "[afc][lane_data][tool_keys]") {
    AmsBackendAfc afc(nullptr, nullptr);
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    // Old firmware: keys are lane names and arrive before any mapping exists.
    feed_lane_data(afc, {{"lane1", {{"color", "#FF0000"}, {"material", "PLA"}}}});

    CHECK(afc.get_slot_info(0).color_rgb == 0xFF0000);
    CHECK(afc.get_slot_info(0).material == "PLA");
}

TEST_CASE("a T-only payload never bootstraps the slot registry", "[afc][lane_data][tool_keys]") {
    AmsBackendAfc afc(nullptr, nullptr);

    feed_lane_data(afc, {
                            {"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)},
                            {"T1", tool_record("#00FF00", "PETG", "Green PETG", "Prusament", 9)},
                        });

    // Slots are named by lanes (discovery's job). "T0"/"T1" slots here would
    // poison the registry for the whole session — lane data never resolves to
    // them and the status path only iterates slots that exist.
    CHECK(AfcTestAccess::slots(afc).slot_count() == 0);
}

TEST_CASE("T-keyed payload arriving before the mapping is parked and replayed",
          "[afc][lane_data][tool_keys]") {
    AmsBackendAfc afc(nullptr, nullptr);
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    // DB reply wins the race: no lane has a map yet.
    feed_lane_data(afc, {{"T0", tool_record("#FF0000", "PLA", "Red PLA", "Overture", 7)}});
    CHECK(afc.get_slot_info(0).color_rgb == kDefaultColor); // parked, not dropped

    // A mapping for a DIFFERENT tool leaves T0 parked.
    feed_lane_status(afc, "lane2", {{"map", "T1"}});
    CHECK(afc.get_slot_info(0).color_rgb == kDefaultColor);
    CHECK(afc.get_slot_info(1).color_rgb == kDefaultColor);

    // The subscription names lane1's tool; the parked payload must replay.
    feed_lane_status(afc, "lane1", {{"map", "T0"}});
    CHECK(afc.get_slot_info(0).color_rgb == 0xFF0000);
    CHECK(afc.get_slot_info(0).spool_name == "Red PLA");
    CHECK(afc.get_slot_info(0).brand == "Overture");
}
