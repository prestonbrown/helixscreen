// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "card_thumbnail_plan.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::CardThumbnailPlan;
using helix::CardThumbnailState;
using helix::plan_card_thumbnails;

namespace {

constexpr size_t KB = 1024;
constexpr size_t EST = 80 * KB; // one card's image

std::vector<CardThumbnailState> files(size_t n) {
    std::vector<CardThumbnailState> v(n);
    for (auto& f : v) {
        f.fetchable = true;
    }
    return v;
}

using Indices = std::vector<size_t>;

} // namespace

TEST_CASE("cards in the window are fetched in order, within the budget", "[card_thumbnail_plan]") {
    const auto f = files(30);
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 8, 20, 0, EST, 5 * EST);
    CHECK(plan.fetch == Indices{8, 9, 10, 11, 12}); // 5 fit; the rest wait
    CHECK(plan.drop.empty());
}

TEST_CASE("held and in-flight thumbnails count against the budget", "[card_thumbnail_plan]") {
    auto f = files(10);
    f[0].held = EST;
    f[1].tried = true; // in flight
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 10, /*in_flight=*/1, EST, 4 * EST);
    // 80K held + 80K in flight leaves room for two more.
    CHECK(plan.fetch == Indices{2, 3});
}

TEST_CASE("wide thumbnails smaller than a slot still take a whole slot", "[card_thumbnail_plan]") {
    // A 16:9 image fills a third of the card box, but decodes into a full slot;
    // counting its bytes would plan more decodes than the pool has slots.
    const size_t slots = 12;
    auto f = files(30);
    for (size_t i = 0; i < 6; ++i) {
        f[i].held = EST / 3;
    }
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 30, 0, EST, slots * EST);
    CHECK(plan.fetch.size() == slots - 6);
}

TEST_CASE("after a lane refusal, re-planning fetches nothing until a slot frees",
          "[card_thumbnail_plan]") {
    // The lane refused card 2; card 1 is still in flight. A listing re-sync
    // re-plans the same window with no completion in between.
    auto f = files(10);
    f[0].held = EST;
    f[1].tried = true;
    const CardThumbnailPlan resync =
        plan_card_thumbnails(f, 0, 10, /*in_flight=*/1, EST, 12 * EST, /*lane_refused=*/true);
    CHECK(resync.fetch.empty());

    // A refusal holds back fetches only: a card that left the window with a
    // fetch in flight still goes, and one holding a thumbnail is kept.
    const CardThumbnailPlan scrolled = plan_card_thumbnails(f, 4, 10, 1, EST, 12 * EST, true);
    CHECK(scrolled.drop == Indices{1});
    CHECK(scrolled.fetch.empty());

    // A completion frees a slot and clears the refusal: card 2 is fetched again.
    f[1].tried = false;
    f[1].held = EST;
    const CardThumbnailPlan freed = plan_card_thumbnails(f, 0, 10, 0, EST, 12 * EST, false);
    CHECK(freed.fetch.front() == 2);
}

TEST_CASE("a window already over budget starts nothing, even a backlog of refused fetches",
          "[card_thumbnail_plan]") {
    // Nine cards whose fetches the lane refused earlier come back untried; with
    // a 220px target each is ~145KB, and the budget holds only six.
    auto f = files(9);
    const size_t big = 220 * 220 * 3;
    const CardThumbnailPlan first_pass = plan_card_thumbnails(f, 0, 9, 0, big, 960 * KB);
    CHECK(first_pass.fetch.size() == 6);

    for (size_t i : first_pass.fetch) {
        f[i].held = big; // they all landed
    }
    const CardThumbnailPlan second_pass = plan_card_thumbnails(f, 0, 9, 0, big, 960 * KB);
    CHECK(second_pass.fetch.empty());

    // In flight counts the same as held.
    const CardThumbnailPlan in_flight_pass =
        plan_card_thumbnails(files(9), 0, 9, /*in_flight=*/7, big, 960 * KB);
    CHECK(in_flight_pass.fetch.empty());
}

TEST_CASE("cards leaving the window keep their thumbnails while the budget has room",
          "[card_thumbnail_plan]") {
    auto f = files(12);
    f[1].held = EST;       // scrolled out, holding: kept for the way back
    f[2].tried = true;     // scrolled out, fetch in flight or failed: dropped
    f[3].fetchable = true; // scrolled out, never touched
    f[6].held = EST;       // still on screen
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 4, 12, 0, EST, 960 * KB);
    CHECK(plan.drop == Indices{2});
    CHECK(plan.fetch == Indices{4, 5, 7, 8, 9, 10, 11});
}

TEST_CASE("a card on screen takes the slot of the least recently shown card off it",
          "[card_thumbnail_plan]") {
    // Budget of 4 slots: 2 on screen hold, 2 off screen hold, 1 on screen needs one.
    auto f = files(8);
    f[0].held = EST;
    f[0].last_shown = 5; // off screen, shown recently
    f[1].held = EST;
    f[1].last_shown = 2; // off screen, shown longest ago
    f[4].held = EST;
    f[5].held = EST;
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 4, 7, 0, EST, 4 * EST);
    CHECK(plan.fetch == Indices{6});
    CHECK(plan.drop == Indices{1});
}

TEST_CASE("off-screen thumbnails beyond the budget go oldest first, even with nothing to fetch",
          "[card_thumbnail_plan]") {
    auto f = files(10);
    for (size_t i = 0; i < 6; ++i) {
        f[i].held = EST;
        f[i].last_shown = static_cast<uint32_t>(10 + i); // 0 oldest, 5 newest
    }
    f[8].held = EST;
    f[9].held = EST;
    // Window 8..10 holds 2 slots; a 5-slot budget keeps the 3 newest off screen.
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 8, 10, 0, EST, 5 * EST);
    CHECK(plan.fetch.empty());
    CHECK(plan.drop == Indices{0, 1, 2});
}

TEST_CASE("cards on screen are never dropped, even over budget", "[card_thumbnail_plan]") {
    auto f = files(6);
    for (auto& x : f) {
        x.held = EST;
    }
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 6, 0, EST, 3 * EST);
    CHECK(plan.drop.empty());
    CHECK(plan.fetch.empty());
}

TEST_CASE("in-flight fetches outrank kept off-screen thumbnails", "[card_thumbnail_plan]") {
    auto f = files(6);
    f[0].held = EST; // off screen
    f[4].tried = true;
    f[5].tried = true;
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 4, 6, /*in_flight=*/2, EST, 2 * EST);
    CHECK(plan.drop == Indices{0});
}

TEST_CASE("directories, tried and holding files are not fetched", "[card_thumbnail_plan]") {
    auto f = files(4);
    f[0].fetchable = false; // a directory, or a file with no thumbnail
    f[1].tried = true;      // failed while on screen: not retried until it leaves
    f[2].held = EST;
    const CardThumbnailPlan plan = plan_card_thumbnails(f, 0, 4, 0, EST, 960 * KB);
    CHECK(plan.fetch == Indices{3});
}

TEST_CASE("a window past the list end is clamped", "[card_thumbnail_plan]") {
    const auto f = files(3);
    CHECK(plan_card_thumbnails(f, 2, 50, 0, EST, 960 * KB).fetch == Indices{2});
    CHECK(plan_card_thumbnails(f, 40, 50, 0, EST, 960 * KB).fetch.empty());
    CHECK(plan_card_thumbnails({}, 0, 10, 0, EST, 960 * KB).fetch.empty());
}

TEST_CASE("only a decode that ran out of memory is tried again while its card stays shown",
          "[card_thumbnail_plan]") {
    using helix::ThumbnailDecodeFailure;
    CHECK(helix::card_thumbnail_retry_while_shown(ThumbnailDecodeFailure::OutOfMemory));
    CHECK_FALSE(helix::card_thumbnail_retry_while_shown(ThumbnailDecodeFailure::BadImage));
    CHECK_FALSE(helix::card_thumbnail_retry_while_shown(ThumbnailDecodeFailure::Unsupported));
    CHECK_FALSE(helix::card_thumbnail_retry_while_shown(ThumbnailDecodeFailure::None));
}
