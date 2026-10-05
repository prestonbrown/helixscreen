// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin {

constexpr size_t kMaxCanvasUnits = 4096; // spec cap; a polyline counts each point, a fill none
constexpr size_t kMaxCanvasTokens = 32;  // distinct color and font tokens per list
constexpr int32_t kMaxCanvasCoord = 16384;

enum class CanvasOp : uint8_t { Line, Polyline, Rect, Arc, Circle, Text };
constexpr uint8_t kNoToken = 0xFF;

/// One drawing command. Coordinates are canvas-relative pixels, origin at the
/// top-left of the widget's content box. Token fields index DisplayList::tokens.
struct CanvasPrim {
    CanvasOp op;
    uint8_t color = kNoToken;        // stroke, or fill for Rect/Circle
    uint8_t border = kNoToken;       // Rect/Circle border; Polyline area-fill color
    uint8_t font = kNoToken;         // Text
    int32_t width = 1;               // stroke or border width
    int32_t radius = 0;              // Rect corner radius; Arc/Circle radius
    uint8_t opa = LV_OPA_COVER;      // stroke/fill alpha, 0 transparent to 255 opaque
    uint8_t fill_opa = LV_OPA_COVER; // Polyline area-fill alpha
    lv_value_precise_t a = 0, b = 0, c = 0,
                       d = 0;      // Line x1 y1 x2 y2; Rect x y w h;
                                   // Arc cx cy start end (degrees); Circle cx cy; Text x y;
                                   // Polyline baseline y in a
    uint32_t first = 0, count = 0; // Polyline: range in points; Text: range in text
};

struct DisplayList {
    std::vector<CanvasPrim> prims;
    std::vector<lv_point_precise_t> points;
    std::string text;
    std::vector<std::string> tokens; // color tokens (e.g. "primary") and font tokens (e.g. "body")
    size_t units = 0;
    /// Bytes this list holds, the figure charged to the plugin's memory cap.
    size_t bytes() const;
};

/// Registers the plugin_canvas XML widget once per process.
void register_plugin_canvas_widget();

/// Publishes `list` as the committed list for `name` and invalidates every live
/// instance. A null list blanks the canvas.
void canvas_commit(const std::string& name, std::unique_ptr<DisplayList> list);
/// The committed list for `name`, or nullptr.
const DisplayList* canvas_committed(const std::string& name);
/// Content size the most recent live instance of `name` reported, {0, 0} when none.
std::pair<int32_t, int32_t> canvas_size(const std::string& name);
/// Live plugin_canvas instances named `name`.
size_t canvas_instance_count(const std::string& name);
/// Called (main thread, synchronously, from the size event) when an instance of `name`
/// reports a content size different from the last one. An empty function removes it.
/// The listener must not enter Lua; it defers.
void canvas_set_size_listener(const std::string& name, std::function<void(int32_t, int32_t)> fn);
/// The font a `font_<name>` XML const resolves to, or null when no such const exists.
/// theme_manager_get_font itself falls back to the default font, so the const check
/// here is what makes an unknown token detectable; the binding validates with this.
const lv_font_t* canvas_resolve_font(const std::string& name);
/// Replays `list` into `layer` with `content` as its origin and resolves every token now.
void draw_display_list(lv_layer_t* layer, const lv_area_t& content, const DisplayList& list);

} // namespace helix::plugin
