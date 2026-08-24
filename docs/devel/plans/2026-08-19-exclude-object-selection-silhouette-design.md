# Exclude-Object Selection Silhouette + Renderer DRY Consolidation

Give a selected exclude-object a white silhouette outline hugging its real shape, in every
G-code view, matching OrcaSlicer's selection treatment. Corner brackets stay as they are.
Carry the renderer duplication cleanup that the feature touches, per the 1.1 quality process.

Branch: `devel/1.1`. Status: design, pending implementation.

---

## Problem

OrcaSlicer marks a selected object with two cues: a white outline tracing the object's real
contour, and a bounding box with corner marks. HelixScreen has only the second.

### The renderer / platform matrix

This is the load-bearing fact for the whole design, and it is not what the docs imply.

| Renderer | Serves | Built on |
|----------|--------|----------|
| `GCodeLayerRenderer` | the 2D isometric view, all platforms | everywhere |
| `GCodeGLESRenderer` | the 3D view | `ENABLE_GLES_3D=yes` only: `pi`, `pi32`, `pi-both`, `pi32-both`, `x86`, `x86-both` |
| `GCodeRenderer` (CPU wireframe) | **the 3D view on every embedded printer** | `ENABLE_GLES_3D=no`: `ad5m`, `ad5m-br`, `ad5x`, `cc1`, `k1-dynamic`, `k2`, `snapmaker-u1`, `pi-fbdev`, `pi32-fbdev`, `x86-fbdev`, `yocto` |

`GCodeRenderer` is often described as a dead fallback. It is not. `Makefile:422` filters out
only `gcode_gles_renderer.cpp`, and on non-GLES builds `renderer_` **is** `GCodeRenderer`
(`ui_gcode_viewer.cpp:32-38`). `is_using_2d_mode()` returns `render_mode_ != Render3D` in that
build (`:390`), and `render_mode_` reaches `Render3D` through two live paths that do not go
through `decide_render_mode()`:

- the user-facing setting, `/display/gcode_render_mode` = 1, via `ui_settings_printing.cpp:245`
  and `ui_panel_settings.cpp:188`, applied at `ui_panel_print_status.cpp:810-812`
- the CLI `--render-3d` (`cli_args.cpp:533`)

both landing in `ui_gcode_viewer_set_render_mode()` (`ui_gcode_viewer.cpp:2102`).

So on AD5M, AD5X, CC1, K1C, K2 Plus and Snapmaker U1, selecting "3D" renders through
`GCodeRenderer`. **A silhouette that only lands in the GLES renderer would not appear on any
printer we ship to.** Do not delete this file.

### Where the 2D isometric view actually renders

`ViewMode::FRONT` is the isometric view, and it renders through the **cached software Bresenham
path** (`gcode_layer_renderer.cpp:1035`), not the `lv_draw_line` path. Selection styling in that
path (`:822-847`) is:

- excluded: color override to `0xFF6B35`, alpha `EXCLUDED_ALPHA` = 153, width **unchanged**
- highlighted: color override to `0x42A5F5`, **no width change, no opacity change**

The `lv_draw_line` path at `:1268-1285` does apply width 3 / width 1, but it serves the
non-FRONT view modes. `docs/devel/EXCLUDE_OBJECTS.md` documents the `lv_draw_line` behavior as
if it were the isometric behavior. It is not, and that doc needs correcting.

### Current state, all three renderers

| View | Selected | Excluded |
|------|----------|----------|
| 2D isometric (`FRONT`, cache path) | color to blue only, plus brackets | color + alpha 153 |
| 2D other view modes (`lv_draw_line`) | blue, width 3, `LV_OPA_COVER` | `0xFF6B35`, width 1, `LV_OPA_60` |
| 2D ghost pass (`:1743-1749`) | **not handled** | 12% brightness |
| 3D GLES | brackets only (`gcode_gles_renderer.cpp:1653`) | **nothing** (`:1667`) |
| 3D CPU wireframe | `theme("success")` = **green**, width 3 | `theme("danger")`, width 1, `LV_OPA_60` |
| baked 3D geometry (`gcode_geometry_builder.cpp:947-962`) | x1.8 brightness, **dead code, no callers** | no exclusion concept |

Five divergent answers to one product question, including a green highlight on the printers
and a blue one on the desktop.

## Why the cheap 3D approach does not work

Corner brackets in 3D come from *object metadata*, not the mesh: `render_brackets_3d()`
(`gcode_gles_renderer.cpp:1906`) reads `ParsedGCodeFile::objects[name].bounding_box`, builds 48
floats on the CPU, and draws them with a separate unlit line program. That is why brackets were
easy, and it is a real precedent for per-object drawing.

The obvious extension, drawing white lines from `GCodeObject::polygon` extruded to bbox height,
fails on fidelity. `EXCLUDE_OBJECT_DEFINE POLYGON=` is the **convex hull** of the footprint.
Two samples from `assets/test_gcodes/`:

- `calicat_calico.gcode`: a cat, described by an 8-point octagon.
- `stand_s.gcode`: an S, described by a many-point rounded blob tracing arcs.

An outline from that metadata traces a blob around an S. Only the toolpath geometry carries the
real contour, so the silhouette must come from geometry in every renderer.

---

## Design

The unifying insight: **two of the three renderers draw lines, so both get the same two-pass
halo.** Only the GLES renderer needs a GPU technique.

### 1. Two-pass halo (2D isometric + 3D CPU wireframe)

For the selected object only, per layer:

1. Pass A - draw that object's segments in the outline color at `core_width + 2 * halo_px`.
2. Pass B - draw all segments normally, unchanged.

Painter's order is already bottom-to-top and the toolpath is dense, so Pass B overpaints the
halo everywhere except the object's outer boundary. The surviving rim traces the real toolpath,
which is what yields the true contour.

**2D isometric** implements this in the software Bresenham cache path against the raw ARGB
buffer, using the shared thick-line primitive from item DRY-3 below. Not in `lv_draw_line`.
The `lv_draw_line` path gets the same treatment for the non-FRONT view modes, via the same
shared style resolver, so the two stop disagreeing.

**3D CPU wireframe** implements it in `GCodeRenderer`, which already projects each segment and
emits `lv_draw_line`. Same two-pass structure. This is what puts the feature on the printers.

Details:
- Skipped entirely when the highlight set is empty, so the unselected case costs nothing.
- Not applied in the ghost pass (`gcode_layer_renderer.cpp:1743`). Ghost layers are faded
  context; a halo there is costlier and reads wrong.
- Today's blue recolor is dropped in favour of the object keeping its filament color, as in
  Orca. The green highlight in the CPU wireframe goes away with it.
- `halo_px` is 1 on small panels, 2 otherwise. A 3px core plus 2px per side would swallow small
  objects at 480x272.

### 2. GLES 3D: per-object vertex spans, then a shell pass

**Spans.** `generate_ribbon_vertices()` already resolves `seg_obj_name`
(`gcode_geometry_builder.cpp:948`). While emitting vertices, close a run whenever the object
name changes:

```
struct ObjectRun {
    uint32_t vertex_offset;   // within that layer's VBO
    uint16_t vertex_count;    // runs split if they exceed uint16 range
    int16_t  object_index;    // interned, matches ToolpathSegment::object_name_index
};
```

Stored per layer, parallel to the existing `layer_strip_ranges`. Slicers emit
`EXCLUDE_OBJECT_START/END` blocks, so segments are already near-contiguous per object per layer
and runs are typically 1 to 3 per object per layer.

**Shell pass.** Draw the selected object's runs before the lit pass: flat outline color,
position extruded along the decoded vertex normal, front faces culled, depth test **on** so the
shell is correctly occluded by objects in front. The lit pass then paints the real surface over
the shell interior, leaving a rim. Reuses `oct_decode()` from the existing vertex shader; needs
one small program alongside `line_program_`. Cull-face and depth state must be saved and
restored, the way `render_brackets_3d()` brackets its own `glDisable(GL_DEPTH_TEST)`.

**Cached-state key.** `CachedRenderState` (`gcode_gles_renderer.h:302-320`) already keys on
`highlight_count` and `highlight_set_hash`, so repeat frames with an unchanged selection still
hit `draw_cached_to_lvgl()` and do no GPU work.

### 3. Degradation

Correcting an error in the previous draft: there is no constrained-device GLES path to degrade,
because the constrained devices do not build the GLES renderer at all. GLES targets are Pi and
desktop only.

- If the run count for a file would exceed 64k runs, skip span construction and leave GLES 3D
  at brackets-only. Logged at `debug`.
- Files with no exclude-object metadata allocate no span table.
- GLES 3D only runs on geometry budget tiers 1 to 3; tier 4 and above already falls back to 2D
  (`geometry_budget_manager.cpp:48-112`).
- The halo path has no tiering. It is a second line draw over a subset of segments.

---

## DRY consolidation

Scoped to what this feature touches, following the branch's established convention of small
focused units (`include/bed_coord_mapper.h` + `src/ui/bed_coord_mapper.cpp`, header-only
`include/bed_dimensions.h`).

**DRY-1. `include/gcode_selection_style.h`** (header-only, ~60 lines). The single place the
selection product decision lives, consumed by all three renderers.
`struct SegmentStyle { uint32_t rgb; uint8_t opa; int width; }`, the excluded / highlighted /
bracket / halo constants with both `uint32_t` and `glm::vec4` accessors, `bracket_arm_length(const AABB&)`,
and `SegmentStyle resolve(bool excluded, bool highlighted, bool extrusion)`. Collapses the five
divergent copies in the table above, and the bracket sizing formula currently duplicated between
`gcode_layer_renderer.cpp:1485` and `gcode_gles_renderer.cpp:1942` with each commenting that it
matches the other. Also resolves the bracket color being `0xC0C0C0` in 2D and `0.75f` (= `0xBF`)
in 3D under a comment claiming they match.

**DRY-2. `include/gcode_selection_state.h` + `src/rendering/gcode_selection_state.cpp`** (~50 + ~50).
Owns the excluded and highlighted sets, the memoized hash (currently only the GLES renderer
memoizes, `gcode_gles_renderer.cpp:1653-1665`; the layer renderer does full set comparison at
`:280` and `:291`, and `ui_gcode_viewer.cpp:2292` adds a third guard for excluded only),
`classify(name)` returning `const std::string&`, and `apply(names) -> InvalidationScope`.

Two efficiency wins fall out, which matters given the constraint on this work:
- `resolve_object_name()` returns `std::string` **by value** (`gcode_layer_renderer.cpp:1322`)
  while four call sites bind it to `const std::string&`. Correct via lifetime extension, but it
  allocates a string per segment per frame on the hot cache path. Returning a reference into the
  intern table removes that.
- A highlight change currently calls `invalidate_cache()`, which clears the **ghost** cache too
  (`:533-550`) and restarts a multi-second background ghost render, even though the ghost pass
  never renders highlight. An invalidation scope lets highlight stop doing that.

**DRY-3. `include/gcode_raster.h` + `src/rendering/gcode_raster.cpp`** (~60 + ~120).
`struct RasterTarget { uint8_t* data; size_t stride; int w, h; }` with `blend`, `blend_alpha`,
`line_bresenham`, `line_aa`, and one `thick_line`. Required by the feature: the halo needs a
thick-line primitive, and there are currently four near-identical rasterizers plus three
`blend_pixel` variants in `gcode_layer_renderer.cpp`. Reported drift to confirm during
implementation: the Bresenham and AA offset loops appear to use different width formulas
(`width` vs `width+1` lines for even widths), which would mean SSAO-on draws every extrusion a
pixel wider than SSAO-off, and `MIN_LINE_LENGTH` appears shadowed by a local `0.5f` against the
file-scope `0.001f`. Both need verifying before being treated as facts.

**DRY-4. Grow `include/gcode_projection.h`** rather than adding a unit; it already is the shared
projection boundary and is already used by the layer renderer, the GLES renderer and the
thumbnail renderer. Add `project_clip_to_screen(mvp, vec3, w, h) -> optional<vec2>`,
`point_segment_distance(p, a, b)`, and `PICK_THRESHOLD_PX`. Consolidates three clip-to-screen
implementations with three different degenerate-`w` predicates (`== 0`, `abs < 1e-4`, `<= 1e-4`),
three copies of the nearest-point-on-segment pick math, and `PICK_THRESHOLD_PX` defined three
times.

**DRY-5. `include/lv_draw_buf_guard.h`** (header-only, ~50). A `SafeDrawBuf` wrapper for the
`lv_is_initialized()` -> `lv_draw_wait_for_finish()` -> `lv_draw_buf_destroy()` sequence, which
is the #929 use-after-free invariant currently maintained by copy-paste across four sites
(`gcode_layer_renderer.cpp:508`, `:571`, `:893`, and `gcode_gles_renderer.cpp:306`/`:1422`),
with only one of them emitting crash breadcrumbs. Also folds the three `ensure_*_cache` and
three `blit_*_cache` copies. This is a crash-safety invariant held together by discipline, which
is the strongest argument on the list for extracting it.

**DRY-6. Two `AABB` additions in `include/gcode_parser.h`**, next to the existing
`for_each_bracket_arm`: `default_plate_bbox()` for the `{0,0,0}..{200,200,0}` empty-bbox fallback
duplicated four times, and `expand_by(float)` for the tube-width expansion that exists twice with
drifted constants (`max_tube_width * 1.5f` at `gcode_geometry_builder.cpp:520` vs
`tube_width * 0.5f * 1.41421356f` at `:689`, both commented as sqrt(2)). The constant
disagreement needs a decision, not just a helper.

**DRY-7. Move `GhostRenderMode`** to a shared header. It is defined identically twice in
`namespace helix::gcode` (`include/gcode_gles_renderer.h:28`, `include/gcode_renderer.h:52`) and
only compiles because the two headers are mutually exclusive; any TU including both fails.

### Explicitly not doing

- **Not deleting `src/rendering/gcode_renderer.cpp`.** Verified reachable, and it is the 3D
  renderer on the entire printer fleet. See the platform matrix.
- **Verify-then-delete** `GeometryBuilder::set_highlighted_objects`, `highlighted_objects_`, and
  the `HIGHLIGHT_BRIGHTNESS` block (`gcode_geometry_builder.cpp:948-962`,
  `include/gcode_geometry_builder.h:468-476`, `:582`). Reported as having no callers. Confirm
  with a clean tree-wide search before removing, not on report alone.
- **Not touching** `AABB::for_each_bracket_arm` or `helix::gcode::project` (already correctly
  shared), the deliberate `local_resolve_name` / `local_should_render` lambda duplication in the
  ghost thread (`gcode_layer_renderer.cpp:1643-1663`, a documented thread-safety snapshot), or
  the SDL/EGL context guards (mutually exclusive platform code).
- **Not consolidating** the four representations of the default teal (`0x26A69A` hex,
  `"#26A69A"` string, split bytes, and rounded normalized floats). Real duplication, but outside
  what this feature touches.

---

## Efficiency and memory budget

"Efficiency and memory can't take a huge hit" is a stated constraint, so:

| Dimension | Impact |
|-----------|--------|
| `PackedVertex` size | unchanged, 12 bytes |
| VBO memory | unchanged, no per-vertex object id |
| GLES main lit pass | untouched, still one `glDrawArrays` per layer |
| unselected case, all renderers | zero, halo and shell both gated on a non-empty selection |
| 2D isometric, selected | one extra thick line per segment of the selected object only |
| 3D CPU wireframe, selected | same, one extra `lv_draw_line` per segment of that object |
| GLES per real frame | one pass over the selected object's triangles |
| GLES repeat frames | zero, existing frame-skip blits the cached image |
| new heap | span table only, GLES targets only |

Span table in proportion: 6 bytes per object-run against `GeometryBudgetManager`'s estimate of
300 to 1300 bytes **per segment** (`geometry_budget_manager.h:12`). Worst realistic plate, 30
objects across 1000 layers at one run each, is roughly 180KB indexing a geometry allocation
measured in tens of MB, with the 64k-run guard as a hard ceiling. It exists only on Pi and
desktop, never on a printer.

**The DRY work is net-positive on performance**, which is the argument for doing it alongside
rather than after: DRY-2 removes a per-segment-per-frame `std::string` allocation on the hot
cache path, and stops a highlight change from invalidating the ghost cache and restarting a
multi-second background render.

Rejected on efficiency grounds: the stencil plus screen-space dilate variant for GLES. Uniform
pixel-width outlines, but it needs a stencil renderbuffer we do not allocate (color RGBA8 plus
`GL_DEPTH_COMPONENT16` only, `gcode_gles_renderer.cpp:647-656`) or a second FBO, plus a
full-viewport post pass on every selection change, on hardware where we already maintain a
`GL_RENDERER` denylist for `panfrost` and a persistent crash-loop guard (#966, #1084, #1085).

---

## Out of scope

- **Excluded-object visuals in GLES 3D.** Excluding an object changes nothing on screen there
  today. Real gap, and the span work is its prerequisite, but dimming excluded objects means
  splitting the main lit pass per layer on every frame for the rest of the print, which is
  exactly the hot-path cost this work is constrained to protect. Separate change. (The
  constrained-device tint in the previous draft would have split that draw for a transient
  single selection; excluded dimming is persistent and multi-object, which is why they differ.)
- **`ExcludeObjectMapView`**, the top-down plan map. It discards the excluded and current-object
  distinction once its canvas exists (`ui_exclude_object_map_view.cpp:659` on main). Note this
  file already differs on `devel/1.1`, where `CoordMapper` was extracted to
  `include/bed_coord_mapper.h` and bed defaults centralized into `bed_dimensions_from_volume()`.
  Re-read it on the branch before touching.
- **Corner brackets.** Geometry unchanged in all views: 24 arms via
  `AABB::for_each_bracket_arm()` (`gcode_parser.h:88`), `min(0.2 * shortest_edge, 5mm)`. Only
  their color constant moves, into DRY-1.
- **Two definitions of "is support"** in `gcode_layer_renderer.cpp`: a case-insensitive object
  *name* substring match (`:1336-1361`) versus `FeatureType::Support` (`:88`). One question,
  two answers, but not this change.

---

## Testing

Unit, and each must fail if the behavior is removed:

- `resolve()` in DRY-1 returns the documented style for each of the six
  excluded/highlighted/extrusion/travel combinations, and both renderers consume it (no
  renderer-local color constants left).
- `bracket_arm_length()` agrees with what both bracket emitters previously computed, including
  the degenerate `< 0.01f` guard and the empty-bbox case that 2D currently does not check
  (`gcode_layer_renderer.cpp:1477`).
- `GeometryBuilder` span construction: runs cover exactly that object's vertices, no gaps, no
  overlap, offsets in range for their layer VBO. Multi-object fixture with interleaved
  `EXCLUDE_OBJECT_START/END` per layer.
- Run splitting past the `uint16` count range; the 64k-run guard leaves spans empty; no span
  table for a file without exclude-object metadata.
- Halo emits nothing when the highlight set is empty, and emits only for the selected object's
  segments, in **both** the Bresenham cache path and the `lv_draw_line` path.
- DRY-2: `classify()` returns a reference, not a copy (a test that would fail on the current
  by-value signature). A highlight change does **not** invalidate the ghost cache; an exclusion
  change still does.
- DRY-3: `thick_line` produces the same pixel width in the AA and non-AA paths for both even and
  odd widths. This is the SSAO/non-SSAO mismatch, written as a test rather than trusted.

Visual, per the platform matrix, because a desktop-only check would miss the printers entirely:

- GLES build (`x86`): `demo print-status`, then `helix-screen ctl` to select an object. Verify
  against `stand_s.gcode` (the concave S is the whole point) and `calicat_calico.gcode`.
- Non-GLES build (`x86-fbdev`) with render mode forced to 3D via `--render-3d`, exercising
  `GCodeRenderer`. This is the path that ships to AD5M, AD5X, CC1, K1C, K2 and U1.
- Smallest panel, 480x272, for halo legibility.

Judge geometry with `ctl geom` where possible; the outline itself needs eyes. Pin the socket and
config dir per the project's parallel-instance rules.

Keep green: `test_gcode_parser.cpp`, the exclude-object suites in `docs/devel/EXCLUDE_OBJECTS.md`,
and the interface drift tests.

---

## Risks

- **Cap normals** (GLES). Tube cross-section normals are radial and extrude cleanly
  (`gcode_geometry_builder.cpp:1024-1041`), but segment cap normals are `-dir` and may spike.
  Clamp the extrusion, or exclude cap vertices from the shell pass.
- **`tube_sides` = 4** on budget tier 3: each strip is a single planar face
  (`gcode_geometry_builder.h:203-209`), so a normal-extruded shell looks chunkier. Verify at
  tier 3, not only tier 1.
- **GL state leakage.** The shell pass changes cull face and depth state mid-frame; anything
  unrestored corrupts later passes including brackets.
- **DRY-3 is a real refactor, not a move.** Unifying the rasterizers changes pixel output if the
  two width formulas currently disagree. That is the point, but it will move screenshots and
  needs to be landed as its own reviewable commit.
- **Halo legibility at 480x272** needs a check on the real smallest panel, not desktop SDL.
- **Unverified claims carried from the duplication inventory** (the SSAO width mismatch, the
  shadowed `MIN_LINE_LENGTH`, the dead `set_highlighted_objects`). Treated as leads to confirm,
  not findings. The same inventory asserted `gcode_renderer.cpp` was unreachable in every build
  and recommended deleting it as the lowest-risk item on the list; that was wrong, and it would
  have removed 3D rendering from every printer we ship.

---

## Files expected to change

| File | Change |
|------|--------|
| `include/gcode_selection_style.h` | new, DRY-1 |
| `include/gcode_selection_state.h` + `src/rendering/gcode_selection_state.cpp` | new, DRY-2 |
| `include/gcode_raster.h` + `src/rendering/gcode_raster.cpp` | new, DRY-3 |
| `include/lv_draw_buf_guard.h` | new, DRY-5 |
| `include/gcode_projection.h`, `src/rendering/gcode_projection.cpp` | DRY-4 additions |
| `include/gcode_parser.h` | DRY-6 `AABB` additions |
| `src/rendering/gcode_layer_renderer.cpp` | halo in the Bresenham cache path and the `lv_draw_line` path, adopt DRY-1..5 |
| `src/rendering/gcode_renderer.cpp` | halo in the CPU wireframe 3D path, adopt DRY-1/4/7 |
| `src/rendering/gcode_gles_renderer.cpp`, `include/gcode_gles_renderer.h` | shell shader and pass, adopt DRY-1/2/5/7 |
| `src/rendering/gcode_geometry_builder.cpp`, `include/gcode_geometry_builder.h` | object runs, remove dead highlight bake |
| `ui_xml/globals.xml` | `selection_outline` token |
| `docs/devel/EXCLUDE_OBJECTS.md` | correct the visual-states table: it documents the `lv_draw_line` widths as the isometric behavior, and claims a "brightened color" for a GLES path that does nothing |
| `tests/unit/` | per the testing section |

## Sequencing

Land as separate reviewable commits, in this order, so a pixel-affecting refactor is never
mixed with a feature:

1. DRY-1 and DRY-2 (pure consolidation, no visual change intended)
2. DRY-3 (may change pixel output; own commit)
3. DRY-4 through DRY-7
4. Halo in the 2D isometric renderer
5. Halo in the CPU wireframe 3D renderer
6. GLES object runs, then the shell pass
7. Doc corrections

MAJOR work by the project's classification, so it goes in a worktree. Note
`scripts/setup-worktree.sh` runs `git worktree add -b "$BRANCH"` with **no start point**
(`:227`), so it would branch from the main tree's HEAD. Create the branch off `devel/1.1`
explicitly first:

```
git branch feature/gcode-selection-silhouette devel/1.1
scripts/setup-worktree.sh feature/gcode-selection-silhouette
```
