// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_object_thumbnail_renderer.h"

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace helix::gcode {

// Check cancellation every N layers to avoid overhead
static constexpr int CANCEL_CHECK_INTERVAL = 10;

// ============================================================================
// CONSTRUCTION / DESTRUCTION
// ============================================================================

GCodeObjectThumbnailRenderer::GCodeObjectThumbnailRenderer() = default;

GCodeObjectThumbnailRenderer::~GCodeObjectThumbnailRenderer() {
    cancel();
}

// ============================================================================
// PUBLIC API
// ============================================================================

void GCodeObjectThumbnailRenderer::render_async(const ParsedGCodeFile* gcode, int thumb_width,
                                                int thumb_height, uint32_t color,
                                                ThumbnailCompleteCallback callback) {
    // Cancel any in-progress render
    cancel();

    if (!gcode || gcode->objects.empty()) {
        spdlog::debug("[ObjectThumbnail] No objects to render");
        if (callback) {
            callback(std::make_unique<ObjectThumbnailSet>());
        }
        return;
    }

    cancel_.store(false, std::memory_order_relaxed);
    rendering_.store(true, std::memory_order_relaxed);

    // Wrap thread spawn — pthread_create EAGAIN under tight thread limits
    // (AD5M/CC1) throws std::system_error which would propagate through the
    // calling event-cb frame and abort via std::terminate ([L083]).
    // NOTE: capture callback by COPY (`cb = callback`) inside the lambda so
    // the original `callback` parameter is still valid in the catch handler.
    // Using std::move would leave callback empty if the std::thread ctor
    // throws after the lambda is constructed but before the worker runs.
    try {
        thread_ = std::thread([this, gcode, thumb_width, thumb_height, color, cb = callback]() {
            auto result = render_impl(gcode, thumb_width, thumb_height, color);

            rendering_.store(false, std::memory_order_relaxed);

            if (!cancel_.load(std::memory_order_relaxed) && cb) {
                // Marshal result to UI thread. Use shared_ptr for lambda capture so the
                // ObjectThumbnailSet is freed even if the UI queue is drained on shutdown
                // before this lambda runs (std::function requires copyable lambdas).
                auto shared = std::shared_ptr<ObjectThumbnailSet>(result.release());
                helix::ui::queue_update([cb, shared]() {
                    cb(std::make_unique<ObjectThumbnailSet>(std::move(*shared)));
                });
            }
        });
    } catch (const std::system_error& e) {
        spdlog::error("[ObjectThumbnail] Failed to spawn render thread: {}", e.what());
        rendering_.store(false, std::memory_order_relaxed);
        if (callback) {
            helix::ui::queue_update(
                [cb = std::move(callback)]() { cb(std::make_unique<ObjectThumbnailSet>()); });
        }
    }
}

std::unique_ptr<ObjectThumbnailSet>
GCodeObjectThumbnailRenderer::render_sync(const ParsedGCodeFile* gcode, int thumb_width,
                                          int thumb_height, uint32_t color) {
    cancel_.store(false, std::memory_order_relaxed);
    rendering_.store(true, std::memory_order_relaxed);

    auto result = render_impl(gcode, thumb_width, thumb_height, color);

    rendering_.store(false, std::memory_order_relaxed);
    return result;
}

void GCodeObjectThumbnailRenderer::cancel() {
    cancel_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    cancel_.store(false, std::memory_order_relaxed);
}

// ============================================================================
// CORE RENDERING
// ============================================================================

std::unique_ptr<ObjectThumbnailSet>
GCodeObjectThumbnailRenderer::render_impl(const ParsedGCodeFile* gcode, int thumb_width,
                                          int thumb_height, uint32_t color) {
    auto start_time = std::chrono::steady_clock::now();

    auto result = std::make_unique<ObjectThumbnailSet>();

    if (!gcode || gcode->objects.empty()) {
        return result;
    }

    // Build per-object render contexts with coordinate transforms
    auto contexts = build_contexts(gcode, thumb_width, thumb_height);

    if (contexts.empty()) {
        spdlog::debug("[ObjectThumbnail] No valid object contexts (all empty bounding boxes?)");
        return result;
    }

    // Single pass through all layers and segments
    size_t segments_rendered = 0;
    for (size_t layer_idx = 0; layer_idx < gcode->layers.size(); ++layer_idx) {
        // Periodic cancellation check
        if ((layer_idx % CANCEL_CHECK_INTERVAL) == 0 && cancel_.load(std::memory_order_relaxed)) {
            spdlog::debug("[ObjectThumbnail] Cancelled at layer {}/{}", layer_idx,
                          gcode->layers.size());
            return result;
        }

        const auto& layer = gcode->layers[layer_idx];
        for (const auto& seg : layer.segments) {
            // Skip non-extrusion and unnamed segments
            if (!seg.is_extrusion || seg.object_name_index < 0) {
                continue;
            }

            // Find the render context for this object
            const std::string& seg_obj_name = gcode->get_object_name(seg.object_name_index);
            if (seg_obj_name.empty()) {
                continue;
            }
            auto it = contexts.find(seg_obj_name);
            if (it == contexts.end()) {
                continue;
            }

            auto& ctx = it->second;

            // Convert world coordinates to pixel coordinates (FRONT view with Z)
            int px0, py0, px1, py1;
            world_to_pixel(ctx, seg.start.x, seg.start.y, seg.start.z, px0, py0);
            world_to_pixel(ctx, seg.end.x, seg.end.y, seg.end.z, px1, py1);

            // Depth shading: shared with layer renderer (bottom darker, back darker)
            float avg_z = (seg.start.z + seg.end.z) * 0.5f;
            float avg_y = (seg.start.y + seg.end.y) * 0.5f;
            float brightness =
                compute_depth_brightness(avg_z, ctx.z_min, ctx.z_max, avg_y, ctx.y_min, ctx.y_max);

            // Apply brightness to ARGB8888 color
            uint8_t b = static_cast<uint8_t>((color & 0xFF) * brightness);
            uint8_t g = static_cast<uint8_t>(((color >> 8) & 0xFF) * brightness);
            uint8_t r = static_cast<uint8_t>(((color >> 16) & 0xFF) * brightness);
            uint8_t a = (color >> 24) & 0xFF;
            uint32_t shaded = b | (g << 8) | (r << 16) | (a << 24);

            // Draw the line
            draw_line(ctx, px0, py0, px1, py1, shaded);
            ++segments_rendered;
        }
    }

    // Convert contexts to output thumbnails
    for (auto& [name, ctx] : contexts) {
        ObjectThumbnail thumb;
        thumb.object_name = name;
        thumb.pixels = std::move(ctx.pixels);
        thumb.width = ctx.width;
        thumb.height = ctx.height;
        thumb.stride = ctx.stride;
        result->thumbnails.push_back(std::move(thumb));
    }

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    spdlog::debug("[ObjectThumbnail] Rendered {} thumbnails ({} segments) in {}ms",
                  result->thumbnails.size(), segments_rendered, ms);

    return result;
}

std::unordered_map<std::string, GCodeObjectThumbnailRenderer::ObjectRenderContext>
GCodeObjectThumbnailRenderer::build_contexts(const ParsedGCodeFile* gcode, int thumb_width,
                                             int thumb_height) {
    std::unordered_map<std::string, ObjectRenderContext> contexts;

    if (thumb_width <= 0 || thumb_height <= 0) {
        return contexts;
    }

    // Padding factor for auto-fit (5% each side, matching layer renderer)
    constexpr float PADDING = 0.05f;

    for (const auto& [name, obj] : gcode->objects) {
        const auto& bbox = obj.bounding_box;

        // Skip objects with empty/degenerate bounding boxes
        if (bbox.is_empty()) {
            continue;
        }

        // Use shared auto-fit with FRONT projection (isometric view)
        auto fit = compute_auto_fit(bbox, ViewMode::FRONT, thumb_width, thumb_height, PADDING);

        ObjectRenderContext ctx;
        ctx.name = name;
        ctx.width = thumb_width;
        ctx.height = thumb_height;
        ctx.stride = thumb_width * 4;

        // Store projection params for world_to_pixel
        ctx.projection.view_mode = ViewMode::FRONT;
        ctx.projection.scale = fit.scale;
        ctx.projection.offset_x = fit.offset_x;
        ctx.projection.offset_y = fit.offset_y;
        ctx.projection.offset_z = fit.offset_z;
        ctx.projection.canvas_width = thumb_width;
        ctx.projection.canvas_height = thumb_height;

        // Store Z/Y ranges for depth shading (shared compute_depth_brightness handles zero-range)
        ctx.z_min = bbox.min.z;
        ctx.z_max = bbox.max.z;
        ctx.y_min = bbox.min.y;
        ctx.y_max = bbox.max.y;

        // Allocate and zero-fill pixel buffer (transparent black)
        size_t buf_size = static_cast<size_t>(ctx.height) * ctx.stride;
        ctx.pixels = std::make_unique<uint8_t[]>(buf_size);
        std::memset(ctx.pixels.get(), 0, buf_size);

        contexts.emplace(name, std::move(ctx));
    }

    return contexts;
}

// ============================================================================
// DRAWING PRIMITIVES
// ============================================================================

void GCodeObjectThumbnailRenderer::world_to_pixel(const ObjectRenderContext& ctx, float wx,
                                                  float wy, float wz, int& px, int& py) {
    // Use shared projection (FRONT view: isometric with Z)
    auto p = project(ctx.projection, wx, wy, wz);
    px = p.x;
    py = p.y;
}

void GCodeObjectThumbnailRenderer::put_pixel(ObjectRenderContext& ctx, int x, int y,
                                             uint32_t color) {
    if (x < 0 || x >= ctx.width || y < 0 || y >= ctx.height) {
        return;
    }

    uint8_t* pixel = ctx.pixels.get() + y * ctx.stride + x * 4;

    // LVGL ARGB8888: byte order is B, G, R, A on little-endian
    pixel[0] = color & 0xFF;         // B
    pixel[1] = (color >> 8) & 0xFF;  // G
    pixel[2] = (color >> 16) & 0xFF; // R
    pixel[3] = (color >> 24) & 0xFF; // A
}

void GCodeObjectThumbnailRenderer::draw_line(ObjectRenderContext& ctx, int x0, int y0, int x1,
                                             int y1, uint32_t color) {
    // Bresenham's line algorithm
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        // Plot 2x2 block for thicker lines (put_pixel bounds-checks)
        put_pixel(ctx, x0, y0, color);
        put_pixel(ctx, x0 + 1, y0, color);
        put_pixel(ctx, x0, y0 + 1, color);
        put_pixel(ctx, x0 + 1, y0 + 1, color);

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

} // namespace helix::gcode

#endif // HELIX_HAS_GCODE_VIEWER
