// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A HelixScreen edit stands on its lane as Pending while its write is in
// flight, and goes when the write echoes, fails, or times out.

#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ams::LaneId;
using helix::ams::Observation;
using helix::ams::ObservationSource;

namespace {

struct LaneFixture : HelixTestFixture {
    LaneFixture() {
        helix::ams::reset_lane_sources();
    }
    ~LaneFixture() override {
        helix::ams::reset_lane_sources();
    }
    const LaneId lane = helix::ams::lane_id_for(0, 1);

    void edit(const Observation& obs) {
        helix::ams::file_pending(lane, obs);
    }
    [[nodiscard]] std::optional<Observation> pending() const {
        return helix::ams::lane_sources(lane).pending;
    }
};

Observation pending_material(std::string material) {
    Observation obs(ObservationSource::Pending);
    obs.material = std::move(material);
    return obs;
}

} // namespace

TEST_CASE_METHOD(LaneFixture, "a pending edit shows over what firmware states", "[lane][pending]") {
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::ingest(lane, firmware);
    edit(pending_material("PETG"));
    CHECK(helix::ams::resolved_lane(lane).material == "PETG");
}

TEST_CASE_METHOD(LaneFixture, "a second edit before the first echoes keeps both",
                 "[lane][pending]") {
    edit(pending_material("PETG"));
    Observation colour(ObservationSource::Pending);
    colour.color_rgb = 0x00FF00;
    edit(colour);
    REQUIRE(pending().has_value());
    CHECK(pending()->material == "PETG");
    CHECK(pending()->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LaneFixture, "the echo withdraws exactly the fields it repeats",
                 "[lane][pending]") {
    Observation edit_both(ObservationSource::Pending);
    edit_both.material = "PETG";
    edit_both.color_rgb = 0x00FF00;
    edit(edit_both);

    SECTION("a firmware frame stating an older value withdraws nothing") {
        Observation stale(ObservationSource::Firmware);
        stale.material = "PLA";
        stale.color_rgb = 0xFF0000;
        helix::ams::ingest(lane, stale);
        REQUIRE(pending().has_value());
        CHECK(pending()->material == "PETG");
        CHECK(helix::ams::resolved_lane(lane).material == "PETG");
    }
    SECTION("firmware echoing the material leaves the colour pending") {
        Observation echo(ObservationSource::Firmware);
        echo.material = "PETG";
        echo.color_rgb = 0xFF0000;
        helix::ams::ingest(lane, echo);
        REQUIRE(pending().has_value());
        CHECK_FALSE(pending()->material.has_value());
        CHECK(pending()->color_rgb == 0x00FF00u);
        CHECK(helix::ams::resolved_lane(lane).color_rgb == 0x00FF00u);
    }
    SECTION("a record echoing both withdraws the edit") {
        Observation echo(ObservationSource::Record);
        echo.material = "PETG";
        echo.color_rgb = 0x00FF00;
        helix::ams::ingest(lane, echo);
        CHECK_FALSE(pending().has_value());
        CHECK(helix::ams::resolved_lane(lane).material == "PETG");
    }
    SECTION("Spoolman and the meter are not echoes") {
        Observation spool(ObservationSource::Spoolman);
        spool.material = "PETG";
        spool.color_rgb = 0x00FF00;
        helix::ams::ingest(lane, spool);
        CHECK(pending().has_value());
    }
}

TEST_CASE_METHOD(LaneFixture, "an unlink is echoed by a record that names no spool",
                 "[lane][pending]") {
    Observation linked(ObservationSource::Record);
    linked.spoolman_id = 42;
    helix::ams::ingest(lane, linked);
    Observation unlink(ObservationSource::Pending);
    unlink.spoolman_id = 0;
    edit(unlink);
    REQUIRE(pending().has_value());
    CHECK(helix::ams::linked_spool_id(helix::ams::lane_sources(lane)) == 0);

    // Firmware silent on the spool says nothing about the unlink.
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::ingest(lane, firmware);
    REQUIRE(pending().has_value());

    // The rewritten record no longer naming a spool is the echo.
    Observation record(ObservationSource::Record);
    record.brand = "Polymaker";
    helix::ams::ingest(lane, record);
    CHECK_FALSE(pending().has_value());
}

TEST_CASE_METHOD(LaneFixture, "a cleared field withdraws the earlier edit of it",
                 "[lane][pending]") {
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::ingest(lane, firmware);
    edit(pending_material("PETG"));
    Observation colour(ObservationSource::Pending);
    colour.color_rgb = 0x00FF00;
    edit(colour);

    edit(pending_material(""));
    REQUIRE(pending().has_value());
    CHECK_FALSE(pending()->material.has_value());
    CHECK(pending()->color_rgb == 0x00FF00u);
    CHECK(helix::ams::resolved_lane(lane).material == "PLA");
}

TEST_CASE_METHOD(LaneFixture, "a colour name cleared beside a new colour stays pending",
                 "[lane][pending]") {
    Observation named(ObservationSource::Record);
    named.color_rgb = 0xFF0000;
    named.color_name = "Red";
    helix::ams::ingest(lane, named);
    Observation pick(ObservationSource::Pending);
    pick.color_rgb = 0x00FF00;
    pick.color_name = "";
    edit(pick);
    CHECK(helix::ams::resolved_lane(lane).color_name == std::string());
}

TEST_CASE_METHOD(LaneFixture, "a weight echo matches within the editor's resolution",
                 "[lane][pending]") {
    Observation weight(ObservationSource::Pending);
    weight.remaining_weight_g = 500.0F;
    edit(weight);
    Observation record(ObservationSource::Record);
    record.remaining_weight_g = 500.05F;
    helix::ams::ingest(lane, record);
    CHECK_FALSE(pending().has_value());
}

TEST_CASE_METHOD(LaneFixture, "a withdrawn edit leaves what Moonraker holds", "[lane][pending]") {
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::ingest(lane, firmware);
    edit(pending_material("PETG"));
    helix::ams::withdraw_pending(lane);
    CHECK_FALSE(pending().has_value());
    CHECK(helix::ams::resolved_lane(lane).material == "PLA");
}

TEST_CASE_METHOD(LaneFixture, "an edit nothing echoes is withdrawn after the timeout",
                 "[lane][pending]") {
    edit(pending_material("PETG"));
    const auto now = std::chrono::steady_clock::now();
    CHECK(helix::ams::expire_pending(now, {lane}).empty());
    CHECK(pending().has_value());
    const auto expired = helix::ams::expire_pending(
        now + helix::ams::kPendingWriteTimeout + std::chrono::seconds(1), {lane});
    REQUIRE(expired.size() == 1);
    CHECK(expired.front() == lane);
    CHECK_FALSE(pending().has_value());
}

TEST_CASE_METHOD(LaneFixture, "ingest refuses a pending edit and file_pending a reading",
                 "[lane][pending]") {
    helix::ams::ingest(lane, pending_material("PETG"));
    CHECK_FALSE(pending().has_value());
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::file_pending(lane, firmware);
    CHECK_FALSE(helix::ams::lane_sources(lane).firmware.has_value());
}

TEST_CASE_METHOD(LaneFixture, "a clear drops the record, the edit, Spoolman and the meter",
                 "[lane][pending]") {
    for (ObservationSource source :
         {ObservationSource::Sensed, ObservationSource::Firmware, ObservationSource::Record,
          ObservationSource::Spoolman, ObservationSource::Metered}) {
        Observation obs(source);
        obs.present = true;
        obs.remaining_weight_g = 1.0F;
        helix::ams::ingest(lane, obs);
    }
    edit(pending_material("PETG"));
    helix::ams::reset_lane_to_machine_readings(lane);
    const auto sources = helix::ams::lane_sources(lane);
    CHECK(sources.sensed.has_value());
    CHECK(sources.firmware.has_value());
    CHECK_FALSE(sources.record.has_value());
    CHECK_FALSE(sources.pending.has_value());
    CHECK_FALSE(sources.spoolman.has_value());
    CHECK_FALSE(sources.metered.has_value());
}

TEST_CASE_METHOD(LaneFixture, "a waiter on a lane with nothing pending hears Echoed at once",
                 "[lane][pending][save_ack]") {
    std::optional<helix::ams::PendingOutcome> heard;
    helix::ams::await_pending(lane, helix::ams::pending_filings(lane),
                              [&](helix::ams::PendingOutcome o, const std::string&) { heard = o; });
    CHECK(heard == helix::ams::PendingOutcome::Echoed);
}

TEST_CASE_METHOD(LaneFixture, "a waiter hears how the pending edit ended",
                 "[lane][pending][save_ack]") {
    std::optional<helix::ams::PendingOutcome> heard;
    std::string why;
    auto listen = [&](helix::ams::PendingOutcome o, const std::string& reason) {
        heard = o;
        why = reason;
    };
    edit(pending_material("PETG"));

    SECTION("the echo") {
        helix::ams::await_pending(lane, helix::ams::pending_filings(lane), listen);
        REQUIRE_FALSE(heard.has_value());
        Observation record(ObservationSource::Record);
        record.material = "PETG";
        helix::ams::ingest(lane, record);
        CHECK(heard == helix::ams::PendingOutcome::Echoed);
    }
    SECTION("a failed write") {
        helix::ams::await_pending(lane, helix::ams::pending_filings(lane), listen);
        helix::ams::withdraw_pending(lane, "database is read-only");
        CHECK(heard == helix::ams::PendingOutcome::Failed);
        CHECK(why == "database is read-only");
    }
    SECTION("the timeout") {
        helix::ams::await_pending(lane, helix::ams::pending_filings(lane), listen);
        helix::ams::expire_pending(std::chrono::steady_clock::now() +
                                       helix::ams::kPendingWriteTimeout + std::chrono::seconds(1),
                                   {lane});
        CHECK(heard == helix::ams::PendingOutcome::TimedOut);
    }
    SECTION("a partial echo leaves the waiter waiting") {
        Observation colour(ObservationSource::Pending);
        colour.color_rgb = 0x00FF00;
        edit(colour);
        helix::ams::await_pending(lane, helix::ams::pending_filings(lane), listen);
        Observation record(ObservationSource::Record);
        record.material = "PETG";
        helix::ams::ingest(lane, record);
        CHECK_FALSE(heard.has_value());
        helix::ams::withdraw_pending(lane);
        CHECK(heard == helix::ams::PendingOutcome::Failed);
    }
}

TEST_CASE_METHOD(LaneFixture, "a waiter that arrives after the edit ended hears how it ended",
                 "[lane][pending][save_ack]") {
    const std::uint64_t before = helix::ams::pending_filings(lane);
    edit(pending_material("PETG"));
    helix::ams::withdraw_pending(lane, "refused");

    std::optional<helix::ams::PendingOutcome> heard;
    std::string why;
    helix::ams::await_pending(lane, before,
                              [&](helix::ams::PendingOutcome o, const std::string& reason) {
                                  heard = o;
                                  why = reason;
                              });
    CHECK(heard == helix::ams::PendingOutcome::Failed);
    CHECK(why == "refused");

    // Asked from after that edit, nothing has been filed: nothing to wait on.
    heard.reset();
    helix::ams::await_pending(lane, helix::ams::pending_filings(lane),
                              [&](helix::ams::PendingOutcome o, const std::string&) { heard = o; });
    CHECK(heard == helix::ams::PendingOutcome::Echoed);
}

TEST_CASE_METHOD(LaneFixture,
                 "a clear-only edit waits on its record write without masking anything",
                 "[lane][pending][save_ack]") {
    Observation firmware(ObservationSource::Firmware);
    firmware.material = "PLA";
    helix::ams::ingest(lane, firmware);
    const std::uint64_t before = helix::ams::pending_filings(lane);
    edit(pending_material(""));

    // Nothing pending is shown: the lane reads what Moonraker holds at once.
    REQUIRE(pending().has_value());
    CHECK_FALSE(pending()->material.has_value());
    CHECK(helix::ams::resolved_lane(lane).material == "PLA");

    std::optional<helix::ams::PendingOutcome> heard;
    std::string why;
    helix::ams::await_pending(lane, before,
                              [&](helix::ams::PendingOutcome o, const std::string& r) {
                                  heard = o;
                                  why = r;
                              });
    // A firmware frame says nothing about whether the record write landed.
    helix::ams::ingest(lane, firmware);
    REQUIRE_FALSE(heard.has_value());

    SECTION("the record read after the write ends it") {
        Observation record(ObservationSource::Record);
        record.brand = "Polymaker";
        helix::ams::ingest(lane, record);
        CHECK(heard == helix::ams::PendingOutcome::Echoed);
    }
    SECTION("a failed write is reported") {
        helix::ams::withdraw_pending(lane, "database is read-only");
        CHECK(heard == helix::ams::PendingOutcome::Failed);
        CHECK(why == "database is read-only");
    }
}

TEST_CASE("linking a spool files no vendor id as an edit, since Spoolman states it",
          "[lane][pending][review]") {
    helix::SlotInfo original;
    helix::SlotInfo linked = original;
    linked.spoolman_id = 42;
    linked.spoolman_vendor_id = 7;
    linked.brand = "Polymaker";
    const Observation obs = helix::ams::user_edit_observation(original, linked);
    CHECK(obs.spoolman_id == 42);
    CHECK_FALSE(obs.spoolman_vendor_id.has_value());
}

TEST_CASE_METHOD(LaneFixture, "a backend's poll expires only its own lanes' edits",
                 "[lane][pending][review]") {
    const LaneId other = helix::ams::lane_id_for(1, 0);
    edit(pending_material("PETG"));
    helix::ams::file_pending(other, pending_material("ABS"));
    const auto later = std::chrono::steady_clock::now() + helix::ams::kPendingWriteTimeout +
                       std::chrono::seconds(1);
    const auto expired = helix::ams::expire_pending(later, {other});
    REQUIRE(expired.size() == 1);
    CHECK(expired.front() == other);
    CHECK(pending().has_value());
    helix::ams::reset_lane_sources();
}

TEST_CASE_METHOD(LaneFixture, "trimming a lane's edits keeps a waiter waiting",
                 "[lane][pending][review]") {
    Observation both(ObservationSource::Pending);
    both.material = "PETG";
    both.color_rgb = 0x00FF00;
    edit(both);
    std::optional<helix::ams::PendingOutcome> heard;
    helix::ams::await_pending(lane, helix::ams::pending_filings(lane),
                              [&](helix::ams::PendingOutcome o, const std::string&) { heard = o; });
    helix::ams::retract_lane_declarations(lane, [](Observation& o) { o.material.reset(); });
    CHECK_FALSE(heard.has_value());
    REQUIRE(pending().has_value());
    CHECK(pending()->color_rgb == 0x00FF00u);
    CHECK_FALSE(pending()->material.has_value());
}
