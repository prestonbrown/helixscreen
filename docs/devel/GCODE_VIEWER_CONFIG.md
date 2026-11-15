# G-code Viewer Configuration

Configuration knobs for the G-code viewer. For the pipeline these settings sit inside - render-mode selection and precedence, streaming vs full parse, downloads and caches - see [the G-code pipeline chapter](architecture/16-gcode-pipeline.md); for runtime env overrides (`HELIX_GCODE_MODE`, `HELIX_GCODE_STREAMING`) see [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).

Everything below is read at runtime and verified against the code. Config keys
live under `gcode_viewer` in `settings.json` unless noted; the viewer loads them
once at startup, so changes need a restart.

There is no shading-model key: shading is controlled by `HELIX_SSAO` and the
device tier, below.

## Which renderer draws

`display/gcode_render_mode` (a **display** key, not a `gcode_viewer` one) picks
the renderer:

| Value | Mode | Notes |
|-------|------|-------|
| 0 | Auto | 3D where GLES is available, 2D otherwise. Default. On an SDL build "available" is decided at runtime: the renderer resolves GLES2 from the SDL context, and a machine without it gets 2D. |
| 1 | 3D | Only offered on `ENABLE_GLES_3D` builds |
| 2 | 2D Layers | Software rasterizer, every platform |
| 3 | Thumbnail Only | No toolpath rendering at all |

On a build without GLES there is no 3D mode at all, and the settings dropdown
omits the option. A stored value of `1` from a device that does have GLES
degrades to 2D rather than reaching anything.

`HELIX_GCODE_MODE=3D|2D` overrides for one run. `3D` on a build without a 3D
renderer falls back to 2D and says so in the log.

### Auto drops to the thumbnail when a preview would cost too much

In Auto, and only there, a streamed preview also has to be affordable in CPU on
this board, because on a printer's own board that CPU comes out of Klipper's
cores. The projected cost is one index scan of the file plus a ghost pass of
about the same size (`include/gcode_preview_cost.h`), against a budget of 15
CPU-seconds with the printer idle and 5 while a print runs. Over budget, the
slicer thumbnail is shown instead, silently: no toast, a single info line in the
log. An explicit 2D or 3D choice is the user's and is never overridden.

The projection needs this board's index-scan speed, in MB of G-code per CPU
second. It is learned from the first idle scan and stored at
`display/gcode_preview_rate` with the board's fingerprint and the app version;
a stored rate from another board or version is ignored. From then on a file is
judged before anything is downloaded (`helix::ui::preview_gate()`). Before a rate
is learned, or with no rate for the current version, the scan judges itself from
its first 2% and a quarter-second of CPU and stops early when the projection is
over budget. A CC1 scans about 10 MB/cpu-s: a 41 MB file previews when idle and
shows the thumbnail mid-print.

## Shading, and what it costs

Two things travel under the name "enhanced shading", and they are priced very
differently:

| | What it does | Cost |
|---|---|---|
| Outline pass | Darkens the silhouette edge, plus cheap normal-based shading | ~2 ms per cache revalidation, measured on an AD5M |
| Antialiasing | Wu's algorithm instead of Bresenham for every stroke | **~6.1x** the aliased rasterization cost |

They are separate flags because tying them together forced a constrained device
to choose between looking flat and building slowly.

**Defaults by tier:**

| Device | Outline | Antialiasing |
|--------|---------|--------------|
| Unconstrained | on | on |
| Constrained (`MemoryInfo::is_constrained_device()`) | **on** | **off** |

`HELIX_SSAO` overrides both together, so a forced comparison stays honest:

| Value | Effect |
|-------|--------|
| `1` | Both on, whatever the tier says |
| `0` | Both off |
| unset / anything else | Tier default |

Only the exact strings `0` and `1` are honoured. Confirm which way it went from
the log line at viewer init (`enhanced shading`, `outline shading on,
antialiasing off`, or `HELIX_SSAO=...`).

For scale, on the 135,197-segment test plate the rasterizer spends roughly 29 ms
aliased against 178 ms antialiased, and the adaptive controller settles at 49
layers per frame instead of 22 - so a preview appears about 2.2x faster with
antialiasing off.

## Geometry (3D only)

### `tube_sides`
**Type:** int **Default:** 4 **Values:** 4, 8, 16
Cross-section of the extruded tube. 4 is a rectangle and the cheapest; 8 and 16
are elliptical, 16 matching OrcaSlicer. Anything else logs a warning and falls
back to 16. In practice the geometry budget tier decides: the viewer's 3D build
always hands the builder the tier's side count (16, 8 or 4, see
[the G-code pipeline chapter](architecture/16-gcode-pipeline.md)), which
replaces this value whatever it is.

The tier ladder gates on memory first, and on slow GPUs also on triangle count:
`select_tier()` (`src/rendering/geometry_budget_manager.cpp#select_tier`)
takes a `max_triangles` cap supplied by `gpu_triangle_budget()`
(`include/gcode_gl_fallback.h#gpu_triangle_budget`), which keys off the kernel
driver behind the DRM render node. A tier whose estimated triangles
(`TRIS_PER_SEG_*`, calibrated on a Pi 3B with a 3DBenchy) exceed the cap is
skipped, so on vc4 (VideoCore IV, Pi 0-3, ~4M tris/s) a 1M-triangle budget
demotes heavy files to coarser tubes or the 2D fallback instead of drawing
multi-second frames that wedge the GPU until reboot. Pi 4/5 render through v3d
and are uncapped.

## Streaming and pacing (2D)

### `streaming_mode`
**Type:** string **Default:** `"auto"`
Whether large files stream layer-by-layer instead of loading whole.

### `streaming_threshold_percent`
**Type:** int **Default:** 40
Share of available memory a file may occupy before streaming engages. Capped at
15 on boards with 2GB total RAM or less.

### `layers_per_frame`
**Type:** int **Default:** 0 (adaptive)
How many layers the progressive cache build draws per frame. `0` hands the
decision to the adaptive controller, which is almost always what you want -
it is also the thing that reacts to antialiasing being on.

### `adaptive_layer_target_ms`
**Type:** int **Default:** 16
Per-frame budget the adaptive controller aims at. Raising it builds the preview
sooner at the cost of frame smoothness.

## Memory

Both renderers report their own footprint, itemized, at debug level whenever
their buffer set changes:

```
[GCodeLayerRenderer] memory after solid cache created: 1243 KB total
    (solid_cache 427, ghost_cache 427, ghost_raw 383, ssao_undo 8)
```

Useful when deciding whether a device tier can afford something. Note that
process RSS cannot answer that question: cross-run RSS carries megabytes of
noise, and `free()` does not lower it without `malloc_trim`.

## Measuring preview cost on a board

`helix-screen bench-gcode <file> [width height]` times what a streamed preview of
that file costs here, without the UI: the layer index scan, the renderer's own
ghost pass (fit, sampling, parse and raster) on a canvas of that size (default
320x240, about a CC1's print-status preview), and single mid-file layer parses.
It prints wall and CPU time for each. It reads no config and takes no lock, so it
runs beside a live instance; keep the file on real storage, since `/tmp` is RAM
on most boards. `HELIX_BENCH_REPEAT=N` repeats the ghost pass, cold each time,
so a profiler can catch it; `HELIX_BENCH_VERBOSE=1` adds the renderer's debug
lines (fit, sampled layer count); `HELIX_BENCH_GHOST_PNG=<path>` writes the ghost
as the print-status screen blends it, for comparing sampling changes by eye.

## Example

```json
{
  "gcode_viewer": {
    "tube_sides": 8,
    "streaming_mode": "auto",
    "streaming_threshold_percent": 40,
    "layers_per_frame": 0,
    "adaptive_layer_target_ms": 16
  }
}
```
