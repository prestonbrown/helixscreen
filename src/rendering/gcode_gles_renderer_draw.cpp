// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer_internal.h"

#ifdef ENABLE_GLES_3D

namespace helix {
namespace gcode {

namespace {

/// Matrix that turns a raw quantized int16 position back into millimetres:
/// `mm = q / scale_factor + min_bounds`. Composed into u_mvp / u_model_view so
/// the GPU can consume PackedVertex positions directly and the vertex shader
/// needs no knowledge of quantization. See PackedVertex in
/// include/gcode_geometry_builder.h.
glm::mat4 dequant_matrix(const QuantizationParams& q) {
    // A zero scale would produce a degenerate matrix and collapse the model to a
    // point; fall back to identity-scale rather than emit NaNs.
    const float inv = (q.scale_factor != 0.0f) ? (1.0f / q.scale_factor) : 1.0f;
    return glm::translate(glm::mat4(1.0f), q.min_bounds) *
           glm::scale(glm::mat4(1.0f), glm::vec3(inv));
}

} // namespace

// Fixed fill light direction (front-right)
static constexpr glm::vec3 kLightFrontDir{0.6985074f, 0.1397015f, 0.6985074f};

// ============================================================
// FBO Rendering
// ============================================================

/// Layer ranges for the solid and ghost passes. ghosting is true when
/// 0 <= progress_layer_ < max_layer.
void GCodeGLESRenderer::pass_ranges(int& draw_start, int& draw_end, int& solid_end,
                                    int& ghost_start, bool& ghosting) const {
    const int max_layer = static_cast<int>(layer_vbos_.size()) - 1;
    draw_start = (layer_start_ >= 0) ? layer_start_ : 0;
    draw_end = (layer_end_ >= 0) ? std::min(layer_end_, max_layer) : max_layer;
    ghosting = progress_layer_ >= 0 && progress_layer_ < max_layer;
    solid_end = ghosting ? std::min(progress_layer_, draw_end) : draw_end;
    ghost_start = ghosting ? std::max(progress_layer_ + 1, draw_start) : draw_end + 1;
}

bool GCodeGLESRenderer::setup_frame(const GCodeCamera& camera, float scale, bool clear,
                                    glm::mat4& mvp, glm::mat4& mvp_dequant,
                                    const RibbonGeometry& geom) {
    int render_w = std::max(1, static_cast<int>(viewport_width_ * scale));
    int render_h = std::max(1, static_cast<int>(viewport_height_ * scale));

    // Create/resize FBO
    if (!create_fbo(render_w, render_h)) {
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    glViewport(0, 0, render_w, render_h);

    if (clear) {
        // Transparent clear: the image composites over the same gradient canvas
        // as the 2D preview, so a pixel nothing drew must carry alpha 0.
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_DEPTH_TEST);

    // Alpha is coverage. The solid pass outputs u_base_alpha = 1.0 and the ghost
    // blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) tops out around 1 - a + a*a, which
    // stays below kExcludedAlpha at the 2% ghost opacity, so the tag values
    // remain writable only by write_alpha_tag(), and a 254 (253) in the readback
    // still means exactly "visible pixel of a selected (excluded) object".

    // Select active geometry. Callers pass the geometry they are about to
    // draw: the main one everywhere except the moving mesh, whose own bounds
    // quantized its vertices.
    active_geometry_ = &geom;

    if (!active_geometry_ || layer_vbos_.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    // Use shader program
    glUseProgram(program_);

    mvp = build_mvp(camera);
    frame_mvp_ = mvp;

    // Normal matrix (inverse transpose of upper-left 3x3 of model-view).
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 view = camera.get_view_matrix();
    glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(view * model)));

    // Vertex positions arrive as raw quantized int16. Dequantization is affine
    // (mm = q / scale_factor + min_bounds), so it composes into the matrices we
    // already upload instead of costing a uniform and a shader edit. Applied on
    // the right so it runs first, before model/view/projection.
    //
    // Normals are unaffected: they are a separate octahedral attribute, and this
    // transform is a uniform positive scale plus a translation, which cannot
    // skew a normal or flip winding.
    const glm::mat4 dequant = dequant_matrix(active_geometry_->quantization);

    // Set uniforms
    mvp_dequant = mvp * dequant;
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp_dequant));
    glUniformMatrix3fv(u_normal_matrix_, 1, GL_FALSE, glm::value_ptr(normal_mat));

    glm::mat4 model_view = view * model * dequant;
    glUniformMatrix4fv(u_model_view_, 1, GL_FALSE, glm::value_ptr(model_view));

    // Light 0: Camera-following directional light (tracks camera position)
    glm::vec3 cam_pos = camera.get_camera_position();
    glm::vec3 cam_target = camera.get_target();
    glm::vec3 cam_light_world = glm::normalize(cam_pos - cam_target);

    // Light 1: Fixed fill light from front-right (prevents black shadows)
    // Both transformed to view space (normals are in view space via u_normal_matrix)
    glm::mat3 view_model_rot = glm::mat3(view * model);
    glm::vec3 light_dirs[2] = {glm::normalize(view_model_rot * cam_light_world),
                               glm::normalize(view_model_rot * kLightFrontDir)};
    glm::vec3 light_colors[2] = {glm::vec3(kCameraLightIntensity), // Camera light: primary
                                 glm::vec3(kFillLightIntensity)};  // Fill light: subtle
    glUniform3fv(u_light_dir_, 2, glm::value_ptr(light_dirs[0]));
    glUniform3fv(u_light_color_, 2, glm::value_ptr(light_colors[0]));

    glm::vec3 ambient{kAmbientIntensity};
    glUniform3fv(u_ambient_, 1, glm::value_ptr(ambient));

    // Material
    glUniform1f(u_specular_intensity_, specular_intensity_);
    glUniform1f(u_specular_shininess_, specular_shininess_);
    // Matches depth_shading::kLiftStrength on the 2D path, so the same model in
    // the same filament reads the same way in either renderer.
    glUniform1f(u_lift_strength_, helix::gcode::depth_shading::kLiftStrength);

    // Per-vertex color mode: use vertex colors when geometry has a color palette.
    // With per-tool AMS overrides, the palette is updated in-place so vertex colors
    // always reflect the correct AMS slot colors. Only fall back to uniform color
    // when palette has a single-tool override (legacy path).
    bool has_palette = active_geometry_ && !active_geometry_->color_palette.empty();
    bool has_vertex_colors = has_palette && !palette_.has_override;
    glUniform1f(u_use_vertex_color_, has_vertex_colors ? 1.0f : 0.0f);
    return true;
}

/// How far the ghost pass lightens toward white.
static constexpr float kGhostLightenScale = 4.0f;

bool GCodeGLESRenderer::render_moving(const ParsedGCodeFile& gcode, const GCodeCamera& camera,
                                      int widget_w, int widget_h, RenderFrame& out) {
    cancel_job();
    moving_frame_valid_ = false;
    const auto frame_t0 = std::chrono::steady_clock::now();
    const render_schedule::MovingPlan plan = render_schedule::hold_half_resolution(
        render_schedule::plan_moving(uploaded_triangles_, mesh_triangles_, gpu_rate_tris_per_ms_),
        moving_half_res_held_);
    moving_half_res_held_ = plan.half_resolution;
    // The mesh has its own bounds and its own quantization, so it must be the
    // geometry setup_frame dequantizes with, and it replaces the layer VBOs
    // the draw reads. Stride stays 1: the mesh is already banded.
    const std::vector<LayerVBO>* draw_vbos = &layer_vbos_;
    const RibbonGeometry* draw_geom = geometry_.get();
    if (plan.use_mesh && geometry_ && geometry_->moving_mesh && !moving_vbos_.empty()) {
        draw_vbos = &moving_vbos_;
        draw_geom = geometry_->moving_mesh.get();
    }
    glm::mat4 mvp, mvp_dequant;
    if (!setup_frame(camera, plan.half_resolution ? 0.5f : 1.0f, true, mvp, mvp_dequant,
                     *draw_geom)) {
        return false;
    }

    int draw_start, draw_end, solid_end, ghost_start;
    bool ghosting;
    pass_ranges(draw_start, draw_end, solid_end, ghost_start, ghosting);

    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();

    if (ghosting) {
        if (draw_start <= solid_end) {
            draw_layers(*draw_vbos, draw_start, solid_end, 1.0f, 1.0f, plan.stride);
        }
        if (ghost_start <= draw_end) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            draw_layers(*draw_vbos, ghost_start, draw_end, kGhostLightenScale,
                        ghost_opacity_ / 255.0f, plan.stride);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    } else {
        draw_layers(*draw_vbos, draw_start, draw_end, 1.0f, 1.0f, plan.stride);
    }

    glUseProgram(0);
    render_brackets_3d(gcode, mvp);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    record_rate(triangles_rendered_ - before, ms, /*publish=*/false);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    moving_frame_valid_ = read_frame(widget_w, widget_h, /*overlays_drawn=*/false, out);
    const float frame_ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - frame_t0)
            .count();
    spdlog::trace("[GCode GLES] Moving frame: {}stride {}, {} res, {:.1f}ms draw, {:.1f}ms total",
                  plan.use_mesh && draw_vbos == &moving_vbos_ ? "mesh " : "", plan.stride,
                  plan.half_resolution ? "half" : "full", ms, frame_ms);
    // A gesture's first frame pays for resizing the FBO from the still size, so
    // only the frames after it say how fast rotating is.
    const bool counts = moving_frames_in_gesture_++ > 0;
    if (counts &&
        render_schedule::moving_too_slow(slow_moving_streak_, frame_ms, plan.half_resolution)) {
        spdlog::warn("[GCode GLES] Moving frames take {:.0f}ms at half resolution (over {:.0f}ms "
                     "{} times running): too slow to rotate, falling back to 2D",
                     frame_ms, render_schedule::kMaxUsableMovingFrameMs,
                     render_schedule::kSlowMovingFramesToFallBack);
        too_slow_ = true;
        free_vbos(layer_vbos_);
        free_vbos(moving_vbos_);
        gl_render_failed_ = true;
        cancel_job();
    }
    return moving_frame_valid_;
}

int GCodeGLESRenderer::draw_layers(const std::vector<LayerVBO>& vbos, int layer_start,
                                   int layer_end, float color_scale, float alpha, int stride,
                                   size_t max_triangles) {
    // Set uniforms for this draw batch
    glUniform4fv(u_base_color_, 1, glm::value_ptr(filament_color_));
    glUniform1f(u_color_scale_, color_scale);
    glUniform1f(u_base_alpha_, alpha);

    constexpr size_t kStride = PackedVertex::stride();

    // Enable vertex attributes once before the loop (a_position_ and a_normal_
    // are validated >= 0 during compile_shaders)
    glEnableVertexAttribArray(static_cast<GLuint>(a_position_));
    glEnableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_color_));
    }

    size_t submitted = 0;
    int layer = layer_start;
    for (; layer <= layer_end; layer += stride) {
        if (layer < 0 || layer >= static_cast<int>(vbos.size()))
            continue;
        const auto& lv = vbos[static_cast<size_t>(layer)];
        if (!lv.vbo || lv.vertex_count == 0)
            continue;

        glBindBuffer(GL_ARRAY_BUFFER, lv.vbo);

        // Position: quantized int16, NOT normalized — the raw integer reaches the
        // shader and u_mvp / u_model_view carry the dequantization.
        glVertexAttribPointer(static_cast<GLuint>(a_position_), 3, GL_SHORT, GL_FALSE,
                              static_cast<GLsizei>(kStride),
                              reinterpret_cast<void*>(PackedVertex::position_offset()));

        // Normal: int8[2] octahedral, decoded in the vertex shader.
        glVertexAttribPointer(static_cast<GLuint>(a_normal_), 2, GL_BYTE, GL_TRUE,
                              static_cast<GLsizei>(kStride),
                              reinterpret_cast<void*>(PackedVertex::normal_offset()));

        if (a_color_ >= 0) {
            // Color: RGBA8 unorm; alpha byte is present but ignored by the shader.
            glVertexAttribPointer(static_cast<GLuint>(a_color_), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(kStride),
                                  reinterpret_cast<void*>(PackedVertex::color_offset()));
        }

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(lv.vertex_count));
        triangles_rendered_ += lv.vertex_count / 3;
        submitted += lv.vertex_count / 3;
        if (submitted >= max_triangles) {
            layer += stride;
            break;
        }
    }

    glDisableVertexAttribArray(static_cast<GLuint>(a_position_));
    glDisableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glDisableVertexAttribArray(static_cast<GLuint>(a_color_));
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // One glGetError() per draw batch — NOT per primitive (a per-glDrawArrays
    // check would stall the GPU pipeline). On constrained Mali/Panfrost GPUs
    // (e.g. Allwinner CB1) the driver can fault under memory pressure with the
    // phong shader + ribbon VBOs; a fatal error here means we must abandon GPU
    // rendering. The viewer polls render_failed() after render() and falls back
    // to the pure-CPU 2D renderer for the rest of the session.
    GLenum draw_err = glGetError();
    if (gl_draw_error_is_fatal(draw_err)) {
        spdlog::error("[GCode GLES] Fatal GL error after glDrawArrays: 0x{:04X} — disabling GPU "
                      "rendering, falling back to 2D",
                      draw_err);
        gl_render_failed_ = true;
        cancel_job();
    }
    return layer;
}

// ============================================================
// Time-sliced refinement
// ============================================================

void GCodeGLESRenderer::start_job(bool incremental, const CachedRenderState& scene) {
    const int previous_progress = job_.progress_layer;
    int draw_start, draw_end, solid_end, ghost_start;
    bool ghosting;
    pass_ranges(draw_start, draw_end, solid_end, ghost_start, ghosting);
    job_ = RefineJob{};
    job_.active = true;
    job_.incremental = incremental;
    job_.phase = JobPhase::Solid;
    job_.solid_end_limit = draw_end;
    job_.progress_layer = progress_layer_;
    job_.started = std::chrono::steady_clock::now();
    if (incremental) {
        job_.first_slice = false;
        job_.solid_start = std::max(previous_progress + 1, draw_start);
        job_.solid_end = solid_end;
        job_.ghost_start = 0;
        job_.ghost_end = -1;
    } else {
        job_.solid_start = draw_start;
        job_.solid_end = ghosting ? solid_end : draw_end;
        job_.ghost_start = ghost_start;
        job_.ghost_end = ghosting ? draw_end : -1;
        have_complete_image_ = false;
    }
    job_.next_layer = job_.solid_start;
    job_scene_ = scene;
}

bool GCodeGLESRenderer::run_slice(const ParsedGCodeFile& gcode, const GCodeCamera& camera) {
    glm::mat4 mvp, mvp_dequant;
    if (!geometry_) {
        cancel_job();
        return false;
    }
    if (!setup_frame(camera, kStillSupersample, job_.first_slice, mvp, mvp_dequant, *geometry_)) {
        cancel_job();
        return false;
    }
    job_.first_slice = false;
    const size_t quota = render_schedule::slice_quota(gpu_rate_tris_per_ms_);
    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();
    while (job_.phase != JobPhase::Done && triangles_rendered_ - before < quota) {
        const size_t left = quota - (triangles_rendered_ - before);
        if (job_.phase == JobPhase::Solid) {
            if (job_.next_layer > job_.solid_end) {
                job_.phase =
                    job_.ghost_start <= job_.ghost_end ? JobPhase::Ghost : JobPhase::Overlays;
                job_.next_layer = job_.ghost_start;
                continue;
            }
            job_.next_layer =
                draw_layers(layer_vbos_, job_.next_layer, job_.solid_end, 1.0f, 1.0f, 1, left);
        } else if (job_.phase == JobPhase::Ghost) {
            if (job_.next_layer > job_.ghost_end) {
                job_.phase = JobPhase::Overlays;
                continue;
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            job_.next_layer = draw_layers(layer_vbos_, job_.next_layer, job_.ghost_end,
                                          kGhostLightenScale, ghost_opacity_ / 255.0f, 1, left);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        } else { // Overlays: the selection tag and brackets a moving frame skips
            glUseProgram(0);
            // The tag covers exactly the solid layers (solid_end is the
            // progress layer while ghosting, the last drawn layer otherwise).
            // An incremental job drew onto a finished image that already
            // carries its tag. The excluded grey is per-layer, so it covers
            // exactly the layers this job drew, incremental or not.
            render_excluded(gcode, mvp_dequant, job_.solid_start, job_.solid_end);
            if (gl_render_failed_)
                return false;
            if (!job_.incremental) {
                render_selection_tag(gcode, mvp_dequant, job_.solid_start, job_.solid_end);
                if (gl_render_failed_)
                    return false;
            }
            render_brackets_3d(gcode, mvp);
            job_.phase = JobPhase::Done;
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    record_rate(triangles_rendered_ - before, ms, /*publish=*/true);
    job_.slices++;
    job_.max_slice_ms = std::max(job_.max_slice_ms, ms);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (job_.phase != JobPhase::Done) {
        return false;
    }
    job_.active = false;
    spdlog::debug(
        "[GCode GLES] Refine done: {} slices, max slice {:.1f}ms, {:.0f}ms wall, "
        "rate {:.0f} tris/ms{}",
        job_.slices, job_.max_slice_ms,
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - job_.started)
            .count(),
        gpu_rate_tris_per_ms_, job_.incremental ? " (incremental)" : "");
    return true;
}

void GCodeGLESRenderer::cancel_job() {
    job_ = RefineJob{};
    have_complete_image_ = false;
}

// ============================================================
// LVGL Output
// ============================================================

void GCodeGLESRenderer::draw_cached_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords) {
    // LVGL thread: the newest finished frame render() installed.
    if (!draw_buf_ || !draw_buf_->data)
        return;

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area = helix::draw_buf_blit_area(*draw_buf_, *widget_coords);
    lv_draw_image(layer, &img_dsc, &area);
}

bool GCodeGLESRenderer::read_frame(int widget_w, int widget_h, bool overlays_drawn,
                                   RenderFrame& out) {
    if (!fbo_.id || widget_w <= 0 || widget_h <= 0)
        return false;

    // Read pixels from FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);

    // Read RGBA from GPU (matches GL_RGBA8_OES renderbuffer format)
    // Reuse persistent readback buffer to avoid per-frame allocation
    size_t readback_size = static_cast<size_t>(fbo_width_ * fbo_height_ * 4);
    if (readback_buf_.size() != readback_size) {
        readback_buf_.resize(readback_size);
    }
    glReadPixels(0, 0, fbo_width_, fbo_height_, GL_RGBA, GL_UNSIGNED_BYTE, readback_buf_.data());
    check_gl_error("glReadPixels");

    // The white silhouette, derived from the tag render_selection_tag() left in
    // the alpha byte. Byte 3 is alpha in the readback's RGBA and in the
    // rasterizer's ARGB8888 alike, so the tag scan reads the same offset either
    // way — but the rim COLOUR is an XML token and need not be grey, so the
    // readback's channel order has to be named. glReadPixels above asked for
    // GL_RGBA, which puts red at byte 0 where an ARGB8888 buffer puts blue.
    if (selection_.any_highlighted()) {
        // The rim is sized for the widget the frame is displayed at, then
        // scaled into readback pixels: a supersampled still strokes its rim at
        // 2x so the box filter lands it back at the same on-screen width a
        // widget-sized frame would have shown.
        const int rim = selection::outline_width_px_scaled(widget_w, fbo_width_);
        const RasterTarget rt{readback_buf_.data(), static_cast<size_t>(fbo_width_) * 4, fbo_width_,
                              fbo_height_};
        size_t tagged = 0;
        if (spdlog::should_log(spdlog::level::trace)) {
            for (size_t i = 3; i < readback_buf_.size(); i += 4) {
                tagged += (readback_buf_[i] == helix::gcode::kSelectedAlpha);
            }
        }
        helix::gcode::stroke_selection_rim(rt, rim, rim, sel_palette_.outline,
                                           helix::gcode::ChannelOrder::Rgba);
        spdlog::trace("[GCode GLES] Selection rim: {} tagged px of {}, rim {}px", tagged,
                      static_cast<size_t>(fbo_width_) * fbo_height_, rim);
    }

    // The red stripes over excluded objects, from the tag render_excluded() left.
    // Scaled like the rim, and told the readback is bottom-up so the stripes
    // lean the same way as the 2D view's. A moving frame skips the overlays, so
    // it carries no tag to find.
    if (overlays_drawn && selection_.any_excluded()) {
        const RasterTarget rt{readback_buf_.data(), static_cast<size_t>(fbo_width_) * 4, fbo_width_,
                              fbo_height_};
        helix::gcode::stroke_exclusion_hatch(
            rt, selection::scale_px(selection::kHatchPeriodPx, widget_w, fbo_width_),
            selection::scale_px(selection::kHatchStripePx, widget_w, fbo_width_),
            sel_palette_.excluded, helix::gcode::ChannelOrder::Rgba,
            helix::gcode::RowOrder::BottomUp);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Convert GL RGBA → LVGL ARGB8888 (BGR byte order + coverage alpha), flip Y,
    // and scale if needed. The rim pass above has already consumed the tag, so
    // from here on alpha means opacity: any nonzero readback alpha (the 254 tag
    // included) becomes fully opaque, and only never-drawn pixels stay clear.
    out.width = widget_w;
    out.height = widget_h;
    out.mvp = frame_mvp_;
    out.generation = scene_gen_.load();
    out.pixels.resize(static_cast<size_t>(widget_w) * widget_h * 4);
    auto* dest = out.pixels.data();
    const auto* src = readback_buf_.data();
    bool needs_scale = (fbo_width_ != widget_w || fbo_height_ != widget_h);
    const size_t dst_stride = static_cast<size_t>(widget_w) * 4;

    // A 2x supersampled frame comes down through a 2x2 box filter: the extra
    // samples are the whole point of rendering big, and nearest sampling would
    // throw three of every four away. Any other size ratio (the half-res
    // moving FBO upscaled, or a same-size frame) takes the paths below.
    if (fbo_width_ == widget_w * 2 && fbo_height_ == widget_h * 2) {
        const auto avg4 = [](unsigned a, unsigned b, unsigned c, unsigned d) {
            return static_cast<uint8_t>((a + b + c + d + 2) / 4);
        };
        // A partially covered block averages to partial alpha, and its averaged
        // RGB is coverage-scaled (empty samples cleared to black). LVGL wants
        // straight alpha, so divide the darkness back out.
        const auto unpremul = [](uint8_t v, uint8_t a) {
            return static_cast<uint8_t>(std::min<unsigned>(255, v * 255u / a));
        };
        for (int dy = 0; dy < widget_h; ++dy) {
            // Y-flip on the source, same as the loops below.
            const uint8_t* row_a =
                src + static_cast<size_t>(fbo_height_ - 1 - (dy * 2)) * fbo_width_ * 4;
            const uint8_t* row_b =
                src + static_cast<size_t>(fbo_height_ - 1 - (dy * 2 + 1)) * fbo_width_ * 4;
            auto* dst_row = dest + static_cast<size_t>(dy) * dst_stride;
            for (int dx = 0; dx < widget_w; ++dx) {
                const size_t s0 = static_cast<size_t>(dx * 2) * 4;
                const size_t s1 = s0 + 4;
                const size_t di = static_cast<size_t>(dx) * 4;
                const uint8_t a = avg4(row_a[s0 + 3] ? 255u : 0u, row_a[s1 + 3] ? 255u : 0u,
                                       row_b[s0 + 3] ? 255u : 0u, row_b[s1 + 3] ? 255u : 0u);
                if (a == 0) {
                    dst_row[di + 0] = dst_row[di + 1] = dst_row[di + 2] = dst_row[di + 3] = 0;
                    continue;
                }
                uint8_t r = avg4(row_a[s0 + 0], row_a[s1 + 0], row_b[s0 + 0], row_b[s1 + 0]);
                uint8_t g = avg4(row_a[s0 + 1], row_a[s1 + 1], row_b[s0 + 1], row_b[s1 + 1]);
                uint8_t b = avg4(row_a[s0 + 2], row_a[s1 + 2], row_b[s0 + 2], row_b[s1 + 2]);
                if (a != 255) {
                    r = unpremul(r, a);
                    g = unpremul(g, a);
                    b = unpremul(b, a);
                }
                dst_row[di + 0] = b;
                dst_row[di + 1] = g;
                dst_row[di + 2] = r;
                dst_row[di + 3] = a;
            }
        }
        return true;
    }

    // Row-based conversion: RGBA→BGRA with Y-flip
    for (int dy = 0; dy < widget_h; ++dy) {
        int sy = needs_scale ? (dy * fbo_height_ / widget_h) : dy;
        int gl_row = fbo_height_ - 1 - sy;
        const auto* src_row = src + static_cast<size_t>(gl_row * fbo_width_) * 4;
        auto* dst_row = dest + static_cast<size_t>(dy) * dst_stride;

        if (needs_scale) {
            for (int dx = 0; dx < widget_w; ++dx) {
                int sx = dx * fbo_width_ / widget_w;
                size_t si = static_cast<size_t>(sx) * 4;
                size_t di = static_cast<size_t>(dx) * 4;
                dst_row[di + 0] = src_row[si + 2];           // B
                dst_row[di + 1] = src_row[si + 1];           // G
                dst_row[di + 2] = src_row[si + 0];           // R
                dst_row[di + 3] = src_row[si + 3] ? 255 : 0; // A
            }
        } else {
            // No scaling: convert entire row RGBA→BGRA
            for (int dx = 0; dx < widget_w; ++dx) {
                size_t si = static_cast<size_t>(dx) * 4;
                size_t di = static_cast<size_t>(dx) * 4;
                dst_row[di + 0] = src_row[si + 2];           // B
                dst_row[di + 1] = src_row[si + 1];           // G
                dst_row[di + 2] = src_row[si + 0];           // R
                dst_row[di + 3] = src_row[si + 3] ? 255 : 0; // A
            }
        }
    }
    return true;
}

// ============================================================
// Selection Brackets (3D, GPU-side — drawn inside the FBO)
// ============================================================

static const char* kLineVertexShader = R"(
    uniform mat4 u_mvp;
    attribute vec3 a_position;
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
    }
)";

static const char* kLineFragmentShader = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() {
        gl_FragColor = u_color;
    }
)";

bool GCodeGLESRenderer::init_line_program() {
    if (line_program_)
        return true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kLineVertexShader);
    if (!vs)
        return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kLineFragmentShader);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Line program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    line_program_ = prog;
    line_u_mvp_ = glGetUniformLocation(prog, "u_mvp");
    line_u_color_ = glGetUniformLocation(prog, "u_color");
    line_a_position_ = glGetAttribLocation(prog, "a_position");

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    line_vbo_ = vbo;
    return true;
}

void GCodeGLESRenderer::render_brackets_3d(const ParsedGCodeFile& gcode, const glm::mat4& mvp) {
    if (!selection_.any_highlighted())
        return;
    if (!line_program_ && !init_line_program())
        return;

    // Collect line endpoints (one vec3 per vertex, two verts per line).
    // 8 corners * 3 axes * 2 verts = 48 vertices per object.
    std::vector<glm::vec3> verts;
    verts.reserve(selection_.highlighted().size() * 48);

    for (const auto& name : selection_.highlighted()) {
        auto it = gcode.objects.find(name);
        if (it == gcode.objects.end())
            continue;
        const AABB& bbox = it->second.bounding_box;
        if (bbox.is_empty())
            continue;

        // Run the bbox through the same quantization the geometry pipeline
        // applied to vertices, so brackets sit on the rendered surface rather
        // than offset by a fraction of a quantization step.
        glm::vec3 bmin = bbox.min;
        glm::vec3 bmax = bbox.max;
        if (geometry_) {
            const auto& quant = geometry_->quantization;
            bmin = quant.dequantize_vec3(quant.quantize_vec3(bbox.min));
            bmax = quant.dequantize_vec3(quant.quantize_vec3(bbox.max));
        }

        // Shared sizing, so 2D and 3D bracket proportions cannot drift apart.
        // Measured on the quantized box, not the raw one.
        const AABB quantized{bmin, bmax};
        const float bracket_len = selection::bracket_arm_length(quantized);
        if (bracket_len == 0.0f)
            continue;

        quantized.for_each_bracket_arm(bracket_len,
                                       [&](const glm::vec3& origin, const glm::vec3& tip) {
                                           verts.push_back(origin);
                                           verts.push_back(tip);
                                       });
    }

    if (verts.empty())
        return;

    // Draw on top of existing geometry. Brackets are unlit and uniform color.
    glDisable(GL_DEPTH_TEST);
    glUseProgram(line_program_);
    glUniformMatrix4fv(line_u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    // Silver, fully opaque, from the same constant the 2D path draws.
    const glm::vec4 bracket_rgba = selection::to_vec4(sel_palette_.bracket);
    glUniform4f(line_u_color_, bracket_rgba.r, bracket_rgba.g, bracket_rgba.b, bracket_rgba.a);

    glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec3)),
                 verts.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glVertexAttribPointer(static_cast<GLuint>(line_a_position_), 3, GL_FLOAT, GL_FALSE,
                          sizeof(glm::vec3), nullptr);

    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));

    glDisableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

// ============================================================
// Selection Silhouette (3D shell pass, drawn under the lit geometry)
// ============================================================

static const char* kShellVertexDecls = R"(
    // Selection tag. Consumes the same packed vertex layout as the lit program,
    // so it draws straight from the layer VBOs with no extra buffer:
    //   a_position : vec3, raw quantized int16 (dequantization folded into u_mvp)
    // The normal is not read: this pass reproduces the object exactly where the
    // lit pass already drew it, and writes nothing but alpha.
    uniform mat4 u_mvp;
    attribute vec3 a_position;
)";

static const char* kShellVertexMain = R"(
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
    }
)";

bool GCodeGLESRenderer::init_shell_program() {
    if (shell_program_)
        return true;

    const std::string vertex_src =
        std::string(kShellVertexDecls) + kOctDecodeGlsl + kShellVertexMain;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src.c_str());
    if (!vs)
        return false;
    // Same flat-color fragment shader the bracket program uses — one uniform color,
    // no lighting. No reason for a second copy.
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kLineFragmentShader);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Shell program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    shell_u_mvp_ = glGetUniformLocation(prog, "u_mvp");
    shell_u_color_ = glGetUniformLocation(prog, "u_color");
    shell_a_position_ = glGetAttribLocation(prog, "a_position");

    if (shell_a_position_ < 0) {
        spdlog::error("[GCode GLES] Tag program missing a_position");
        glDeleteProgram(prog);
        return false;
    }

    shell_program_ = prog;
    spdlog::debug("[GCode GLES] Selection tag program ready");
    return true;
}

std::vector<bool> GCodeGLESRenderer::object_mask(const ParsedGCodeFile& gcode,
                                                 const std::unordered_set<std::string>& names) {
    // Same table the geometry builder resolved segment.object_name_index against
    // (the viewer builds the geometry from, and renders, one ParsedGCodeFile), so
    // the indices in object_runs are directly comparable. An indexed array rather
    // than a set: it is consulted per run per layer inside a per-frame pass.
    std::vector<bool> mask(gcode.object_name_table.size(), false);
    bool any = false;
    for (size_t i = 0; i < gcode.object_name_table.size(); ++i) {
        if (names.count(gcode.object_name_table[i]) != 0) {
            mask[i] = true;
            any = true;
        }
    }
    if (!any) {
        mask.clear();
    }
    return mask;
}

size_t GCodeGLESRenderer::draw_object_runs(const std::vector<bool>& mask, int layer_start,
                                           int layer_end, int a_position, int a_normal) {
    constexpr size_t kStride = PackedVertex::stride();
    size_t runs_drawn = 0;

    glEnableVertexAttribArray(static_cast<GLuint>(a_position));
    if (a_normal >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_normal));
    }

    for (int layer = layer_start; layer <= layer_end; ++layer) {
        if (layer < 0 || layer >= static_cast<int>(layer_vbos_.size()))
            continue;
        const auto& lv = layer_vbos_[static_cast<size_t>(layer)];
        if (!lv.vbo || lv.vertex_count == 0)
            continue;

        const auto runs = active_geometry_->layer_object_runs(static_cast<size_t>(layer));
        if (runs.empty())
            continue;

        bool bound = false;
        for (const auto& run : runs) {
            if (run.object_index < 0 || static_cast<size_t>(run.object_index) >= mask.size() ||
                !mask[static_cast<size_t>(run.object_index)]) {
                continue;
            }
            // Runs are validated at build time, but the VBO can lag the geometry
            // during incremental upload — never let a stale run read past the buffer.
            if (static_cast<size_t>(run.vertex_offset) + run.vertex_count > lv.vertex_count)
                continue;

            if (!bound) {
                glBindBuffer(GL_ARRAY_BUFFER, lv.vbo);
                glVertexAttribPointer(static_cast<GLuint>(a_position), 3, GL_SHORT, GL_FALSE,
                                      static_cast<GLsizei>(kStride),
                                      reinterpret_cast<void*>(PackedVertex::position_offset()));
                if (a_normal >= 0) {
                    glVertexAttribPointer(static_cast<GLuint>(a_normal), 2, GL_BYTE, GL_TRUE,
                                          static_cast<GLsizei>(kStride),
                                          reinterpret_cast<void*>(PackedVertex::normal_offset()));
                }
                bound = true;
            }

            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.vertex_offset),
                         static_cast<GLsizei>(run.vertex_count));
            ++runs_drawn;
        }
    }

    glDisableVertexAttribArray(static_cast<GLuint>(a_position));
    if (a_normal >= 0) {
        glDisableVertexAttribArray(static_cast<GLuint>(a_normal));
    }
    return runs_drawn;
}

bool GCodeGLESRenderer::check_overlay_error(const char* pass) {
    // One error check per pass, matching draw_layers. A fault here is the same
    // class of driver failure and must fall back the same way.
    GLenum err = glGetError();
    if (!gl_draw_error_is_fatal(err)) {
        return true;
    }
    spdlog::error("[GCode GLES] Fatal GL error in {} pass: 0x{:04X} — disabling GPU rendering, "
                  "falling back to 2D",
                  pass, err);
    gl_render_failed_ = true;
    cancel_job();
    return false;
}

void GCodeGLESRenderer::write_alpha_tag(const std::vector<bool>& mask, const glm::mat4& mvp_dequant,
                                        int layer_start, int layer_end, uint8_t tag) {
    // Alpha only. The color channels already hold the lit image and must survive
    // untouched; overwriting alpha with the flat tag value is what separates a
    // tagged pixel from plain coverage (255).
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glUseProgram(shell_program_);
    glUniformMatrix4fv(shell_u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp_dequant));
    glUniform4f(shell_u_color_, 0.0f, 0.0f, 0.0f, static_cast<float>(tag) / 255.0f);
    const size_t runs = draw_object_runs(mask, layer_start, layer_end, shell_a_position_, -1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    spdlog::trace("[GCode GLES] Alpha tag {}: {} runs over layers {}..{}", tag, runs, layer_start,
                  layer_end);
}

/// Saves the GL state an overlay pass touches and restores it on scope exit;
/// anything left unrestored corrupts the frame.
///
/// Depth test stays enabled, but the function is relaxed to LEQUAL. The renderer
/// otherwise runs on the GL default of LESS, and re-drawing a triangle at exactly
/// the depth it already wrote fails LESS on every fragment. With LEQUAL it passes
/// exactly where this object is the frontmost thing and fails where something
/// else occludes it, which is the definition of "visible". Depth writes go off:
/// the buffer is already correct.
namespace {
struct OverlayGlState {
    GLint program = 0;
    GLint buffer = 0;
    GLboolean depth_mask = GL_TRUE;
    GLint depth_func = GL_LESS;
    GLboolean blend = GL_FALSE;

    OverlayGlState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        blend = glIsEnabled(GL_BLEND);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
    }
    ~OverlayGlState() {
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
        glDepthMask(depth_mask);
        glDepthFunc(static_cast<GLenum>(depth_func));
        if (blend) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(static_cast<GLuint>(program));
    }
    OverlayGlState(const OverlayGlState&) = delete;
    OverlayGlState& operator=(const OverlayGlState&) = delete;
};
} // namespace

void GCodeGLESRenderer::render_excluded(const ParsedGCodeFile& gcode, const glm::mat4& mvp_dequant,
                                        int solid_start, int solid_end) {
    if (!selection_.any_excluded())
        return;
    if (!active_geometry_ || active_geometry_->object_runs.empty())
        return; // no exclude-object metadata, or the run-count guard tripped
    const std::vector<bool> mask = object_mask(gcode, selection_.excluded());
    if (mask.empty())
        return;
    if (!shell_program_ && !init_shell_program())
        return;

    {
        OverlayGlState saved;

        // Re-light the excluded runs in flat grey with the lit pass's own
        // program: the shading survives, the filament hue does not. Every other
        // uniform (matrices, lights) is still what setup_frame left this frame,
        // and setup_frame re-sets the ones changed here before the next draw.
        glUseProgram(program_);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glUniform1f(u_use_vertex_color_, 0.0f);
        glUniform4fv(u_base_color_, 1,
                     glm::value_ptr(selection::to_vec4(selection::excluded_grey(0xFFFFFF))));
        glUniform1f(u_color_scale_, 1.0f);

        glDisable(GL_BLEND);
        glUniform1f(u_base_alpha_, 1.0f);
        draw_object_runs(mask, solid_start, solid_end, a_position_, a_normal_);

        write_alpha_tag(mask, mvp_dequant, solid_start, solid_end, kExcludedAlpha);
    }
    check_overlay_error("excluded");
}

void GCodeGLESRenderer::render_selection_tag(const ParsedGCodeFile& gcode,
                                             const glm::mat4& mvp_dequant, int layer_start,
                                             int layer_end) {
    // Gate before anything else, including the program link: an unselected plate
    // must cost nothing at all.
    if (!selection_.any_highlighted())
        return;
    if (!active_geometry_ || active_geometry_->object_runs.empty())
        return; // no exclude-object metadata, or the run-count guard tripped
    if (layer_end < layer_start)
        return;
    const std::vector<bool> mask = object_mask(gcode, selection_.highlighted());
    if (mask.empty())
        return;
    if (!shell_program_ && !init_shell_program())
        return;

    {
        OverlayGlState saved;
        write_alpha_tag(mask, mvp_dequant, layer_start, layer_end, kSelectedAlpha);
    }
    check_overlay_error("selection tag");
}

// ============================================================
// Object Picking (CPU-side, no GL needed)
// ============================================================

std::optional<std::string> GCodeGLESRenderer::pick_object(const glm::vec2& screen_pos,
                                                          const ParsedGCodeFile& gcode,
                                                          const GCodeCamera& camera) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    glm::mat4 transform = build_mvp(camera);
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    constexpr float kPickThreshold = selection::kPickThresholdPx;

    // Stage 1: which objects could the tap possibly be on?
    //
    // Stage 2 costs two mat4 multiplies and a point-to-segment distance for
    // EVERY segment of every visible layer, and the test plate parses to 135,197
    // of them. Projecting eight corners per object first, and skipping segments
    // whose object's projected box is nowhere near the tap, turns most of that
    // into one array lookup. The 2D picker got this treatment in 1ee6ba494; this
    // is the same idea against the same data.
    //
    // Objects are keyed by interned index, so the candidate set is a flat array
    // rather than a set of strings.
    std::vector<bool> candidate(gcode.object_name_table.size(), false);
    bool any_candidate = false;
    for (const auto& [name, obj] : gcode.objects) {
        const AABB& box = obj.bounding_box;
        // Defined but never extruded: the corners are +/-inf and projecting them
        // yields garbage that would match every tap.
        if (box.is_empty()) {
            continue;
        }
        int16_t idx = -1;
        for (size_t i = 0; i < gcode.object_name_table.size(); ++i) {
            if (gcode.object_name_table[i] == name) {
                idx = static_cast<int16_t>(i);
                break;
            }
        }
        if (idx < 0) {
            continue;
        }

        // This used to reject corners with std::abs(clip.w) < EPSILON, which let a
        // corner BEHIND the camera through and mirrored it onto the screen,
        // corrupting the candidate box. project_aabb_to_screen() rejects w <= EPSILON.
        const ScreenBounds sb =
            project_aabb_to_screen(transform, box, viewport_width_, viewport_height_);
        // A box that projects to nothing usable stays a candidate: better to pay
        // for stage 2 than to drop a pick outright.
        if (!sb.valid || sb.contains(screen_pos.x, screen_pos.y, kPickThreshold)) {
            candidate[static_cast<size_t>(idx)] = true;
            any_candidate = true;
        }
    }
    // No object metadata at all (or nothing near the tap): fall through with
    // everything eligible rather than silently refusing to pick.
    if (!any_candidate && gcode.objects.empty()) {
        candidate.assign(candidate.size(), true);
    }

    int ls = layer_start_;
    int le = (layer_end_ < 0 || layer_end_ >= static_cast<int>(gcode.layers.size()))
                 ? static_cast<int>(gcode.layers.size()) - 1
                 : layer_end_;

    for (int layer_idx = ls; layer_idx <= le; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode.layers.size()))
            continue;
        const auto& layer = gcode.layers[static_cast<size_t>(layer_idx)];

        for (const auto& segment : layer.segments) {
            // The shared draw decision: auxiliary geometry (purge, prime
            // tower) is dropped by the builder, so it must not be pickable
            // here either - a tap on the blank tower area must select
            // nothing, not an invisible object.
            if (!segment_drawable(segment, /*is_support=*/false, show_extrusions_, show_extrusions_,
                                  /*show_travel=*/false)) {
                continue;
            }
            if (segment.object_name_index < 0)
                continue;
            // Stage 1 result: one array lookup instead of two mat4 multiplies.
            if (static_cast<size_t>(segment.object_name_index) < candidate.size() &&
                !candidate[static_cast<size_t>(segment.object_name_index)]) {
                continue;
            }

            // Was std::abs(w) < EPSILON here too, with the same behind-camera
            // defect as the candidate pass above.
            const auto start_screen =
                project_clip_to_screen(transform, segment.start, viewport_width_, viewport_height_);
            const auto end_screen =
                project_clip_to_screen(transform, segment.end, viewport_width_, viewport_height_);
            if (!start_screen || !end_screen)
                continue;

            // Both endpoints must be on screen. This was spelled as an NDC range
            // check on [-1, 1]; in screen pixels that is exactly the viewport rect.
            const auto on_screen = [&](const glm::vec2& p) {
                return p.x >= 0.0f && p.x <= static_cast<float>(viewport_width_) && p.y >= 0.0f &&
                       p.y <= static_cast<float>(viewport_height_);
            };
            if (!on_screen(*start_screen) || !on_screen(*end_screen)) {
                continue;
            }

            const float dist = point_segment_distance(screen_pos, *start_screen, *end_screen);

            if (dist < kPickThreshold && dist < closest_distance) {
                closest_distance = dist;
                picked_object = gcode.get_object_name(segment.object_name_index);
            }
        }
    }

    // Fallback: after 3D geometry build, ParsedGCodeFile::clear_segments() frees
    // per-layer toolpath data to save memory — segment iteration above then
    // returns nothing. Project each defined object's AABB to screen space and
    // hit-test the resulting 2D bounding rect so taps still work post-build.
    if (!picked_object) {
        float closest_inside_area = std::numeric_limits<float>::max();
        for (const auto& [name, obj] : gcode.objects) {
            const auto& bbox = obj.bounding_box;
            if (bbox.is_empty())
                continue;

            const ScreenBounds sb =
                project_aabb_to_screen(transform, bbox, viewport_width_, viewport_height_);
            if (!sb.valid)
                continue;

            if (!sb.contains(screen_pos.x, screen_pos.y))
                continue;

            // Prefer the smallest (innermost) hit when objects overlap on screen.
            float area = sb.area();
            if (area < closest_inside_area) {
                closest_inside_area = area;
                picked_object = name;
            }
        }
    }

    return picked_object;
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
