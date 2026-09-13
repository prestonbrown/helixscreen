// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_echo.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::OwnWriteEchoes;

namespace {

/// A declaration shaped the way user_edit_observation() returns one.
Observation declaration() {
    Observation obs(ObservationSource::LocalUser);
    obs.material = "PETG";
    obs.brand = "Polymaker";
    obs.color_rgb = 0x00FF00u;
    return obs;
}

/// A producer record repeating every field of declaration().
Observation echoed() {
    Observation obs(ObservationSource::VendorCache);
    obs.material = "PETG";
    obs.brand = "Polymaker";
    obs.color_rgb = 0x00FF00u;
    return obs;
}

/// One armed guard on slot 0, declaring declaration() against @p boundary.
OwnWriteEchoes armed(const std::string& boundary = "TAG-A") {
    OwnWriteEchoes echoes;
    echoes.stage(0, declaration());
    echoes.arm(0, boundary);
    return echoes;
}

} // namespace

TEST_CASE("an armed declaration withholds the fields the producer repeats", "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    Observation record = echoed();
    CHECK(echoes.withhold(0, "TAG-A", record) == 3);
    CHECK_FALSE(record.material.has_value());
    CHECK_FALSE(record.brand.has_value());
    CHECK_FALSE(record.color_rgb.has_value());
}

TEST_CASE("a field the producer states differently passes and consumes its declaration",
          "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    // The tag is re-read and reports what is physically printed on it. These
    // are not the values we wrote, so every one is the producer's own
    // statement and must be filed.
    Observation reasserted(ObservationSource::VendorCache);
    reasserted.material = "PLA";
    reasserted.brand = "Snapmaker";
    reasserted.color_rgb = 0xED2C2Cu;
    CHECK(echoes.withhold(0, "TAG-A", reasserted) == 0);
    CHECK(reasserted.material == "PLA");
    CHECK(reasserted.brand == "Snapmaker");
    REQUIRE(reasserted.color_rgb.has_value());
    CHECK(*reasserted.color_rgb == 0xED2C2Cu);

    // Having shown it can say something else, the producer owns these fields
    // from here: our values coming back are no longer ours to withhold, even
    // on the same spool. Without this, a lane whose boundary never moves
    // withholds for the life of the backend.
    Observation later = echoed();
    CHECK(echoes.withhold(0, "TAG-A", later) == 0);
    CHECK(later.material == "PETG");
    CHECK(later.brand == "Polymaker");
    REQUIRE(later.color_rgb.has_value());
    CHECK(*later.color_rgb == 0x00FF00u);
}

TEST_CASE("a field the producer says nothing about leaves its declaration standing",
          "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    // Silence is not a differing value: a frame that mentions one key says
    // nothing about the others.
    Observation partial(ObservationSource::VendorCache);
    partial.material = "PETG";
    CHECK(echoes.withhold(0, "TAG-A", partial) == 1);
    CHECK_FALSE(partial.material.has_value());

    // The brand and the colour were never contradicted, only unmentioned, so
    // both are still declared. So is the material, which the frame above
    // matched: agreeing with a declaration does not consume it.
    Observation full = echoed();
    CHECK(echoes.withhold(0, "TAG-A", full) == 3);
    CHECK_FALSE(full.material.has_value());
    CHECK_FALSE(full.brand.has_value());
    CHECK_FALSE(full.color_rgb.has_value());
}

TEST_CASE("a boundary naming another spool disarms and withholds nothing", "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    Observation swapped = echoed();
    CHECK(echoes.withhold(0, "TAG-B", swapped) == 0);
    CHECK(swapped.material == "PETG");
    CHECK(swapped.brand == "Polymaker");
    REQUIRE(swapped.color_rgb.has_value());
    CHECK(*swapped.color_rgb == 0x00FF00u);

    // Disarmed, not merely skipped for one frame: the original boundary
    // returning does not revive it.
    Observation again = echoed();
    CHECK(echoes.withhold(0, "TAG-A", again) == 0);
    CHECK(again.material == "PETG");
}

TEST_CASE("an empty boundary is the producer saying nothing, not a change", "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    Observation record = echoed();
    CHECK(echoes.withhold(0, "", record) == 3);
    CHECK_FALSE(record.material.has_value());

    // Still armed against the original token.
    Observation next = echoed();
    CHECK(echoes.withhold(0, "TAG-A", next) == 3);
}

TEST_CASE("arming with no token yet ends on the first token that arrives", "[lane][echo]") {
    OwnWriteEchoes echoes = armed("");

    // Nothing has named a spool, so nothing has contradicted the write.
    Observation unread = echoed();
    CHECK(echoes.withhold(0, "", unread) == 3);

    // A first reading is a spool this write was not made against.
    Observation first = echoed();
    CHECK(echoes.withhold(0, "TAG-A", first) == 0);
    CHECK(first.material == "PETG");
}

TEST_CASE("a declaration of nothing this guard can withhold is not armed", "[lane][echo]") {
    OwnWriteEchoes echoes;

    // What user_edit_observation() returns for a binding change: the spool id
    // alone, because linking carries the spool's own colour, brand and
    // material into the same commit and nobody chose those.
    Observation binding(ObservationSource::LocalUser);
    binding.spoolman_id = 42;
    echoes.stage(0, binding);
    echoes.arm(0, "TAG-A");

    Observation record = echoed();
    record.spoolman_id = 42;
    CHECK(echoes.withhold(0, "TAG-A", record) == 0);
    CHECK(record.material == "PETG");
    CHECK(record.brand == "Polymaker");
    REQUIRE(record.spoolman_id.has_value());
    CHECK(*record.spoolman_id == 42);
}

TEST_CASE("a write reseeding a meter does not withhold what the meter reports", "[lane][echo]") {
    OwnWriteEchoes echoes;

    Observation declared(ObservationSource::LocalUser);
    declared.material = "PETG";
    declared.remaining_weight_g = 750.0F;
    declared.total_weight_g = 1000.0F;
    echoes.stage(0, declared);
    echoes.arm(0, "TAG-A");

    Observation record(ObservationSource::VendorCache);
    record.material = "PETG";
    record.remaining_weight_g = 750.0F;
    record.total_weight_g = 1000.0F;
    CHECK(echoes.withhold(0, "TAG-A", record) == 1);
    CHECK_FALSE(record.material.has_value());
    REQUIRE(record.remaining_weight_g.has_value());
    CHECK(*record.remaining_weight_g == 750.0F);
    REQUIRE(record.total_weight_g.has_value());
    CHECK(*record.total_weight_g == 1000.0F);
}

TEST_CASE("a staging that was never armed withholds nothing", "[lane][echo]") {
    OwnWriteEchoes echoes;
    echoes.stage(0, declaration());

    Observation record = echoed();
    CHECK(echoes.withhold(0, "TAG-A", record) == 0);
    CHECK(record.material == "PETG");
}

TEST_CASE("abandoning a staged write leaves nothing to withhold", "[lane][echo]") {
    OwnWriteEchoes echoes;
    echoes.stage(0, declaration());
    echoes.abandon(0);
    echoes.arm(0, "TAG-A");

    Observation record = echoed();
    CHECK(echoes.withhold(0, "TAG-A", record) == 0);
    CHECK(record.material == "PETG");
}

TEST_CASE("a second edit replaces the declaration the first armed", "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    Observation second(ObservationSource::LocalUser);
    second.material = "ABS";
    echoes.stage(0, second);
    echoes.arm(0, "TAG-A");

    Observation record = echoed();
    record.material = "ABS";
    CHECK(echoes.withhold(0, "TAG-A", record) == 1);
    CHECK_FALSE(record.material.has_value());
    CHECK(record.brand == "Polymaker");
    REQUIRE(record.color_rgb.has_value());
    CHECK(*record.color_rgb == 0x00FF00u);
}

TEST_CASE("slots do not share a declaration", "[lane][echo]") {
    OwnWriteEchoes echoes = armed();

    Observation other = echoed();
    CHECK(echoes.withhold(1, "TAG-A", other) == 0);
    CHECK(other.material == "PETG");

    Observation mine = echoed();
    CHECK(echoes.withhold(0, "TAG-A", mine) == 3);
}

TEST_CASE("pruning the staging drops the fields the write did not carry", "[lane][echo]") {
    OwnWriteEchoes echoes;
    echoes.stage(0, declaration());

    // The caller's write omitted the brand, so firmware kept what it had and
    // what comes back is firmware's.
    auto* staged = echoes.staged(0);
    REQUIRE(staged != nullptr);
    staged->brand.reset();
    echoes.arm(0, "TAG-A");

    Observation record = echoed();
    CHECK(echoes.withhold(0, "TAG-A", record) == 2);
    CHECK(record.brand == "Polymaker");
    CHECK_FALSE(record.material.has_value());
    CHECK_FALSE(record.color_rgb.has_value());
}

TEST_CASE("a relocated declaration withholds the field the read path spells it under",
          "[lane][echo]") {
    OwnWriteEchoes echoes;

    Observation declared(ObservationSource::LocalUser);
    declared.spool_name = "Matte";
    echoes.stage(0, declared);

    // The write sends the user's spool_name under a key the read path files
    // as the product line, so the declaration has to move with it.
    auto* staged = echoes.staged(0);
    REQUIRE(staged != nullptr);
    staged->product_name = staged->spool_name;
    staged->spool_name.reset();
    echoes.arm(0, "TAG-A");

    Observation record(ObservationSource::VendorCache);
    record.product_name = "Matte";
    CHECK(echoes.withhold(0, "TAG-A", record) == 1);
    CHECK_FALSE(record.product_name.has_value());
}

TEST_CASE("staged reports nothing for a slot with no edit", "[lane][echo]") {
    OwnWriteEchoes echoes;
    CHECK(echoes.staged(0) == nullptr);

    echoes.stage(0, declaration());
    CHECK(echoes.staged(0) != nullptr);
    CHECK(echoes.staged(1) == nullptr);
}
