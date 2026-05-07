// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_layer_renderer.h"

#include "config.h"
#include "gcode_parser.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "system/crash_handler.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace helix {
namespace gcode {

// ============================================================================
// Named Color Constants
// ============================================================================

namespace {

/// Orange-red color for excluded objects (strikethrough style)
constexpr uint32_t kExcludedObjectColor = 0xFF6B35;
constexpr uint8_t kExcludedR = (kExcludedObjectColor >> 16) & 0xFF;
constexpr uint8_t kExcludedG = (kExcludedObjectColor >> 8) & 0xFF;
constexpr uint8_t kExcludedB = kExcludedObjectColor & 0xFF;

/// Selection blue for highlighted objects
constexpr uint32_t kHighlightedObjectColor = 0x42A5F5;
constexpr uint8_t kHighlightedR = (kHighlightedObjectColor >> 16) & 0xFF;
constexpr uint8_t kHighlightedG = (kHighlightedObjectColor >> 8) & 0xFF;
constexpr uint8_t kHighlightedB = kHighlightedObjectColor & 0xFF;

/// Light grey for selection bracket wireframes
constexpr uint32_t kBracketColor = 0xC0C0C0;

/// Object pick distance threshold (pixels)
constexpr float kPickThresholdPx = 15.0f;

/// Alpha value for excluded objects (60%)
constexpr uint8_t kExcludedAlpha = 153;

/// Ghost darkening factor (40% brightness)
constexpr int kGhostDarkenPercent = 40;

/// Default extrusion width when metadata is unavailable (mm)
constexpr float kDefaultExtrusionWidthMm = 0.4f;

/// Extrusion pixel width clamp range
constexpr int kMinExtrusionPixelWidth = 1;
constexpr int kMaxExtrusionPixelWidth = 8;

/// Minimum line length for thick line perpendicular computation
constexpr float kMinLineLength = 0.001f;

} // namespace

// ============================================================================
// Construction
// ============================================================================

GCodeLayerRenderer::GCodeLayerRenderer() {
    // Initialize default colors from theme
    reset_colors();

    // Load configuration values
    load_config();
}

GCodeLayerRenderer::~GCodeLayerRenderer() {
    // Cancel background thread first (must complete before destroying buffers)
    cancel_background_ghost_render();

    destroy_cache();
    destroy_ssao_cache();
    destroy_ghost_cache();
}

// ============================================================================
// Data Source
// ============================================================================

void GCodeLayerRenderer::set_gcode(const ParsedGCodeFile* gcode) {
    // Cancel ghost thread before changing pointers it reads (race fix for #387)
    cancel_background_ghost_render();
    gcode_ = gcode;
    streaming_controller_ = nullptr; // Clear streaming mode
    bounds_valid_ = false;
    current_layer_ = 0;
    warmup_frames_remaining_ = WARMUP_FRAMES; // Allow panel to render before heavy caching
    invalidate_cache();

    if (gcode_) {
        spdlog::debug("[GCodeLayerRenderer] Set G-code: {} layers, {} total segments",
                      gcode_->layers.size(), gcode_->total_segments);
    }
}

void GCodeLayerRenderer::set_streaming_controller(GCodeStreamingController* controller) {
    // Cancel ghost thread before changing pointers it reads (race fix for #387)
    cancel_background_ghost_render();
    streaming_controller_ = controller;
    gcode_ = nullptr; // Clear full-file mode
    bounds_valid_ = false;
    current_layer_ = 0;
    warmup_frames_remaining_ = WARMUP_FRAMES; // Allow panel to render before heavy caching
    invalidate_cache();

    if (streaming_controller_) {
        spdlog::info(
            "[GCodeLayerRenderer] Set streaming controller: {} layers, cache budget {:.1f}MB",
            streaming_controller_->get_layer_count(),
            static_cast<double>(streaming_controller_->get_cache_budget()) / (1024 * 1024));
    }
}

// ============================================================================
// Layer Selection
// ============================================================================

void GCodeLayerRenderer::set_current_layer(int layer) {
    int max_layer = get_layer_count() - 1;
    if (max_layer < 0) {
        current_layer_ = 0;
        return;
    }

    current_layer_ = std::clamp(layer, 0, max_layer);
}

int GCodeLayerRenderer::get_layer_count() const {
    if (streaming_controller_) {
        return static_cast<int>(streaming_controller_->get_layer_count());
    }
    return gcode_ ? static_cast<int>(gcode_->layers.size()) : 0;
}

// ============================================================================
// Canvas Setup
// ============================================================================

void GCodeLayerRenderer::set_canvas_size(int width, int height) {
    // Ensure minimum dimensions to prevent division by zero in auto_fit()
    canvas_width_ = std::max(1, width);
    canvas_height_ = std::max(1, height);
    bounds_valid_ = false; // Recalculate fit on next render
}

void GCodeLayerRenderer::set_content_offset_y(float offset_percent) {
    // Clamp to reasonable range
    content_offset_y_percent_ = std::clamp(offset_percent, -1.0f, 1.0f);
}

// ============================================================================
// Colors
// ============================================================================

void GCodeLayerRenderer::set_extrusion_color(lv_color_t color) {
    color_extrusion_ = color;
    use_custom_extrusion_color_ = true;
}

void GCodeLayerRenderer::set_travel_color(lv_color_t color) {
    color_travel_ = color;
    use_custom_travel_color_ = true;
}

void GCodeLayerRenderer::set_support_color(lv_color_t color) {
    color_support_ = color;
    use_custom_support_color_ = true;
}

void GCodeLayerRenderer::set_tool_color_palette(const std::vector<std::string>& hex_colors) {
    tool_palette_.set_from_hex_palette(hex_colors);
    if (tool_palette_.has_tool_colors()) {
        spdlog::debug("[GCodeLayerRenderer] Set tool color palette: {} colors", hex_colors.size());
    }
}

void GCodeLayerRenderer::set_tool_color_overrides(const std::vector<uint32_t>& ams_colors) {
    if (ams_colors.empty()) {
        return;
    }

    // Replace tool_palette_ entries with AMS colors
    tool_palette_.tool_colors.resize(ams_colors.size());
    for (size_t i = 0; i < ams_colors.size(); ++i) {
        tool_palette_.tool_colors[i] = lv_color_hex(ams_colors[i]);
    }

    // Clear any single-color override since per-tool overrides supersede it
    tool_palette_.has_override = false;

    // Invalidate caches so new colors take effect
    invalidate_cache();

    spdlog::debug("[GCodeLayerRenderer] Applied {} tool color overrides", ams_colors.size());
}

void GCodeLayerRenderer::reset_colors() {
    // Use theme colors for default appearance
    // Extrusion: info blue for visibility against dark background
    color_extrusion_ = theme_manager_get_color("info");

    // Travel: subtle secondary color (grey)
    color_travel_ = theme_manager_get_color("text_muted");

    // Support: orange/warning color to distinguish from model
    color_support_ = theme_manager_get_color("warning");

    use_custom_extrusion_color_ = false;
    use_custom_travel_color_ = false;
    use_custom_support_color_ = false;
    tool_palette_ = GCodeColorPalette{};
}

void GCodeLayerRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    if (names == excluded_objects_) {
        return; // No change - skip expensive cache invalidation
    }
    excluded_objects_ = names;
    invalidate_cache();
}

void GCodeLayerRenderer::set_highlighted_objects(const std::unordered_set<std::string>& names) {
    if (names == highlighted_objects_) {
        return; // No change - skip expensive cache invalidation
    }
    if (names.empty()) {
        spdlog::debug("[GCodeLayerRenderer] Selection cleared");
    } else {
        for (const auto& name : names) {
            spdlog::debug("[GCodeLayerRenderer] Selection brackets active for '{}'", name);
        }
    }
    highlighted_objects_ = names;
    invalidate_cache();
}

// ============================================================================
// Viewport Control
// ============================================================================

void GCodeLayerRenderer::auto_fit() {
    int layer_count = get_layer_count();
    if (layer_count == 0) {
        scale_ = 1.0f;
        offset_x_ = 0.0f;
        offset_y_ = 0.0f;
        return;
    }

    // Get bounding box from either full file or streaming index stats
    AABB bb;
    if (streaming_controller_) {
        // In streaming mode, compute actual X/Y bounds from layer data
        // The index stats only have Z bounds, so we sample layers for X/Y
        const auto& stats = streaming_controller_->get_index_stats();

        // Initialize with Z bounds from index
        bb.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  stats.min_z};
        bb.max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  stats.max_z};

        // Sample a few layers to compute X/Y bounds (first, middle, last)
        size_t layer_count = streaming_controller_->get_layer_count();
        std::vector<size_t> sample_layers;
        if (layer_count > 0) {
            sample_layers.push_back(0); // First layer
            if (layer_count > 2) {
                sample_layers.push_back(layer_count / 2); // Middle layer
            }
            if (layer_count > 1) {
                sample_layers.push_back(layer_count - 1); // Last layer
            }
        }

        bool found_bounds = false;
        for (size_t layer_idx : sample_layers) {
            auto segments = streaming_controller_->get_layer_segments(layer_idx);
            if (segments && !segments->empty()) {
                for (const auto& seg : *segments) {
                    if (!seg.is_extrusion)
                        continue;
                    bb.min.x = std::min(bb.min.x, std::min(seg.start.x, seg.end.x));
                    bb.max.x = std::max(bb.max.x, std::max(seg.start.x, seg.end.x));
                    bb.min.y = std::min(bb.min.y, std::min(seg.start.y, seg.end.y));
                    bb.max.y = std::max(bb.max.y, std::max(seg.start.y, seg.end.y));
                    found_bounds = true;
                }
            }
        }

        // Fallback to 200x200 if no layer data available yet
        if (!found_bounds) {
            bb.min.x = 0.0f;
            bb.min.y = 0.0f;
            bb.max.x = 200.0f;
            bb.max.y = 200.0f;
            spdlog::debug(
                "[GCodeLayerRenderer] Streaming: no layers loaded yet, using default 200x200");
        } else {
            spdlog::info("[GCodeLayerRenderer] Streaming: computed bounds X[{:.1f},{:.1f}] "
                         "Y[{:.1f},{:.1f}] from {} layers",
                         bb.min.x, bb.max.x, bb.min.y, bb.max.y, sample_layers.size());
        }
    } else if (gcode_) {
        bb = gcode_->global_bounding_box;
    } else {
        return;
    }

    // Use shared auto-fit computation
    ViewMode current_view = get_view_mode();
    auto fit = helix::gcode::compute_auto_fit(bb, current_view, canvas_width_, canvas_height_);
    scale_ = fit.scale;
    offset_x_ = fit.offset_x;
    offset_y_ = fit.offset_y;
    offset_z_ = fit.offset_z;

    // Store bounds for reference (including Z for depth shading)
    bounds_min_x_ = bb.min.x;
    bounds_max_x_ = bb.max.x;
    bounds_min_y_ = bb.min.y;
    bounds_max_y_ = bb.max.y;
    bounds_min_z_ = bb.min.z;
    bounds_max_z_ = bb.max.z;

    bounds_valid_ = true;

    spdlog::debug("[GCodeLayerRenderer] auto_fit: canvas={}x{}, mode={}, "
                  "scale={:.2f}, center=({:.1f},{:.1f},{:.1f})",
                  canvas_width_, canvas_height_, static_cast<int>(current_view), scale_, offset_x_,
                  offset_y_, offset_z_);
}

void GCodeLayerRenderer::fit_layer() {
    if (!gcode_ || gcode_->layers.empty()) {
        scale_ = 1.0f;
        offset_x_ = 0.0f;
        offset_y_ = 0.0f;
        return;
    }

    if (current_layer_ < 0 || current_layer_ >= static_cast<int>(gcode_->layers.size())) {
        return;
    }

    // Use current layer's bounding box with shared auto-fit (always top-down for single layer)
    const auto& bb = gcode_->layers[current_layer_].bounding_box;

    bounds_min_x_ = bb.min.x;
    bounds_max_x_ = bb.max.x;
    bounds_min_y_ = bb.min.y;
    bounds_max_y_ = bb.max.y;

    auto fit =
        helix::gcode::compute_auto_fit(bb, ViewMode::TOP_DOWN, canvas_width_, canvas_height_);
    scale_ = fit.scale;
    offset_x_ = fit.offset_x;
    offset_y_ = fit.offset_y;

    bounds_valid_ = true;
}

void GCodeLayerRenderer::set_scale(float scale) {
    scale_ = std::max(0.001f, scale);
}

void GCodeLayerRenderer::set_offset(float x, float y) {
    offset_x_ = x;
    offset_y_ = y;
}

// ============================================================================
// Layer Information
// ============================================================================

GCodeLayerRenderer::LayerInfo GCodeLayerRenderer::get_layer_info() const {
    LayerInfo info{};
    info.layer_number = current_layer_;

    int layer_count = get_layer_count();
    if (layer_count == 0) {
        return info;
    }

    if (current_layer_ < 0 || current_layer_ >= layer_count) {
        return info;
    }

    if (streaming_controller_) {
        // Streaming mode: get Z height from controller, segments on demand
        info.z_height = streaming_controller_->get_layer_z(static_cast<size_t>(current_layer_));

        // Get segments to compute counts (this will cache the layer)
        // Use shared_ptr to keep data alive during iteration
        auto segments =
            streaming_controller_->get_layer_segments(static_cast<size_t>(current_layer_));
        if (segments) {
            info.segment_count = segments->size();
            info.extrusion_count = 0;
            info.travel_count = 0;
            info.has_supports = false;

            for (const auto& seg : *segments) {
                if (seg.is_extrusion) {
                    ++info.extrusion_count;
                    if (!info.has_supports && is_support_segment(seg)) {
                        info.has_supports = true;
                    }
                } else {
                    ++info.travel_count;
                }
            }
        }
    } else if (gcode_) {
        // Full file mode
        const Layer& layer = gcode_->layers[current_layer_];
        info.z_height = layer.z_height;
        info.segment_count = layer.segments.size();
        info.extrusion_count = layer.segment_count_extrusion;
        info.travel_count = layer.segment_count_travel;

        // Check for support segments in this layer
        info.has_supports = false;
        for (const auto& seg : layer.segments) {
            if (is_support_segment(seg)) {
                info.has_supports = true;
                break;
            }
        }
    }

    return info;
}

bool GCodeLayerRenderer::has_support_detection() const {
    // Support detection relies on object names from EXCLUDE_OBJECT
    // If there are named objects, we can potentially detect supports
    // Note: Streaming mode doesn't have full object metadata, so return false
    if (streaming_controller_) {
        return false; // Object metadata not available in streaming mode
    }
    return gcode_ && !gcode_->objects.empty();
}

// ============================================================================
// Rendering
// ============================================================================

void GCodeLayerRenderer::destroy_cache() {
    if (cache_buf_) {
        if (lv_is_initialized()) {
            // Mechanism B (#929): cache_buf_ is referenced by parallel-render-thread
            // draw tasks via dsc.src in blit_cache(); freeing it while a task is
            // in flight UAFs in argb8888_image_blend (cluster:pstat-async-delete,
            // v0.99.54 production telemetry pinned the source pointer to a freed
            // mmap'd cache_buf_).
            //
            // lv_draw_wait_for_finish() blocks until every draw unit's pending
            // tasks complete (no-op when LV_USE_OS == 0, so single-threaded
            // builds are unaffected). After it returns, no in-flight task
            // references cache_buf_ and lv_draw_buf_destroy is safe.
            crash_handler::breadcrumb::note("cache_buf", "destroy_pre");
            lv_draw_wait_for_finish();
            lv_draw_buf_destroy(cache_buf_);
            crash_handler::breadcrumb::note("cache_buf", "destroy_post");
        }
        cache_buf_ = nullptr;
    }
    cached_up_to_layer_ = -1;
    cached_width_ = 0;
    cached_height_ = 0;
}

void GCodeLayerRenderer::invalidate_cache() {
    // Clear the cache buffer content but keep the buffer allocated
    if (cache_buf_) {
        lv_draw_buf_clear(cache_buf_, nullptr);
    }
    cached_up_to_layer_ = -1;
    ssao_cache_valid_ = false;

    // Cancel any in-progress background ghost rendering
    cancel_background_ghost_render();

    // Also invalidate ghost cache (new gcode = need new ghost)
    if (ghost_buf_) {
        lv_draw_buf_clear(ghost_buf_, nullptr);
    }
    ghost_cache_valid_ = false;
    ghost_rendered_up_to_ = -1;
}

// ============================================================================
// SSAO Post-Processing
// ============================================================================

void GCodeLayerRenderer::ensure_ssao_cache(int width, int height) {
    if (ssao_buf_ && ssao_cached_width_ == width && ssao_cached_height_ == height)
        return;

    destroy_ssao_cache();
    ssao_buf_ = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if (!ssao_buf_) {
        spdlog::error("[GCodeLayerRenderer] Failed to create SSAO buffer {}x{}", width, height);
        return;
    }
    lv_draw_buf_clear(ssao_buf_, nullptr);
    ssao_cached_width_ = width;
    ssao_cached_height_ = height;
}

void GCodeLayerRenderer::destroy_ssao_cache() {
    if (ssao_buf_) {
        if (lv_is_initialized()) {
            // ssao_buf_ feeds dsc.src in apply_ssao()'s blit (line ~595) — same
            // parallel-render UAF pattern as cache_buf_ (#929). Wait for in-flight
            // draw tasks before freeing.
            lv_draw_wait_for_finish();
            lv_draw_buf_destroy(ssao_buf_);
        }
        ssao_buf_ = nullptr;
    }
    ssao_cached_width_ = 0;
    ssao_cached_height_ = 0;
    ssao_cache_valid_ = false;
}

void GCodeLayerRenderer::apply_ssao() {
    if (!cache_buf_)
        return;

    const int w = cached_width_;
    const int h = cached_height_;

    ensure_ssao_cache(w, h);
    if (!ssao_buf_)
        return;

    auto* src_data = static_cast<uint8_t*>(cache_buf_->data);
    auto* dst_data = static_cast<uint8_t*>(ssao_buf_->data);
    uint32_t stride = cache_buf_->header.stride;
    uint32_t stride_px = stride / 4;

    auto* src = reinterpret_cast<const uint32_t*>(src_data);
    auto* dst = reinterpret_cast<uint32_t*>(dst_data);

    // Copy source to destination first
    std::memcpy(dst_data, src_data, static_cast<size_t>(h) * stride);

    uint32_t start_ms = lv_tick_get();

    // =========================================================================
    // Silhouette outline: 1px dark border on alpha boundary
    // For each empty pixel adjacent to a filled pixel, draw a dark outline.
    // Makes the model pop from the background.
    // =========================================================================
    constexpr float kOutlineDarken = 0.3f; // Outline brightness (0=black, 1=original)

    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            uint32_t pixel = src[y * stride_px + x];
            uint8_t alpha = (pixel >> 24) & 0xFF;

            if (alpha > 0) {
                // Filled pixel: check if it's on the silhouette edge
                // (has at least one empty neighbor in 4-connected)
                bool on_edge = ((src[(y - 1) * stride_px + x] >> 24) == 0) ||
                               ((src[(y + 1) * stride_px + x] >> 24) == 0) ||
                               ((src[y * stride_px + (x - 1)] >> 24) == 0) ||
                               ((src[y * stride_px + (x + 1)] >> 24) == 0);

                if (on_edge) {
                    uint8_t r = static_cast<uint8_t>(((pixel >> 16) & 0xFF) * kOutlineDarken);
                    uint8_t g = static_cast<uint8_t>(((pixel >> 8) & 0xFF) * kOutlineDarken);
                    uint8_t b = static_cast<uint8_t>((pixel & 0xFF) * kOutlineDarken);
                    dst[y * stride_px + x] =
                        (static_cast<uint32_t>(alpha) << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
    }

    uint32_t elapsed = lv_tick_elaps(start_ms);
    spdlog::debug("[GCodeLayerRenderer] SSAO (outline) applied in {}ms ({}x{})", elapsed, w, h);

    ssao_cache_valid_ = true;
}

void GCodeLayerRenderer::blit_ssao_cache(lv_layer_t* target) {
    if (!ssao_buf_)
        return;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = ssao_buf_;

    lv_area_t coords = {widget_offset_x_, widget_offset_y_,
                        widget_offset_x_ + ssao_cached_width_ - 1,
                        widget_offset_y_ + ssao_cached_height_ - 1};

    lv_draw_image(target, &dsc, &coords);
}

void GCodeLayerRenderer::ensure_cache(int width, int height) {
    // Recreate cache if dimensions changed
    if (cache_buf_ && (cached_width_ != width || cached_height_ != height)) {
        destroy_cache();
    }

    if (!cache_buf_) {
        // Create the draw buffer (no canvas widget - avoids clip area contamination
        // from overlays/toasts on lv_layer_top())
        // Must stay ARGB8888 — blend_pixel() writes 4-byte BGRA pixels directly
        cache_buf_ = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if (!cache_buf_) {
            spdlog::error("[GCodeLayerRenderer] Failed to create cache buffer {}x{}", width,
                          height);
            return;
        }

        // Clear to transparent
        lv_draw_buf_clear(cache_buf_, nullptr);

        cached_width_ = width;
        cached_height_ = height;
        cached_up_to_layer_ = -1;

        spdlog::debug("[GCodeLayerRenderer] Created cache buffer: {}x{}", width, height);
        helix::MemoryMonitor::log_now("gcode_cache_buffer_created");
    }
}

void GCodeLayerRenderer::render_layers_to_cache(int from_layer, int to_layer) {
    if (!cache_buf_)
        return;

    // Need either gcode file or streaming controller
    if (!gcode_ && !streaming_controller_)
        return;

    // Capture transform params for coordinate conversion
    // This ensures consistent rendering with widget offset set to 0 for cache
    TransformParams transform = capture_transform_params();
    transform.canvas_width = cached_width_;
    transform.canvas_height = cached_height_;

    int layer_count = get_layer_count();
    size_t segments_rendered = 0;

    // Compute extrusion line width in pixels (scale-dependent)
    int line_width = get_extrusion_pixel_width();

    for (int layer_idx = from_layer; layer_idx <= to_layer; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= layer_count)
            continue;

        // Get segments from appropriate source
        // For streaming mode, hold shared_ptr to keep data alive during iteration
        std::shared_ptr<const std::vector<ToolpathSegment>> segments_holder;
        const std::vector<ToolpathSegment>* segments = nullptr;

        if (streaming_controller_) {
            // Streaming mode: get segments from controller (returns shared_ptr)
            segments_holder =
                streaming_controller_->get_layer_segments(static_cast<size_t>(layer_idx));
            segments = segments_holder.get();
        } else if (gcode_) {
            // Full file mode: get segments from parsed file
            segments = &gcode_->layers[layer_idx].segments;
        }

        if (!segments)
            continue;

        for (const auto& seg : *segments) {
            if (!should_render_segment(seg))
                continue;

            // Skip non-extrusion moves for solid rendering (travels are subtle)
            if (!seg.is_extrusion)
                continue;

            // Convert world coordinates to screen using cached transform
            glm::ivec2 p1 = world_to_screen_raw(transform, seg.start.x, seg.start.y, seg.start.z);
            glm::ivec2 p2 = world_to_screen_raw(transform, seg.end.x, seg.end.y, seg.end.z);

            // Skip zero-length segments
            if (p1.x == p2.x && p1.y == p2.y)
                continue;

            // Per-segment tool color (or fallback to single extrusion color)
            lv_color_t seg_color = color_extrusion_;
            if (tool_palette_.has_tool_colors()) {
                seg_color = tool_palette_.resolve(seg.tool_index, color_extrusion_);
            }
            uint8_t r = seg_color.red, g = seg_color.green, b = seg_color.blue;

            // Calculate color with depth shading for 3D-like appearance
            if (depth_shading_.load(std::memory_order_relaxed) &&
                get_view_mode() == ViewMode::FRONT) {
                float avg_z = (seg.start.z + seg.end.z) * 0.5f;
                float avg_y = (seg.start.y + seg.end.y) * 0.5f;
                float brightness = compute_depth_brightness(avg_z, bounds_min_z_, bounds_max_z_,
                                                            avg_y, bounds_min_y_, bounds_max_y_);

                // Normal-based shading: segment direction gives us a surface normal
                // in screen space. Light from upper-left (-0.707, -0.707).
                // Perpendicular to segment = surface normal of the "tube".
                if (ssao_enabled_.load(std::memory_order_relaxed)) {
                    float dx = static_cast<float>(p2.x - p1.x);
                    float dy = static_cast<float>(p2.y - p1.y);
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.5f) {
                        // Normal perpendicular to segment (rotated 90° CCW)
                        float nx = -dy / len;
                        float ny = dx / len;
                        // Dot product with light direction (-0.707, -0.707)
                        constexpr float kLightX = -0.707f;
                        constexpr float kLightY = -0.707f;
                        float ndotl = nx * kLightX + ny * kLightY;
                        // Map [-1, 1] → brightness modifier
                        constexpr float kNormalStrength = 0.12f;
                        float normal_mod = 1.0f + ndotl * kNormalStrength;
                        brightness *= normal_mod;
                        if (brightness > 1.0f)
                            brightness = 1.0f;
                        if (brightness < 0.15f)
                            brightness = 0.15f;
                    }
                }

                r = static_cast<uint8_t>(r * brightness);
                g = static_cast<uint8_t>(g * brightness);
                b = static_cast<uint8_t>(b * brightness);
            }

            // Override color for excluded/highlighted objects
            if (seg.object_name_index >= 0) {
                const std::string& obj_name = resolve_object_name(seg.object_name_index);
                if (!obj_name.empty()) {
                    if (excluded_objects_.count(obj_name) > 0) {
                        // Excluded: orange-red with reduced alpha
                        r = kExcludedR;
                        g = kExcludedG;
                        b = kExcludedB;
                        uint32_t color = (static_cast<uint32_t>(kExcludedAlpha) << 24) | (r << 16) |
                                         (g << 8) | b;
                        draw_thick_line_bresenham_solid(p1.x, p1.y, p2.x, p2.y, color, line_width);
                        ++segments_rendered;
                        continue;
                    }
                    if (highlighted_objects_.count(obj_name) > 0) {
                        // Highlighted: selection blue, full alpha
                        r = kHighlightedR;
                        g = kHighlightedG;
                        b = kHighlightedB;
                    }
                }
            }

            // Build ARGB8888 color (full alpha for solid layers)
            uint32_t color = (255u << 24) | (r << 16) | (g << 8) | b;

            // Draw using software line drawing - bypasses LVGL draw API for AD5M compatibility
            if (ssao_enabled_.load(std::memory_order_relaxed)) {
                draw_thick_line_aa_solid(p1.x, p1.y, p2.x, p2.y, color, line_width);
            } else {
                draw_thick_line_bresenham_solid(p1.x, p1.y, p2.x, p2.y, color, line_width);
            }
            ++segments_rendered;
        }
    }

    spdlog::trace("[GCodeLayerRenderer] Rendered layers {}-{}: {} segments to cache (direct), "
                  "buf={}x{} stride={}",
                  from_layer, to_layer, segments_rendered, cached_width_, cached_height_,
                  cache_buf_ ? cache_buf_->header.stride : 0);
}

void GCodeLayerRenderer::blit_cache(lv_layer_t* target) {
    if (!cache_buf_)
        return;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = cache_buf_;

    lv_area_t coords = {widget_offset_x_, widget_offset_y_, widget_offset_x_ + cached_width_ - 1,
                        widget_offset_y_ + cached_height_ - 1};

    lv_draw_image(target, &dsc, &coords);
}

// ============================================================================
// Ghost Cache (faded preview of all layers)
// ============================================================================

void GCodeLayerRenderer::destroy_ghost_cache() {
    if (ghost_buf_) {
        if (lv_is_initialized()) {
            // ghost_buf_ feeds dsc.src in render_ghost_layers's blit (line ~894) —
            // same parallel-render UAF pattern as cache_buf_ (#929). Wait for
            // in-flight draw tasks before freeing.
            lv_draw_wait_for_finish();
            lv_draw_buf_destroy(ghost_buf_);
        }
        ghost_buf_ = nullptr;
    }
    ghost_cached_width_ = 0;
    ghost_cached_height_ = 0;
    ghost_cache_valid_ = false;
    ghost_rendered_up_to_ = -1;
}

void GCodeLayerRenderer::ensure_ghost_cache(int width, int height) {
    // Recreate if dimensions changed (tracked independently from solid cache)
    if (ghost_buf_ && (ghost_cached_width_ != width || ghost_cached_height_ != height)) {
        destroy_ghost_cache();
    }

    if (!ghost_buf_) {
        ghost_buf_ = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if (!ghost_buf_) {
            spdlog::error("[GCodeLayerRenderer] Failed to create ghost buffer {}x{}", width,
                          height);
            return;
        }

        lv_draw_buf_clear(ghost_buf_, nullptr);
        ghost_cached_width_ = width;
        ghost_cached_height_ = height;
        ghost_cache_valid_ = false;
        spdlog::debug("[GCodeLayerRenderer] Created ghost cache buffer: {}x{}", width, height);
        helix::MemoryMonitor::log_now("gcode_ghost_buffer_created");
    }
}

void GCodeLayerRenderer::render_ghost_layers(int from_layer, int to_layer) {
    if (!ghost_buf_ || !gcode_)
        return;

    // Manually initialize layer for offscreen rendering (no canvas widget)
    // This avoids clip area contamination from overlays/toasts on lv_layer_top()
    lv_layer_t ghost_layer;
    lv_memzero(&ghost_layer, sizeof(ghost_layer));
    ghost_layer.draw_buf = ghost_buf_;
    ghost_layer.color_format = LV_COLOR_FORMAT_ARGB8888;
    ghost_layer.buf_area.x1 = 0;
    ghost_layer.buf_area.y1 = 0;
    ghost_layer.buf_area.x2 = ghost_cached_width_ - 1;
    ghost_layer.buf_area.y2 = ghost_cached_height_ - 1;
    ghost_layer._clip_area = ghost_layer.buf_area; // Full buffer as clip area
    ghost_layer.phy_clip_area = ghost_layer.buf_area;

    int saved_offset_x = widget_offset_x_;
    int saved_offset_y = widget_offset_y_;
    widget_offset_x_ = 0;
    widget_offset_y_ = 0;

    size_t segments_rendered = 0;
    for (int layer_idx = from_layer; layer_idx <= to_layer; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode_->layers.size()))
            continue;

        const Layer& layer_data = gcode_->layers[layer_idx];
        for (const auto& seg : layer_data.segments) {
            if (should_render_segment(seg)) {
                // Render with reduced opacity for ghost effect
                render_segment(&ghost_layer, seg, true); // ghost=true
                ++segments_rendered;
            }
        }
    }

    // Dispatch pending draw tasks (equivalent to lv_canvas_finish_layer)
    lv_draw_dispatch_wait_for_request();
    while (ghost_layer.draw_task_head) {
        lv_draw_dispatch_layer(nullptr, &ghost_layer);
        if (ghost_layer.draw_task_head) {
            lv_draw_dispatch_wait_for_request();
        }
    }

    widget_offset_x_ = saved_offset_x;
    widget_offset_y_ = saved_offset_y;

    spdlog::trace("[GCodeLayerRenderer] Rendered ghost layers {}-{}: {} segments", from_layer,
                  to_layer, segments_rendered);
    helix::MemoryMonitor::log_now("gcode_ghost_render_done");
}

void GCodeLayerRenderer::blit_ghost_cache(lv_layer_t* target) {
    if (!ghost_buf_)
        return;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = ghost_buf_;
    dsc.opa = LV_OPA_40; // 40% opacity for ghost

    lv_area_t coords = {widget_offset_x_, widget_offset_y_,
                        widget_offset_x_ + ghost_cached_width_ - 1,
                        widget_offset_y_ + ghost_cached_height_ - 1};

    lv_draw_image(target, &dsc, &coords);
}

void GCodeLayerRenderer::render(lv_layer_t* layer, const lv_area_t* widget_area) {
    int layer_count = get_layer_count();
    if (layer_count == 0) {
        spdlog::debug("[GCodeLayerRenderer] render(): no gcode data");
        return;
    }

    if (current_layer_ < 0 || current_layer_ >= layer_count) {
        spdlog::debug("[GCodeLayerRenderer] render(): layer out of range ({} / {})", current_layer_,
                      layer_count);
        return;
    }

    uint32_t start_time = lv_tick_get();

    // Store widget screen offset for world_to_screen()
    if (widget_area) {
        widget_offset_x_ = widget_area->x1;
        widget_offset_y_ = widget_area->y1;
    }

    // Auto-fit if bounds not yet computed
    if (!bounds_valid_) {
        auto_fit();
    }

    size_t segments_rendered = 0;

    // Snapshot view mode once per frame for consistent use throughout render()
    ViewMode current_view_mode = get_view_mode();

    // For FRONT view, use incremental cache with progressive rendering
    if (current_view_mode == ViewMode::FRONT) {
        int target_layer = std::min(current_layer_, layer_count - 1);

        // Ensure cache buffers exist and are correct size
        ensure_cache(canvas_width_, canvas_height_);
        if (ghost_mode_enabled_.load(std::memory_order_relaxed)) {
            ensure_ghost_cache(canvas_width_, canvas_height_);
        }

        // =====================================================================
        // GHOST CACHE: Background thread rendering (non-blocking)
        // Uses unified background thread for both streaming and non-streaming modes.
        // The background thread renders all layers to a raw buffer, then we copy
        // to LVGL buffer on main thread when ready.
        // =====================================================================
        bool ghost_enabled = ghost_mode_enabled_.load(std::memory_order_relaxed);
        if (ghost_enabled && ghost_buf_ && !ghost_cache_valid_) {
            if (ghost_thread_ready_.load()) {
                // Background thread finished - copy to LVGL buffer
                copy_raw_to_ghost_buf();
            } else if (!ghost_thread_running_.load()) {
                // Start background thread if not running
                start_background_ghost_render();
            }
            // else: background thread is running, wait for it
        }

        // =====================================================================
        // WARM-UP FRAMES: Skip heavy rendering to let panel layout complete
        // =====================================================================
        if (warmup_frames_remaining_ > 0) {
            warmup_frames_remaining_--;
            // Just blit ghost cache (if available) and return - no heavy caching yet
            if (ghost_enabled && ghost_buf_) {
                blit_ghost_cache(layer);
            }
            // Request another frame to continue after warmup
            last_frame_render_ms_ = 1; // Minimal time so adaptation doesn't spike
            return;
        }

        // =====================================================================
        // SOLID CACHE: Progressive rendering up to current print layer
        // =====================================================================
        if (cache_buf_) {
            // Check if we need to render new layers
            if (target_layer > cached_up_to_layer_) {
                // Progressive rendering: only render up to layers_per_frame_ at a time
                // This prevents UI freezing during initial load or big jumps
                int from_layer = cached_up_to_layer_ + 1;
                int to_layer = std::min(from_layer + layers_per_frame_ - 1, target_layer);

                render_layers_to_cache(from_layer, to_layer);
                cached_up_to_layer_ = to_layer;

                // If we haven't caught up yet, caller should check needs_more_frames()
                // and invalidate the widget to trigger another frame
                if (cached_up_to_layer_ < target_layer) {
                    spdlog::debug(
                        "[GCodeLayerRenderer] Progressive: rendered to layer {}/{}, more needed",
                        cached_up_to_layer_, target_layer);
                }
            } else if (target_layer < cached_up_to_layer_) {
                // Going backwards - need to re-render from scratch (progressively)
                lv_draw_buf_clear(cache_buf_, nullptr);
                cached_up_to_layer_ = -1;

                int to_layer = std::min(layers_per_frame_ - 1, target_layer);
                render_layers_to_cache(0, to_layer);
                cached_up_to_layer_ = to_layer;
                // Caller checks needs_more_frames() for continuation
            }
            // else: same layer, just blit cached image

            // =====================================================================
            // BLIT: Ghost first (underneath), then solid on top
            // =====================================================================
            if (ghost_enabled && ghost_buf_) {
                blit_ghost_cache(layer);
            }

            // Apply SSAO post-processing when enabled and cache is fully built
            bool ssao_on = ssao_enabled_.load(std::memory_order_relaxed);
            bool cache_complete = (cached_up_to_layer_ >= target_layer);
            if (ssao_on && cache_complete) {
                if (!ssao_cache_valid_) {
                    apply_ssao();
                }
                blit_ssao_cache(layer);
            } else {
                blit_cache(layer);
            }
            segments_rendered = last_segment_count_;
        }
    } else {
        // TOP_DOWN or ISOMETRIC: render single layer directly (no caching needed)
        // Get segments from appropriate source
        // For streaming mode, hold shared_ptr to keep data alive during iteration
        std::shared_ptr<const std::vector<ToolpathSegment>> segments_holder;
        const std::vector<ToolpathSegment>* segments = nullptr;

        if (streaming_controller_) {
            // Streaming mode: get segments from controller (returns shared_ptr)
            segments_holder =
                streaming_controller_->get_layer_segments(static_cast<size_t>(current_layer_));
            segments = segments_holder.get();
            // Use default centering for streaming mode
            // (Could be improved by computing bounds from segments if needed)
        } else if (gcode_) {
            // Full file mode: get segments and bounding box from parsed file
            const auto& layer_bb = gcode_->layers[current_layer_].bounding_box;
            offset_x_ = (layer_bb.min.x + layer_bb.max.x) / 2.0f;
            offset_y_ = (layer_bb.min.y + layer_bb.max.y) / 2.0f;
            segments = &gcode_->layers[current_layer_].segments;
        }

        if (segments) {
            for (const auto& seg : *segments) {
                if (!should_render_segment(seg))
                    continue;
                render_segment(layer, seg);
                ++segments_rendered;
            }
        }
    }

    // Draw selection brackets on top of everything
    render_selection_brackets(layer);

    // Track render time for diagnostics
    last_render_time_ms_ = lv_tick_get() - start_time;
    last_frame_render_ms_ = last_render_time_ms_;
    last_segment_count_ = segments_rendered;

    // Adapt layers_per_frame for next frame (if in adaptive mode)
    if (config_layers_per_frame_ == 0 && current_view_mode == ViewMode::FRONT) {
        adapt_layers_per_frame();
    }

    // Log performance if layer changed or slow render
    if (current_layer_ != last_rendered_layer_ || last_render_time_ms_ > 50) {
        spdlog::trace("[GCodeLayerRenderer] Layer {}: {}ms (cached_up_to={}, lpf={})",
                      current_layer_, last_render_time_ms_, cached_up_to_layer_, layers_per_frame_);
        last_rendered_layer_ = current_layer_;
    }
}

bool GCodeLayerRenderer::needs_more_frames() const {
    int layer_count = get_layer_count();
    if (layer_count == 0) {
        return false;
    }

    // Only relevant for FRONT view mode (uses caching)
    if (get_view_mode() != ViewMode::FRONT) {
        return false;
    }

    int target_layer = std::min(current_layer_, layer_count - 1);

    // Solid cache incomplete?
    if (cached_up_to_layer_ < target_layer) {
        return true;
    }

    // Ghost rendering in background?
    // Keep triggering frames while ghost is building so we can show progress
    if (ghost_mode_enabled_.load(std::memory_order_relaxed) && !ghost_cache_valid_) {
        if (ghost_thread_running_.load() || ghost_thread_ready_.load()) {
            return true;
        }
    }

    return false;
}

bool GCodeLayerRenderer::should_render_segment(const ToolpathSegment& seg) const {
    if (seg.is_extrusion) {
        if (is_support_segment(seg)) {
            return show_supports_.load(std::memory_order_relaxed);
        }
        return show_extrusions_.load(std::memory_order_relaxed);
    }
    return show_travels_.load(std::memory_order_relaxed);
}

void GCodeLayerRenderer::render_segment(lv_layer_t* layer, const ToolpathSegment& seg, bool ghost) {
    // Convert world coordinates to screen (uses Z for FRONT view)
    glm::ivec2 p1 = world_to_screen(seg.start.x, seg.start.y, seg.start.z);
    glm::ivec2 p2 = world_to_screen(seg.end.x, seg.end.y, seg.end.z);

    // Skip zero-length segments
    if (p1.x == p2.x && p1.y == p2.y) {
        return;
    }

    // Initialize line drawing descriptor
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);

    lv_color_t base_color;
    if (ghost) {
        // Ghost mode: use darkened version of the model's extrusion color
        // This provides visual continuity between ghost and solid layers
        lv_color_t model_color = color_extrusion_;
        // Darken for ghost effect
        base_color = lv_color_make(model_color.red * kGhostDarkenPercent / 100,
                                   model_color.green * kGhostDarkenPercent / 100,
                                   model_color.blue * kGhostDarkenPercent / 100);
    } else {
        base_color = get_segment_color(seg);
    }

    // Apply depth shading for 3D-like appearance
    if (depth_shading_.load(std::memory_order_relaxed) && get_view_mode() == ViewMode::FRONT) {
        float avg_z = (seg.start.z + seg.end.z) * 0.5f;
        float avg_y = (seg.start.y + seg.end.y) * 0.5f;
        float brightness = compute_depth_brightness(avg_z, bounds_min_z_, bounds_max_z_, avg_y,
                                                    bounds_min_y_, bounds_max_y_);

        uint8_t r = static_cast<uint8_t>(base_color.red * brightness);
        uint8_t g = static_cast<uint8_t>(base_color.green * brightness);
        uint8_t b = static_cast<uint8_t>(base_color.blue * brightness);
        dsc.color = lv_color_make(r, g, b);
    } else {
        dsc.color = base_color;
    }

    // Check excluded/highlighted state for width/opacity
    const std::string& seg_obj_name = resolve_object_name(seg.object_name_index);
    bool is_excluded = !seg_obj_name.empty() && excluded_objects_.count(seg_obj_name) > 0;
    bool is_highlighted = !seg_obj_name.empty() && highlighted_objects_.count(seg_obj_name) > 0;

    if (is_excluded) {
        dsc.width = 1;
        dsc.opa = LV_OPA_60;
    } else if (is_highlighted) {
        dsc.width = 3;
        dsc.opa = LV_OPA_COVER;
    } else if (seg.is_extrusion) {
        dsc.width = 2;
        dsc.opa = LV_OPA_COVER;
    } else {
        dsc.width = 1;
        dsc.opa = LV_OPA_50;
    }

    // LVGL 9: points are stored in the descriptor struct
    dsc.p1.x = static_cast<lv_value_precise_t>(p1.x);
    dsc.p1.y = static_cast<lv_value_precise_t>(p1.y);
    dsc.p2.x = static_cast<lv_value_precise_t>(p2.x);
    dsc.p2.y = static_cast<lv_value_precise_t>(p2.y);

    lv_draw_line(layer, &dsc);
}

// ============================================================================
// Transformation - Single Source of Truth
// ============================================================================

GCodeLayerRenderer::TransformParams GCodeLayerRenderer::capture_transform_params() const {
    return TransformParams{
        .view_mode = get_view_mode(),
        .scale = scale_,
        .offset_x = offset_x_,
        .offset_y = offset_y_,
        .offset_z = offset_z_,
        .canvas_width = canvas_width_,
        .canvas_height = canvas_height_,
        .content_offset_y_percent = content_offset_y_percent_,
    };
}

glm::ivec2 GCodeLayerRenderer::world_to_screen(float x, float y, float z) const {
    // Use the shared transformation logic
    TransformParams params = capture_transform_params();
    glm::ivec2 raw = world_to_screen_raw(params, x, y, z);

    // Add widget's screen offset (drawing is in screen coordinates)
    return {raw.x + widget_offset_x_, raw.y + widget_offset_y_};
}

std::string GCodeLayerRenderer::resolve_object_name(int16_t index) const {
    if (index < 0)
        return {};
    if (gcode_) {
        return gcode_->get_object_name(index);
    }
    if (streaming_controller_) {
        return streaming_controller_->get_object_name(index);
    }
    return {};
}

/// Case-insensitive check for "support" substring in an object name.
/// Used by both the main-thread member function and the background ghost thread.
static bool name_looks_like_support(const std::string& name) {
    static constexpr const char kSupport[] = "support";
    static constexpr size_t kSupportLen = 7;

    if (name.size() < kSupportLen)
        return false;

    for (size_t i = 0; i <= name.size() - kSupportLen; ++i) {
        bool match = true;
        for (size_t j = 0; j < kSupportLen; ++j) {
            if (std::tolower(static_cast<unsigned char>(name[i + j])) != kSupport[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

bool GCodeLayerRenderer::is_support_segment(const ToolpathSegment& seg) const {
    if (seg.object_name_index < 0)
        return false;
    return name_looks_like_support(resolve_object_name(seg.object_name_index));
}

std::optional<std::string> GCodeLayerRenderer::pick_object_at(int screen_x, int screen_y) const {
    // Need data source
    if (!gcode_ && !streaming_controller_)
        return std::nullopt;

    int layer_count = get_layer_count();
    if (current_layer_ < 0 || current_layer_ >= layer_count)
        return std::nullopt;

    // Capture transform params (no widget offset for cache coords)
    TransformParams transform = capture_transform_params();

    const float PICK_THRESHOLD = kPickThresholdPx;
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    // Get segments for current layer
    std::shared_ptr<const std::vector<ToolpathSegment>> segments_holder;
    const std::vector<ToolpathSegment>* segments = nullptr;

    if (streaming_controller_) {
        segments_holder =
            streaming_controller_->get_layer_segments(static_cast<size_t>(current_layer_));
        segments = segments_holder.get();
    } else if (gcode_) {
        segments = &gcode_->layers[static_cast<size_t>(current_layer_)].segments;
    }

    if (!segments)
        return std::nullopt;

    glm::vec2 click_pos(static_cast<float>(screen_x), static_cast<float>(screen_y));

    for (const auto& seg : *segments) {
        if (!should_render_segment(seg))
            continue;

        if (seg.object_name_index < 0)
            continue;

        const std::string& obj_name = resolve_object_name(seg.object_name_index);
        if (obj_name.empty())
            continue;

        // Project segment endpoints to screen space
        glm::ivec2 p1 = world_to_screen_raw(transform, seg.start.x, seg.start.y, seg.start.z);
        glm::ivec2 p2 = world_to_screen_raw(transform, seg.end.x, seg.end.y, seg.end.z);

        // Calculate distance from click to line segment
        glm::vec2 v(static_cast<float>(p2.x - p1.x), static_cast<float>(p2.y - p1.y));
        glm::vec2 w(click_pos.x - static_cast<float>(p1.x), click_pos.y - static_cast<float>(p1.y));

        float segment_length_sq = glm::dot(v, v);
        float t = (segment_length_sq > 0.0001f)
                      ? std::clamp(glm::dot(w, v) / segment_length_sq, 0.0f, 1.0f)
                      : 0.0f;

        glm::vec2 closest_point(static_cast<float>(p1.x) + t * v.x,
                                static_cast<float>(p1.y) + t * v.y);
        float dist = glm::length(click_pos - closest_point);

        if (dist < PICK_THRESHOLD && dist < closest_distance) {
            closest_distance = dist;
            picked_object = obj_name;
        }
    }

    return picked_object;
}

lv_color_t GCodeLayerRenderer::get_segment_color(const ToolpathSegment& seg) const {
    // Check excluded/highlighted state first
    if (seg.object_name_index >= 0) {
        const std::string& obj_name = resolve_object_name(seg.object_name_index);
        if (!obj_name.empty()) {
            if (excluded_objects_.count(obj_name) > 0) {
                return lv_color_hex(kExcludedObjectColor);
            }
            if (highlighted_objects_.count(obj_name) > 0) {
                return lv_color_hex(kHighlightedObjectColor);
            }
        }
    }

    // Existing logic below
    if (!seg.is_extrusion) {
        return color_travel_;
    }
    if (is_support_segment(seg)) {
        return color_support_;
    }
    // Per-tool color from palette (multi-color prints or AMS overrides)
    if (tool_palette_.has_tool_colors()) {
        return tool_palette_.resolve(seg.tool_index, color_extrusion_);
    }
    return color_extrusion_;
}

void GCodeLayerRenderer::render_selection_brackets(lv_layer_t* layer) {
    // Only render if we have highlighted objects and full gcode data
    if (highlighted_objects_.empty() || !gcode_) {
        return;
    }

    for (const auto& object_name : highlighted_objects_) {
        auto it = gcode_->objects.find(object_name);
        if (it == gcode_->objects.end()) {
            continue;
        }

        const AABB& bbox = it->second.bounding_box;

        // Calculate corner bracket length (20% of shortest edge, capped at 5mm)
        // Same formula as 3D renderer
        float dx = bbox.max.x - bbox.min.x;
        float dy = bbox.max.y - bbox.min.y;
        float dz = bbox.max.z - bbox.min.z;
        float min_edge = std::min({dx, dy, dz});
        float bracket_len = std::min(min_edge * 0.2f, 5.0f);

        // If bounding box is degenerate, skip
        if (bracket_len < 0.01f) {
            continue;
        }

        // Set up line drawing style
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(kBracketColor);
        dsc.width = 2;
        dsc.opa = LV_OPA_COVER;

        // Define all 8 corners of the AABB
        const glm::vec3 corners[8] = {
            {bbox.min.x, bbox.min.y, bbox.min.z}, // 0: min,min,min
            {bbox.max.x, bbox.min.y, bbox.min.z}, // 1: max,min,min
            {bbox.max.x, bbox.max.y, bbox.min.z}, // 2: max,max,min
            {bbox.min.x, bbox.max.y, bbox.min.z}, // 3: min,max,min
            {bbox.min.x, bbox.min.y, bbox.max.z}, // 4: min,min,max
            {bbox.max.x, bbox.min.y, bbox.max.z}, // 5: max,min,max
            {bbox.max.x, bbox.max.y, bbox.max.z}, // 6: max,max,max
            {bbox.min.x, bbox.max.y, bbox.max.z}, // 7: min,max,max
        };

        // For each corner, define 3 bracket directions (along X, Y, Z axes)
        // Sign: +1 if bracket goes toward max, -1 if toward min
        const float signs[8][3] = {
            {+1, +1, +1}, // corner 0: min,min,min -> bracket toward max
            {-1, +1, +1}, // corner 1: max,min,min
            {-1, -1, +1}, // corner 2: max,max,min
            {+1, -1, +1}, // corner 3: min,max,min
            {+1, +1, -1}, // corner 4: min,min,max
            {-1, +1, -1}, // corner 5: max,min,max
            {-1, -1, -1}, // corner 6: max,max,max
            {+1, -1, -1}, // corner 7: min,max,max
        };

        for (int c = 0; c < 8; ++c) {
            glm::ivec2 corner_screen = world_to_screen(corners[c].x, corners[c].y, corners[c].z);

            // Draw bracket line along X axis
            glm::ivec2 bx = world_to_screen(corners[c].x + signs[c][0] * bracket_len, corners[c].y,
                                            corners[c].z);

            dsc.p1.x = static_cast<lv_value_precise_t>(corner_screen.x);
            dsc.p1.y = static_cast<lv_value_precise_t>(corner_screen.y);
            dsc.p2.x = static_cast<lv_value_precise_t>(bx.x);
            dsc.p2.y = static_cast<lv_value_precise_t>(bx.y);
            lv_draw_line(layer, &dsc);

            // Draw bracket line along Y axis
            glm::ivec2 by = world_to_screen(corners[c].x, corners[c].y + signs[c][1] * bracket_len,
                                            corners[c].z);

            dsc.p2.x = static_cast<lv_value_precise_t>(by.x);
            dsc.p2.y = static_cast<lv_value_precise_t>(by.y);
            lv_draw_line(layer, &dsc);

            // Draw bracket line along Z axis
            glm::ivec2 bz = world_to_screen(corners[c].x, corners[c].y,
                                            corners[c].z + signs[c][2] * bracket_len);

            dsc.p2.x = static_cast<lv_value_precise_t>(bz.x);
            dsc.p2.y = static_cast<lv_value_precise_t>(bz.y);
            lv_draw_line(layer, &dsc);
        }
    }
}

// ============================================================================
// Background Thread Ghost Rendering
// ============================================================================
// LVGL drawing APIs are not thread-safe. To avoid blocking the UI during
// ghost cache generation, we render to a raw pixel buffer in a background
// thread using software Bresenham line drawing, then copy to the LVGL
// draw buffer on the main thread when complete.

void GCodeLayerRenderer::start_background_ghost_render() {
    // Cancel any existing render first
    cancel_background_ghost_render();

    // Need either gcode or streaming controller
    int layer_count = get_layer_count();
    if (layer_count == 0) {
        return;
    }

    // Allocate raw buffer if dimensions changed or not allocated
    int width = canvas_width_;
    int height = canvas_height_;
    size_t stride = static_cast<size_t>(width) * 4; // ARGB8888 = 4 bytes per pixel
    size_t buffer_size = stride * static_cast<size_t>(height);

    if (ghost_raw_width_ != width || ghost_raw_height_ != height || !ghost_raw_buffer_) {
        ghost_raw_buffer_ = std::make_unique<uint8_t[]>(buffer_size);
        ghost_raw_width_ = width;
        ghost_raw_height_ = height;
        ghost_raw_stride_ = stride;
    }

    // Clear buffer to transparent black (ARGB = 0x00000000)
    std::memset(ghost_raw_buffer_.get(), 0, buffer_size);

    // Reset flags
    ghost_thread_cancel_.store(false);
    ghost_thread_ready_.store(false);

    // Launch background thread - only set running flag after successful creation
    try {
        ghost_thread_ = std::thread(&GCodeLayerRenderer::background_ghost_render_thread, this);
        ghost_thread_running_.store(true);
    } catch (const std::system_error& e) {
        spdlog::error("[GCodeLayerRenderer] Failed to start ghost render thread: {}", e.what());
        return;
    }

    spdlog::debug("[GCodeLayerRenderer] Started background ghost render thread ({}x{})", width,
                  height);
}

void GCodeLayerRenderer::cancel_background_ghost_render() {
    // Signal cancellation and join if the thread is joinable.
    // This handles the case where the thread completed naturally but wasn't
    // joined yet - we MUST join before assigning a new thread or std::terminate() is called.
    ghost_thread_cancel_.store(true);
    if (ghost_thread_.joinable()) {
        ghost_thread_.join();
    }
    ghost_thread_running_.store(false);
    ghost_thread_cancel_.store(false); // Reset for next run
}

// =============================================================================
// Ghost Build Progress
// =============================================================================

float GCodeLayerRenderer::get_ghost_build_progress() const {
    // Background thread running: return 0.5 (in progress)
    // Background thread ready: return 1.0 (complete)
    // Otherwise: return 1.0 (nothing to do or already done)
    return ghost_thread_ready_.load() ? 1.0f : (ghost_thread_running_.load() ? 0.5f : 1.0f);
}

bool GCodeLayerRenderer::is_ghost_build_complete() const {
    return ghost_thread_ready_.load() || ghost_cache_valid_;
}

bool GCodeLayerRenderer::is_ghost_build_running() const {
    return ghost_thread_running_.load();
}

void GCodeLayerRenderer::background_ghost_render_thread() {
    // Works with both full-file mode (gcode_) and streaming mode (streaming_controller_)
    if (!ghost_raw_buffer_ || (!gcode_ && !streaming_controller_)) {
        ghost_thread_running_.store(false);
        return;
    }

    // Use std::chrono for timing - lv_tick_get() is not thread-safe
    auto start_time = std::chrono::steady_clock::now();
    size_t segments_rendered = 0;
    int total_layers = get_layer_count();

    // =========================================================================
    // THREAD SAFETY: Capture ALL shared state at thread start
    // These values may be modified by the main thread during rendering, so we
    // take a snapshot to ensure consistent rendering throughout.
    // =========================================================================

    // Use TransformParams for unified coordinate conversion - includes content offset!
    // This is the SINGLE SOURCE OF TRUTH for coordinate transforms.
    TransformParams transform = capture_transform_params();
    // Override canvas size with ghost buffer dimensions (may differ from display)
    transform.canvas_width = ghost_raw_width_;
    transform.canvas_height = ghost_raw_height_;

    // Visibility flags (can be changed via set_show_*() on main thread)
    const bool local_show_travels = show_travels_.load(std::memory_order_relaxed);
    const bool local_show_extrusions = show_extrusions_.load(std::memory_order_relaxed);
    const bool local_show_supports = show_supports_.load(std::memory_order_relaxed);

    // Color (can be changed via set_extrusion_color() on main thread)
    const lv_color_t local_color_extrusion = color_extrusion_;

    // Tool palette (for multi-color ghost rendering)
    const GCodeColorPalette local_tool_palette = tool_palette_;
    const bool local_use_custom_extrusion = use_custom_extrusion_color_;

    // Capture extrusion pixel width (uses scale_ which may change on main thread)
    const int local_line_width = get_extrusion_pixel_width();

    // Capture excluded objects for ghost rendering (thread-safe copy)
    const auto local_excluded = excluded_objects_;

    // Capture data source pointers — these may be nulled on the main thread after
    // cancel_background_ghost_render() joins us, but we must not read the member
    // fields during iteration (the objects they point to could be destroyed).
    auto* local_streaming = streaming_controller_;
    auto* local_gcode = gcode_;

    // Local object name resolver using captured pointers (not member fields)
    auto local_resolve_name = [local_gcode, local_streaming](int16_t index) -> std::string {
        if (index < 0)
            return {};
        if (local_gcode)
            return local_gcode->get_object_name(index);
        if (local_streaming)
            return local_streaming->get_object_name(index);
        return {};
    };

    // Visibility check for a segment given its pre-resolved object name.
    // Uses name_looks_like_support() (shared with is_support_segment()) to avoid duplication.
    auto local_should_render = [&](const ToolpathSegment& seg,
                                   const std::string& obj_name) -> bool {
        if (seg.is_extrusion) {
            if (seg.object_name_index >= 0 && name_looks_like_support(obj_name))
                return local_show_supports;
            return local_show_extrusions;
        }
        return local_show_travels;
    };

    // Compute ghost color once (darkened extrusion color from captured value)
    // ARGB8888: A in high byte, R, G, B in lower bytes
    uint8_t ghost_r = local_color_extrusion.red * kGhostDarkenPercent / 100;
    uint8_t ghost_g = local_color_extrusion.green * kGhostDarkenPercent / 100;
    uint8_t ghost_b = local_color_extrusion.blue * kGhostDarkenPercent / 100;
    uint8_t ghost_a = 255; // Full alpha, we'll apply 40% when blitting
    uint32_t ghost_color = (ghost_a << 24) | (ghost_r << 16) | (ghost_g << 8) | ghost_b;

    // Render all layers to raw buffer
    // Works with both full-file mode (gcode_) and streaming mode (streaming_controller_)
    for (int layer_idx = 0; layer_idx < total_layers; ++layer_idx) {
        // Check for cancellation periodically
        if (ghost_thread_cancel_.load()) {
            spdlog::debug("[GCodeLayerRenderer] Ghost render cancelled at layer {}/{}", layer_idx,
                          total_layers);
            ghost_thread_running_.store(false);
            return;
        }

        // Get segments from appropriate source
        // CRITICAL: For streaming mode, hold shared_ptr to keep data alive during iteration.
        // This prevents use-after-free if cache evicts the layer while we're iterating.
        std::shared_ptr<const std::vector<ToolpathSegment>> segments_holder;
        const std::vector<ToolpathSegment>* segments = nullptr;

        if (local_streaming) {
            // Streaming mode: get segments from controller (returns shared_ptr)
            segments_holder = local_streaming->get_layer_segments(static_cast<size_t>(layer_idx));
            segments = segments_holder.get();
        } else if (local_gcode) {
            segments = &local_gcode->layers[layer_idx].segments;
        }

        if (!segments)
            continue;

        for (const auto& seg : *segments) {
            // Resolve object name once per segment (used for support detection + exclusion)
            const std::string obj_name = local_resolve_name(seg.object_name_index);

            if (!local_should_render(seg, obj_name))
                continue;

            // Use unified world_to_screen_raw - includes content offset!
            glm::ivec2 p1 = world_to_screen_raw(transform, seg.start.x, seg.start.y, seg.start.z);
            glm::ivec2 p2 = world_to_screen_raw(transform, seg.end.x, seg.end.y, seg.end.z);

            // Skip zero-length segments
            if (p1.x == p2.x && p1.y == p2.y)
                continue;

            // Per-segment ghost color (tool palette or single color)
            uint32_t seg_color = ghost_color;
            if (local_tool_palette.has_tool_colors()) {
                lv_color_t tc = local_tool_palette.resolve(seg.tool_index, local_color_extrusion);
                uint8_t tr = tc.red * kGhostDarkenPercent / 100;
                uint8_t tg = tc.green * kGhostDarkenPercent / 100;
                uint8_t tb = tc.blue * kGhostDarkenPercent / 100;
                seg_color = (255u << 24) | (tr << 16) | (tg << 8) | tb;
            }
            if (!obj_name.empty() && local_excluded.count(obj_name) > 0) {
                // Excluded: dim orange-red
                uint8_t ex_r = kExcludedR * kGhostDarkenPercent / 100;
                uint8_t ex_g = kExcludedG * kGhostDarkenPercent / 100;
                uint8_t ex_b = kExcludedB * kGhostDarkenPercent / 100;
                seg_color = (255u << 24) | (ex_r << 16) | (ex_g << 8) | ex_b;
            }

            // Draw line using Bresenham algorithm (width-aware)
            draw_thick_line_bresenham(p1.x, p1.y, p2.x, p2.y, seg_color, local_line_width);
            ++segments_rendered;
        }
    }

    // Mark as ready for main thread to copy
    ghost_thread_ready_.store(true);
    ghost_thread_running_.store(false);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_time)
                       .count();
    spdlog::debug(
        "[GCodeLayerRenderer] Background ghost render complete: {} layers, {} segments in {}ms",
        total_layers, segments_rendered, elapsed);
}

void GCodeLayerRenderer::copy_raw_to_ghost_buf() {
    if (!ghost_thread_ready_.load() || !ghost_raw_buffer_ || !ghost_buf_) {
        return;
    }

    // Validate dimensions match - if canvas was resized during background render,
    // the raw buffer dimensions won't match the LVGL buffer
    // LVGL 9: dimensions are in the header struct
    uint32_t lvgl_width = ghost_buf_->header.w;
    uint32_t lvgl_height = ghost_buf_->header.h;
    if (ghost_raw_width_ != static_cast<int>(lvgl_width) ||
        ghost_raw_height_ != static_cast<int>(lvgl_height)) {
        spdlog::warn("[GCodeLayerRenderer] Ghost buffer dimension mismatch (raw {}x{} vs LVGL "
                     "{}x{}), discarding",
                     ghost_raw_width_, ghost_raw_height_, lvgl_width, lvgl_height);
        ghost_thread_ready_.store(false);
        return;
    }

    // Get LVGL buffer stride (may differ from our raw stride due to alignment)
    // LVGL 9: stride is in the header struct
    uint32_t lvgl_stride = ghost_buf_->header.stride;

    if (static_cast<size_t>(lvgl_stride) == ghost_raw_stride_) {
        // Fast path: strides match, single memcpy
        size_t buffer_size = ghost_raw_stride_ * static_cast<size_t>(ghost_raw_height_);
        std::memcpy(ghost_buf_->data, ghost_raw_buffer_.get(), buffer_size);
    } else {
        // Slow path: strides differ, copy row by row
        spdlog::debug("[GCodeLayerRenderer] Stride mismatch (raw {} vs LVGL {}), row-by-row copy",
                      ghost_raw_stride_, lvgl_stride);
        for (int y = 0; y < ghost_raw_height_; ++y) {
            std::memcpy(static_cast<uint8_t*>(ghost_buf_->data) + y * lvgl_stride,
                        ghost_raw_buffer_.get() + y * ghost_raw_stride_,
                        ghost_raw_width_ * 4); // Copy only actual pixel data (4 bytes per pixel)
        }
    }

    ghost_cache_valid_ = true;
    ghost_thread_ready_.store(false); // Consumed

    spdlog::debug("[GCodeLayerRenderer] Copied raw ghost buffer to LVGL ({}x{})", ghost_raw_width_,
                  ghost_raw_height_);
}

void GCodeLayerRenderer::blend_pixel(int x, int y, uint32_t color) {
    // Bounds check
    if (x < 0 || x >= ghost_raw_width_ || y < 0 || y >= ghost_raw_height_) {
        return;
    }

    // Calculate pixel offset (ARGB8888 = 4 bytes per pixel)
    uint8_t* pixel = ghost_raw_buffer_.get() + y * ghost_raw_stride_ + x * 4;

    // Simple overwrite for now (could add alpha blending later)
    // LVGL uses ARGB8888: byte order is B, G, R, A on little-endian
    pixel[0] = color & 0xFF;         // B
    pixel[1] = (color >> 8) & 0xFF;  // G
    pixel[2] = (color >> 16) & 0xFF; // R
    pixel[3] = (color >> 24) & 0xFF; // A
}

void GCodeLayerRenderer::blend_pixel_solid(int x, int y, uint32_t color) {
    // Bounds check using cached dimensions
    if (x < 0 || x >= cached_width_ || y < 0 || y >= cached_height_ || !cache_buf_) {
        return;
    }

    // Get stride from LVGL buffer (may differ from width * 4 due to alignment)
    uint32_t stride = cache_buf_->header.stride;

    // Calculate pixel offset (ARGB8888 = 4 bytes per pixel)
    uint8_t* pixel = static_cast<uint8_t*>(cache_buf_->data) + y * stride + x * 4;

    // LVGL uses ARGB8888: byte order is B, G, R, A on little-endian
    pixel[0] = color & 0xFF;         // B
    pixel[1] = (color >> 8) & 0xFF;  // G
    pixel[2] = (color >> 16) & 0xFF; // R
    pixel[3] = (color >> 24) & 0xFF; // A
}

void GCodeLayerRenderer::blend_pixel_solid_alpha(int x, int y, uint32_t color, uint8_t coverage) {
    if (x < 0 || x >= cached_width_ || y < 0 || y >= cached_height_ || !cache_buf_)
        return;
    if (coverage == 0)
        return;

    uint32_t stride = cache_buf_->header.stride;
    uint8_t* pixel = static_cast<uint8_t*>(cache_buf_->data) + y * stride + x * 4;

    uint8_t src_b = color & 0xFF;
    uint8_t src_g = (color >> 8) & 0xFF;
    uint8_t src_r = (color >> 16) & 0xFF;

    if (coverage == 255 || pixel[3] == 0) {
        // Full coverage or empty destination: just write
        pixel[0] = src_b;
        pixel[1] = src_g;
        pixel[2] = src_r;
        pixel[3] = coverage;
    } else {
        // Alpha blend: src over dst
        uint8_t dst_a = pixel[3];
        uint16_t inv = 255 - coverage;
        pixel[0] = static_cast<uint8_t>((src_b * coverage + pixel[0] * inv) / 255);
        pixel[1] = static_cast<uint8_t>((src_g * coverage + pixel[1] * inv) / 255);
        pixel[2] = static_cast<uint8_t>((src_r * coverage + pixel[2] * inv) / 255);
        pixel[3] = static_cast<uint8_t>(coverage + (dst_a * inv) / 255);
    }
}

void GCodeLayerRenderer::draw_line_aa_solid(int x0, int y0, int x1, int y1, uint32_t color) {
    // Xiaolin Wu's anti-aliased line algorithm
    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) {
        std::swap(x0, y0);
        std::swap(x1, y1);
    }
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    float gradient = (dx < 0.001f) ? 1.0f : dy / dx;

    // Strip alpha from color — we'll set it per-pixel via coverage
    uint32_t base_color = color & 0x00FFFFFF;

    // First endpoint
    float yend = static_cast<float>(y0);
    float intery = yend + gradient;

    if (steep) {
        blend_pixel_solid_alpha(static_cast<int>(yend), x0, base_color, 255);
    } else {
        blend_pixel_solid_alpha(x0, static_cast<int>(yend), base_color, 255);
    }

    // Second endpoint
    if (steep) {
        blend_pixel_solid_alpha(y1, x1, base_color, 255);
    } else {
        blend_pixel_solid_alpha(x1, y1, base_color, 255);
    }

    // Main loop — draw pixels with fractional coverage for AA
    for (int x = x0 + 1; x < x1; x++) {
        int iy = static_cast<int>(intery);
        float frac = intery - iy;
        uint8_t coverage_lo = static_cast<uint8_t>((1.0f - frac) * 255);
        uint8_t coverage_hi = static_cast<uint8_t>(frac * 255);

        if (steep) {
            blend_pixel_solid_alpha(iy, x, base_color, coverage_lo);
            blend_pixel_solid_alpha(iy + 1, x, base_color, coverage_hi);
        } else {
            blend_pixel_solid_alpha(x, iy, base_color, coverage_lo);
            blend_pixel_solid_alpha(x, iy + 1, base_color, coverage_hi);
        }
        intery += gradient;
    }
}

void GCodeLayerRenderer::draw_thick_line_aa_solid(int x0, int y0, int x1, int y1, uint32_t color,
                                                  int width) {
    if (width <= 1) {
        draw_line_aa_solid(x0, y0, x1, y1, color);
        return;
    }

    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    constexpr float kMinLineLength = 0.5f;
    float len = std::sqrt(dx * dx + dy * dy);

    if (len < kMinLineLength) {
        draw_line_aa_solid(x0, y0, x1, y1, color);
        return;
    }

    // Perpendicular direction for thickness offset
    float px = -dy / len;
    float py = dx / len;

    // Draw parallel lines offset perpendicular to the main line
    int half = width / 2;
    for (int i = -half; i <= half; i++) {
        float offset = static_cast<float>(i);
        int ox = static_cast<int>(std::round(px * offset));
        int oy = static_cast<int>(std::round(py * offset));
        draw_line_aa_solid(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
    }
}

int GCodeLayerRenderer::get_extrusion_pixel_width() const {
    float width_mm = kDefaultExtrusionWidthMm;

    if (gcode_) {
        // Full-file mode: prefer extrusion_width_mm, then nozzle_diameter_mm
        if (gcode_->extrusion_width_mm > 0.0f) {
            width_mm = gcode_->extrusion_width_mm;
        } else if (gcode_->nozzle_diameter_mm > 0.0f) {
            width_mm = gcode_->nozzle_diameter_mm;
        }
    }
    // Streaming mode: no metadata available, use default 0.4mm

    int pixel_width = static_cast<int>(std::round(width_mm * scale_));
    return std::clamp(pixel_width, kMinExtrusionPixelWidth, kMaxExtrusionPixelWidth);
}

void GCodeLayerRenderer::draw_thick_line_bresenham(int x0, int y0, int x1, int y1, uint32_t color,
                                                   int width) {
    if (width <= 1) {
        draw_line_bresenham(x0, y0, x1, y1, color);
        return;
    }

    // Compute perpendicular direction to the line
    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    float len = std::sqrt(dx * dx + dy * dy);

    if (len < kMinLineLength) {
        draw_line_bresenham(x0, y0, x1, y1, color);
        return;
    }

    // Perpendicular unit vector (rotated 90 degrees)
    float px = -dy / len;
    float py = dx / len;

    // Draw parallel lines offset by [-width/2, +width/2]
    float half = static_cast<float>(width - 1) * 0.5f;
    for (int i = 0; i < width; ++i) {
        float offset = static_cast<float>(i) - half;
        int ox = static_cast<int>(std::round(px * offset));
        int oy = static_cast<int>(std::round(py * offset));
        draw_line_bresenham(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
    }
}

void GCodeLayerRenderer::draw_thick_line_bresenham_solid(int x0, int y0, int x1, int y1,
                                                         uint32_t color, int width) {
    if (width <= 1) {
        draw_line_bresenham_solid(x0, y0, x1, y1, color);
        return;
    }

    // Compute perpendicular direction to the line
    float dx = static_cast<float>(x1 - x0);
    float dy = static_cast<float>(y1 - y0);
    float len = std::sqrt(dx * dx + dy * dy);

    if (len < kMinLineLength) {
        draw_line_bresenham_solid(x0, y0, x1, y1, color);
        return;
    }

    // Perpendicular unit vector (rotated 90 degrees)
    float px = -dy / len;
    float py = dx / len;

    // Draw parallel lines offset by [-width/2, +width/2]
    float half = static_cast<float>(width - 1) * 0.5f;
    for (int i = 0; i < width; ++i) {
        float offset = static_cast<float>(i) - half;
        int ox = static_cast<int>(std::round(px * offset));
        int oy = static_cast<int>(std::round(py * offset));
        draw_line_bresenham_solid(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
    }
}

void GCodeLayerRenderer::draw_line_bresenham_solid(int x0, int y0, int x1, int y1, uint32_t color) {
    // Bresenham's line algorithm for software line drawing to solid cache

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        blend_pixel_solid(x0, y0, color);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy) {
            if (x0 == x1)
                break;
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            if (y0 == y1)
                break;
            err += dx;
            y0 += sy;
        }
    }
}

void GCodeLayerRenderer::draw_line_bresenham(int x0, int y0, int x1, int y1, uint32_t color) {
    // Bresenham's line algorithm for software line drawing
    // This runs in the background thread where LVGL APIs are not available

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        blend_pixel(x0, y0, color);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy) {
            if (x0 == x1)
                break;
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            if (y0 == y1)
                break;
            err += dx;
            y0 += sy;
        }
    }
}

// ============================================================================
// Configuration
// ============================================================================

void GCodeLayerRenderer::load_config() {
    auto* config = Config::get_instance();
    if (!config) {
        spdlog::debug("[GCodeLayerRenderer] No config instance, using defaults");
        return;
    }

    // Load layers_per_frame: 0 = adaptive, 1-100 = fixed
    config_layers_per_frame_ = config->get<int>("/gcode_viewer/layers_per_frame", 0);
    config_layers_per_frame_ = std::clamp(config_layers_per_frame_, 0, MAX_LAYERS_PER_FRAME);

    if (config_layers_per_frame_ > 0) {
        // Fixed value from config
        layers_per_frame_ =
            std::clamp(config_layers_per_frame_, MIN_LAYERS_PER_FRAME, MAX_LAYERS_PER_FRAME);
        spdlog::info("[GCodeLayerRenderer] Using fixed layers_per_frame: {}", layers_per_frame_);
    } else {
        // Adaptive mode - start with default, adjust based on render time
        layers_per_frame_ = DEFAULT_LAYERS_PER_FRAME;
        spdlog::debug("[GCodeLayerRenderer] Using adaptive layers_per_frame (starting at {})",
                      layers_per_frame_);
    }

    // Load adaptive target (only used when config_layers_per_frame_ == 0)
    adaptive_target_ms_ =
        config->get<int>("/gcode_viewer/adaptive_layer_target_ms", DEFAULT_ADAPTIVE_TARGET_MS);
    adaptive_target_ms_ = std::clamp(adaptive_target_ms_, 1, 100); // Sensible bounds

    spdlog::debug("[GCodeLayerRenderer] Adaptive target: {}ms", adaptive_target_ms_);

    // Detect device tier and apply appropriate limits for constrained devices
    auto mem_info = ::helix::get_system_memory_info();
    is_constrained_device_ = mem_info.is_constrained_device();

    if (is_constrained_device_) {
        max_layers_per_frame_ = CONSTRAINED_MAX_LPF;
        if (config_layers_per_frame_ == 0) { // Adaptive mode
            layers_per_frame_ = CONSTRAINED_START_LPF;
        }
        spdlog::info(
            "[GCodeLayerRenderer] Constrained device detected: lpf capped at {}, starting at {}",
            max_layers_per_frame_, layers_per_frame_);
    }
}

void GCodeLayerRenderer::adapt_layers_per_frame() {
    // Only adapt when in adaptive mode (config value == 0)
    if (config_layers_per_frame_ != 0) {
        return;
    }

    // Skip adaptation if no meaningful render time yet
    if (last_frame_render_ms_ == 0) {
        return;
    }

    // Adaptive algorithm:
    // - If render time < target: increase layers_per_frame to cache faster
    // - If render time > target: decrease layers_per_frame to avoid UI stutter
    // - Use exponential moving average to smooth adjustments

    int old_lpf = layers_per_frame_;

    if (last_frame_render_ms_ < static_cast<uint32_t>(adaptive_target_ms_)) {
        // Under budget - can render more layers
        // Scale up proportionally but cap growth (conservative on constrained devices)
        float ratio = static_cast<float>(adaptive_target_ms_) / std::max(1u, last_frame_render_ms_);
        float max_growth = is_constrained_device_ ? CONSTRAINED_GROWTH_CAP : 2.0f;
        ratio = std::min(ratio, max_growth);
        int new_lpf = static_cast<int>(layers_per_frame_ * ratio);
        // Smooth increase (take average of current and target)
        layers_per_frame_ = (layers_per_frame_ + new_lpf) / 2;
    } else if (last_frame_render_ms_ > static_cast<uint32_t>(adaptive_target_ms_ * 2)) {
        // Significantly over budget - reduce aggressively
        float ratio = static_cast<float>(adaptive_target_ms_) / std::max(1u, last_frame_render_ms_);
        layers_per_frame_ = static_cast<int>(layers_per_frame_ * ratio);
    } else if (last_frame_render_ms_ > static_cast<uint32_t>(adaptive_target_ms_)) {
        // Slightly over budget - reduce gradually
        layers_per_frame_ = layers_per_frame_ * 3 / 4; // 75% of current
    }

    // Clamp to valid range (using device-aware max)
    layers_per_frame_ = std::clamp(layers_per_frame_, MIN_LAYERS_PER_FRAME, max_layers_per_frame_);

    // Log significant changes
    if (layers_per_frame_ != old_lpf) {
        spdlog::trace("[GCodeLayerRenderer] Adaptive lpf: {} -> {} (render={}ms, target={}ms)",
                      old_lpf, layers_per_frame_, last_frame_render_ms_, adaptive_target_ms_);
    }
}

} // namespace gcode
} // namespace helix
