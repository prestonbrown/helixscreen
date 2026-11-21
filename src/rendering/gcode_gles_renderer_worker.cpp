// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer_internal.h"
#include "system/telemetry_manager.h"

#ifdef ENABLE_GLES_3D

namespace helix {
namespace gcode {

// ============================================================
// Main Render Entry Point
// ============================================================

void GCodeGLESRenderer::render(lv_layer_t* layer,
                               const std::shared_ptr<const ParsedGCodeFile>& gcode,
                               const GCodeCamera& camera, const lv_area_t* widget_coords) {
    // GL comes up here, on the LVGL thread: the EGL path reuses the display
    // backend's current EGLDisplay (current per thread) and SDL creates its
    // window here. init_gl() leaves the context current on no thread, and the
    // worker makes it current for each step from then on.
    if (!gl_initialized_) {
        if (!init_gl()) {
            return; // GPU not available
        }
        worker_.start(
            [this](const RenderRequest& req, RenderFrame& back) { return gl_step(req, back); });
    }

    // Sticky bail: once a fatal GL error or a denylisted GPU has disabled the
    // GPU path this session, nothing more is drawn. The viewer polls
    // render_failed() and falls back to the pure-CPU 2D renderer.
    if (render_failed())
        return;

    RenderRequest req;
    req.camera = camera;
    req.width = lv_area_get_width(widget_coords);
    req.height = lv_area_get_height(widget_coords);
    req.gcode = gcode;
    worker_.post(std::move(req));

    // The draw event fires once per invalidated area of a refresh, so a frame
    // adopted here could show in some strips and not others. New frames are
    // adopted between refreshes (adopt_ready_frame); this only blits.
    draw_cached_to_lvgl(layer, widget_coords);
}

bool GCodeGLESRenderer::adopt_ready_frame() {
    if (!worker_.take_frame(taken_frame_)) {
        return false;
    }
    install_frame(taken_frame_);
    return true;
}

void GCodeGLESRenderer::set_frame_ready_callback(std::function<void()> cb) {
    worker_.set_frame_ready_callback(std::move(cb));
}

void GCodeGLESRenderer::install_frame(const RenderFrame& frame) {
    if (frame.generation != scene_gen_.load() || frame.width <= 0 || frame.height <= 0 ||
        frame.pixels.size() < static_cast<size_t>(frame.width) * frame.height * 4) {
        return; // drawn for geometry that has since been replaced or released
    }
    if (!draw_buf_ || draw_buf_width_ != frame.width || draw_buf_height_ != frame.height) {
        // May still be in flight to the parallel render thread (#929 cluster).
        helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");
        draw_buf_bytes_ = 0;
        draw_buf_ =
            lv_draw_buf_create(static_cast<uint32_t>(frame.width),
                               static_cast<uint32_t>(frame.height), LV_COLOR_FORMAT_ARGB8888, 0);
        if (!draw_buf_ || !draw_buf_->data) {
            spdlog::error("[GCode GLES] Failed to create draw buffer");
            helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");
            draw_buf_width_ = 0;
            draw_buf_height_ = 0;
            return;
        }
        draw_buf_width_ = frame.width;
        draw_buf_height_ = frame.height;
        draw_buf_bytes_ = static_cast<size_t>(frame.width) * frame.height * 4;
    }
    // draw_buf_ rows are padded to LV_DRAW_BUF_STRIDE_ALIGN; the frame's are not.
    const size_t row_bytes = static_cast<size_t>(frame.width) * 4;
    const uint32_t dst_stride = draw_buf_->header.stride;
    auto* dest = static_cast<uint8_t*>(draw_buf_->data);
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(dest + static_cast<size_t>(y) * dst_stride,
                    frame.pixels.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
    }
    latch_shown_image(frame.mvp, frame.width, frame.height);
    shown_gen_ = frame.generation;
}

bool GCodeGLESRenderer::has_frame() const {
    return has_shown_image_ && shown_gen_ == scene_gen_.load();
}

GCodeRenderWorker::StepOutcome GCodeGLESRenderer::gl_step(const RenderRequest& req,
                                                          RenderFrame& back) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool failed_before = gl_render_failed_.load();
    GCodeRenderWorker::StepOutcome out = step_locked(req, back);
    if (gl_render_failed_.load() && !failed_before) {
        // The viewer learns of the fallback on its next draw; make sure there is one.
        out.notify = true;
        out.more_work = false;
    }
    return out;
}

GCodeRenderWorker::StepOutcome GCodeGLESRenderer::step_locked(const RenderRequest& req,
                                                              RenderFrame& back) {
    using Outcome = GCodeRenderWorker::StepOutcome;
    if (gl_render_failed_) {
        // Nothing is drawn any more, but a released file's VBOs still have to go.
        if (!retired_vbos_.empty()) {
#if LV_USE_SDL
            SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
            EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
            if (guard.ok()) {
                free_vbos(retired_vbos_);
            }
        }
        return {};
    }

    // Decide first, so a step with nothing to draw (most of them: every draw
    // event posts) never touches the GL context.
    enum class Work { Idle, Upload, Moving, Slice };
    Work work = Work::Idle;
    CachedRenderState moving_state;
    if (geometry_ && req.gcode && req.width > 0 && req.height > 0) {
        if (!geometry_uploaded_) {
            work = Work::Upload;
        } else if (interaction_mode_) {
            // Finger down: draw the whole frame now at whatever detail the
            // measured GPU rate affords, instead of handing out slices. The
            // draw event fires several times per refresh, each posting the
            // same camera; only the first of them draws.
            moving_state = snapshot_state(req.camera);
            if (!(moving_frame_valid_ && moving_epoch_ == scene_epoch_ &&
                  moving_state == moving_state_)) {
                work = Work::Moving;
            }
        } else {
            const CachedRenderState current_state = snapshot_state(req.camera);
            render_schedule::JobInputs in;
            in.scene_changed = frame_dirty_ || !current_state.same_scene(job_scene_);
            in.have_complete_image = have_complete_image_;
            in.job_running = job_.active;
            in.job_incremental = job_.incremental;
            in.selection_active = selection_.any_highlighted();
            in.job_progress = job_.progress_layer;
            in.new_progress = progress_layer_;
            switch (render_schedule::decide_job(in)) {
            case render_schedule::JobAction::Keep:
                break;
            case render_schedule::JobAction::Restart:
                start_job(false, current_state);
                break;
            case render_schedule::JobAction::Incremental:
                start_job(true, current_state);
                break;
            case render_schedule::JobAction::Extend:
                job_.solid_end = std::min(progress_layer_, job_.solid_end_limit);
                job_.progress_layer = progress_layer_;
                job_.phase = JobPhase::Solid;
                break;
            }
            frame_dirty_ = false;
            if (job_.active) {
                work = Work::Slice;
            }
        }
    }
    if (work == Work::Idle && retired_vbos_.empty()) {
        return {};
    }

#if LV_USE_SDL
    SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
    EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
    if (!guard.ok())
        return {};

    free_vbos(retired_vbos_);
    if (work == Work::Idle) {
        return {};
    }

    // Layer 1 — proactive GL_RENDERER denylist (issues #966 / #1084 / #1085).
    // Evaluated exactly once, now that the GL context is current so GL_RENDERER
    // is valid. Known-bad Mali/Panfrost GPUs SIGSEGV inside glDrawArrays, which
    // the reactive glGetError() guard cannot catch — bail before the first draw.
    if (!gpu_checked_) {
        gpu_checked_ = true;
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        spdlog::info("[GCode GLES] GL_RENDERER: {}", renderer ? renderer : "(null)");
        helix::telemetry_context::set_gl_renderer(renderer);
        if (gl_renderer_is_denylisted(renderer)) {
            spdlog::error("[GCode GLES] GPU '{}' is denylisted (known to fault inside the driver) "
                          "— disabling GPU rendering, falling back to 2D",
                          renderer ? renderer : "(null)");
            gl_render_failed_ = true;
            cancel_job();
            return {};
        }
    }

    switch (work) {
    case Work::Idle:
        return {};

    case Work::Upload: {
        // Incremental VBO upload: one time-budgeted batch of layers per step.
        if (upload_total_layers_ == 0) {
            free_vbos(layer_vbos_); // Free old VBOs inside GL context
            free_vbos(moving_vbos_);
            mesh_triangles_ = 0; // stale if the geometry was replaced
            size_t num_layers =
                geometry_->layer_strip_ranges.empty() ? 1 : geometry_->layer_strip_ranges.size();
            layer_vbos_.resize(num_layers);
            upload_total_layers_ = num_layers;
            spdlog::info("[GCode GLES] Starting incremental VBO upload: {} layers", num_layers);
        }

        bool done = upload_geometry_chunk(*geometry_, layer_vbos_, upload_next_layer_,
                                          upload_total_layers_);
        if (gl_render_failed_) {
            upload_total_layers_ = 0;
            return {};
        }
        if (!done) {
            return Outcome{false, true, false};
        }
        geometry_uploaded_ = true;
        upload_next_layer_ = 0;
        upload_total_layers_ = 0;
        uploaded_triangles_ = 0;
        for (const auto& vbo : layer_vbos_) {
            uploaded_triangles_ += vbo.vertex_count / 3;
        }
        // Free pre-computed interleaved buffers — all data is now in GPU VBOs.
        // The CPU fallback path (for tool color re-upload) re-expands from
        // the compact vertices/strips directly, so these aren't needed.
        size_t freed = 0;
        for (auto& pb : geometry_->prepared_buffers) {
            freed += pb.data.capacity(); // data is std::vector<uint8_t> — capacity is bytes
            pb.data.clear();
            pb.data.shrink_to_fit();
        }
        geometry_->prepared_buffers.clear();
        geometry_->prepared_buffers.shrink_to_fit();
        if (freed > 0) {
            spdlog::info("[GCode GLES] Freed {} MB of upload buffers after VBO upload",
                         freed / (1024 * 1024));
        }
        // The moving mesh rides along here rather than in set_prebuilt_
        // geometry: a tool-color override rewinds geometry_uploaded_ and
        // re-enters this branch, so the mesh re-uploads with whatever the
        // palette says by then.
        upload_moving_mesh();
        if (gl_render_failed_) {
            return {};
        }
        // The first frame of the uploaded geometry is the next step's job.
        return Outcome{false, true, false};
    }

    case Work::Moving: {
        arm_gpu_guard();
        const bool published = render_moving(*req.gcode, req.camera, req.width, req.height, back);
        clear_gpu_guard();
        moving_state_ = moving_state;
        moving_epoch_ = scene_epoch_;
        return Outcome{published, false, false};
    }

    case Work::Slice: {
        // Layer 2 — crash-loop breaker. Arm the guard file immediately before the
        // real GPU draw; if the driver hard-faults inside run_slice the process
        // dies with the file still present and the next startup promotes it to a
        // persistent block. Cleared right after the first successful frame.
        arm_gpu_guard();
        const bool done = run_slice(*req.gcode, req.camera);
        bool published = false;
        if (done) {
            published = read_frame(req.width, req.height, /*overlays_drawn=*/true, back);
            have_complete_image_ = published;
        }
        // Mid-refinement the widget keeps showing the last finished image
        // rather than a partially drawn one.
        clear_gpu_guard();
        return Outcome{published, !done && job_.active && !gl_render_failed_, false};
    }
    }
    return {};
}

// ============================================================
// GPU crash-loop breaker (Layer 2, issues #966 / #1084 / #1085)
// ============================================================

void GCodeGLESRenderer::arm_gpu_guard() {
    if (gpu_guard_armed_)
        return;
    gpu_guard_armed_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::ofstream guard(path, std::ios::out | std::ios::trunc);
    if (guard.is_open()) {
        guard << "1";
        spdlog::debug("[GCode GLES] Armed GPU crash-loop guard: {}", path);
    } else {
        spdlog::warn("[GCode GLES] Could not write GPU crash-loop guard: {}", path);
    }
}

void GCodeGLESRenderer::clear_gpu_guard() {
    if (!gpu_guard_armed_ || gpu_guard_cleared_)
        return;
    gpu_guard_cleared_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        spdlog::warn("[GCode GLES] Could not remove GPU crash-loop guard {}: {}", path,
                     ec.message());
    } else {
        spdlog::debug("[GCode GLES] Cleared GPU crash-loop guard after first successful frame");
    }
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
