// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_cache_request.cpp
 * @brief ThumbnailRequest and the unified guarded fetch core.
 *
 * ThumbnailCache::fetch(ThumbnailRequest, ThumbnailLoadContext, ...) is the one
 * entry point every consumer is being moved onto. Its whole reason to exist is
 * the staleness guard: a load that a newer request has superseded must never
 * reach the caller's success callback, because that callback writes a widget or
 * a subject describing a file the UI has already moved off.
 *
 * The trap when testing that is proving a negative against a path that could
 * never have fired anyway. A fetch with a null api and nothing in the cache
 * calls on_error and returns - "on_success did not fire" is then true whether
 * or not the guard exists at all. So these tests plant a real pre-scaled .bin
 * first, which makes fetch() resolve synchronously with an actual success to
 * deliver, and assert BOTH sides: a live context receives that path, a
 * superseded one does not.
 */

#include "../../include/async_lifetime_guard.h"
#include "../../include/thumbnail_cache.h"
#include "../../include/thumbnail_load_context.h"
#include "../../include/thumbnail_processor.h"
#include "../../include/ui_update_queue.h"
#include "../lvgl_test_fixture.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Smallest valid PNG the cache and the processor will both accept: a 10x10
/// solid-colour square, 75 bytes. Same bytes as tests/unit/test_thumbnail_scaling.cpp.
// clang-format off
const std::vector<uint8_t> TINY_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x02, 0x50, 0x58, 0xEA, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x68, 0x70, 0x50, 0xC0,
    0x83, 0x18, 0x46, 0xA5, 0xB1, 0x21, 0x00, 0x24, 0x51, 0x57, 0x81, 0xF7,
    0xEC, 0xA3, 0x23, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};
// clang-format on

/// Fixed target rather than get_target_for_display(): the pre-scaled key must
/// be identical between the plant and the lookup, and must not depend on which
/// display the test binary happens to have created.
helix::ThumbnailTarget target_120() {
    helix::ThumbnailTarget t;
    t.width = 120;
    t.height = 120;
    return t;
}

/// Unique per process so a leftover from another shard cannot answer for us.
std::string unique_key(const char* tag) {
    return std::string("thumb_request_") + tag + "_" + std::to_string(::getpid()) + ".png";
}

/// Write raw bytes straight into the cache's PNG slot for `key`.
///
/// Deliberately NOT save_raw_png(): that validates the PNG magic bytes, and the
/// fallback case below needs a file that exists (so fetch_optimized takes its
/// "PNG is cached, queue the pre-scale" branch) but cannot be decoded (so the
/// processor reports an error and process_and_callback substitutes the PNG).
/// Only a direct write can be both.
void plant_cached_png(const ThumbnailCache& cache, const std::string& key,
                      const std::vector<uint8_t>& bytes) {
    const std::string path = cache.get_cache_path(key);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(std::filesystem::exists(path));
}

/// A fetch that reaches process_and_callback has delivered nothing by the time
/// fetch() returns: the pre-scale runs on the processor pool and its result is
/// marshalled back through UpdateQueue. Join the pool, then drain — repeatedly,
/// because a drained callback can commit more pool work.
void settle(const std::function<bool()>& done) {
    for (int i = 0; i < 20 && !done(); ++i) {
        helix::ThumbnailProcessor::instance().wait_for_completion();
        helix::ui::UpdateQueue::instance().drain();
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailRequest: fetch delivers when the context is live",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    auto& processor = helix::ThumbnailProcessor::instance();
    const helix::ThumbnailTarget target = target_120();
    const std::string key = unique_key("live");

    const auto planted = processor.process_sync(TINY_PNG, key, target);
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    ThumbnailRequest req;
    req.key = key;
    req.target = target;

    // The request overload of get_if_cached must see the same pre-scaled file
    // the fetch path will resolve to.
    CHECK(cache.get_if_cached(req) == planted.output_path);

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;
    auto ctx = ThumbnailLoadContext::create(guard, &gen);

    std::string delivered;
    cache.fetch(
        req, ctx, [&delivered](const std::string& path, bool /*degraded*/) { delivered = path; },
        [](const std::string& error) { FAIL_CHECK("unexpected fetch error: " << error); });

    CHECK(delivered == planted.output_path);

    cache.invalidate(key);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailRequest: fetch suppresses a superseded context",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    auto& processor = helix::ThumbnailProcessor::instance();
    const helix::ThumbnailTarget target = target_120();
    const std::string key = unique_key("stale");

    const auto planted = processor.process_sync(TINY_PNG, key, target);
    REQUIRE(planted.success);

    ThumbnailRequest req;
    req.key = key;
    req.target = target;

    // Pinned: without this the "no callback" assertion below would hold for the
    // trivial reason that there was nothing to deliver.
    REQUIRE_FALSE(cache.get_if_cached(req).empty());

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;
    auto ctx = ThumbnailLoadContext::create(guard, &gen);

    // A newer request supersedes ctx before its callback can run.
    ++gen;
    REQUIRE_FALSE(ctx.is_valid());

    bool success_fired = false;
    cache.fetch(
        req, ctx, [&success_fired](const std::string&, bool) { success_fired = true; }, nullptr);

    CHECK_FALSE(success_fired);

    cache.invalidate(key);
}

/// The detail-sized fetch is the path both active-print consumers use, and its
/// old wrapper had no source_modified parameter at all - so mtime validation
/// never ran on it. Re-slice a model under the same filename and the cache kept
/// serving the old image forever. The two blocks below are deliberately paired:
/// the first proves the planted entry IS servable, so the second's "not served"
/// is about freshness and not about an empty cache. With no api the re-fetch
/// cannot proceed, which is what makes the difference observable - fresh source
/// means on_error, a stale cache hit means on_success carrying the old path.
TEST_CASE_METHOD(LVGLTestFixture,
                 "ThumbnailRequest: detail view honors a source newer than the cached file",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    auto& processor = helix::ThumbnailProcessor::instance();

    // The detail target the active-print consumers build their request from.
    const helix::ThumbnailTarget target =
        helix::ThumbnailProcessor::get_target_for_display(helix::ThumbnailSize::Detail);
    const std::string key = unique_key("detail_mtime");

    const auto planted = processor.process_sync(TINY_PNG, key, target);
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    ThumbnailRequest req;
    req.key = key;
    req.target = target;

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;

    {
        auto ctx = ThumbnailLoadContext::create(guard, &gen);
        std::string delivered;
        cache.fetch(
            req, ctx,
            [&delivered](const std::string& path, bool /*degraded*/) { delivered = path; },
            [](const std::string& error) {
                FAIL_CHECK("unexpected detail view error: " << error);
            });
        REQUIRE(delivered == planted.output_path);
    }

    {
        auto ctx = ThumbnailLoadContext::create(guard, &gen);
        std::string delivered;
        std::string reported_error;
        req.source_modified = 4102444800; // 2100-01-01, newer than any cached file
        cache.fetch(
            req, ctx,
            [&delivered](const std::string& path, bool /*degraded*/) { delivered = path; },
            [&reported_error](const std::string& error) { reported_error = error; });

        CHECK(delivered.empty());
        CHECK_FALSE(reported_error.empty());
    }

    cache.invalidate(key);
}

/// process_and_callback() wires the ThumbnailProcessor's ERROR handler to
/// on_success, handing the caller the raw PNG when pre-scaling fails. That is a
/// deliberate graceful degradation - the PNG still renders, just slower - but
/// until the callback carried a flag it was indistinguishable from a real
/// pre-scale, so nobody downstream could tell they were holding the slow path.
///
/// The two cases below are a pair on purpose. Asserting only the fallback would
/// pass against an implementation that hardcoded `degraded = true` everywhere,
/// and asserting only the success would pass against one that hardcoded
/// `false`. Both have to be observed on the same build for the flag to mean
/// anything. Each drives the SAME entry point (fetch -> fetch_optimized step 2
/// -> process_and_callback); the only difference is whether the planted PNG can
/// actually be decoded.

/// ThumbnailRequest::format is the only way to ask for the full-resolution PNG
/// instead of the pre-scaled .bin. Two call sites need it: print-select's
/// no-.bin-yet fallback (Snapmaker U1 / AD5M, where the detail preview is the
/// only render) and the history detail overlay.
///
/// Both cases below plant exactly ONE of the two artifacts and assert the other
/// format comes back empty. That is what makes the routing observable: a build
/// where both branches resolve the same way fails one half or the other, and a
/// single-sided assertion would pass against a hardcoded branch.
///
/// process_sync() writes only the .bin; plant_cached_png() writes only the PNG.
/// So each key is unambiguously one artifact and not the other.
TEST_CASE_METHOD(LVGLTestFixture,
                 "ThumbnailRequest: format selects PNG or pre-scaled on get_if_cached",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    auto& processor = helix::ThumbnailProcessor::instance();
    const helix::ThumbnailTarget target = target_120();

    // Entry A: pre-scaled .bin, no cached PNG.
    const std::string bin_key = unique_key("fmt_bin");
    cache.invalidate(bin_key);
    const auto planted = processor.process_sync(TINY_PNG, bin_key, target);
    REQUIRE(planted.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(planted.output_path));

    // Entry B: full-resolution PNG, no .bin.
    const std::string png_key = unique_key("fmt_png");
    cache.invalidate(png_key);
    plant_cached_png(cache, png_key, TINY_PNG);
    const std::string png_path = ThumbnailCache::to_lvgl_path(cache.get_cache_path(png_key));

    ThumbnailRequest bin_req;
    bin_req.key = bin_key;
    bin_req.target = target;

    ThumbnailRequest png_req;
    png_req.key = png_key;
    png_req.target = target;

    // Default must stay Prescaled so every pre-existing caller is unaffected.
    REQUIRE(bin_req.format == ThumbnailRequest::ThumbnailFormat::Prescaled);

    CHECK(cache.get_if_cached(bin_req) == planted.output_path);
    CHECK(cache.get_if_cached(png_req).empty());

    bin_req.format = ThumbnailRequest::ThumbnailFormat::FullPng;
    png_req.format = ThumbnailRequest::ThumbnailFormat::FullPng;

    // Exact mirror image: FullPng sees the PNG and is blind to the .bin.
    CHECK(cache.get_if_cached(bin_req).empty());
    CHECK(cache.get_if_cached(png_req) == png_path);

    cache.invalidate(bin_key);
    cache.invalidate(png_key);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailRequest: format selects PNG or pre-scaled on fetch",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    const helix::ThumbnailTarget target = target_120();

    // Both keys start identically: a decodable PNG in the cache and no .bin, so
    // the ONLY thing that can differ downstream is which branch fetch() took.
    const std::string pre_key = unique_key("fetchfmt_pre");
    const std::string png_key = unique_key("fetchfmt_png");
    cache.invalidate(pre_key);
    cache.invalidate(png_key);
    plant_cached_png(cache, pre_key, TINY_PNG);
    plant_cached_png(cache, png_key, TINY_PNG);

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;

    // --- Prescaled: must run the pre-scaler and hand back a .bin. ---
    ThumbnailRequest pre_req;
    pre_req.key = pre_key;
    pre_req.target = target;
    REQUIRE(cache.get_if_cached(pre_req).empty()); // no .bin yet

    bool pre_fired = false;
    std::string pre_delivered;
    {
        auto ctx = ThumbnailLoadContext::create(guard, &gen);
        cache.fetch(
            pre_req, ctx,
            [&](const std::string& path, bool /*degraded*/) {
                pre_fired = true;
                pre_delivered = path;
            },
            [](const std::string& error) { FAIL_CHECK("unexpected prescaled error: " << error); });
        settle([&pre_fired] { return pre_fired; });
    }
    REQUIRE(pre_fired);
    REQUIRE(pre_delivered.size() > 4);
    CHECK(pre_delivered.substr(pre_delivered.size() - 4) == ".bin");

    // --- FullPng: must hand back the cached PNG, untouched by the pre-scaler. ---
    ThumbnailRequest png_req;
    png_req.key = png_key;
    png_req.target = target;
    png_req.format = ThumbnailRequest::ThumbnailFormat::FullPng;

    bool png_fired = false;
    // Seeded true so a callback that never runs cannot read as a passing check.
    bool png_degraded = true;
    std::string png_delivered;
    {
        auto ctx = ThumbnailLoadContext::create(guard, &gen);
        cache.fetch(
            png_req, ctx,
            [&](const std::string& path, bool degraded) {
                png_fired = true;
                png_degraded = degraded;
                png_delivered = path;
            },
            [](const std::string& error) { FAIL_CHECK("unexpected PNG error: " << error); });
        settle([&png_fired] { return png_fired; });
    }
    REQUIRE(png_fired);
    CHECK(png_delivered == ThumbnailCache::to_lvgl_path(cache.get_cache_path(png_key)));
    // A PNG delivered because a PNG was asked for is not a degraded fallback.
    CHECK_FALSE(png_degraded);

    // And the pre-scaler was never invoked for this key - which is the part a
    // FullPng branch that silently fell through to fetch_optimized would fail.
    ThumbnailRequest png_as_bin;
    png_as_bin.key = png_key;
    png_as_bin.target = target;
    CHECK(cache.get_if_cached(png_as_bin).empty());

    // --- The staleness guard must still apply on the FullPng branch. ---
    const std::string stale_key = unique_key("fetchfmt_stale");
    cache.invalidate(stale_key);
    plant_cached_png(cache, stale_key, TINY_PNG);

    ThumbnailRequest stale_req;
    stale_req.key = stale_key;
    stale_req.target = target;
    stale_req.format = ThumbnailRequest::ThumbnailFormat::FullPng;
    // Pinned: without a real cache hit "nothing fired" would be trivially true.
    REQUIRE_FALSE(cache.get_if_cached(stale_req).empty());

    auto stale_ctx = ThumbnailLoadContext::create(guard, &gen);
    ++gen;
    REQUIRE_FALSE(stale_ctx.is_valid());

    bool stale_fired = false;
    cache.fetch(
        stale_req, stale_ctx, [&stale_fired](const std::string&, bool) { stale_fired = true; },
        nullptr);
    settle([] { return false; });
    CHECK_FALSE(stale_fired);

    cache.invalidate(pre_key);
    cache.invalidate(png_key);
    cache.invalidate(stale_key);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailCache: a completed pre-scale reports degraded=false",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    const helix::ThumbnailTarget target = target_120();
    const std::string key = unique_key("degraded_false");
    cache.invalidate(key);

    plant_cached_png(cache, key, TINY_PNG);

    ThumbnailRequest req;
    req.key = key;
    req.target = target;

    // No .bin yet - this is what forces fetch() past its pre-scaled-hit branch
    // and into process_and_callback, the function under test.
    REQUIRE(cache.get_if_cached(req).empty());

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;
    auto ctx = ThumbnailLoadContext::create(guard, &gen);

    bool fired = false;
    // Seeded to the OPPOSITE of the expectation so a callback that never runs
    // cannot be mistaken for a passing assertion.
    bool degraded = true;
    std::string delivered;
    std::string reported_error;

    cache.fetch(
        req, ctx,
        [&](const std::string& path, bool is_degraded) {
            fired = true;
            degraded = is_degraded;
            delivered = path;
        },
        [&reported_error](const std::string& error) { reported_error = error; });

    settle([&fired] { return fired; });

    INFO("error reported: " << reported_error);
    REQUIRE(fired);
    CHECK_FALSE(degraded);
    // A real pre-scale delivers the .bin it produced, not the PNG it read.
    CHECK(delivered.size() > 4);
    CHECK(delivered.substr(delivered.size() - 4) == ".bin");

    cache.invalidate(key);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailCache: the PNG fallback reports degraded=true",
                 "[thumbnail][request]") {
    ThumbnailCache cache;
    const helix::ThumbnailTarget target = target_120();
    const std::string key = unique_key("degraded_true");
    cache.invalidate(key);

    // Exists, so fetch_optimized takes its "PNG is cached" branch; undecodable,
    // so the pre-scale fails and process_and_callback substitutes this file.
    const std::vector<uint8_t> not_a_png(64, 0x7F);
    plant_cached_png(cache, key, not_a_png);

    ThumbnailRequest req;
    req.key = key;
    req.target = target;

    REQUIRE(cache.get_if_cached(req).empty());

    std::atomic<uint32_t> gen{0};
    helix::AsyncLifetimeGuard guard;
    auto ctx = ThumbnailLoadContext::create(guard, &gen);

    bool fired = false;
    bool degraded = false;
    std::string delivered;
    std::string reported_error;

    cache.fetch(
        req, ctx,
        [&](const std::string& path, bool is_degraded) {
            fired = true;
            degraded = is_degraded;
            delivered = path;
        },
        [&reported_error](const std::string& error) { reported_error = error; });

    settle([&fired] { return fired; });

    INFO("error reported: " << reported_error);
    // The fallback is delivered through the SUCCESS channel - that is the whole
    // problem this flag exists to make visible.
    REQUIRE(fired);
    CHECK(degraded);
    CHECK(delivered == ThumbnailCache::to_lvgl_path(cache.get_cache_path(key)));

    cache.invalidate(key);
}
