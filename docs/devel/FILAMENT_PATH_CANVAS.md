# Filament Path Canvas Rendering

**Version:** 1.0
**Last Updated:** 2026-06-13
**Status:** Shipped (`feature/filament-path-redesign`, merge `d52258fe7`, 2026-06-09)

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Architecture Overview](#architecture-overview)
3. [The Three-Layer Model](#the-three-layer-model)
4. [The Geometry Module](#the-geometry-module)
5. [The Shared Tube Stroker](#the-shared-tube-stroker)
6. [RenderCtx & Phase Decomposition](#renderctx--phase-decomposition)
7. [Topology Renderers](#topology-renderers)
8. [Cache & Invalidation](#cache--invalidation)
9. [Animation Overlay](#animation-overlay)
10. [Extending the Renderer](#extending-the-renderer)
11. [Testing](#testing)
12. [File Map](#file-map)

---

## Executive Summary

The filament-path canvas is the AMS detail-panel visualization: it draws the
physical route filament takes from each spool slot, through prep sensors, an
optional hub/selector, the output (bowden) tube, the toolhead sensor, and into
the nozzle — colored to reflect what filament is loaded and animated to show
flow and heat during loads/unloads.

The redesign replaced a ~3500-line monolithic draw callback with a layered,
phase-decomposed renderer built on two new reusable modules: a **pure geometry
library** (`pathgeo`) that produces arc-filleted filament paths with no LVGL
dependency, and a **tube stroker** that renders those paths as concentric tube
strokes. The same tube stroker drives both this detail canvas and the AMS
overview canvas.

### Key Design Goals

1. **Per-frame animation never repaints tube geometry.** Topology is cached on
   `lv_canvas` children; only flow dots / heat glow / a moving filament tip are
   redrawn every frame, on a cheap `DRAW_POST` pass.
2. **Geometry is pure and testable.** All path math lives in
   `helix::ui::pathgeo`, float-based, LVGL-free, exercised by headless unit
   tests.
3. **One stroker, two canvases.** The detail canvas and the system overview
   canvas share the tube-rendering implementation — no duplicated lane drawing.
4. **Hit-testing reads what was drawn.** The render pass records the exact
   boxes it draws for the hub, buffer, and bypass; the click handler tests
   against those rects rather than re-deriving geometry.

> **No feature flag.** The redesign shipped as the *only* renderer. There is no
> `HELIX_LAYERED_FILAMENT_PATH` flag (the name appears in an early scratchpad
> but never reached the codebase), no compile-time switch, and no surviving
> legacy draw path. The layered model is unconditional.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│  Panels (AMS detail, etc.)                                    │
│  push printer state via C setters in                          │
│  ui_filament_path_canvas.h  (set_topology, set_active_slot,   │
│  set_filament_segment, set_slot_filament, ...)                │
└───────────────────────────┬──────────────────────────────────┘
                            │ setters → layered_mark_dirty()
┌───────────────────────────▼──────────────────────────────────┐
│  ui_filament_path_layers.cpp                                  │
│  - two lv_canvas children (static, overlay) + draw_bufs       │
│  - dirty flags, async refresh, size-change handling           │
└───────────────────────────┬──────────────────────────────────┘
                            │ layered_render_overlay()
┌───────────────────────────▼──────────────────────────────────┐
│  ui_filament_path_topology.cpp                                │
│  render_overlay_content() → render_linear_hub / render_mixed  │
│  / render_parallel — phase functions per topology             │
└──────────┬──────────────────────────────┬────────────────────┘
           │ draw_lane*/draw_merge_fan      │ draw_sensor_dot/hub/nozzle
┌──────────▼──────────────┐  ┌──────────────▼────────────────────┐
│ filament_tube_stroker   │  │ ui_filament_path_glyphs.cpp        │
│ (strokes pathgeo paths) │  │ sensor dots, hub box, buffer,      │
└──────────┬──────────────┘  │ nozzle, toolhead, badges           │
           │                 └────────────────────────────────────┘
┌──────────▼──────────────┐
│ filament_path_geometry  │  helix::ui::pathgeo — pure float math:
│ (pathgeo)               │  PathPoint / PathSeg / FilamentPath,
└─────────────────────────┘  route_orthogonal, route_polyline_filleted,
                             build_merge_fan, path_point_at
```

Per-widget state lives in one `FilamentPathData`, owned by a registry keyed on
the `lv_obj_t*` (`ui_filament_path_internal.h`). The XML → Subjects → C++
pattern applies: XML or `ui_filament_path_canvas_create()` builds the widget,
panels push printer state through the C setters.

---

## The Three-Layer Model

Rendering is split into three layers so per-frame animation never repaints the
expensive tube geometry (`ui_filament_path_internal.h:17-26`).

```cpp
struct LayerState {
    lv_obj_t*      static_canvas  = nullptr;  // Layer 1: state-independent content
    lv_obj_t*      overlay_canvas = nullptr;  // Layer 2: full topology render
    lv_draw_buf_t* static_buf     = nullptr;  // ARGB8888 backing buffer
    lv_draw_buf_t* overlay_buf    = nullptr;  // ARGB8888 backing buffer
    bool           static_dirty   = true;
    bool           overlay_dirty  = true;
    int32_t        canvas_w = 0;
    int32_t        canvas_h = 0;
};
```

1. **Static canvas** (`lv_canvas` child) — reserved for state-independent
   content. The infrastructure exists (`layered_render_static()`); it is
   currently cleared transparent, available for idle-topology content that
   never changes with filament state.

2. **Overlay canvas** (`lv_canvas` child) — the full active topology render:
   every lane, the hub/selector box, all sensors, the nozzle. Painted by
   `render_overlay_content()`. LVGL composites it natively over the static
   canvas. Repainted only when filament/topology state changes — **not** every
   frame.

3. **DRAW_POST pass** — flow dots, heat glow, and the moving segment-transition
   filament tip. Painted directly on top of the cached canvases every frame by
   `filament_path_draw_cb()` → `render_animation_overlay()`. This is the only
   per-frame cost during animation; it touches no canvas buffer.

### Canvas geometry

Both canvases are sized to the widget plus a top overhang (`layered_overhang()`
in `ui_filament_path_layers.cpp`) so lane-entry geometry that rises above the
widget bounds isn't clipped. The parent widget carries
`LV_OBJ_FLAG_OVERFLOW_VISIBLE`; canvases are positioned at a negative-Y origin
so their buffer coordinates map to absolute display coordinates. Buffers are
`LV_COLOR_FORMAT_ARGB8888` for full transparency between layers.

---

## The Geometry Module

`include/filament_path_geometry.h` / `src/ui/filament_path_geometry.cpp` —
namespace `helix::ui::pathgeo` (aliased `pg` in callers). Pure, float-based,
screen-space (`+y` is **down**, angle 0 is `+x`, positive sweep is clockwise on
screen). No LVGL types — this is the layer the unit tests hammer directly.

### Core types

```cpp
struct PathPoint { float x = 0.0f; float y = 0.0f; };

struct PathSeg {
    enum Type { LINE, ARC };
    Type      type = LINE;
    PathPoint p0;            // LINE start  / ARC computed start
    PathPoint p1;            // LINE end    / ARC computed end
    PathPoint center;        // ARC only: circle center
    float     radius      = 0.0f;
    float     start_angle = 0.0f;   // radians
    float     sweep       = 0.0f;   // signed; positive = clockwise on screen
};

struct FilamentPath {
    static constexpr int MAX_SEGS = 16;
    PathSeg segs[MAX_SEGS];
    int     count = 0;
    void add_line(float x0, float y0, float x1, float y1);
    void add_arc(float cx, float cy, float r, float a0, float sweep);
    void clear();
};
```

### Path math

```cpp
float     seg_length(const PathSeg& s);          // LINE: euclidean; ARC: |sweep|*radius
float     path_length(const FilamentPath& p);
PathPoint path_point_at(const FilamentPath& p, float d, PathPoint* tangent_out = nullptr);
```

`path_point_at()` walks the path to arc-length `d` and optionally returns the
unit tangent — this is how the animation overlay places flow dots and the
moving tip along a cached path.

### Arc-fillet routing

The routing functions turn coarse waypoints into smooth tube centerlines by
inserting tangent-circular arcs at corners:

```cpp
// Orthogonal lane: vertical → quarter-arc → horizontal → quarter-arc → vertical.
// Fillet radius clamped to fit (min of fillet_r, |dx|/2, dy/2); degenerate
// cases fall back to straight lines or a 45° jog.
void route_orthogonal(FilamentPath& out, float x0, float y0,
                      float x1, float y1, float fillet_r);

// General filleted polyline through n waypoints. Inserts a circular arc at each
// interior vertex (trim length t = r_eff / tan(half-turn-angle)); collinear and
// starved corners handled automatically.
void route_polyline_filleted(FilamentPath& out, const PathPoint* pts,
                             int n, float fillet_r);

// Non-overlapping merge fan: n slots converging onto a hub. Uses a common slope
// per side (left/right of hub) so diagonals stay parallel and never cross;
// reserves fillet room so corners don't starve. Emits a 4-point polyline per
// lane (feed each to route_polyline_filleted).
struct MergeLaneIn  { float slot_x; float start_y; };
struct MergeLaneOut { PathPoint pts[4]; };
void build_merge_fan(const MergeLaneIn* lanes, int n,
                     float hub_cx, float hub_top, float hub_w,
                     float entry_margin, float fillet_r, float max_slope,
                     MergeLaneOut* out);
```

`build_merge_fan()` is the heart of HUB/MIXED routing: rather than detecting and
resolving lane overlaps after the fact, it constructs separation-by-design by
giving every lane on a side the same diagonal slope.

---

## The Shared Tube Stroker

`include/filament_tube_stroker.h` / `src/ui/filament_tube_stroker.cpp` — renders
a `pathgeo::FilamentPath` as a tube. **Used by both** the detail canvas
(`ui_filament_path_topology.cpp`) and the AMS overview canvas
(`ui_system_path_canvas.cpp`) — the single source of truth for lane rendering.

### Stroking model

A tube is drawn as **concentric passes** (e.g. wide low-opacity glow → outline
→ core → highlight). `stroke_path()` walks the path segment by segment:

- Straight runs use `lv_draw_line` with float coordinates.
- Arcs are sampled as chords from the exact float parametrization — **not**
  `lv_draw_arc`, which rounds integer center/radius and produces visibly faceted
  corners at small radii.
- Interior joints use butt caps (avoids double-blending in translucent passes);
  first/last segment caps are round.

### Public interface

```cpp
struct TubePass { lv_color_t color; int32_t width; lv_opa_t opa; };

struct LaneStyle {
    bool       solid;  // solid filament tube vs hollow idle PTFE bore
    lv_color_t color;  // filament color (solid) or idle wall color (hollow)
    lv_color_t bg;     // background for the hollow bore
    int32_t    width;  // tube outer width
    bool       glow;   // wide low-opacity backdrop (active lanes only)
};

void      stroke_path(lv_layer_t* layer, const pg::FilamentPath& path,
                      const TubePass* passes, int n_passes);
int       build_passes(const LaneStyle& style, TubePass* out);
LaneStyle lane_style(bool has_filament, lv_color_t tool_color,
                     lv_color_t idle_color, lv_color_t bg, int32_t active_w);

// High-level lane drawing. The optional `record` out-param captures the
// centerline into a FilamentPath for the animation overlay to replay.
void draw_lane(lv_layer_t*, const pg::FilamentPath&, const LaneStyle&,
               pg::FilamentPath* record = nullptr);
void draw_lane_vline (lv_layer_t*, int32_t x, int32_t y0, int32_t y1,
                      const LaneStyle&, pg::FilamentPath* record = nullptr);
void draw_lane_route (lv_layer_t*, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      float fillet_r, const LaneStyle&, pg::FilamentPath* record = nullptr);
void draw_lane_hline (lv_layer_t*, int32_t x0, int32_t x1, int32_t y,
                      const LaneStyle&, pg::FilamentPath* record = nullptr);

struct MergeFanLane {
    int32_t slot_x; int32_t start_y; LaneStyle style;
    pg::FilamentPath* record = nullptr;
};
void draw_merge_fan(lv_layer_t*, const MergeFanLane* lanes, int n,
                    int32_t hub_cx, int32_t hub_top, int32_t hub_w,
                    float fillet_r, int32_t* entry_x_out = nullptr);

// Color helpers
lv_color_t tube_darken(lv_color_t, uint8_t);
lv_color_t tube_lighten(lv_color_t, uint8_t);
lv_color_t tube_blend(lv_color_t, lv_color_t, float);
lv_color_t get_glow_color(lv_color_t);
bool       reduced_effects();   // drops glow passes on low-perf platforms
```

`reduced_effects()` lets the stroker shed the wide glow pass on constrained
devices without the caller branching.

---

## RenderCtx & Phase Decomposition

The render pass threads a small context through every phase
(`ui_filament_path_internal.h:362`):

```cpp
struct RenderCtx {
    lv_layer_t*       layer;  // target layer for this pass (mapped to absolute coords)
    FilamentPathData* data;   // widget state: theme, config, anim, hit rects, path cache
    BaseGeometry      geo;    // pre-computed canvas dims, per-slot X array, center X
};
```

`BaseGeometry` is computed once per render (`compute_base_geometry()`),
eliminating the repeated sibling-widget coordinate reads that the old monolith
did inside every per-slot loop. Phases take `const RenderCtx&` plus only their
per-call values.

The overlay-canvas content dispatches by topology
(`render_overlay_content()`):

```cpp
void render_overlay_content(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    if      (data->topology == PathTopology::MIXED)    render_mixed(obj, layer, data);
    else if (data->topology == PathTopology::PARALLEL) render_parallel(obj, layer, data);
    else                                               render_linear_hub(obj, layer, data);
}
```

Each topology renderer is a sequence of named phases. For LINEAR/HUB:

```cpp
void render_linear_hub(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    RenderCtx ctx{layer, data, compute_base_geometry(obj, data)};
    data->hits.hub_valid = data->hits.buffer_valid = data->hits.bypass_valid = false;

    LinearHubFrame f = compute_linear_hub_frame(ctx);  // layout + per-slot colors
    apply_debug_flow_override(obj, ctx, f);
    build_linear_hub_merge_fan(ctx, f);                // merge-fan geometry

    draw_entry_lanes(ctx, f);      // entry tubes + prep sensors
    draw_bypass_section(ctx, f);   // bypass path (records bypass hit rect)
    draw_hub_section(ctx, f);      // hub/selector box (records hub hit rect)
    draw_output_section(ctx, f);   // output tube + output sensor
    draw_toolhead_section(ctx, f); // toolhead sensor dot
    draw_nozzle_section(ctx, f);   // nozzle glyph; records active path into path_cache
}
```

This mirrors the scratchpad's `compute_geometry / draw_static_topology /
draw_filament_overlay / draw_animation_overlay` idea, but landed as a finer,
per-topology phase split where the static/state concerns share a precomputed
*frame* struct (`LinearHubFrame`, `MixedFrame`) rather than a single
three-function pass.

---

## Topology Renderers

The supported topologies are `PathTopology` (`include/ams_types.h:493`):

```cpp
enum class PathTopology {
    LINEAR   = 0,  // Happy Hare: a selector picks one input at a time
    HUB      = 1,  // AFC (Box Turtle): inputs merge through a hub
    PARALLEL = 2,  // Tool Changer: each slot is its own toolhead
    MIXED    = 3,  // some lanes direct, some through a hub to a shared extruder
};
```

Filament position along a path is tracked by `PathSegment`
(`include/ams_types.h:571`): `NONE, SPOOL, PREP, LANE, HUB, OUTPUT, TOOLHEAD,
NOZZLE`.

| Topology | Renderer | Shape |
|----------|----------|-------|
| LINEAR / HUB | `render_linear_hub()` | Entry lanes converge through a merge fan into a hub/selector box, one output tube to a shared nozzle. LINEAR and HUB share the renderer; they differ in layout/labeling. |
| PARALLEL | `render_parallel()` | Each slot is an independent column with its own sensor and nozzle. Per-slot loop calls `draw_parallel_slot()`. |
| MIXED | `render_mixed()` | A subset of lanes route directly to their own nozzle; the rest merge through a hub to a shared toolhead. `compute_mixed_frame()` identifies which lanes are hub-routed. |

Per-slot render state (color, segment, mounted-ness, at-sensor, at-nozzle) is
derived once into `SlotRenderStates` by `compute_slot_render_states()` and read
by the topology bodies, so the same state isn't re-derived across phases.

---

## Cache & Invalidation

Two records are produced by the overlay render and consumed later:

```cpp
struct PathCache {            // the active filament path, replayed by DRAW_POST
    pg::FilamentPath path = {};
    int32_t center_x = 0;
    int32_t nozzle_y = 0;
    int32_t sensor_r = 0;
    bool    valid    = false;
};

struct HitRects {            // exact drawn boxes, read by the click handler
    lv_area_t hub = {};    bool hub_valid    = false;
    lv_area_t buffer = {}; bool buffer_valid = false;
    lv_area_t bypass = {}; bool bypass_valid = false;
};
```

The nozzle/lane phases append the active centerline into `path_cache.path` via
the stroker's `record` out-param; the DRAW_POST animation replays it without
re-running the topology phase. Hub/buffer/bypass phases record their drawn boxes
into `hits`, and `filament_path_click_cb()` tests taps against those rects — the
single source of truth, no geometry re-derivation.

### What invalidates what

State setters call `layered_mark_dirty(obj, static_dirty, overlay_dirty)`
(`ui_filament_path_layers.cpp:183`):

```cpp
void layered_mark_dirty(lv_obj_t* obj, bool static_dirty, bool overlay_dirty) {
    auto* data = get_data(obj);
    if (data) {
        if (static_dirty)  data->layers.static_dirty  = true;
        if (overlay_dirty) data->layers.overlay_dirty = true;
        if (overlay_dirty) data->path_cache.valid = false;  // force re-record
        if (data->layers.static_canvas)
            lv_async_call(layered_refresh_async, obj);       // deduped repaint
    }
    lv_obj_invalidate(obj);  // schedule the cheap DRAW_POST pass
}
```

| Trigger | Marks dirty | Effect |
|---------|-------------|--------|
| Topology / slot-count / theme / size change | static + overlay | Full canvas repaint (and buffer realloc if size changed) |
| Filament color / segment / per-slot / active-slot / bypass / buffer change | overlay | Overlay canvas repaint; `path_cache` invalidated for re-record |
| Animation tick (flow / heat / segment tip) | *(neither)* | `lv_obj_invalidate(obj)` only → DRAW_POST pass, no canvas work |

`layered_refresh_async()` runs outside the render phase: it early-returns until
the widget has a real size (`w>0 && h>0`), reallocates buffers on size change
(`layered_ensure_buffers()`), repaints only the dirty layers, then clears the
flags. A `SIZE_CHANGED` event (`layered_size_changed_cb()`) re-marks both layers
dirty and reschedules — critical because the create-time refresh can run before
layout has given the widget a size.

---

## Animation Overlay

Five `lv_anim`-driven systems live in `ui_filament_path_anim.cpp` (segment
transition, error pulse, heat pulse, flow, output-X slide). Each ticks
`AnimState` and calls `lv_obj_invalidate(obj)`; the DRAW_POST handler
(`render_animation_overlay()`) reads `AnimState` and paints on top of the cached
canvases:

- **Flow dots** — `path_point_at()` placing dots along the cached `path_cache`
  at an animated offset.
- **Heat glow** — a pulsing radial glow at the nozzle/sensor when heating.
- **Segment-transition tip** — the moving leading edge of filament as it
  advances along the cached path between segments.

PARALLEL computes its mounted-slot entry line on demand; MIXED has no per-frame
animation. Because this pass never touches a canvas buffer, an active animation
costs only the DRAW_POST repaint, not a topology re-render.

---

## Extending the Renderer

**Add a setter / new state input.** Add the C setter in
`ui_filament_path_canvas.h` / `.cpp`, store it in `FilamentPathData`, and call
`layered_mark_dirty(obj, /*static*/false, /*overlay*/true)` (or both, if it
changes layout). Setters that only affect animation should
`lv_obj_invalidate(obj)` directly instead, to avoid a canvas repaint.

**Add a new glyph.** Put the draw helper in `ui_filament_path_glyphs.cpp`, take
a `lv_layer_t*` plus geometry, and call it from the relevant topology phase. If
it's clickable, record its box into `data->hits` and add a `_valid` flag, then
test it in `filament_path_click_cb()`.

**Add a new lane route.** Build the centerline with `pathgeo`
(`route_orthogonal` / `route_polyline_filleted` / `build_merge_fan`), then hand
it to `filament_tube_stroker::draw_lane()` (or `stroke_path()` for a custom pass
set). Pass a `record` `FilamentPath*` if the lane should support flow animation.
Keep all coordinate math in `pathgeo` so it stays unit-testable.

**Add a new topology.** Extend `PathTopology` in `include/ams_types.h`, add a
`render_<name>()` with its own phase sequence and a `<Name>Frame` struct (mirror
`compute_linear_hub_frame()` / `compute_mixed_frame()`), and dispatch to it from
`render_overlay_content()`. Derive per-slot state once via
`compute_slot_render_states()`.

**Reuse on a new canvas.** The tube stroker has no detail-canvas coupling —
`ui_system_path_canvas.cpp` already uses it standalone. Build paths with
`pathgeo`, style with `lane_style()`, stroke with `draw_lane*()`.

---

## Testing

All geometry is exercised headless (no LVGL needed); rendering and hit-testing
use the LVGL test fixture.

| Test file | Tags | Covers |
|-----------|------|--------|
| `tests/unit/test_filament_path_geometry.cpp` | `[filament-path][geometry]` | `seg_length`, `path_length`, `path_point_at`, `route_orthogonal`, `route_polyline_filleted`, `build_merge_fan` — pure math, no LVGL |
| `tests/unit/test_filament_path_mixed_render.cpp` | `[filament-path][mixed][topology]`, `[filament-path][parallel][topology]` | MIXED/PARALLEL produce opaque overlay pixels once laid out; `SIZE_CHANGED` reschedules the async refresh post-layout |
| `tests/unit/test_filament_path_canvas.cpp` | `[canvas][hit_test]`, `[filament-path][canvas]` | Hit-rect tests (hub box dead-center / argument order), SIZE_CHANGED handler |

```bash
./build/bin/helix-tests "[filament-path]"
./build/bin/helix-tests "[geometry]"
```

The geometry tests are the cheapest regression net for routing changes — they
run without a display and assert exact arc tangents and lane separation.

---

## File Map

| File | Contents |
|------|----------|
| `include/filament_path_geometry.h`, `src/ui/filament_path_geometry.cpp` | `helix::ui::pathgeo` — pure path math, arc-fillet routing, merge fan |
| `include/filament_tube_stroker.h`, `src/ui/filament_tube_stroker.cpp` | Concentric tube stroking; `draw_lane*`, `draw_merge_fan`, color helpers; shared by detail + overview canvases |
| `include/ui_filament_path_canvas.h` | Public widget API: create/register + the C state setters |
| `src/ui/ui_filament_path_internal.h` | `FilamentPathData`, `LayerState`, `PathCache`, `HitRects`, `RenderCtx`, `BaseGeometry`, `ThemeCache`, `AnimState` |
| `src/ui/ui_filament_path_canvas.cpp` | Widget lifecycle, theme, click dispatch, DRAW_POST callback, C API, XML registration |
| `src/ui/ui_filament_path_layers.cpp` | Canvas/buffer management, dirty flags, async refresh, size-change handling, teardown |
| `src/ui/ui_filament_path_topology.cpp` | The three topology renderers + phase functions + DRAW_POST `render_animation_overlay()` |
| `src/ui/ui_filament_path_glyphs.cpp` | Sensor dots, hub box, buffer coil, nozzle, toolhead, badges |
| `src/ui/ui_filament_path_anim.cpp` | The five `lv_anim`-driven animation systems |
| `src/ui/ui_system_path_canvas.cpp` | AMS overview canvas — second consumer of the tube stroker |

---

## References

- `docs/devel/FILAMENT_MANAGEMENT.md` — AMS/AFC/Happy Hare/ACE/IFS/CFS backends that feed this widget
- `docs/devel/BED_MESH_RENDERING_INTERNALS.md` — sibling software-renderer doc
- `docs/devel/LVGL9_XML_GUIDE.md` — XML widget registration and subject bindings
- The original design brainstorm lived in a local `.claude/scratchpad/` note (not tracked, predates the shipped naming and the dropped feature-flag plan)
