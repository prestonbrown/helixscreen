// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "geometry_budget_manager.h"

#include <cstddef>

namespace helix::gcode::render_schedule {

/// Wall time one still-frame slice aims for. Leaves an LVGL frame room for the rest of the UI.
constexpr float kSliceTargetMs = 12.0f;
/// Draw time a moving (finger-down) frame may spend.
constexpr float kMovingBudgetMs = 25.0f;
/// Throughput assumed before any slice has been measured. A Pi 3B's VideoCore IV draws about
/// 4M triangles/s, so the first slice and the first moving frame stay short on the weakest GPU.
constexpr float kSeedRateTrisPerMs = 4000.0f;
/// Floor on a slice so per-slice overhead never dominates.
constexpr size_t kMinSliceTriangles = 5000;
/// Weight of the newest measurement in the smoothed rate.
constexpr float kRateSmoothing = 0.3f;

/// The rate to plan with: the measured one, or the seed while nothing has been measured.
inline float effective_rate(float rate) {
    return rate > 0.0f ? rate : kSeedRateTrisPerMs;
}

/// Fold one measured submission into the smoothed triangles-per-millisecond rate.
/// A sample with no triangles or no elapsed time carries no information and is ignored.
inline float update_rate(float prev_rate, size_t triangles, float elapsed_ms) {
    if (triangles == 0 || elapsed_ms <= 0.0f) {
        return prev_rate;
    }
    const float sample = static_cast<float>(triangles) / elapsed_ms;
    if (prev_rate <= 0.0f) {
        return sample;
    }
    return prev_rate + kRateSmoothing * (sample - prev_rate);
}

/// Triangles the next still-frame slice may submit.
inline size_t slice_quota(float rate) {
    const auto quota = static_cast<size_t>(effective_rate(rate) * kSliceTargetMs);
    return quota < kMinSliceTriangles ? kMinSliceTriangles : quota;
}

/// How a moving frame is drawn: the prebuilt exterior `mesh` when one exists,
/// otherwise every `stride`-th layer; optionally at half resolution.
struct MovingPlan {
    bool use_mesh = false;
    int stride = 1;
    bool half_resolution = false;
};

/// Full detail when the whole model fits the moving budget at the planning rate.
/// Otherwise a prebuilt moving mesh takes over if one was built (drawn whole
/// when it fits, at half resolution when even it overflows); with no mesh the
/// smallest layer stride that fits, at half resolution.
inline MovingPlan plan_moving(size_t total_triangles, size_t mesh_triangles, float rate) {
    auto budget = static_cast<size_t>(effective_rate(rate) * kMovingBudgetMs);
    if (budget == 0) {
        budget = 1;
    }
    if (total_triangles <= budget) {
        return {};
    }
    if (mesh_triangles > 0) {
        return {true, 1, mesh_triangles > budget};
    }
    return {false, static_cast<int>((total_triangles + budget - 1) / budget), true};
}

/// Keeps a gesture at half resolution once one of its frames needed it. The
/// measured rate is triangles per millisecond, but a half-resolution frame
/// also fills a quarter of the pixels, so its sample overstates what a
/// full-resolution frame can do. Planning from it alone flips the next frame
/// back to full resolution, which overruns the budget, and every flip
/// reallocates the framebuffer.
inline MovingPlan hold_half_resolution(MovingPlan plan, bool held) {
    if (held) {
        plan.half_resolution = true;
    }
    return plan;
}

/// How the moving mesh is banded.
struct BandPlan {
    int band_layers;           ///< layers one shell band covers (1 = off)
    bool surfaces_every_layer; ///< keep top/bottom/bridge skins on every layer, not every n-th
};

/// Plan the moving mesh. `shell_segments` are band-tall exterior extrusions (walls), which
/// the mesh keeps on every n-th layer. `surface_segments` are extrusions kept one layer tall
/// regardless of the stride: horizontal skins plus the whole first layer. Surfaces are what
/// keep the model from reading as hollow, so they get their share of the budget first, as
/// long as that share is at most half; past that they are strided with the shell. The mesh
/// draws at 4 tube sides, so its triangles per segment are kTrisPerSegN4. The floor of 2
/// keeps bands from degenerating into the full-resolution build. 0 budget (no planning rate
/// at all) means no mesh, so the answer is the off value.
inline BandPlan plan_bands(size_t shell_segments, size_t surface_segments,
                           size_t budget_triangles) {
    if (budget_triangles == 0) {
        return {1, false};
    }
    constexpr size_t kTris = GeometryBudgetManager::kTrisPerSegN4;
    auto layers_for = [](size_t triangles, size_t budget) {
        const size_t layers = (triangles + budget - 1) / budget; // ceil divide
        return static_cast<int>(layers < 2 ? 2 : layers);
    };
    const size_t surface_triangles = surface_segments * kTris;
    if (surface_triangles * 2 <= budget_triangles) {
        return {layers_for(shell_segments * kTris, budget_triangles - surface_triangles), true};
    }
    return {layers_for((shell_segments + surface_segments) * kTris, budget_triangles), false};
}

/// Deepest band a still image may use. Past this the steps read at fit zoom
/// and the 2D view is the better picture.
constexpr int kMaxStillBandLayers = 4;

/// Band plan for a still that the GPU triangle cap refused at full layer
/// resolution: the same banding as the moving mesh, sized to the cap. A
/// band_layers of 1 means no 3D still fits and the viewer stays 2D.
inline BandPlan plan_still_bands(size_t shell_segments, size_t surface_segments,
                                 size_t max_triangles) {
    const BandPlan p = plan_bands(shell_segments, surface_segments, max_triangles);
    if (p.band_layers > kMaxStillBandLayers) {
        return {1, false};
    }
    return p;
}

/// What the renderer does with its time-sliced job after a state change.
enum class JobAction {
    Keep,        ///< keep going (or keep showing the cached image)
    Restart,     ///< start a full still job from layer 0
    Extend,      ///< the running incremental job grows to the new progress layer
    Incremental, ///< draw only the newly finished layers onto the retained buffers
};

struct JobInputs {
    bool scene_changed = false; ///< camera, colors, range, viewport or file differ from the job's
    bool have_complete_image =
        false; ///< the last finished image matches the scene and job_progress
    bool job_running = false;
    bool job_incremental = false;
    bool selection_active = false; ///< highlighted objects need the full selection passes
    int job_progress = -1; ///< progress layer of the running or last finished job; -1 = no ghost
    int new_progress = -1; ///< progress layer now requested; -1 = no ghost
};

inline JobAction decide_job(const JobInputs& in) {
    if (in.scene_changed) {
        return JobAction::Restart;
    }
    if (in.new_progress == in.job_progress) {
        return (in.job_running || in.have_complete_image) ? JobAction::Keep : JobAction::Restart;
    }
    // Anything but an advance restarts: a same-file reprint begins at layer 1
    // against the finished image with no scene change of its own, and layers
    // cannot be un-drawn off a retained image.
    const bool advanced = in.job_progress >= 0 && in.new_progress > in.job_progress;
    if (!advanced || in.selection_active) {
        return JobAction::Restart;
    }
    if (in.job_running) {
        return in.job_incremental ? JobAction::Extend : JobAction::Restart;
    }
    return in.have_complete_image ? JobAction::Incremental : JobAction::Restart;
}

} // namespace helix::gcode::render_schedule
