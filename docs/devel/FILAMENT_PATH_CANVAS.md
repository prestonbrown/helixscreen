# Filament Path Canvas Rendering

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Architecture Overview](#architecture-overview)
3. [The Two-Layer Model](#the-two-layer-model)
4. [The Geometry Module](#the-geometry-module)
5. [The Shared Tube Stroker](#the-shared-tube-stroker)
6. [RenderCtx & Phase Decomposition](#renderctx--phase-decomposition)
7. [Topology Renderers](#topology-renderers)
8. [Cache & Invalidation](#cache--invalidation)
9. [Animation Overlay](#animation-overlay)
10. [Off-page stubs](#off-page-stubs)
11. [Extending the Renderer](#extending-the-renderer)
12. [Testing](#testing)
13. [File Map](#file-map)

---

## Executive Summary

The filament-path canvas is the AMS detail-panel visualization: it draws the
physical route filament takes from each spool slot, through prep sensors, an
optional hub/selector, the output (bowden) tube, the toolhead sensor, and into
the nozzle, colored to reflect what filament is loaded and animated to show
flow and heat during loads/unloads.

It is a layered, phase-decomposed renderer built on two reusable modules: a **pure geometry
library** (`pathgeo`) that produces arc-filleted filament paths with no LVGL
dependency, and a **tube stroker** that renders those paths as concentric tube
strokes. The same route plan and painter drive both this detail canvas and the
AMS overview canvas.

### Key Design Goals

1. **Per-frame animation never repaints tube geometry.** Topology is cached on
   `lv_canvas` children; only flow chevrons / heat glow / a moving filament tip are
   redrawn every frame, on a cheap `DRAW_POST` pass.
2. **Geometry is pure and testable.** All path math lives in
   `helix::ui::pathgeo`, float-based, LVGL-free, exercised by headless unit
   tests.
3. **One plan and painter, two canvases.** The detail canvas and the system
   overview canvas each plan their routes into a `PathPlan` (span styles from
   `span_style()`, clamp bands from `band_state()`) and paint it with
   `paint_tubes()` / `paint_box_bands()`, with no duplicated lane drawing.
4. **Hit-testing reads what was drawn.** The render pass records the exact
   boxes it draws for the hub, buffer, and bypass; the click handler tests
   against those rects rather than re-deriving geometry.

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
│  - one lv_canvas child (overlay) + its draw_buf               │
│  - dirty flags, async refresh, size-change handling           │
└───────────────────────────┬──────────────────────────────────┘
                            │ layered_render_overlay()
┌───────────────────────────▼──────────────────────────────────┐
│  ui_filament_path_topology.cpp                                │
│  render_overlay_content() → render_linear_hub / render_mixed  │
│  / render_parallel : plan (ui_filament_path_plan) + glyphs    │
└──────────┬──────────────────────────────┬────────────────────┘
           │ paint_tubes → stroke_path      │ hub/buffer/nozzle/badges
┌──────────▼──────────────┐  ┌──────────────▼────────────────────┐
│ filament_tube_stroker   │  │ ui_filament_path_glyphs.cpp        │
│ (strokes pathgeo paths) │  │ hub box, buffer, filament tip,     │
└──────────┬──────────────┘  │ nozzle, toolhead, badges           │
           │                 └────────────────────────────────────┘
┌──────────▼──────────────┐
│ filament_path_geometry  │  helix::ui::pathgeo : pure float math:
│ (pathgeo)               │  PathPoint / PathSeg / FilamentPath,
└─────────────────────────┘  route_orthogonal, route_polyline_filleted,
                             build_merge_fan, path_point_at
```

Per-widget state lives in one `FilamentPathData`, owned by a registry keyed on
the `lv_obj_t*` (`ui_filament_path_internal.h`). The XML → Subjects → C++
pattern applies: XML or `ui_filament_path_canvas_create()` builds the widget,
panels push printer state through the C setters.

---

## The Two-Layer Model

Rendering is split into two layers so per-frame animation never repaints the
expensive tube geometry (`src/ui/filament/ui_filament_path_internal.h`).

```cpp
struct LayerState {
    lv_obj_t*      overlay_canvas = nullptr;  // Layer 1: full topology render
    lv_draw_buf_t* overlay_buf    = nullptr;  // ARGB8888 backing buffer
    bool           overlay_dirty  = true;
    int32_t        canvas_w = 0;
    int32_t        canvas_h = 0;
};
```

1. **Overlay canvas** (`lv_canvas` child): the full active topology render:
   every lane, the hub/selector box, all sensors, the nozzle. Painted by
   `render_overlay_content()`. LVGL composites it natively. Repainted only when
   filament/topology state changes, **not** every frame.

2. **DRAW_POST pass**: flow chevrons, heat glow, and the moving segment-transition
   filament tip. Painted directly on top of the cached canvas every frame by
   `filament_path_draw_cb()` → `render_animation_overlay()`. This is the only
   per-frame cost during animation; it touches no canvas buffer.

A canvas holding nothing costs a full-size ARGB8888 buffer (~780KB on an
800x480 panel's AMS view) and a composite on every frame, so content that never
changes with filament state still goes on the overlay canvas.

### Canvas geometry

The canvas is sized to the widget plus a top overhang (`layered_overhang()`
in `ui_filament_path_layers.cpp`) so lane-entry geometry that rises above the
widget bounds isn't clipped. The parent widget carries
`LV_OBJ_FLAG_OVERFLOW_VISIBLE`; the canvas is positioned at a negative-Y origin
so its buffer coordinates map to absolute display coordinates. The buffer is
`LV_COLOR_FORMAT_ARGB8888` so the widget background shows through.

---

## The Geometry Module

`include/filament_path_geometry.h` / `src/ui/filament/filament_path_geometry.cpp`:
namespace `helix::ui::pathgeo` (aliased `pg` in callers). Pure, float-based,
screen-space (`+y` is **down**, angle 0 is `+x`, positive sweep is clockwise on
screen). No LVGL types; this is the layer the unit tests hammer directly.

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
    static constexpr int kMaxSegs = 16;
    PathSeg segs[kMaxSegs];
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
unit tangent; this is how the animation overlay places flow chevrons and the
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

`include/filament_tube_stroker.h` / `src/ui/filament/filament_tube_stroker.cpp` renders
a `pathgeo::FilamentPath` as a tube. `paint_tubes()` (`ui_filament_path_plan.cpp`)
strokes every route of a plan through it, for the detail canvas and the AMS
overview canvas alike: the single source of truth for lane rendering.

### Stroking model

A tube is a PTFE sleeve drawn as **concentric opaque passes** in three layers:
halo (active route only: two bands at gauge+6 and gauge+3, pre-blended from the
background toward the wall color at 0.25 and 0.55), wall (`{wall, gauge}`), then
bore (`{bore, gauge-2}`, so walls are 1 px). The bore shows the filament color
when loaded and the background when empty. Only a loaded lane on the active
route gets accent (`primary`) walls and the halo; an errored run takes error
walls. Detail lanes use `ThemeCache::tube_gauge` (`line_width_active + 2`).
`stroke_path()` walks the path segment by segment:

- Straight runs use `lv_draw_line` with float coordinates.
- Arcs are sampled as chords from the exact float parametrization, **not**
  `lv_draw_arc`, which rounds integer center/radius and produces visibly faceted
  corners at small radii.
- Opaque passes round-join every segment and chord; translucent passes use
  butt caps at interior joints (no double-blending). First/last caps are round.

### Public interface

```cpp
struct TubePass { lv_color_t color; int32_t width; lv_opa_t opa; };

struct LaneStyle {
    lv_color_t wall;   // idle wall token, accent on the active route, error on error
    lv_color_t bore;   // filament color when loaded, background when empty
    lv_color_t bg;     // background the halo bands pre-blend against
    int32_t    width;  // outer gauge, walls included
    bool       halo;   // active route only; halo color is the wall color
};
enum class TubeLayer : uint8_t { Halo, Wall, Bore };

void      stroke_path(lv_layer_t* layer, const pg::FilamentPath& path,
                      const TubePass* passes, int n_passes);
int       build_passes(const LaneStyle& style, TubeLayer layer, TubePass* out,
                       bool simple = reduced_effects());
LaneStyle lane_style(bool has_filament, bool active, lv_color_t fill,
                     lv_color_t idle_wall, lv_color_t accent, lv_color_t bg,
                     int32_t gauge);

// Color helpers
lv_color_t tube_darken(lv_color_t, uint8_t);
lv_color_t tube_lighten(lv_color_t, uint8_t);
lv_color_t tube_blend(lv_color_t, lv_color_t, float);
bool       reduced_effects();   // drops the halo on low-perf platforms
```

`reduced_effects()` lets the stroker shed the halo on constrained devices
without the caller branching; walls and bore still draw.

---

## RenderCtx & Phase Decomposition

The render pass threads a small context through every phase
(`src/ui/filament/ui_filament_path_internal.h#RenderCtx`):

```cpp
struct RenderCtx {
    lv_layer_t*       layer;  // target layer for this pass (mapped to absolute coords)
    FilamentPathData* data;   // widget state: theme, config, anim, hit rects, path cache
    BaseGeometry      geo;    // pre-computed canvas dims, per-slot X array, center X
};
```

`BaseGeometry` is computed once per render (`compute_base_geometry()`), so no
per-slot loop re-reads sibling-widget coordinates. Phases take `const RenderCtx&` plus only their
per-call values.

The overlay-canvas content dispatches by topology
(`render_overlay_content()`):

```cpp
void render_overlay_content(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    // records the widget origin into data->hits.origin first
    if      (data->topology == int(PathTopology::MIXED))    render_mixed(obj, layer, data);
    else if (data->topology == int(PathTopology::PARALLEL)) render_parallel(obj, layer, data);
    else                                                    render_linear_hub(obj, layer, data);
}
```

LINEAR/HUB is frame → plan → paint → boxes and glyphs
(`src/ui/filament/ui_filament_path_topology.cpp#render_linear_hub`):

```cpp
void render_linear_hub(lv_obj_t* obj, lv_layer_t* layer, FilamentPathData* data) {
    // hit valid flags reset, debug override, LINEAR output_x snap
    const LinearHubFrame f = compute_linear_hub_frame(*data, ctx.geo); // pure layout + merge fan
    static PathPlan plan;                     // ~14 KB, off the (ESP32) LVGL task stack
    plan_linear_hub(f, *data, ctx.geo, plan); // routes + span styles + sensor bands
    paint_tubes(layer, plan, palette);        // halo -> walls -> bores -> bands
    draw_hub_section(ctx, f);                 // hub/selector box (records hits.hub)
    // then, below the hub: buffer tension control (hits.buffer), hits.bypass, nozzle glyph,
    // and path_cache = filled_prefix(plan.routes[plan.active_route])
}
```

PARALLEL and MIXED take the same shape with `plan_parallel` and `plan_mixed`
(rows from `parallel_rows()`, MIXED from `compute_mixed_frame`), then draw the
MIXED hub box and every toolhead glyph over the painted tubes. Their toolheads
stand on a glyph-bottom line near the canvas bottom; the sensors and the hub
sit fixed fractions of the run down to them. A PARALLEL unit has no housing, so
`ams_detail_sync_lane_entry()` hands the canvas the spools' flange bottom
instead of a box front, and `parallel_rows()` hangs each tube's round cap from
it whenever that leaves room above the sensor.

Each toolhead carries its tool label as a corner badge (`ui_toolhead_badge.h`):
the spool tool badge's look from the shared `ams_slot_tool_badge_*` tokens and
`tool_badge_colors()`, in the glyph's lower-right corner for every nozzle style
(`kToolheadBadgeCorner`), clear of the tube entering at the top center.
`toolhead_bounds()` gives each style's drawn extent. The overview uses the
same badge.

### The route plan

The LINEAR/HUB frame (`compute_linear_hub_frame(data, g, glyph_top)`) fits the
HUB box width with `pathgeo::merge_fan_width()`: the hub widens until
neighbouring fan tubes clear the tube gauge, its halo and 2 px, clamped to the
slot row (where a fan zone shorter than its two fillet legs leaves only
vertical drops, as at the micro breakpoint). With three or more lanes it first
borrows unused output-run height by lowering the hub. With the bypass hidden,
the hub and buffer stack above the toolhead glyph (`glyph_top` is
`toolhead_top_y()`), and the toolhead band sits in the gap above the glyph.

**The buffer on the trunk.** A unit with a buffer carries its tension control on the
trunk's final vertical drop, never on a sideways run. `place_under_unit()` in
`compute_linear_hub_frame()` is the one rule for a hub and a selector (bar or rail art): the
buffer stands with equal gaps to the unit's outlet above and the head below, and the lower gap
holds the bypass merge and the toolhead band in the clear room over the head. A selector's
exit stands under the active lane, so its filament jogs to the center first and the buffer
stands below the jog. Where the room is short, the hub or selector rises toward the lanes
(never over the lanes' numbers; the bar's prep sensors toward the box edge, the rail art toward
the lanes' fan room); only a canvas too short for that draws a smaller head. Under a drawn
unit the canvas reaches up into the unit and still ends at its container's bottom
(`ams_detail_sync_lane_entry`), so the head stands near the bottom at every size. The control
is `buffer_control_height(hub_h)` tall: the capsule (`buffer_capsule_height()`); its thumb
stands into the gaps.

**A drawing too tall for its path.** The canvas reports the least height that lays its path
out at full size (`ui_filament_path_canvas_required_height()`, from
`linear_hub_required_height()`: a full-size head and the buffer kept on the trunk). Where the
path below a drawn unit falls short of it, or (in portrait, whose path grows to it) the diagram
then runs past what is on screen, `ams_detail_sync_lane_entry()` caps the unit's spools for that
unit, slot count, width and screen, and the panel lays the unit out again; a unit that fits is
untouched. The drawing keeps at least half the size its width gives it; past that the head
gives up the rest. In portrait the AMS panels scroll, so the sidebar's actions stand below
the diagram at their own height.

**Lane numbers and labels under a drawn unit.** Each lane's sensor band, and the number on
it, stands below where the lane leaves the drawing's slanted foot by half a number
(`set_lane_band_drop`). Where the numbers alternate rows, odd lanes drop a number further
(less the foot's slant), and the planner keeps the hub a radius below the lowest number
(`badges_bottom` in `compute_linear_hub_frame()`). The labels above the unit, their ticks and
which lanes show one (none for an empty lane) are the unit view's: FILAMENT_MANAGEMENT.md,
"Labels over a drawn unit".

`src/ui/filament/ui_filament_path_plan.{h,cpp}`, pure apart from `paint_tubes` and
`draw_sensor_band`, so `[filament-path][plan]` tests it without a display.

- **Routes.** One route per slot, starting at the slot's entry. Every route is
  one contiguous centerline: tubes never stop at a sensor. The trunk (hub bottom
  → nozzle inlet) belongs to exactly one route: the active slot's, or, while the
  bypass is active, the bypass route from the merge down (the AMS trunk then
  ends at the merge and is styled from AMS state alone), or a separate idle trunk
  when nothing is active. Inactive slots end at their hub-top entry (HUB) or the
  selector top (LINEAR). PARALLEL: one route per tool, entry → sensor → nozzle
  top, the mounted tool active. MIXED: hub lanes run entry → sensor → fan → hub
  top, one trunk route runs hub bottom → nozzle top (styled from the first hub
  lane that reached the nozzle), direct lanes run entry → sensor → their own
  nozzle top.
- **Span tags.** Each run is tagged with the `PathSegment` at which it fills:
  spool entry → prep `SPOOL`; prep → hub entry / selector top `LANE`; hub or
  selector interior `HUB`; hub bottom → bypass merge (or the toolhead when the
  bypass is hidden) `OUTPUT`; merge → toolhead sensor `TOOLHEAD`; toolhead →
  inlet `NOZZLE`. `span_style()` turns a tag, how far the lane's filament
  reached, whether it is the active route and the error segment into a
  `SpanStyle` (`Plain`/`Active`/`Error` walls, bore, filled, painted). The hub
  interior is `painted = false` under the opaque HUB box; the LINEAR selector
  passage is painted and shows through the 60% selector. PARALLEL and MIXED
  lanes are `SPOOL` down to their sensor and through the hub fan, `NOZZLE` from
  the sensor (or the hub bottom) to the nozzle top.
- **Bands.** Clamp bands replace the sensor dots: prep (per slot with a prep
  sensor), hub entry (HUB), output (hub bottom, not on-toolhead), bypass merge
  (bypass shown) and toolhead (whenever the unit reports a toolhead sensor,
  `FilamentPathData::has_toolhead_sensor`). MIXED lanes each get one band at
  their sensor, read as `TOOLHEAD`; PARALLEL lanes get it only when the unit
  publishes a toolhead sensor, so a tool changer's tubes run unbanded. A PARALLEL lane whose backend
  publishes an entry sensor (`slot_has_prep_sensor`) gets a second band three
  tube gauges below its cap, read as `PREP`; the run between the two bands is
  `LANE`, so filament only the entry sensor sees fills the tube to that band. `band_state()`: Error on the active
  route at the error segment, Active once triggered on the active route, Loaded
  (the lane's own filament color) once triggered off it, else Empty.
- **Paint.** `coalesce()` joins adjacent painted segments with equal styles into
  one stroke, so a run has caps only at its ends. `paint_tubes()` strokes halo
  (Active strokes only, dropped by `reduced_effects()`), then every wall, then
  bores (empty, filled off the active route, the active route last so its fill
  wins at a T), then the bands.

Each topology's static and state concerns share a precomputed *frame* struct
(`LinearHubFrame`, `MixedFrame`).

---

## Topology Renderers

The supported topologies are `PathTopology` (`include/ams_types.h#PathTopology`):

```cpp
enum class PathTopology {
    LINEAR   = 0,  // Happy Hare: a selector picks one input at a time
    HUB      = 1,  // AFC (Box Turtle): inputs merge through a hub
    PARALLEL = 2,  // Tool Changer: each slot is its own toolhead
    MIXED    = 3,  // some lanes direct, some through a hub to a shared extruder
};
```

Filament position along a path is tracked by `PathSegment`
(`include/ams_types.h#PathSegment`): `NONE, SPOOL, PREP, LANE, HUB, OUTPUT, TOOLHEAD,
NOZZLE`.

| Topology | Renderer | Shape |
|----------|----------|-------|
| LINEAR / HUB | `render_linear_hub()` | Entry lanes converge through a merge fan into a hub/selector box, one output tube to a shared nozzle. LINEAR and HUB share the renderer; they differ in layout/labeling. |
| PARALLEL | `render_parallel()` | Each slot is an independent column with its own sensor and nozzle: `plan_parallel()`, then `draw_parallel_tool()` per slot. |
| MIXED | `render_mixed()` | A subset of lanes route directly to their own nozzle; the rest merge through a hub to a shared toolhead. `compute_mixed_frame()` identifies which lanes are hub-routed. |

A unit view draws the unit's combiner, not the selector its backend reports
(`docs/devel/UNIT_ART.md` § "Combiner"). For a unit with art whose path ends at one
nozzle (LINEAR or HUB), `combined_topology()` (`src/ui/filament/ui_filament_path_plan.h`)
decides the layout, and `ams_detail_setup_path_canvas()` applies it:

| Combiner | Drawn as |
|----------|----------|
| internal or external | HUB: each lane leaves the unit (through its head-on art when it has one) and the fan converges into the hub box below it |
| none, or a sliding selector (ERCF, Tradrack) | The topology as reported: the selector rail in LINEAR's bar |

PARALLEL and MIXED keep their own drawings. The hub box carries an output sensor band
only when the backend reports a hub sensor (`add_hub_output_band`). Head-on art that
finds no room above the hub's place takes the selector's slot and hangs the hub below it,
clear of the bypass merge. The buffer keeps its place under the hub where the room is
there; otherwise its reading moves to the hub's outlet band
(`LinearHubFrame::sync_on_output`). Where neither fits, the hub box draws without the art.

Per-slot render state (color, segment, mounted-ness, at-sensor, at-nozzle) is
derived once into `SlotRenderStates` by `compute_slot_render_states()` and read
by the topology bodies, so the same state isn't re-derived across phases.
The active slot takes the system's filament segment over its own, except that
a bare `SPOOL` (no strand in the path) never makes an empty slot look loaded:
filament presence is the slot's own report, so an empty active lane draws no
fill and no glow. Its toolhead still draws as mounted.

---

## Cache & Invalidation

Two records are produced by the overlay render and consumed later:

```cpp
struct PathCache {            // the active filament path, replayed by DRAW_POST
    pg::FilamentPath path = {};
    int32_t center_x = 0;
    int32_t nozzle_y = 0;
    int32_t sensor_r = 0;
    lv_point_t origin = {0, 0};   // widget top-left at that draw
    bool    valid    = false;
};

struct HitRects {            // exact drawn boxes, read by the click handler
    lv_area_t hub = {};    bool hub_valid    = false;
    lv_area_t buffer = {}; bool buffer_valid = false;
    lv_area_t bypass = {}; bool bypass_valid = false;
};
```

LINEAR/HUB stores the active route's filled prefix (`filled_prefix()`) in
`path_cache.path`; the DRAW_POST animation replays it without re-running the
topology render. Hub/buffer/bypass phases record their drawn boxes
into `hits`, and `filament_path_click_cb()` tests taps against those rects: the
single source of truth, no geometry re-derivation.

A PARALLEL tap goes through the pure `helix::ui::parallel_tap_at()`
(`include/ui_filament_path_canvas.h`) over a `ParallelTapLayout` built from the same
`parallel_rows()` and `toolhead_glyph_bounds()` the renderer draws with. Above the head
glyph's top, and above the sensor row however tall the glyph is drawn, a tap is the lane's
(tube or spool); below that it is a head's only inside the drawn glyph, and where
neighbours overlap the nearest column answers. A lane tap reaches the slot callback. A head
tap reaches the toolhead callback (`filament_path_canvas_set_toolhead_callback()`), or the
slot callback when none is set. The AMS panel's toolhead callback offers a tool change and
falls back to the slot menu for the mounted head or a slot mapped to no tool
(`src/ui/filament/ui_panel_ams.cpp#on_path_toolhead_clicked`).

### What invalidates what

State setters call `layered_mark_dirty(obj)`
(`src/ui/filament/ui_filament_path_layers.cpp#layered_mark_dirty`):

```cpp
void layered_mark_dirty(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (data) {
        data->layers.overlay_dirty = true;
        data->path_cache.valid = false;  // force re-record
        if (data->layers.overlay_canvas)
            data->layers.refresh_timer.schedule_once([obj]() { layered_refresh(obj); });
    }
    lv_obj_invalidate(obj);  // schedule the cheap DRAW_POST pass
}
```

| Trigger | Marks dirty | Effect |
|---------|-------------|--------|
| Topology / slot-count / theme / size change | overlay | Canvas repaint (and buffer realloc if size changed) |
| Filament color / segment / per-slot / active-slot / bypass / buffer change | overlay | Canvas repaint; `path_cache` invalidated for re-record |
| Animation tick (flow / heat / segment tip) | *(nothing)* | `lv_obj_invalidate(obj)` only → DRAW_POST pass, no canvas work |

`layered_refresh()` runs outside the render phase: it early-returns until
the widget has a real size (`w>0 && h>0`), reallocates the buffer on size change
(`layered_ensure_buffers()`), repaints the canvas if dirty, then clears the
flag. A `SIZE_CHANGED` event (`layered_size_changed_cb()`) re-marks the canvas
dirty and reschedules, which matters because the create-time refresh can run before
layout has given the widget a size.

---

## Animation Overlay

Five `lv_anim`-driven systems live in `ui_filament_path_anim.cpp` (segment
transition, error pulse, heat pulse, flow, output-X slide). Each ticks
`AnimState` and calls `lv_obj_invalidate(obj)`; the DRAW_POST handler
(`render_animation_overlay()`) reads `AnimState` and paints on top of the cached
canvas:

- **Flow chevrons**: `chevrons_along()` (`filament_path_geometry.h`) placing
  chevrons along the cached `path_cache`, sized from the tube gauge, pointing
  the way the filament travels, at an animated phase. They run during a
  load/unload (`flow_active`) and during a manual Extrude / Retract
  (`manual_motion`, bound with `bind_motion="filament_manual_motion"`, the
  subject `FilamentPanel` publishes). The cache is in display coordinates
  recorded with the widget's origin, so the pass shifts it by however far the
  widget has moved since; chevrons skip runs hidden in a box (`seg_shown`)
  and the hub and buffer boxes' hit rects. With animations off a manual move
  shows still chevrons; a load/unload shows none.
- **Heat glow**: a pulsing radial glow at the nozzle/sensor when heating.
- **Segment-transition tip**: the moving leading edge of filament as it
  advances along the cached path between segments.

PARALLEL computes its mounted-slot entry line on demand; MIXED has no per-frame
animation. Because this pass never touches a canvas buffer, an active animation
costs only the DRAW_POST repaint, not a topology re-render.

---

## The view scale

Under a unit's drawing (the AMS panel's column, or the overview's unit page) the canvas takes one scale for every size it draws (`ui_filament_path_canvas_set_view_scale`, from `include/path_view_scale.h`): one unit is the column's height over 44, and the tube (0.6 units, held between 4 and 8 px), the sensors, the hub (2.3 units, never shorter than its label), the head (1.1 units, never narrower than three tubes) and the linked-lanes bracket are fixed shares of it, the same on every unit at one screen size. Under it:

- The head keeps its size at every height; the path asks for the least height that holds it (`linear_hub_required_height`), and the drawing above takes the rest.
- PARALLEL and MIXED paths (lanes to heads of their own) are sized the same way (`per_lane_required_height()`): every head at the hub layout's size, a run from each outlet to its head long enough to read, and a pass-through unit's drawing its room. Where the lanes stand closer than a head is wide, the heads spread a head's width apart and each tube bends out to its own (`parallel_head_x()`).
- The hub stands just below the lanes' fan, centered on the lanes it gathers and only as wide as its entries need: the lanes converge into it. Each lane drops straight in or runs one diagonal no flatter than 0.35 (`kFanMinSlope`), bending late on short fillets, the path takes the height those diagonals need (the unit above gives it up), and any height past that goes to the trunk down to the head; the hub carries a gear only when a tap on it has actions (`ui_filament_path_canvas_set_hub_actions`, a selector's).
- The buffer rides in the hub: a small meter on its side, the slot card's turned clockwise (compressed to the right), filled in the state's color, red on a fault. A tap on it opens the buffer's detail. A selector drawn as its rail art carries the meter on the art, in a hub-colored plate over its lower right corner; no capsule stands on the trunk.
- A sensor that sees filament takes the lane's filament color, an empty one stays neutral, and a faulted one turns red, on every unit, drawn or not. Leaving a drawn unit, a lane's load sensor is a short marker at its tube's start, along the unit's bottom edge (`ui_filament_path_canvas_set_art_sensors`); its prep sensor is drawn in the art where the art has one, and otherwise shows on the tube only when it faults. The hub sensor is a band on the hub's outlet edge; the toolhead sensor is a small round dot at the head's inlet with a faint light halo. A drawn prep region is tinted the lane's color while it sees filament (`draw_sensor_faults` in `ui_unit_art.cpp`).

A lane fault names its sensor in `SlotError::sensor`, read from the firmware's sentence by the fault-path providers (`include/fault_path_position.h`).

## Off-page stubs

The overview's unit view shows one unit of a hub at a time; the other units of that hub
are shown on the hub itself (`ui_filament_path_canvas_set_offpage_units(obj, before,
before_drying, after, after_drying)`, fed from `UnitPage::{same_hub_before, same_hub_after,
drying_before, drying_after}`). `compute_linear_hub_frame()` marks each side that has units in
`LinearHubFrame::stubs[0]` (earlier pages) and `[1]` (later pages); the topology renderer
draws a chip for each through `draw_offpage_chips()` just after the hub box.

- **The chip.** A pill as tall as the hub, beside its left edge for earlier pages and its
  right edge (past the gear) for later ones, centered on the hub's rows, where no lane or
  trunk runs (`offpage_chip_rect()`). It reads `+1 unit`, `+N units`
  (`offpage_label_text()`), or only `+N` where the full text would not fit beside the hub
  inside the canvas and the `edge_reserve` columns the paging arrows hold on the hub's rows. A
  drying unit puts the `heat_wave` glyph in the `warning` color before the text.
- **A tap.** The chip's box is recorded in `FilamentPathData::hits.offpage[side]`, and a tap
  on it calls the callback `ui_filament_path_canvas_set_offpage_callback()` registered: the
  unit view pages to that side.
- **No reserved entries.** The chips take no entries on the hub: its width is the lanes' alone.
- **Where there is no chip.** LINEAR (selector), MIXED, PARALLEL and on-toolhead hubs.
- **The hub box.** `ui_filament_path_canvas_get_hub_box()` computes the box from the same
  frame the renderer draws, so a caller can place something level with it before the first
  paint.

---

## Extending the Renderer

**Add a setter / new state input.** Add the C setter in
`ui_filament_path_canvas.h` / `.cpp`, store it in `FilamentPathData`, and call
`layered_mark_dirty(obj)`. Setters that only affect animation should
`lv_obj_invalidate(obj)` directly instead, to avoid a canvas repaint.

**Add a new glyph.** Put the draw helper in `ui_filament_path_glyphs.cpp`, take
a `lv_layer_t*` plus geometry, and call it from the relevant topology phase. If
it's clickable, record its box into `data->hits` and add a `_valid` flag, then
test it in `filament_path_click_cb()`.

**Add a new lane route.** Build the centerline with `pathgeo`
(`route_orthogonal` / `route_polyline_filleted` / `build_merge_fan`), append it
to a `Route` in the topology's plan with the span's `SpanStyle`, and let
`paint_tubes()` stroke it. Keep all coordinate math in `pathgeo` so it stays
unit-testable.

**Add a new topology.** Extend `PathTopology` in `include/ams_types.h`, add a
`render_<name>()` with its own phase sequence and a `<Name>Frame` struct (mirror
`compute_linear_hub_frame()` / `compute_mixed_frame()`), and dispatch to it from
`render_overlay_content()`. Derive per-slot state once via
`compute_slot_render_states()`.

**Reuse on a new canvas.** The plan and painter have no detail-canvas coupling:
`ui_system_path_plan.cpp` plans the overview with them. Fill the shared
`plan_scratch()` with `new_route()` / `append_line()` / `add_band_at_end()`,
style spans with `Lane::style()` and bands with `Lane::band()`, then call
`paint_tubes()`, draw the boxes, and `paint_box_bands()`.

---

## Testing

All geometry is exercised headless (no LVGL needed); rendering and hit-testing
use the LVGL test fixture.

| Test file | Tags | Covers |
|-----------|------|--------|
| `tests/unit/test_filament_path_geometry.cpp` | `[filament-path][geometry]` | `seg_length`, `path_length`, `path_point_at`, `route_orthogonal`, `route_polyline_filleted`, `build_merge_fan`; pure math, no LVGL |
| `tests/unit/test_filament_path_mixed_render.cpp` | `[filament-path][canvas][mixed]` | MIXED/PARALLEL produce opaque overlay pixels once laid out; `SIZE_CHANGED` reschedules the async refresh post-layout |
| `tests/unit/test_filament_path_plan.cpp` | `[filament-path][plan]`, `[filament-path][plan][hits]`, `[filament-path][plan][offpage]` | LINEAR/HUB frame, route plan (contiguity, ownership, span styles, bands, coalesce), the hub/buffer/bypass hit rects of a rendered canvas, and the off-page chips' place and tap |
| `tests/unit/test_toolhead_badge.cpp` | `[toolhead_badge]` | Glyph bounds against drawn pixels per style; the badge corner clears the tube |
| `tests/unit/test_system_path_plan.cpp` | `[system_path]` | The overview's plan: continuous unit routes, hub bands in all four states, dumb hubs, idle trunk, bypass merge, toolchanger routes, hub boxes only at junctions (`nozzle_feed()`), and on the mock rigs at 800x480 and 480x272 no two routes crossing or closer than a haloed tube (`[route_spacing]`) |
| `tests/unit/test_filament_path_canvas.cpp` | `[canvas][hit_test]`, `[filament-path][canvas]` | Hit-rect tests (hub box dead-center / argument order), SIZE_CHANGED handler |
| `tests/unit/test_filament_path_parallel_tap.cpp` | `[filament-path][hit_test][toolhead_tap]` | A rendered PARALLEL canvas: a head tap reaches the toolhead callback (the slot callback when none is set), a tube tap the slot callback |
| `tests/unit/test_ams_panel_toolhead_tap.cpp` | `[ams][path][hit_test]`, `[ams][path][toolhead_tap]` | `parallel_tap_at()` (glyph edges, tube above the glyph and sensor row, between columns, nearest column on overlap); the AMS panel's head tap asks or mounts, the mounted head opens the slot menu, busy is ignored, printing and spools on the bed refuse |

```bash
./build/bin/helix-tests "[filament-path]"
./build/bin/helix-tests "[geometry]"
```

The geometry tests are the cheapest regression net for routing changes: they
run without a display and assert exact arc tangents and lane separation.

---

## File Map

| File | Contents |
|------|----------|
| `include/filament_path_geometry.h`, `src/ui/filament/filament_path_geometry.cpp` | `helix::ui::pathgeo`: pure path math, arc-fillet routing, merge fan |
| `include/filament_tube_stroker.h`, `src/ui/filament/filament_tube_stroker.cpp` | Concentric tube stroking passes and color helpers |
| `include/ui_filament_path_canvas.h` | Public widget API: create/register + the C state setters |
| `src/ui/filament/ui_filament_path_internal.h` | `FilamentPathData`, `LayerState`, `PathCache`, `HitRects`, `RenderCtx`, `BaseGeometry`, `ThemeCache`, `AnimState` |
| `src/ui/filament/ui_filament_path_canvas.cpp` | Widget lifecycle, theme, click dispatch, DRAW_POST callback, C API, XML registration |
| `src/ui/filament/ui_filament_path_layers.cpp` | Canvas/buffer management, dirty flags, async refresh, size-change handling, teardown |
| `src/ui/filament/ui_filament_path_topology.cpp` | The three topology renderers + phase functions + DRAW_POST `render_animation_overlay()` |
| `src/ui/filament/ui_filament_path_plan.h`, `.cpp` | LINEAR/HUB and MIXED frames, route plans for all three topologies, span styles, sensor bands, layered `paint_tubes()` |
| `src/ui/filament/ui_filament_path_glyphs.cpp` | Hub box, buffer tension control, filament tip, nozzle, toolhead, badges |
| `include/ui_toolhead_badge.h`, `src/ui/filament/ui_toolhead_badge.cpp` | Toolhead glyph bounds per style, the tool badge's look and corner rect |
| `src/ui/filament/ui_filament_path_anim.cpp` | The five `lv_anim`-driven animation systems |
| `src/ui/filament/ui_system_path_plan.h`, `.cpp` | AMS overview state, layout and `plan_overview()` onto the shared route plan |
| `src/ui/filament/ui_system_path_canvas.cpp` | AMS overview canvas: widget, setters, boxes and toolheads over the painted plan |

---

## References

- `docs/devel/FILAMENT_MANAGEMENT.md`: AMS/AFC/Happy Hare/ACE/IFS/CFS backends that feed this widget
- `docs/devel/BED_MESH_RENDERING_INTERNALS.md`: sibling software-renderer doc
- `docs/devel/LVGL9_XML_GUIDE.md`: XML widget registration and subject bindings
