// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer_internal.h"

#ifdef ENABLE_GLES_3D

namespace helix {
namespace gcode {

namespace {

/// Rewrite a geometry's palette entries for the tools it drew. The main
/// geometry and the moving mesh each own a palette and a tool->palette map,
/// so the same AMS colors land at different indices in each.
bool apply_tool_palette(RibbonGeometry& geom, const std::vector<uint32_t>& ams_colors) {
    bool changed = false;
    for (size_t tool = 0; tool < ams_colors.size(); ++tool) {
        auto it = geom.tool_palette_map.find(static_cast<uint8_t>(tool));
        if (it == geom.tool_palette_map.end()) {
            continue;
        }
        const uint8_t palette_idx = it->second;
        if (palette_idx < geom.color_palette.size() &&
            geom.color_palette[palette_idx] != ams_colors[tool]) {
            geom.color_palette[palette_idx] = ams_colors[tool];
            changed = true;
        }
    }
    return changed;
}
} // namespace

// ============================================================
// CachedRenderState
// ============================================================

bool GCodeGLESRenderer::CachedRenderState::same_scene(const CachedRenderState& o) const {
    // Epsilon comparisons: tighter for angles, looser for zoom/distance
    auto near_angle = [](float a, float b) { return std::abs(a - b) < kAngleEpsilon; };
    auto near_zoom = [](float a, float b) { return std::abs(a - b) < kZoomEpsilon; };
    return near_angle(azimuth, o.azimuth) && near_angle(elevation, o.elevation) &&
           near_zoom(distance, o.distance) && near_zoom(zoom_level, o.zoom_level) &&
           near_angle(target.x, o.target.x) && near_angle(target.y, o.target.y) &&
           near_angle(target.z, o.target.z) && layer_start == o.layer_start &&
           layer_end == o.layer_end && highlight_count == o.highlight_count &&
           highlight_set_hash == o.highlight_set_hash && exclude_count == o.exclude_count &&
           filament_color == o.filament_color && ghost_opacity == o.ghost_opacity &&
           content_offset_y == o.content_offset_y && viewport_width == o.viewport_width &&
           viewport_height == o.viewport_height;
}

bool GCodeGLESRenderer::CachedRenderState::operator==(const CachedRenderState& o) const {
    return same_scene(o) && progress_layer == o.progress_layer;
}

GCodeGLESRenderer::CachedRenderState
GCodeGLESRenderer::snapshot_state(const GCodeCamera& camera) const {
    CachedRenderState s;
    s.azimuth = camera.get_azimuth();
    s.elevation = camera.get_elevation();
    s.distance = camera.get_distance();
    s.zoom_level = camera.get_zoom_level();
    s.target = camera.get_target();
    s.progress_layer = progress_layer_;
    s.layer_start = layer_start_;
    s.layer_end = layer_end_;
    s.highlight_count = selection_.highlighted().size();
    s.highlight_set_hash = selection_.highlighted_hash();
    s.exclude_count = selection_.excluded().size();
    s.filament_color = filament_color_;
    s.ghost_opacity = ghost_opacity_;
    s.content_offset_y = content_offset_y_percent_;
    s.viewport_width = viewport_width_;
    s.viewport_height = viewport_height_;
    return s;
}

// ============================================================
// Configuration Methods
// ============================================================

void GCodeGLESRenderer::set_viewport_size(int width, int height) {
    // Only this (LVGL) thread writes the viewport, so the unchanged check needs no lock.
    if (width == viewport_width_ && height == viewport_height_)
        return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    viewport_width_ = width;
    viewport_height_ = height;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_interaction_mode(bool interacting) {
    if (interaction_mode_ == interacting)
        return;
    std::lock_guard<std::mutex> lock(state_mutex_);
    interaction_mode_ = interacting;
    // Each gesture plans its resolution afresh, its first frame always draws,
    // and only its own frames decide whether rotating is too slow.
    moving_half_res_held_ = false;
    moving_frame_valid_ = false;
    moving_frames_in_gesture_ = 0;
    slow_moving_streak_ = 0;
}

void GCodeGLESRenderer::record_rate(size_t triangles, float elapsed_ms, bool publish) {
    gpu_rate_tris_per_ms_ =
        render_schedule::update_rate(gpu_rate_tris_per_ms_, triangles, elapsed_ms);
    if (publish && gpu_rate_tris_per_ms_ > 0.0f) {
        render_schedule::last_measured_rate().store(gpu_rate_tris_per_ms_,
                                                    std::memory_order_relaxed);
    }
}

void GCodeGLESRenderer::mark_frame_dirty() {
    frame_dirty_ = true;
    ++scene_epoch_;
}

void GCodeGLESRenderer::set_filament_color(const std::string& hex_color) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // The sscanf("#%02x%02x%02x") this replaced happened to be the only site in
    // the tree that read an 8-digit #RRGGBBAA token correctly - it stopped after
    // six digits. It accepted and silently truncated anything longer, though,
    // and its size()<7 guard documented a rule it did not enforce. Route it
    // through the shared parser so "correct" stops being an accident.
    uint32_t rgb = 0;
    if (!helix::parse_hex_color(hex_color.c_str(), rgb)) {
        return;
    }
    const float r = static_cast<float>((rgb >> 16) & 0xFF);
    const float g = static_cast<float>((rgb >> 8) & 0xFF);
    const float b = static_cast<float>(rgb & 0xFF);
    filament_color_ = glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_extrusion_color(lv_color_t color) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    filament_color_ =
        glm::vec4(color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, 1.0f);
    palette_.has_override = true;
    palette_.override_color = color;
    mark_frame_dirty();
    spdlog::debug("[GCode GLES] set_extrusion_color: R={} G={} B={} → ({:.2f},{:.2f},{:.2f})",
                  color.red, color.green, color.blue, filament_color_.r, filament_color_.g,
                  filament_color_.b);
}

void GCodeGLESRenderer::set_tool_color_overrides(const std::vector<uint32_t>& ams_colors) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!geometry_ || ams_colors.empty()) {
        return;
    }

    // Lock palette during modification to prevent data races with render path
    std::lock_guard<std::mutex> lock(palette_mutex_);

    // The loop below overwrites the baked palette IN PLACE - the vertex data
    // indexes into it, so there is nowhere else to put an override. Snapshot it
    // the first time, because that copy becomes the only surviving record of
    // what the slicer said and clear_tool_color_overrides() restores from it.
    // At most 256 uint32_t (RibbonGeometry caps the palette), so ~1KB: cheaper
    // than re-deriving the file palette and re-running the tool->palette map on
    // the way back out, and it cannot disagree with what was actually baked.
    if (baked_color_palette_.empty()) {
        baked_color_palette_ = geometry_->color_palette;
    }
    if (geometry_->moving_mesh && mesh_baked_palette_.empty()) {
        mesh_baked_palette_ = geometry_->moving_mesh->color_palette;
    }

    // Replace palette entries using tool→palette mapping from geometry build
    bool changed = apply_tool_palette(*geometry_, ams_colors);
    if (geometry_->moving_mesh) {
        changed = apply_tool_palette(*geometry_->moving_mesh, ams_colors) || changed;
    }

    if (changed) {
        // Per-tool overrides replace palette entries baked into vertex data,
        // so clear any single-color override that would bypass vertex colors.
        palette_.has_override = false;
        // Patch only the color bytes in the prepared buffers in place. Avoids
        // discarding the ~100MB pack the background thread just produced and
        // re-expanding it on the foreground thread — only the RGBA8 lanes
        // need to change.
        geometry_->patch_prepared_buffer_colors();
        if (geometry_->moving_mesh) {
            // No-op once the mesh's prepared buffers were freed after upload;
            // the re-upload then expands from the live palette instead.
            geometry_->moving_mesh->patch_prepared_buffer_colors();
        }
        // Force VBO re-upload to push the new colors to the GPU
        // (old VBOs freed inside render() where GL context is active)
        cancel_job();
        geometry_uploaded_ = false;
        upload_next_layer_ = 0;
        upload_total_layers_ = 0;
        mark_frame_dirty();
        spdlog::debug("[GCode GLES] Applied {} tool color overrides, triggering VBO re-upload",
                      ams_colors.size());
    }
}

void GCodeGLESRenderer::clear_tool_color_overrides() {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!geometry_) {
        return;
    }

    std::lock_guard<std::mutex> lock(palette_mutex_);

    // Nothing was ever overridden on this geometry, so the palette it was built
    // with is still the palette it has. Note this is NOT the same test as
    // "ams_colors is empty" on the way in: that one means "no information, leave
    // the slicer palette alone", which is why it must stay a no-op there.
    if (baked_color_palette_.empty()) {
        return;
    }

    const bool changed = geometry_->color_palette != baked_color_palette_;
    geometry_->color_palette = baked_color_palette_;
    baked_color_palette_.clear();

    // The mesh restores from its own snapshot before any early return: a
    // snapshot left behind would be restored onto the next geometry's palette.
    bool mesh_changed = false;
    if (geometry_->moving_mesh && !mesh_baked_palette_.empty()) {
        mesh_changed = geometry_->moving_mesh->color_palette != mesh_baked_palette_;
        geometry_->moving_mesh->color_palette = mesh_baked_palette_;
    }
    mesh_baked_palette_.clear();

    if (!changed && !mesh_changed) {
        return;
    }

    // Same repaint route as applying an override: patch only the RGBA8 lanes in
    // the prepared buffers rather than re-expanding the whole pack, then force
    // the VBOs back up so the GPU sees the restored colors.
    geometry_->patch_prepared_buffer_colors();
    if (geometry_->moving_mesh) {
        geometry_->moving_mesh->patch_prepared_buffer_colors();
    }
    cancel_job();
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    mark_frame_dirty();
    spdlog::debug("[GCode GLES] Retracted tool color overrides, restored {}-entry baked palette",
                  geometry_->color_palette.size());
}

void GCodeGLESRenderer::set_simplification_tolerance(float /*tolerance_mm*/) {
    // Simplification is applied during geometry build, not at render time
}

void GCodeGLESRenderer::set_specular(float intensity, float shininess) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    specular_intensity_ = std::clamp(intensity, kMinSpecularIntensity, kMaxSpecularIntensity);
    specular_shininess_ = std::clamp(shininess, kMinSpecularShininess, kMaxSpecularShininess);
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_debug_face_colors(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    debug_face_colors_ = enable;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_show_travels(bool show) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    show_travels_ = show;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_show_extrusions(bool show) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    show_extrusions_ = show;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_layer_range(int start, int end) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    layer_start_ = start;
    layer_end_ = end;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_highlighted_object(const std::string& name) {
    std::unordered_set<std::string> objects;
    if (!name.empty())
        objects.insert(name);
    set_highlighted_objects(objects);
}

void GCodeGLESRenderer::set_highlighted_objects(const std::unordered_set<std::string>& names) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // SelectionState memoizes the set hash, so the per-frame cached-state build
    // does not iterate every name on each LVGL invalidation tick.
    if (selection_.set_highlighted(names) != InvalidationScope::Nothing) {
        mark_frame_dirty();
    }
}

void GCodeGLESRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (selection_.set_excluded(names) != InvalidationScope::Nothing) {
        mark_frame_dirty();
    }
}

void GCodeGLESRenderer::set_global_opacity(lv_opa_t opacity) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    global_opacity_ = opacity;
    mark_frame_dirty();
}

void GCodeGLESRenderer::reset_colors() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Main thread; the GL passes below consume these as uniforms.
    sel_palette_ = selection::palette_from_theme();
    palette_.has_override = false;
    filament_color_ = kDefaultFilamentColor;
    mark_frame_dirty();
}

void GCodeGLESRenderer::clear_cached_frame() {
    // The draw buffer may still be in flight to an LVGL draw task (#929);
    // safe_draw_buf_destroy waits for those before freeing it.
    if (draw_buf_) {
        helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");
        draw_buf_width_ = 0;
        draw_buf_height_ = 0;
        draw_buf_bytes_ = 0;
    }
    has_shown_image_ = false;

    std::lock_guard<std::mutex> lock(state_mutex_);
    // Nothing is cached any more, so the next draw starts a frame afresh, and
    // a frame the worker finished before now is not shown.
    have_complete_image_ = false;
    moving_frame_valid_ = false;
    ++scene_gen_;
}

RenderingOptions GCodeGLESRenderer::get_options() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    RenderingOptions opts;
    opts.show_extrusions = show_extrusions_;
    opts.show_travels = show_travels_;
    opts.layer_start = layer_start_;
    opts.layer_end = layer_end_;
    opts.highlighted_object = highlighted_object_;
    return opts;
}

// ============================================================
// Ghost / Print Progress
// ============================================================

void GCodeGLESRenderer::set_print_progress_layer(int current_layer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // No frame_dirty_: print progress goes through render_schedule::decide_job,
    // which decides between extending, an incremental pass, or a restart.
    progress_layer_ = current_layer;
}

void GCodeGLESRenderer::set_ghost_opacity(lv_opa_t opacity) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ghost_opacity_ = opacity;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_content_offset_y(float offset_percent) {
    const float clamped = std::clamp(offset_percent, -1.0f, 1.0f);
    // The viewer calls this on every draw. Only this (LVGL) thread writes the
    // offset, so the unchanged check needs no lock and never waits on a step.
    if (std::abs(clamped - content_offset_y_percent_) < 1e-4f) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    content_offset_y_percent_ = clamped;
    mark_frame_dirty();
}

void GCodeGLESRenderer::set_ghost_render_mode(GhostRenderMode mode) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ghost_render_mode_ = mode;
    mark_frame_dirty();
}

int GCodeGLESRenderer::get_max_layer_index() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (geometry_)
        return static_cast<int>(geometry_->max_layer_index);
    return 0;
}

// ============================================================
// Geometry Release
// ============================================================

void GCodeGLESRenderer::release_geometry() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        release_geometry_locked();
    }
    // GPU buffers are freed by the worker, which owns the GL context.
    worker_.wake();
}

void GCodeGLESRenderer::release_geometry_locked() {
    cancel_job();
    size_t freed = geometry_ ? geometry_->memory_usage() : 0;

    // The VBOs need the GL context to free; the worker's next step frees them.
    for (auto& vbos : {&layer_vbos_, &moving_vbos_}) {
        for (auto& lv : *vbos) {
            retired_vbos_.push_back(std::move(lv));
        }
        vbos->clear();
    }

    // Free CPU geometry
    geometry_.reset();
    has_geometry_ = false;
    ++scene_gen_;
    {
        // Nothing left to restore the baked palette onto.
        std::lock_guard<std::mutex> lock(palette_mutex_);
        baked_color_palette_.clear();
        mesh_baked_palette_.clear();
    }
    active_geometry_ = nullptr;
    current_filename_.clear();
    // The image on screen belongs to the file being released; badges for the
    // next one must not project through its transform.
    has_shown_image_ = false;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    mesh_triangles_ = 0;

    // Free readback buffer
    freed += readback_buf_.capacity();
    readback_buf_.clear();
    readback_buf_.shrink_to_fit();

    if (freed > 0) {
        spdlog::info("[GCode GLES] Released geometry: {} MB freed", freed / (1024 * 1024));
    }
}

// ============================================================
// Geometry Loading
// ============================================================

void GCodeGLESRenderer::set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                                              const std::string& filename) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    cancel_job();
    geometry_ = std::move(geometry);
    has_geometry_ = geometry_ != nullptr;
    ++scene_gen_;
    {
        // The snapshot describes the palette of the geometry just replaced.
        // Kept, it would be restored onto a different file's palette.
        std::lock_guard<std::mutex> lock(palette_mutex_);
        baked_color_palette_.clear();
        mesh_baked_palette_.clear();
    }
    current_filename_ = filename;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    mesh_triangles_ = 0;
    mark_frame_dirty();
    spdlog::debug("[GCode GLES] Geometry set: {} strips, {} vertices",
                  geometry_ ? geometry_->strips.size() : 0,
                  geometry_ ? geometry_->vertices.size() : 0);
}

void GCodeGLESRenderer::set_prebuilt_coarse_geometry(std::unique_ptr<RibbonGeometry> /*geometry*/) {
    // Coarse LOD no longer used — GPU handles full geometry at full speed
}

// ============================================================
// Statistics
// ============================================================

size_t GCodeGLESRenderer::get_geometry_color_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (geometry_)
        return geometry_->color_palette.size();
    return 0;
}

std::vector<uint32_t> GCodeGLESRenderer::get_geometry_color_palette() const {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    std::lock_guard<std::mutex> lock(palette_mutex_);
    return geometry_ ? geometry_->color_palette : std::vector<uint32_t>{};
}

size_t GCodeGLESRenderer::get_segments_rendered() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return triangles_rendered_ / 2;
}

helix::gcode::RenderMemoryReport GCodeGLESRenderer::memory_report() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return memory_report_locked();
}

helix::gcode::RenderMemoryReport GCodeGLESRenderer::memory_report_locked() const {
    helix::gcode::RenderMemoryReport r;

    size_t geometry = 0;
    if (geometry_) {
        geometry += geometry_->vertices.size() * sizeof(RibbonVertex);
        geometry += geometry_->strips.size() * sizeof(TriangleStrip);
        geometry += geometry_->strip_color_index.size() * sizeof(uint8_t);
    }
    r.add("geometry", geometry);

    // ARGB8888: the alpha byte is coverage, thresholded to 0/255 during the
    // repack once the rim pass has consumed the selection tag.
    r.add("draw_buf", draw_buf_bytes_.load());

    // Was missing from the old accounting entirely. It is a full RGBA copy of
    // the framebuffer and it is resident for the life of the renderer.
    r.add("readback", readback_buf_.size());

    size_t vbo = 0;
    for (const auto& lv : layer_vbos_) {
        if (lv.vbo) {
            vbo += lv.vertex_count * PackedVertex::stride();
        }
    }
    r.add("vbo_gpu", vbo);

    // Colour RBO (RGBA8, 4 bytes) + depth RBO (16-bit, 2 bytes). Lives in VRAM,
    // reported alongside host heap because on the SoCs we ship to it is the same
    // physical memory.
    r.add("fbo_gpu",
          fbo_.id ? static_cast<size_t>(fbo_width_) * static_cast<size_t>(fbo_height_) * 6 : 0);
    return r;
}

void GCodeGLESRenderer::log_memory_report(const char* when) const {
    if (!spdlog::should_log(spdlog::level::debug)) {
        return;
    }
    spdlog::debug("[GCode GLES] memory after {}: {}", when, memory_report_locked().format());
}

size_t GCodeGLESRenderer::get_triangle_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (geometry_)
        return geometry_->extrusion_triangle_count;
    return 0;
}

glm::mat4 GCodeGLESRenderer::build_mvp(const GCodeCamera& camera) const {
    // Model: -90° CW around Z to match slicer thumbnail orientation.
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 proj = camera.get_projection_matrix();
    // NDC Y range is [-1, 1]; multiply by 2 so the percent maps to a full shift.
    if (std::abs(content_offset_y_percent_) > 0.001f) {
        proj[3][1] += -content_offset_y_percent_ * 2.0f;
    }
    return proj * camera.get_view_matrix() * model;
}

void GCodeGLESRenderer::latch_shown_image(const glm::mat4& mvp, int width, int height) {
    shown_mvp_ = mvp;
    shown_width_ = width;
    shown_height_ = height;
    has_shown_image_ = true;
}

std::optional<glm::vec2> GCodeGLESRenderer::project_to_shown_image(const glm::vec3& world) const {
    // LVGL-thread state only (install_frame latches it), so no lock.
    if (!has_shown_image_) {
        return std::nullopt;
    }
    return project_clip_to_screen(shown_mvp_, world, shown_width_, shown_height_);
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
