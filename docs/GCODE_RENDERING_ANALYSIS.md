# G-Code Rendering Deep Dive: Techniques & Implementation Strategy

**Author:** Claude Code
**Date:** 2025-11-16
**Status:** Research & Recommendations
**Target:** Efficient G-code visualization for helixscreen on limited hardware

---

## Executive Summary

**Bottom Line:** We can implement efficient G-code rendering WITHOUT OpenGL using our existing bed mesh rendering techniques. Porting PrusaSlicer/OrcaSlicer is **NOT recommended** due to heavy OpenGL dependencies. Instead, implement custom software rendering using proven algorithms.

**Recommended Approach:** 2D/2.5D toolpath visualization using:
- Bresenham line algorithm (integer-only, very fast)
- Incremental G-code parsing (memory efficient)
- LVGL canvas with strategic optimizations
- Optional layer-height-based 3D projection (reuse bed_mesh_renderer techniques)

**Feasibility:** HIGH - We already have all the infrastructure needed.

---

## 1. Current State: Existing Rendering Infrastructure

### ✅ What We Already Have

**Excellent Foundation:** `bed_mesh_renderer.cpp/.h` demonstrates advanced software rendering:

| Capability | Implementation | Relevance to G-code |
|------------|---------------|---------------------|
| **3D Perspective Projection** | Lines 356-390 | Can render tool paths with Z-height |
| **Scanline Rasterization** | Lines 454-615 | Efficient triangle/quad filling |
| **Painter's Algorithm** | Line 693-699 | Back-to-front depth sorting |
| **Color Interpolation** | Lines 446-452 | Can color-code by speed/temp/layer |
| **Interactive Rotation** | Lines 153-175 | User can rotate G-code preview |
| **Canvas Optimization** | Line 292 | Fast/slow rendering modes (dragging vs static) |

**Key Insight:** bed_mesh_renderer.cpp:226-302 shows we can render **hundreds of quads** (20×20 mesh = 361 quads = 722 triangles) at **30+ FPS** on embedded hardware using pure software rendering.

### 📋 File Browser Infrastructure

**Already Implemented:** `ui_panel_print_select.cpp` provides:
- G-code file listing from Moonraker
- File metadata (layers, print time, filament usage)
- Thumbnail placeholders (line 608 - TODO)
- Detail overlay system (`print_file_detail.xml`)

**Integration Point:** Add G-code preview canvas to detail overlay.

---

## 2. Porting Analysis: PrusaSlicer/OrcaSlicer

### ❌ Why Porting Is NOT Recommended

**Hard Dependencies on OpenGL 3.2+:**

```cpp
// PrusaSlicer rendering stack (from research):
GLCanvas3D → GLModel::Geometry → Vertex Buffer Objects (VBOs)
          → GLSL Shaders → Texture mapping → GPU acceleration
```

**Architectural Incompatibilities:**

| Component | PrusaSlicer | HelixScreen Reality | Impact |
|-----------|-------------|---------------------|---------|
| **Graphics API** | OpenGL 3.2+ (shaders, VBOs) | Software rendering only | **BLOCKER** |
| **Codebase Size** | ~500K+ LOC (libslic3r + GUI) | Need minimal parser | Too heavy |
| **Dependencies** | wxWidgets, Boost, OpenGL, GLSL | SDL2, LVGL, C++11 | Major conflicts |
| **Memory Model** | Desktop (GB RAM, GPU VRAM) | Embedded (MB RAM) | Unworkable |
| **Build System** | CMake with 30+ deps | Makefile + submodules | Integration nightmare |

**Performance Reality:**
- PrusaSlicer users report **multi-second** G-code load times for large files (100K+ lines)
- GPU vertex buffer management issues (crashes with large models)
- Not designed for embedded constraints

**Verdict:** Porting would require **complete rewrite** of rendering engine. Easier to build from scratch.

---

## 3. LVGL v9 Canvas Capabilities

### ✅ What LVGL Provides

**Drawing Functions (from docs.lvgl.io):**

```c
// Core pixel operations
lv_canvas_set_px(canvas, x, y, color, opa);     // Direct pixel write
lv_canvas_fill_bg(canvas, color, opa);          // Clear canvas

// Advanced drawing (layer-based)
lv_draw_line(canvas, &line_dsc, &point1, &point2);  // Line rendering
lv_draw_triangle(canvas, &tri_dsc, p1, p2, p3);    // Triangle fill
lv_draw_rect(canvas, &rect_dsc, &area);            // Rectangles
lv_draw_arc(canvas, &arc_dsc, &center, radius);    // Arcs (for G2/G3)

// Buffer operations
lv_canvas_copy_buf(canvas, buf, x, y, w, h);    // Bulk pixel copy
```

**Performance Characteristics:**

| Operation | Speed | Use Case |
|-----------|-------|----------|
| `lv_canvas_set_px()` | Slow (per-pixel overhead) | Low-density paths only |
| `lv_draw_line()` | **Fast** (optimized internally) | **Primary method for toolpaths** |
| `lv_draw_triangle()` | Medium (scanline fill) | Layer visualization |
| `lv_canvas_copy_buf()` | **Very Fast** (memcpy) | Double-buffering |

**Key Limitation:** LVGL's `lv_draw_*` functions use **layer-based rendering** which has overhead. For maximum performance, we may need **custom line drawing**.

### 🔍 LVGL v9.4 Performance Improvements (2025)

From web research:
- **30% faster render times** on ESP32-P4 with hardware acceleration
- **Partial frame buffering** (only redraw changed areas)
- **Intelligent invalidation** (minimal CPU load)

**Our Use Case:** Static G-code preview = render once, no animations = IDEAL for LVGL.

---

## 4. Efficient Software Rendering Techniques

### 🚀 Algorithm #1: Bresenham Line Drawing (PRIMARY)

**Why Bresenham:**
- **Integer-only arithmetic** (add/subtract/shift)
- **No floating point** (fast on embedded CPUs)
- **O(n) complexity** where n = line length in pixels
- **Hardware-friendly** (FPGA implementations: 992 pixels in 0.31μs)

**Performance Comparison:**

| Algorithm | Operations | Embedded Suitability |
|-----------|-----------|----------------------|
| **Bresenham** | Integer add/sub/shift | ⭐⭐⭐⭐⭐ IDEAL |
| DDA | Floating-point round() | ⭐⭐ Slow |
| Wu's (anti-aliased) | Floating-point multiply | ⭐⭐⭐ Acceptable |
| Xiaolin Wu | Complex math | ⭐⭐ Overkill |

**Reference Implementation:**

```cpp
// Bresenham's line algorithm (integer-only)
void draw_line_bresenham(lv_obj_t* canvas, int x0, int y0, int x1, int y1, lv_color_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        lv_canvas_set_px(canvas, x0, y0, color, LV_OPA_COVER);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}
```

**Optimization:** For G-code with **thousands of short moves**, Bresenham is **perfect** (no setup overhead, pure iteration).

### 🎨 Algorithm #2: Incremental G-code Parsing

**Problem:** Large G-code files (10MB+, 500K+ lines) won't fit in RAM.

**Solution:** Stream parsing with culling:

```cpp
struct GCodeMove {
    float x, y, z;      // Endpoint (12 bytes)
    float e;            // Extrusion amount (4 bytes)
    uint16_t feedrate;  // Speed (2 bytes)
    uint8_t layer;      // Layer index (1 byte)
    uint8_t type;       // Move type: travel/perimeter/infill (1 byte)
    // Total: 20 bytes per move
};

// For 10K moves: 200 KB (acceptable)
// For 100K moves: 2 MB (use layer filtering)
```

**Parsing Strategy:**

1. **Single-pass streaming** - Read G-code line-by-line
2. **State machine** - Track current position, feedrate, temperature
3. **Filter by layer** - Only parse/render visible layer range
4. **Downsample long lines** - Skip intermediate points for straight moves

**Memory Efficiency:**

| File Size | Lines | Strategy | RAM Usage |
|-----------|-------|----------|-----------|
| 1 MB | 50K | Parse all | ~1 MB |
| 10 MB | 500K | Layer filter (10 layers) | ~200 KB |
| 50 MB | 2M | Layer filter + downsample | ~500 KB |

### 📊 Algorithm #3: Viewport Culling

**Optimization:** Don't render moves outside canvas bounds.

```cpp
bool is_line_visible(int x0, int y0, int x1, int y1, int canvas_w, int canvas_h) {
    // Cohen-Sutherland line clipping (simple reject test)
    int xmin = std::min(x0, x1);
    int xmax = std::max(x0, x1);
    int ymin = std::min(y0, y1);
    int ymax = std::max(y0, y1);

    if (xmax < 0 || xmin > canvas_w) return false;
    if (ymax < 0 || ymin > canvas_h) return false;
    return true;
}
```

**Impact:** For zoomed-in views, can skip **90%+** of moves.

### 🌈 Algorithm #4: Color Coding Schemes

**Reuse heat-map logic** from `bed_mesh_renderer.cpp:392-444`:

| Color Scheme | Maps To | Use Case |
|--------------|---------|----------|
| **Layer height** | Purple (Z=0) → Red (Z=max) | Default view |
| **Feedrate** | Blue (slow) → Yellow (fast) | Speed analysis |
| **Extrusion** | Green (travel) vs Orange (print) | Path verification |
| **Temperature** | Cyan (cool) → Red (hot) | Multi-material |
| **Line type** | Perimeter/Infill/Support | Feature identification |

**Implementation:** Simple value-to-RGB mapping (like `height_to_color()` in bed_mesh_renderer.cpp:392).

---

## 5. Recommended Implementation Architecture

### 🏗️ Component Design

```
┌─────────────────────────────────────────────────────────────┐
│  ui_panel_print_select.cpp                                  │
│  ┌───────────────────┐                                      │
│  │ File Selected     │──┐                                   │
│  └───────────────────┘  │                                   │
│                         │                                   │
│  ┌───────────────────┐  │  Opens detail overlay             │
│  │ Detail Overlay    │<─┘                                   │
│  │ (print_file_      │                                      │
│  │  detail.xml)      │                                      │
│  │                   │                                      │
│  │ ┌───────────────┐ │                                      │
│  │ │ LVGL Canvas   │ │◄─── gcode_preview_renderer          │
│  │ │ (320×240)     │ │                                      │
│  │ └───────────────┘ │                                      │
│  └───────────────────┘                                      │
└─────────────────────────────────────────────────────────────┘
                               │
                               ▼
        ┌──────────────────────────────────────────┐
        │  gcode_preview_renderer.h/.cpp           │
        │  (NEW - Similar to bed_mesh_renderer)   │
        ├──────────────────────────────────────────┤
        │  • gcode_parser (incremental)            │
        │  • move_buffer (filtered array)          │
        │  • projection_engine (2D/2.5D/3D)        │
        │  • line_renderer (Bresenham)             │
        │  • color_mapper (layer/speed/type)       │
        └──────────────────────────────────────────┘
                               │
                               ▼
        ┌──────────────────────────────────────────┐
        │  Moonraker File API                      │
        │  • Stream G-code bytes (chunked)         │
        │  • Use file_metadata.gcode_start_byte    │
        └──────────────────────────────────────────┘
```

### 📁 New Files to Create

```
include/gcode_preview_renderer.h    (~250 lines, similar to bed_mesh_renderer.h)
src/gcode_preview_renderer.cpp      (~800 lines, similar to bed_mesh_renderer.cpp)
include/gcode_parser.h              (~150 lines, simple state machine)
src/gcode_parser.cpp                (~400 lines, G0/G1/G2/G3 parsing)
```

### 🎯 Public API Design

```cpp
// gcode_preview_renderer.h (C-compatible API like bed_mesh_renderer)

typedef struct gcode_preview_renderer gcode_preview_renderer_t;

// Rendering modes
typedef enum {
    GCODE_VIEW_2D_TOP,       // Top-down orthographic (default)
    GCODE_VIEW_2D_SIDE,      // Side view (X-Z plane)
    GCODE_VIEW_3D_ISO,       // Isometric 3D (reuse bed_mesh projection)
} gcode_view_mode_t;

// Color coding schemes
typedef enum {
    GCODE_COLOR_LAYER,       // Purple → Red by Z height
    GCODE_COLOR_FEEDRATE,    // Blue → Yellow by speed
    GCODE_COLOR_TYPE,        // Travel/Perimeter/Infill/Support
    GCODE_COLOR_EXTRUSION,   // Amount of plastic extruded
} gcode_color_scheme_t;

// Create/destroy
gcode_preview_renderer_t* gcode_preview_renderer_create(void);
void gcode_preview_renderer_destroy(gcode_preview_renderer_t* renderer);

// Load G-code data (streaming or full)
bool gcode_preview_renderer_load_file(gcode_preview_renderer_t* renderer,
                                       const char* gcode_path);
bool gcode_preview_renderer_load_stream(gcode_preview_renderer_t* renderer,
                                         const char* gcode_chunk,
                                         size_t chunk_size,
                                         bool is_final_chunk);

// View configuration
void gcode_preview_renderer_set_view_mode(gcode_preview_renderer_t* renderer,
                                           gcode_view_mode_t mode);
void gcode_preview_renderer_set_color_scheme(gcode_preview_renderer_t* renderer,
                                              gcode_color_scheme_t scheme);
void gcode_preview_renderer_set_layer_range(gcode_preview_renderer_t* renderer,
                                             int min_layer, int max_layer);
void gcode_preview_renderer_set_rotation(gcode_preview_renderer_t* renderer,
                                          double angle_x, double angle_z);

// Rendering
bool gcode_preview_renderer_render(gcode_preview_renderer_t* renderer,
                                    lv_obj_t* canvas);

// Statistics (for UI display)
typedef struct {
    int total_moves;
    int visible_moves;
    int total_layers;
    double render_time_ms;
    double min_x, max_x, min_y, max_y, min_z, max_z;
} gcode_preview_stats_t;

const gcode_preview_stats_t* gcode_preview_renderer_get_stats(
    gcode_preview_renderer_t* renderer);
```

### ⚡ Performance Targets

**Based on bed_mesh_renderer performance:**

| Scenario | Moves | Render Time | FPS | Feasibility |
|----------|-------|-------------|-----|-------------|
| Simple part (Benchy) | 10K | 100 ms | 10 FPS | ✅ Easy |
| Medium part | 50K | 500 ms | 2 FPS | ✅ Acceptable (static preview) |
| Complex part | 200K | 2 s | 0.5 FPS | ✅ With layer filtering |
| Huge part | 1M | 10 s | 0.1 FPS | ⚠️ Layer filtering required |

**Optimization Strategy:**
1. **Initial render:** May take 1-2 seconds for complex parts (acceptable for static preview)
2. **Interactive rotation:** Use **downsampled geometry** (every Nth move) for 30 FPS dragging
3. **Layer filtering:** Render only 10-20 layers at a time (like slicer preview)

---

## 6. G-code Parsing Implementation

### 📝 Minimal Parser (G0/G1 Only)

**99% of rendering needs** = linear moves (G0/G1).

```cpp
// gcode_parser.cpp - Simple state machine

struct GCodeState {
    double x, y, z, e;       // Current position + extrusion
    double feedrate;         // Current F value
    int layer;               // Inferred from Z changes
    bool absolute_mode;      // G90 (true) vs G91 (false)
};

bool parse_gcode_line(const char* line, GCodeState* state, GCodeMove* move_out) {
    // Skip comments
    const char* comment = strchr(line, ';');
    if (comment) {
        // Extract layer info from comments like "; LAYER:42"
        if (strncmp(comment, "; LAYER:", 8) == 0) {
            state->layer = atoi(comment + 8);
        }
    }

    // Parse G-code command
    if (strncmp(line, "G0 ", 3) == 0 || strncmp(line, "G1 ", 3) == 0) {
        // Extract X/Y/Z/E/F parameters
        double new_x = state->x, new_y = state->y, new_z = state->z;
        double new_e = state->e, new_f = state->feedrate;

        sscanf(line, "G%*d X%lf Y%lf Z%lf E%lf F%lf",
               &new_x, &new_y, &new_z, &new_e, &new_f);

        // Compute move delta
        move_out->x = new_x;
        move_out->y = new_y;
        move_out->z = new_z;
        move_out->e = new_e - state->e;  // Extrusion delta
        move_out->feedrate = (uint16_t)new_f;
        move_out->layer = state->layer;

        // Classify move type
        if (move_out->e > 0.001) {
            move_out->type = MOVE_EXTRUSION;  // Printing
        } else {
            move_out->type = MOVE_TRAVEL;     // Non-printing move
        }

        // Update state
        state->x = new_x;
        state->y = new_y;
        state->z = new_z;
        state->e = new_e;
        state->feedrate = new_f;

        return true;  // Valid move
    }

    // Handle G90/G91 (absolute/relative mode)
    if (strcmp(line, "G90") == 0) state->absolute_mode = true;
    if (strcmp(line, "G91") == 0) state->absolute_mode = false;

    return false;  // Not a move command
}
```

**Complexity:** ~100 lines for robust G0/G1/G90/G91 parsing.

### 🔄 Optional: Arc Support (G2/G3)

**For curved moves:** Use LVGL's `lv_draw_arc()` or approximate with line segments.

```cpp
// Approximate arc with line segments (simple approach)
void tessellate_arc(double x0, double y0, double x1, double y1,
                    double i, double j, int segments,
                    std::vector<GCodeMove>& moves) {
    // i, j = arc center offset from start point
    double cx = x0 + i;
    double cy = y0 + j;
    double radius = sqrt(i*i + j*j);

    // Compute angles
    double start_angle = atan2(y0 - cy, x0 - cx);
    double end_angle = atan2(y1 - cy, x1 - cx);

    // Generate intermediate points
    for (int seg = 0; seg <= segments; seg++) {
        double t = seg / (double)segments;
        double angle = start_angle + t * (end_angle - start_angle);
        double x = cx + radius * cos(angle);
        double y = cy + radius * sin(angle);

        GCodeMove move;
        move.x = x; move.y = y; move.z = z0;
        move.type = MOVE_EXTRUSION;
        moves.push_back(move);
    }
}
```

**Performance Impact:** Arcs are **rare** in typical G-code (<1% of moves), so not critical.

---

## 7. Projection Modes

### 🔍 Mode 1: 2D Top-Down (DEFAULT)

**Simplest and fastest:**

```cpp
// World to screen (orthographic projection)
int screen_x = (int)((move.x - bounds.min_x) * scale_x);
int screen_y = (int)((move.y - bounds.min_y) * scale_y);

// Auto-fit to canvas
double scale_x = canvas_width / (bounds.max_x - bounds.min_x);
double scale_y = canvas_height / (bounds.max_y - bounds.min_y);
double scale = std::min(scale_x, scale_y) * 0.9;  // 10% padding
```

**Performance:** O(1) per move, no rotation math.

### 📐 Mode 2: 2.5D Isometric

**Add depth perception without full 3D:**

```cpp
// Isometric projection (30° tilt)
int screen_x = (int)((move.x - move.y) * cos(30° * PI/180) * scale);
int screen_y = (int)(((move.x + move.y) * 0.5 - move.z) * scale);
```

**Performance:** O(1) per move, simple trig (can precompute cos/sin).

### 🌐 Mode 3: Full 3D Perspective (REUSE bed_mesh_renderer)

**Copy projection logic** from `bed_mesh_renderer.cpp:356-390`:

```cpp
// Reuse existing perspective projection
bed_mesh_point_3d_t project_3d_to_2d(double x, double y, double z,
                                      int canvas_width, int canvas_height,
                                      const gcode_view_state_t* view) {
    // EXACT SAME CODE as bed_mesh_renderer
    // 1. Z-axis rotation
    // 2. X-axis rotation
    // 3. Camera translation
    // 4. Perspective divide
    // 5. Screen mapping
}
```

**Performance:** O(1) per move, ~10 floating-point ops (acceptable).

**Depth Sorting:** For 3D mode, sort moves by depth (painter's algorithm) like bed mesh quads.

---

## 8. Rendering Pipeline

### 🎬 Full Rendering Flow

```cpp
bool gcode_preview_renderer_render(gcode_preview_renderer_t* renderer,
                                    lv_obj_t* canvas) {
    // 1. Clear canvas (like bed_mesh_renderer.cpp:251)
    lv_canvas_fill_bg(canvas, lv_color_make(40, 40, 40), LV_OPA_COVER);

    // 2. Get canvas dimensions
    int canvas_width = lv_obj_get_width(canvas);
    int canvas_height = lv_obj_get_height(canvas);

    // 3. Compute projection parameters (auto-fit to canvas)
    double scale = compute_auto_scale(renderer->bounds, canvas_width, canvas_height);

    // 4. Project all moves to screen space
    for (auto& move : renderer->moves) {
        move.screen_x = world_to_screen_x(move.x, move.y, move.z, scale, view_mode);
        move.screen_y = world_to_screen_y(move.x, move.y, move.z, scale, view_mode);
    }

    // 5. (Optional) Sort by depth for 3D mode
    if (renderer->view_mode == GCODE_VIEW_3D_ISO) {
        std::sort(renderer->moves.begin(), renderer->moves.end(),
                  [](const GCodeMove& a, const GCodeMove& b) {
                      return a.z > b.z;  // Back-to-front
                  });
    }

    // 6. Render lines
    int prev_x = -1, prev_y = -1;
    for (const auto& move : renderer->moves) {
        // Skip first move (no line to draw)
        if (prev_x < 0) {
            prev_x = move.screen_x;
            prev_y = move.screen_y;
            continue;
        }

        // Viewport culling
        if (!is_line_visible(prev_x, prev_y, move.screen_x, move.screen_y,
                            canvas_width, canvas_height)) {
            prev_x = move.screen_x;
            prev_y = move.screen_y;
            continue;
        }

        // Color coding
        lv_color_t color = compute_move_color(move, renderer->color_scheme,
                                              renderer->bounds);

        // Draw line (either Bresenham or LVGL)
        if (renderer->use_custom_line_renderer) {
            draw_line_bresenham(canvas, prev_x, prev_y,
                               move.screen_x, move.screen_y, color);
        } else {
            lv_draw_line(canvas, prev_x, prev_y,
                        move.screen_x, move.screen_y, color);
        }

        prev_x = move.screen_x;
        prev_y = move.screen_y;
    }

    // 7. Invalidate canvas for LVGL redraw (like bed_mesh_renderer.cpp:298)
    lv_obj_invalidate(canvas);

    return true;
}
```

### 🚀 Performance Optimizations

**During Initial Render (static preview):**
- Parse entire file or layer range
- Render all moves with full quality
- Acceptable time: 1-2 seconds for complex parts

**During Interactive Rotation (if 3D mode):**
- Use **downsampled geometry** (every 10th move)
- Switch to solid colors (no gradients)
- Target: 30 FPS (like bed_mesh dragging mode)

```cpp
// Adaptive downsampling (like bed_mesh_renderer dragging)
int skip_factor = renderer->is_dragging ? 10 : 1;
for (size_t i = 0; i < renderer->moves.size(); i += skip_factor) {
    // Render move[i]
}
```

---

## 9. Integration with Existing UI

### 🔗 Modify `ui_panel_print_select.cpp`

**Minimal changes required:**

```cpp
// In file_detail_opened() callback (line ~400)
static void file_detail_opened(lv_event_t* e) {
    // ... existing code to show detail overlay ...

    // NEW: Initialize G-code preview renderer
    if (!gcode_renderer) {
        gcode_renderer = gcode_preview_renderer_create();
    }

    // Load G-code file (async or cached)
    std::string gcode_path = "/path/to/gcodes/" + selected_file.filename;
    gcode_preview_renderer_load_file(gcode_renderer, gcode_path.c_str());

    // Set default view
    gcode_preview_renderer_set_view_mode(gcode_renderer, GCODE_VIEW_2D_TOP);
    gcode_preview_renderer_set_color_scheme(gcode_renderer, GCODE_COLOR_LAYER);

    // Render to canvas widget (created in print_file_detail.xml)
    lv_obj_t* preview_canvas = lv_obj_find_by_name(detail_overlay, "gcode_preview");
    gcode_preview_renderer_render(gcode_renderer, preview_canvas);
}
```

### 📄 Modify `ui_xml/print_file_detail.xml`

**Add canvas widget:**

```xml
<lv_obj name="detail_overlay" ...>
    <!-- Existing: filename, metadata, buttons -->

    <!-- NEW: G-code preview canvas -->
    <lv_canvas name="gcode_preview"
               width="300" height="200"
               align="center"
               style_bg_color="#282828">
        <!-- Canvas buffer allocated in C++ init -->
    </lv_canvas>

    <!-- NEW: View controls -->
    <lv_dropdown name="view_mode_dropdown"
                 options="2D Top\n2D Side\n3D Iso"
                 align="top_right" x="-10" y="10"/>

    <lv_dropdown name="color_scheme_dropdown"
                 options="Layer\nSpeed\nType"
                 align="top_right" x="-10" y="50"/>
</lv_obj>
```

**Canvas buffer allocation (in C++ init):**

```cpp
// Allocate static buffer for canvas (RGB565 format)
static lv_color_t gcode_canvas_buffer[300 * 200];

void init_gcode_preview_canvas(lv_obj_t* canvas) {
    lv_canvas_set_buffer(canvas, gcode_canvas_buffer,
                        300, 200, LV_COLOR_FORMAT_RGB565);
}
```

---

## 10. Advanced Features (Future Enhancements)

### 🎨 Feature Roadmap

| Feature | Complexity | Value | Priority |
|---------|-----------|-------|----------|
| **Basic 2D top-down** | Low | High | ✅ MVP |
| **Layer filtering** | Low | High | ✅ MVP |
| **Color by speed/type** | Low | Medium | ✅ MVP |
| **3D isometric view** | Medium | Medium | 🔄 Phase 2 |
| **Interactive rotation** | Medium | Low | 🔄 Phase 2 |
| **Arc support (G2/G3)** | Medium | Low | ⏸️ Nice-to-have |
| **Zoom/pan controls** | Medium | Medium | 🔄 Phase 2 |
| **Layer animation** | High | Low | ⏸️ Nice-to-have |
| **Extrusion width** | High | Low | ⏸️ Nice-to-have |

### 💾 Caching Strategy

**Problem:** Re-parsing G-code every time detail view opens is slow.

**Solution:** In-memory cache with LRU eviction:

```cpp
// Cache parsed geometry (keyed by filename + mtime)
struct GCodeCache {
    std::string filename;
    time_t file_mtime;
    std::vector<GCodeMove> moves;
    gcode_bounds_t bounds;
};

static std::map<std::string, GCodeCache> gcode_cache;
static const size_t MAX_CACHE_SIZE = 5;  // Cache last 5 files

// On file open:
auto it = gcode_cache.find(filename);
if (it != gcode_cache.end() && it->second.file_mtime == file_mtime) {
    // Use cached data
    renderer->moves = it->second.moves;
    renderer->bounds = it->second.bounds;
} else {
    // Parse and cache
    parse_gcode_file(filename, renderer);
    gcode_cache[filename] = {filename, file_mtime, renderer->moves, renderer->bounds};

    // LRU eviction if cache too large
    if (gcode_cache.size() > MAX_CACHE_SIZE) {
        gcode_cache.erase(gcode_cache.begin());  // Evict oldest
    }
}
```

**Memory Impact:** ~200 KB per cached file × 5 = **1 MB max** (acceptable).

---

## 11. Comparison: Our Approach vs. Alternatives

| Approach | OpenGL Required | Complexity | Performance | Memory | Verdict |
|----------|----------------|-----------|-------------|--------|---------|
| **Port PrusaSlicer** | ✅ YES | Very High | Excellent (GPU) | High | ❌ NOT FEASIBLE |
| **Port OrcaSlicer** | ✅ YES | Very High | Excellent (GPU) | High | ❌ NOT FEASIBLE |
| **Web-based viewer (iframe)** | ❌ No | Low | Medium (WebGL) | Medium | ⚠️ Requires browser |
| **Offload to server** | ❌ No | Medium | Excellent (server-side) | Low (client) | ⚠️ Network latency |
| **Custom software renderer** | ❌ No | Medium | Good (CPU) | Medium | ✅ **RECOMMENDED** |

### 🏆 Why Custom Software Rendering Wins

**Pros:**
- ✅ No OpenGL dependency (pure CPU)
- ✅ Full control over optimization (Bresenham, culling, downsampling)
- ✅ Reuses existing infrastructure (bed_mesh_renderer patterns)
- ✅ Moderate complexity (~1500 LOC total)
- ✅ Acceptable performance (1-2s render for complex parts)
- ✅ Memory efficient (streaming parser + caching)

**Cons:**
- ⚠️ Slower than GPU rendering (but acceptable for static preview)
- ⚠️ Requires custom implementation (but we have excellent foundation)

---

## 12. Implementation Estimate

### 📅 Development Timeline

| Phase | Tasks | LOC | Effort | Risk |
|-------|-------|-----|--------|------|
| **Phase 1: Core Parser** | G0/G1 parsing, state machine | ~400 | 2-3 days | Low |
| **Phase 2: 2D Renderer** | Bresenham lines, 2D projection, color mapping | ~500 | 3-4 days | Low |
| **Phase 3: UI Integration** | Canvas widget, file loading, caching | ~300 | 2-3 days | Medium |
| **Phase 4: Testing** | Various G-code files, performance tuning | ~200 | 2-3 days | Low |
| **Phase 5: 3D Mode** | Perspective projection, rotation controls | ~600 | 4-5 days | Medium |
| **TOTAL** | | **~2000 LOC** | **13-18 days** | **Medium** |

**MVP (Phases 1-4):** 9-13 days for 2D top-down preview with layer filtering.

### 🧪 Testing Strategy

**Test Files:**
1. Simple cube (10K moves) - Fast render validation
2. Benchy (50K moves) - Medium complexity
3. Complex part (200K+ moves) - Stress test, layer filtering
4. Multi-material (color changes) - Parser robustness

**Performance Targets:**
- ✅ 10K moves: <100 ms render
- ✅ 50K moves: <500 ms render
- ✅ 200K moves (filtered): <2s render

---

## 13. Conclusion & Recommendations

### 🎯 Final Verdict

**DO NOT port PrusaSlicer/OrcaSlicer** - Heavy OpenGL dependencies make it infeasible.

**DO implement custom software renderer** using:
1. ✅ **Bresenham line algorithm** (integer-only, very fast)
2. ✅ **Incremental G-code parser** (memory efficient)
3. ✅ **LVGL canvas** (with strategic optimizations)
4. ✅ **Reuse bed_mesh_renderer techniques** (projection, color mapping, depth sorting)

### 📋 Recommended Action Plan

**Immediate Next Steps:**

1. **Prototype Phase (Week 1):**
   - Create `gcode_parser.cpp` with basic G0/G1 parsing
   - Implement Bresenham line drawing
   - Test with simple G-code file (1000 moves)
   - Measure performance

2. **MVP Phase (Weeks 2-3):**
   - Complete `gcode_preview_renderer.cpp` with 2D top-down view
   - Add layer filtering and color schemes
   - Integrate into `print_file_detail.xml`
   - Test with real print files

3. **Polish Phase (Week 4):**
   - Add caching for parsed G-code
   - Optimize viewport culling
   - Add zoom/pan controls
   - Performance tuning

4. **Advanced Phase (Future):**
   - 3D isometric/perspective modes
   - Interactive rotation
   - Arc support (G2/G3)

### 🚀 Success Criteria

**MVP Success:**
- ✅ Render 50K move G-code in <500ms
- ✅ Layer filtering works smoothly
- ✅ Color coding clearly shows structure
- ✅ No OpenGL dependency
- ✅ <2 MB total memory footprint

**Long-term Success:**
- ✅ 200K+ move files render with layer filtering
- ✅ Interactive 3D rotation at 30 FPS
- ✅ User-friendly zoom/pan controls
- ✅ Cached previews load instantly

---

## 14. References & Resources

### 📚 Algorithms

- **Bresenham Line Algorithm:** [Wikipedia](https://en.wikipedia.org/wiki/Bresenham's_line_algorithm)
- **Cohen-Sutherland Clipping:** [GeeksforGeeks](https://www.geeksforgeeks.org/line-clipping-set-1-cohen-sutherland-algorithm/)
- **Painter's Algorithm:** Used in `bed_mesh_renderer.cpp:693-699`

### 🔧 LVGL Documentation

- **Canvas Widget:** [LVGL v9.4 Canvas Docs](https://docs.lvgl.io/master/details/widgets/canvas.html)
- **Draw Pipeline:** [LVGL v9.4 Draw Docs](https://docs.lvgl.io/master/details/main-modules/draw/draw_pipeline.html)

### 📁 Existing Helixscreen Code

- `bed_mesh_renderer.cpp/.h` - 3D rendering reference
- `ui_panel_print_select.cpp` - File browser integration point
- `moonraker_api.h` - File metadata structure (line 76-94)

### 🛠️ G-code References

- **RepRap G-code Wiki:** [reprap.org/wiki/G-code](https://reprap.org/wiki/G-code)
- **Marlin G-code Reference:** [marlinfw.org/meta/gcode/](https://marlinfw.org/meta/gcode/)

---

**Document Status:** Ready for implementation
**Next Step:** Create prototype G-code parser and Bresenham line renderer
