# Bed Mesh Rendering Internals

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Architecture Overview](#architecture-overview)
3. [Rendering Pipeline](#rendering-pipeline)
4. [Debugging & Profiling](#debugging--profiling)
5. [Constants & Magic Numbers](#constants--magic-numbers)

---

## Executive Summary

The bed mesh rendering system is a **complete 3D graphics pipeline** implemented in pure C++ with no GPU acceleration. It renders interactive 3D bed mesh visualizations into an off-screen pixel buffer on a render thread, so the main thread only blits finished frames.

### Design Philosophy

1. **Software-only rendering:** No GPU dependencies (works on framebuffer displays)
2. **Reactive data binding:** Mesh updates trigger automatic re-rendering
3. **Touch interactivity:** Drag to rotate; pinch to zoom and two-finger drag to pan (screen-space magnify, see Projection)
4. **Declarative XML:** UI layout separate from rendering logic

---

## Architecture Overview

### Three-Layer Architecture

```
┌─────────────────────────────────────────────┐
│   ui_panel_bed_mesh.cpp (379 lines)        │
│   - Data binding to Moonraker subjects     │
│   - Panel lifecycle management             │
│   - Subject-Observer pattern               │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│   ui_bed_mesh.cpp (443 lines)              │
│   - LVGL widget wrapper                    │
│   - Touch event handling (drag rotation)   │
│   - DRAW_POST: blit frame, draw text       │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│   bed_mesh_renderer.cpp (1,639 lines)      │
│   - 3D projection & transformation         │
│   - Rasterization (triangle fill)          │
│   - Depth sorting (painter's algorithm)    │
│   - Overlay rendering (grid, axes, labels) │
└─────────────────────────────────────────────┘
```

### One Render Path: Off-Screen Frames

Every view, the 3D surface and the 2D heatmap alike, is rendered by
`src/rendering/bed_mesh_renderer.cpp#bed_mesh_renderer_render_to_buffer` into a
`helix::mesh::PixelBuffer` on `BedMeshRenderThread`. That function makes no LVGL calls.
Two buffers exist while rendering is on; they move between the render thread and the
widget by pointer swap (`BedMeshRenderThread::acquire_frame`), so a draw with no new frame
copies nothing. `src/ui/calibration/ui_bed_mesh.cpp#bed_mesh_draw_cb` blits the newest frame with
`lv_draw_image()`, then draws what needs LVGL's font engine on the main thread, holding
the render mutex: axis letters and tick labels in 3D (`render_axis_labels`,
`render_numeric_axis_ticks`), the border, touched-cell highlight and Z tooltip in 2D
(`render_heatmap_overlay`). The panel turns rendering off while hidden
(`ui_bed_mesh_set_async_mode(false)`), which frees both buffers; with rendering off the
widget draws nothing unless static mode is on ([below](#static-mode-one-frame-no-thread)).

The reference-grid extent (bed bounds once `bed_mesh_renderer_set_bounds` has run, the mesh
index grid otherwise) and the wall floor/ceiling come from one function,
`src/rendering/bed_mesh_overlays.cpp#compute_bed_extent`, shared by the FOV fit, the grids
and the labels, so labels cannot drift off the walls.

### Static mode: one frame, no thread

`ui_bed_mesh_set_static_mode(widget, true)` (`include/ui_bed_mesh.h`) makes a `<bed_mesh>`
widget draw a non-interactive picture without a `BedMeshRenderThread`. It exists for the home
bed-mesh tile, which must not hold two frame buffers and a thread for a thumbnail.

- **Render on change, blit otherwise.** The draw callback calls `refresh_static_frame()`
  (`src/ui/calibration/ui_bed_mesh.cpp#refresh_static_frame`), which renders on the main thread into one
  `PixelBuffer` through the same `bed_mesh_renderer_render_to_buffer` only when something marked
  it dirty: new mesh data, bounds, a render mode change (`ui_bed_mesh_redraw()` and
  `ui_bed_mesh_set_render_mode()` both mark it), a different widget size, or a new theme
  generation, since the frame bakes the theme colours in. Every other draw blits the cached
  frame, and the 2D overlay (`render_heatmap_overlay`) is drawn over it as in async mode.
- **A running render thread takes precedence.** The static path is reached only when
  `data->render_thread` is null, so the Bed Mesh panel's async mode and a static widget do not
  contend. Turning static mode off frees the cached frame.
- **The 3D fallback is sticky.** A 3D frame (`!info.heatmap`) that takes longer than the
  widget's budget (`kDefaultStaticRenderBudgetMs`, 150 ms) sets `static_3d_banned`, forces the
  renderer to `Force2D` and re-renders the 2D heatmap in the same call. After that
  `ui_bed_mesh_set_render_mode()` rewrites any request other than `Force2D` to `Force2D`, so a
  slow board pays for one 3D frame and never a second. The ban belongs to that widget instance.
  `ui_bed_mesh_static_3d_banned()` reports it, `ui_bed_mesh_static_render_ms()` and
  `ui_bed_mesh_static_render_count()` report the last cost and the number of renders (the
  count is how a test proves an unchanged mesh costs no render), and
  `ui_bed_mesh_set_static_render_budget_ms_for_test()` shrinks the budget.
- **The consumer asks for 3D only where it can afford it.** `BedMeshTileWidget` turns static
  mode on in `attach()` and requests `Force3D` only at the Large rung on the standard platform
  tier; every other rung, and any widget whose canvas reports the ban, requests `Force2D`
  (`src/ui/panel_widgets/bed_mesh_tile_widget.cpp#feed_map`). It feeds the canvas again only
  when `bed_mesh_version` changed, and mesh updates wait while the home panel is covered.

### Mesh state and statistics

`helix::BedMeshState` (`include/bed_mesh_state.h`) is the active bed mesh as the rest of the UI
reads it. It exists from connect (`PrinterSession` attaches it to the API, so the Bed Mesh panel
never has to have been opened) and publishes loaded or not, profile name, grid size, extremes,
range and profile count as subjects, plus `bed_mesh_version`, which bumps on every applied
update. `helix::compute_mesh_stats()` (`include/bed_mesh_stats.h`) is the one pure function that
turns a parsed `BedMeshProfile` into a `MeshStats` (min, max and mean Z, range, grid size, and the
printer coordinates of the extremes). `BedMeshState::apply()` publishes it, `BedMeshPanel`
normalises the matrix it hands the renderer with the same mean, and the home tile classifies the
range with `helix::status::mesh_flatness_status()` (`include/value_status.h`), so the figure the
panel shows and the colour the tile draws cannot disagree.

### Coordinate Space Transformations

The renderer uses **4 distinct coordinate spaces** (see `include/bed_mesh_renderer.h`):

```
MESH SPACE (input)
  Row/col indices, Z heights in mm
  mesh[row][col] = z_height

         ↓ mesh_*_to_world_*()

WORLD SPACE (3D scene)
  Centered coordinates, scaled by BED_MESH_SCALE (50.0)
  x: [-width/2, +width/2]
  y: [-height/2, +height/2]
  z: centered & scaled by z_scale

         ↓ Rotation (angle_x, angle_z)

CAMERA SPACE (after rotation)
  Rotated by tilt (X-axis) and spin (Z-axis)
  Translated by camera distance

         ↓ Perspective projection

SCREEN SPACE (final output)
  2D pixel coordinates
  screen_x, screen_y with centering offsets
```

### Data Structures

#### Mesh Data Storage

```cpp
struct bed_mesh_renderer {
    // Input mesh data (copied from Moonraker)
    std::vector<std::vector<double>> mesh;  // [row][col] → Z height
    int rows, cols;
    double mesh_min_z, mesh_max_z;  // Bounds for color mapping

    // Generated geometry (computed once, reused)
    std::vector<bed_mesh_quad_3d_t> quads;  // One quad per mesh cell

    // Cached projections (updated every frame during rotation)
    std::vector<std::vector<int>> projected_screen_x;  // SOA layout
    std::vector<std::vector<int>> projected_screen_y;

    // View state
    bed_mesh_view_state_t view_state;  // Rotation, kFov, centering
};
```

#### Quad Geometry

```cpp
/**
 * Quad vertex layout (view from above, looking down -Z axis):
 *
 *   mesh[row][col]         mesh[row][col+1]
 *        [2]TL ──────────────── [3]TR
 *         │                      │
 *         │       QUAD           │     ← One mesh cell
 *         │     (row,col)        │
 *         │                      │
 *        [0]BL ──────────────── [1]BR
 *   mesh[row+1][col]       mesh[row+1][col+1]
 *
 * Vertex indices: [0]=BL, [1]=BR, [2]=TL, [3]=TR
 *
 * Split into triangles for rasterization:
 *   Triangle 1: [0]→[1]→[2] (BL→BR→TL, lower-right triangle)
 *   Triangle 2: [1]→[3]→[2] (BR→TR→TL, upper-left triangle)
 */

struct bed_mesh_quad_3d_t {
    bed_mesh_vertex_3d_t vertices[4];  // World-space positions + colors
    int screen_x[4];                   // Cached screen coordinates
    int screen_y[4];
    double depths[4];                  // Cached Z-depths for sorting
    double avg_depth;                  // Average depth (sort key)
    lv_color_t center_color;          // Fallback solid color
};
```

---

## Rendering Pipeline

### High-Level Flow

```
1. Mesh Data Input
   ↓
   BedMeshProfile (JSON from Klipper, defined in moonraker_domain_service.h)

2. Data Conversion & Storage
   ↓
   std::vector<std::vector<float>> → bed_mesh_renderer::mesh

3. Geometry Generation (ONCE per mesh load)
   ↓
   generate_mesh_quads() → quads with world positions + colors

4. Per-Frame Rendering (30-60 FPS)
   ↓
   ┌─────────────────────────────────────────┐
   │ a) Update view state (rotation, kFov)    │
   │ b) Project vertices → screen coords     │ ← 0.01ms (<1%)
   │ c) Sort quads by depth (painter's alg)  │ ← 0.02ms (<1%)
   │ d) Rasterize quads (fill triangles)     │ ← 45-75ms (94-99%)
   │ e) Render overlays (grid, axes, labels) │ ← 0.5-1ms (1-7%)
   └─────────────────────────────────────────┘

5. Output to a PixelBuffer, blitted by the widget's DRAW_POST callback
```

### Detailed Pipeline Stages

#### Stage 1: Geometry Generation (One-Time)

**Function:** `generate_mesh_quads()`
**Frequency:** Only when mesh data/z_scale/color_range changes
**Complexity:** O(rows × cols)

```cpp
// For each mesh cell (row, col):
1. Compute world-space positions for 4 corners
   - mesh_col_to_world_x(col) → centered X coordinate
   - mesh_row_to_world_y(row) → centered Y coordinate (inverted)
   - mesh_z_to_world_z(z_height) → centered & scaled Z

2. Assign vertex colors based on Z height
   - height_to_color(z) → maps to gradient LUT

3. Pre-compute center color for solid fill mode
   - avg_color = average of 4 vertex colors

4. Store quad in vector (pre-allocated capacity)
```

**Optimization:** Pre-allocate vector capacity to avoid reallocations
```cpp
int expected_quads = (rows - 1) * (cols - 1);
renderer->quads.reserve(expected_quads);  // Avoids ~9 reallocations
```

#### Stage 2: Projection (Every Frame)

**Function:** `project_and_cache_quads()`
**Frequency:** Every frame during rotation
**Complexity:** O(quads × 4) = O(rows × cols × 4)

```cpp
// For each quad (4 vertices):
1. Extract world-space position (x, y, z)
2. Apply Z-axis rotation (spin around vertical)
   rotated_x = x * cos(angle_z) - y * sin(angle_z)
   rotated_y = x * sin(angle_z) + y * cos(angle_z)

3. Apply X-axis rotation (tilt up/down)
   final_x = rotated_x
   final_y = rotated_y * cos(angle_x) + z * sin(angle_x)
   final_z = rotated_y * sin(angle_x) - z * cos(angle_x)

4. Translate camera back
   final_z += BED_MESH_CAMERA_DISTANCE

5. Perspective projection (similar triangles), then the two-finger magnify
   screen_x = ((final_x * fov_scale) / final_z) * zoom + pan_x
   screen_y = ((final_y * fov_scale) / final_z) * zoom + pan_y

6. Convert to pixel coordinates (centered in canvas)
   pixel_x = canvas_width/2 + screen_x + center_offset_x
   pixel_y = canvas_height * 0.5 + screen_y + center_offset_y

7. Cache results in quad.screen_x[], quad.screen_y[], quad.depths[]
```

**Two-finger zoom and pan** apply in screen space, after the perspective divide
(`view_state.zoom` / `pan_x` / `pan_y`;
`src/rendering/bed_mesh_projection.cpp#bed_mesh_projection_zoom_at` and
`src/rendering/bed_mesh_projection.cpp#bed_mesh_projection_pan`, driven by
`src/rendering/bed_mesh_renderer.cpp#bed_mesh_renderer_apply_two_finger`).
Zoom is clamped to `BED_MESH_ZOOM_MIN`..`BED_MESH_ZOOM_MAX` (1.0-8.0); pan is a no-op at
the minimum, and reaching it resets pan to zero, so pinching fully out returns the fitted
view. `bed_mesh_renderer_set_bounds` and `bed_mesh_renderer_set_render_mode` also reset
zoom and pan via `bed_mesh_projection_reset_zoom` - bounds first, because the auto-fit
that follows projects through the zoomed function.

**Optimization:** Cache trigonometric values (computed once per frame)
```cpp
view_state->cached_cos_x = std::cos(x_angle_rad);
view_state->cached_sin_x = std::sin(x_angle_rad);
// Saves 1,444 trig calls per frame (20×20 mesh)
```

#### Stage 3: Depth Sorting (Every Frame)

**Function:** `sort_quads_by_depth()`
**Frequency:** Every frame
**Complexity:** O(n log n) where n = number of quads

```cpp
std::sort(quads.begin(), quads.end(), [](const quad& a, const quad& b) {
    return a.avg_depth > b.avg_depth;  // Descending: furthest first
});
```

**Algorithm:** Painter's algorithm (render back-to-front)
- Furthest quads drawn first
- Closer quads overdraw (automatic occlusion)
- No Z-buffer required

**Performance:** ~0.02ms for 361 quads (negligible overhead)

#### Stage 4: Rasterization (Every Frame)

**Function:** `helix::mesh::fill_triangle_gradient()` (`src/rendering/bed_mesh_rasterizer.cpp`) → `PixelBuffer::fill_triangle_gradient()` (`src/rendering/bed_mesh_buffer.cpp`)
**Frequency:** Every frame, for each quad (two triangles per quad)
**Complexity:** O(triangles × scanlines × gradient_segments)

**Triangle Scanline Fill Algorithm:**

```
     v[0] (top, y1)
       /\
      /  \        ← Long edge (v[0] → v[2])
     /    \
  v[1]────\      ← Split at middle vertex
    |      \     ← Short edges:
    |       \      - v[0]→v[1] (upper)
  v[2] (bottom, y3) - v[1]→v[2] (lower)

For each scanline Y from y1 to y3:
  1. Interpolate X and color along the long edge (v[0] → v[2])
  2. Interpolate X and color along the short edge
  3. Fill the horizontal span straight into the PixelBuffer:
     a) narrower than kGradientMinLineWidth (3 px): one average color
     b) otherwise: an adaptive number of segments (thin / medium / wide
        thresholds), each filled with the color sampled at its center
```

The fill writes pixels directly (`fill_hline`); there are no LVGL draw calls on
this path, which is what lets it run off the main thread.

#### Stage 5: Overlay Rendering (Every Frame)

**Functions:** `render_grid_lines()` (into the buffer), `render_axis_labels()` and
`render_numeric_axis_ticks()` (LVGL, main thread)
**Frequency:** Every frame
**Complexity:** O(rows × cols) for grid lines

**Grid Lines:**
```cpp
// Uses cached projected screen coordinates (SOA arrays)
const auto& screen_x = renderer->projected_screen_x;
const auto& screen_y = renderer->projected_screen_y;

// Draw horizontal lines (connect points in same row)
for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols - 1; col++) {
        lv_draw_line(layer,
            {screen_x[row][col], screen_y[row][col]},
            {screen_x[row][col+1], screen_y[row][col+1]});
    }
}
// Similarly for vertical lines
```

**Performance:** 0.5-1ms (well-optimized, uses cached projections)

---

## Debugging & Profiling

### Enable Performance Logging

Run with `-vvv` (trace level) to see per-frame timings:

```bash
# Boot mock, then bring up the bed-mesh overlay (see docs/devel/HELIXCTL.md)
./build/bin/helix-screen --test -vvv 2>&1 | grep "\[PERF\]" &
./build/bin/helix-screen ctl navigate controls; ./build/bin/helix-screen ctl click btn_bed_mesh
```

**Output** (`src/rendering/bed_mesh_renderer.cpp#bed_mesh_renderer_render_to_buffer`):
```
[Bed Mesh Renderer] [PERF] Buffer render: {total}ms | Prepare: {X}ms | Surface: {X}ms | Decorations: {X}ms | FPS: {avg}
```

### Common Issues

**Frame drops during rotation**
- **Cause:** Unnecessary recomputation (z_scale, color_range) or quads regenerated every frame
- **Check:** quads come from `helix::mesh::generate_mesh_quads()` (`src/rendering/bed_mesh_geometry.cpp`), which should run only on a data or scale change

### Profiling Tools

**1. Built-in instrumentation:** the trace line above.

**2. macOS Instruments**
```bash
# Profile CPU usage - bring up the bed-mesh overlay via helix-screen ctl after launch
instruments -t "Time Profiler" ./build/bin/helix-screen --test &
./build/bin/helix-screen ctl navigate controls; ./build/bin/helix-screen ctl click btn_bed_mesh
```

**3. Valgrind (Linux)**
```bash
# Check for memory leaks - bring up the bed-mesh overlay via helix-screen ctl after launch
valgrind --leak-check=full ./build/bin/helix-screen --test &
./build/bin/helix-screen ctl navigate controls; ./build/bin/helix-screen ctl click btn_bed_mesh
```

**4. gprof (GCC)**
```bash
# Compile with profiling
make CXXFLAGS="-pg" clean build
./build/bin/helix-screen --test &
./build/bin/helix-screen ctl navigate controls; ./build/bin/helix-screen ctl click btn_bed_mesh
gprof build/bin/helix-screen gmon.out > profile.txt
```

---

## Constants & Magic Numbers

### Rendering Constants

```cpp
// World space scaling
#define BED_MESH_SCALE 50.0
// Distance between mesh points in world units
// Larger = more spacing between points in 3D scene

// Two-finger magnify range; 1.0 is the fitted view
#define BED_MESH_ZOOM_MIN 1.0
#define BED_MESH_ZOOM_MAX 8.0

// Z-axis scaling
#define BED_MESH_DEFAULT_Z_SCALE 60.0
// Default vertical exaggeration for mesh heights
// Computed dynamically to fit mesh in reasonable height

#define BED_MESH_MIN_Z_SCALE 35.0
#define BED_MESH_MAX_Z_SCALE 120.0
// Limits for Z-scale to prevent extreme flatness or spikiness

#define BED_MESH_DEFAULT_Z_TARGET_HEIGHT 80.0
// Target height for mesh in world units (used in dynamic scaling)

// Color mapping
#define BED_MESH_COLOR_COMPRESSION 0.8
// Use 80% of data range for color mapping (avoids extreme colors)

// Rendering quality: the span fill uses kGradientMinLineWidth (3 px) and the
// thin/medium/wide segment counts in src/rendering/bed_mesh_buffer.cpp. The
// BED_MESH_GRADIENT_* macros in bed_mesh_renderer.h are not read by it.

// Positioning
#define BED_MESH_Z_ORIGIN_VERTICAL_POS 0.5
// Canvas Y position for Z=0 plane (0=top, 0.5=center, 1=bottom)

// Color gradient LUT
#define kColorGradientLutSize 1024
// Number of pre-computed color samples in gradient lookup table
// Higher = more precision, but more memory (1024 = 4 KB)
```

### Default Rotation Angles

The renderer owns the camera orientation (`bed_mesh_view_state_t::angle_x` /
`angle_z`). The `<bed_mesh>` widget does not keep its own copy - the drag
handler reads the current angles through `bed_mesh_renderer_get_view_state()`,
adds the pixel delta, and writes back via `bed_mesh_renderer_set_rotation()`,
which clamps the tilt and wraps the spin.

```cpp
// bed_mesh_renderer.h
#define BED_MESH_DEFAULT_ANGLE_X (-25.0) // Tilt: 25 degrees down from horizontal
#define BED_MESH_DEFAULT_ANGLE_Z (-45.0) // Spin: 45 degrees clockwise from above

#define BED_MESH_ANGLE_X_MIN (-89.0)     // Near top-down
#define BED_MESH_ANGLE_X_MAX (0.0)       // Edge-on
```

---

## References

### Related Documentation

- **ARCHITECTURE.md** - Overall system design patterns
- **DEVELOPER_QUICK_REFERENCE.md** - Common code patterns and examples
- **LVGL9_XML_GUIDE.md** - XML-based UI layout system
- **TESTING.md** - Test infrastructure and Catch2 usage

### Code Files

- `src/rendering/bed_mesh_renderer.cpp` - Core 3D rendering engine
- `src/ui/calibration/ui_bed_mesh.cpp` - LVGL widget wrapper
- `src/ui/calibration/ui_panel_bed_mesh.cpp` - UI integration and data binding
- `include/bed_mesh_renderer.h` - Public API and data structures

### External Resources

- LVGL Documentation: https://docs.lvgl.io/
- Klipper Bed Mesh Guide: https://www.klipper3d.org/Bed_Mesh.html
- Painter's Algorithm: https://en.wikipedia.org/wiki/Painter%27s_algorithm
- Scanline Rendering: https://en.wikipedia.org/wiki/Scanline_rendering

