// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_render_schedule.h"

#include "../catch_amalgamated.hpp"

using namespace helix::gcode::render_schedule;

TEST_CASE("an unmeasured rate falls back to the seed rate", "[gcode][render_schedule]") {
    REQUIRE(effective_rate(0.0f) == kSeedRateTrisPerMs);
    REQUIRE(effective_rate(-1.0f) == kSeedRateTrisPerMs);
    REQUIRE(effective_rate(9000.0f) == 9000.0f);
}

TEST_CASE("the rate learns from slices and ignores empty samples", "[gcode][render_schedule]") {
    REQUIRE(update_rate(0.0f, 48000, 12.0f) == Catch::Approx(4000.0f));
    // Smoothed: 4000 + 0.3 * (8000 - 4000)
    REQUIRE(update_rate(4000.0f, 96000, 12.0f) == Catch::Approx(5200.0f));
    REQUIRE(update_rate(4000.0f, 0, 12.0f) == 4000.0f);
    REQUIRE(update_rate(4000.0f, 1000, 0.0f) == 4000.0f);
}

TEST_CASE("slice quotas aim at the target slice time", "[gcode][render_schedule]") {
    REQUIRE(slice_quota(0.0f) == static_cast<size_t>(kSeedRateTrisPerMs * kSliceTargetMs));
    REQUIRE(slice_quota(10000.0f) == 120000);
    REQUIRE(slice_quota(1.0f) == kMinSliceTriangles);
}

TEST_CASE("the moving plan keeps fast GPUs at full detail", "[gcode][render_schedule]") {
    // Pi 5 class: 40k tris/ms, budget 1M; a 600k model fits.
    MovingPlan p = plan_moving(600000, 0, 40000.0f);
    REQUIRE_FALSE(p.use_mesh);
    REQUIRE(p.stride == 1);
    REQUIRE_FALSE(p.half_resolution);
}

TEST_CASE("the moving plan strides and halves resolution on a Pi 3B", "[gcode][render_schedule]") {
    // 4000 tris/ms * 25 ms = 100k budget; Benchy tier 3 is 523k triangles, no mesh built.
    MovingPlan p = plan_moving(523000, 0, 4000.0f);
    REQUIRE_FALSE(p.use_mesh);
    REQUIRE(p.stride == 6);
    REQUIRE(p.half_resolution);
}

TEST_CASE("an unmeasured rate never submits a huge model in one moving frame",
          "[gcode][render_schedule]") {
    MovingPlan p = plan_moving(4660000, 0, 0.0f);
    REQUIRE(p.stride == 47); // ceil(4.66M / 100k)
    REQUIRE(p.half_resolution);
    REQUIRE(plan_moving(0, 0, 0.0f).stride == 1);
}

TEST_CASE("the moving plan draws a mesh that fits the budget at full resolution",
          "[gcode][render_schedule]") {
    // 100k moving budget: a 90k-triangle mesh fits it whole, so it draws at
    // full resolution rather than halving.
    MovingPlan p = plan_moving(523000, 90000, 4000.0f);
    REQUIRE(p.use_mesh);
    REQUIRE(p.stride == 1);
    REQUIRE_FALSE(p.half_resolution);
}

TEST_CASE("the moving plan halves resolution when the mesh itself overflows the budget",
          "[gcode][render_schedule]") {
    MovingPlan p = plan_moving(523000, 150000, 4000.0f);
    REQUIRE(p.use_mesh);
    REQUIRE(p.stride == 1);
    REQUIRE(p.half_resolution);
}

TEST_CASE("a gesture that dropped to half resolution stays there", "[gcode][render_schedule]") {
    // A fast half-resolution sample makes the next plan ask for full resolution.
    const MovingPlan full = plan_moving(80000, 0, 4000.0f);
    REQUIRE_FALSE(full.half_resolution);

    const MovingPlan held = hold_half_resolution(full, true);
    REQUIRE(held.half_resolution);
    REQUIRE(held.stride == full.stride);
    REQUIRE(held.use_mesh == full.use_mesh);

    REQUIRE_FALSE(hold_half_resolution(full, false).half_resolution);
}

TEST_CASE("3D gives way to 2D only after a run of slow half-resolution frames",
          "[gcode][render_schedule]") {
    int streak = 0;
    for (int i = 1; i < kSlowMovingFramesToFallBack; ++i) {
        REQUIRE_FALSE(moving_too_slow(streak, 400.0f, true));
    }
    // A full-resolution frame has a cheaper rung left, and a fast one breaks the run.
    REQUIRE_FALSE(moving_too_slow(streak, 400.0f, false));
    REQUIRE(streak == 0);
    for (int i = 1; i < kSlowMovingFramesToFallBack; ++i) {
        REQUIRE_FALSE(moving_too_slow(streak, 400.0f, true));
    }
    REQUIRE_FALSE(moving_too_slow(streak, kMaxUsableMovingFrameMs, true));
    REQUIRE(streak == 0);
    for (int i = 1; i < kSlowMovingFramesToFallBack; ++i) {
        REQUIRE_FALSE(moving_too_slow(streak, 400.0f, true));
    }
    REQUIRE(moving_too_slow(streak, 400.0f, true));
}

TEST_CASE("plan_bands sizes shell bands to the budget left after surfaces",
          "[gcode][render_schedule]") {
    // 11 triangles per N=4 segment: 36k shell segments project to 396k
    // triangles, which a 100k budget needs 4-layer bands to cover.
    BandPlan p = plan_bands(36000, 0, 100000);
    REQUIRE(p.band_layers == 4);
    REQUIRE(p.surfaces_every_layer);

    // 2k surface segments (22k triangles) come off the top: 330k shell
    // triangles over the remaining 78k need 5-layer bands.
    p = plan_bands(30000, 2000, 100000);
    REQUIRE(p.band_layers == 5);
    REQUIRE(p.surfaces_every_layer);

    // Exactly half the budget still fits.
    REQUIRE(plan_bands(0, 100000 / 2 / 11, 100000).surfaces_every_layer);

    // A shell the budget holds whole gets 1-layer bands: exterior on every layer.
    p = plan_bands(100, 0, 100000);
    REQUIRE(p.band_layers == 1);
    REQUIRE(p.exterior_only);
    REQUIRE(plan_bands(0, 0, 100000).band_layers == 1);
}

TEST_CASE("plan_bands strides surfaces with the shell past half the budget",
          "[gcode][render_schedule]") {
    // 5k surface segments are 55k triangles, over half of 100k.
    BandPlan p = plan_bands(1000, 5000, 100000);
    REQUIRE_FALSE(p.surfaces_every_layer);
    REQUIRE(p.band_layers == 1); // 66k fits 100k whole

    p = plan_bands(30000, 20000, 100000);
    REQUIRE_FALSE(p.surfaces_every_layer);
    REQUIRE(p.band_layers == 6); // 550k over 100k
}

TEST_CASE("a faster measured GPU gets a denser moving mesh", "[gcode][render_schedule]") {
    // Benchy-sized shell, no surfaces: the band depth tracks the moving budget.
    REQUIRE(moving_budget_triangles(0.0f) == 100000); // unmeasured: Pi 3B seed
    const int seed_bands = plan_bands(400000, 0, moving_budget_triangles(0.0f)).band_layers;
    const int pi4_bands = plan_bands(400000, 0, moving_budget_triangles(12000.0f)).band_layers;
    REQUIRE(seed_bands == 44); // ceil(400k * 11 tris / 100k)
    REQUIRE(pi4_bands == 15);  // ceil(400k * 11 tris / 300k)
}

TEST_CASE("plan_bands with no budget is off", "[gcode][render_schedule]") {
    BandPlan p = plan_bands(36000, 2000, 0);
    REQUIRE(p.band_layers == 1);
    REQUIRE_FALSE(p.surfaces_every_layer);
    REQUIRE_FALSE(p.exterior_only);
}

TEST_CASE("plan_still_bands fits an over-cap still or gives up past 4-layer bands",
          "[gcode][render_schedule]") {
    // Eiffel on vc4: 129k shell and 9k surface segments against a 1M cap.
    BandPlan p = plan_still_bands(129000, 9000, 1000000);
    REQUIRE(p.band_layers == 2);
    REQUIRE(p.surfaces_every_layer);

    // 4-layer bands are the deepest a still may use.
    REQUIRE(plan_still_bands(340000, 0, 1000000).band_layers == 4);
    // Past that the still stays 2D.
    p = plan_still_bands(500000, 0, 1000000);
    REQUIRE(p.band_layers == 1);
    REQUIRE_FALSE(p.surfaces_every_layer);
    REQUIRE_FALSE(p.exterior_only);
}

static JobInputs idle_complete(int progress) {
    JobInputs in{};
    in.have_complete_image = true;
    in.job_progress = progress;
    in.new_progress = progress;
    return in;
}

TEST_CASE("nothing changed means no work", "[gcode][render_schedule]") {
    REQUIRE(decide_job(idle_complete(10)) == JobAction::Keep);
}

TEST_CASE("a running job is left alone when nothing changed", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    REQUIRE(decide_job(in) == JobAction::Keep);
}

TEST_CASE("no complete image restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false; // a moving frame replaced the still image
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("any scene change restarts, even mid incremental job", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.scene_changed = true;
    in.job_running = true;
    in.job_incremental = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress advancing over a complete image is incremental", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.new_progress = 12;
    REQUIRE(decide_job(in) == JobAction::Incremental);
}

TEST_CASE("progress advancing during an incremental job extends it", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    in.job_incremental = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Extend);
}

TEST_CASE("progress advancing during a still job restarts it", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress went backwards restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.new_progress = 3;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("a same-file reprint over the finished image restarts", "[gcode][render_schedule]") {
    // The Reprint button restarts the file with the panel and viewer untouched:
    // no -1, no reload, no scene change. The new print's first layer events
    // arrive far below the finished image's progress, and each must redraw.
    JobInputs in = idle_complete(240);
    in.new_progress = 1;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress went backwards mid incremental job restarts it", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    in.job_incremental = true;
    in.new_progress = 3;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress went backwards with nothing to show restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.new_progress = 3;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("ghost off restarts, and ghost on from off restarts", "[gcode][render_schedule]") {
    JobInputs off = idle_complete(10);
    off.new_progress = -1;
    REQUIRE(decide_job(off) == JobAction::Restart);
    JobInputs on = idle_complete(-1);
    on.new_progress = 4;
    REQUIRE(decide_job(on) == JobAction::Restart);
}

TEST_CASE("a highlighted selection forces full restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.selection_active = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}
