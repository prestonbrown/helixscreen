# G-code Viewer Configuration

Configuration knobs for the G-code viewer. For the pipeline these settings sit inside — render-mode selection and precedence, streaming vs full parse, downloads and caches — see [the G-code pipeline chapter](architecture/16-gcode-pipeline.md); for runtime env overrides (`HELIX_GCODE_MODE`, `HELIX_GCODE_STREAMING`) see [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).

Everything below is read at runtime and verified against the code. Config keys
live under `gcode_viewer` in `settings.json` unless noted; the viewer loads them
once at startup, so changes need a restart.

> **`shading_model` is gone.** It was documented here as a three-way choice
> between `flat`, `smooth` and `phong`, with a comparison table and a
> recommendation per device tier. Nothing ever read it. It was seeded into
> `settings.json.template` as `"smooth"` and into `config.cpp`'s defaults as
> `"phong"` — two files disagreeing about the default of a key that had no
> effect either way, which is what gave it away. The setters behind it
> (`GCodeGLESRenderer::set_smooth_shading()`, which ignored its argument, and
> `GeometryBuilder::set_smooth_shading()`) had no callers. Removed along with the
> key. Shading is controlled by `HELIX_SSAO` and the device tier, below.

## Which renderer draws

`display/gcode_render_mode` (a **display** key, not a `gcode_viewer` one) picks
the renderer:

| Value | Mode | Notes |
|-------|------|-------|
| 0 | Auto | 3D where GLES is available, 2D otherwise. Default. |
| 1 | 3D | Only offered on `ENABLE_GLES_3D` builds |
| 2 | 2D Layers | Software rasterizer, every platform |
| 3 | Thumbnail Only | No toolpath rendering at all |

On a build without GLES there is no 3D mode at all, and the settings dropdown
omits the option. A stored value of `1` from a device that does have GLES
degrades to 2D rather than reaching anything.

`HELIX_GCODE_MODE=3D|2D` overrides for one run. `3D` on a build without a 3D
renderer falls back to 2D and says so in the log.

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
layers per frame instead of 22 — so a preview appears about 2.2x faster with
antialiasing off.

## Geometry (3D only)

### `tube_sides`
**Type:** int **Default:** 4 **Values:** 4, 8, 16
Cross-section of the extruded tube. 4 is a diamond and the cheapest; 16 is
circular and matches OrcaSlicer. Anything else logs a warning and falls back to
16. The geometry budget tier can override this downward on constrained devices.

## Streaming and pacing (2D)

### `streaming_mode`
**Type:** string **Default:** `"auto"`
Whether large files stream layer-by-layer instead of loading whole.

### `streaming_threshold_percent`
**Type:** int **Default:** 40
Share of available memory a file may occupy before streaming engages.

### `layers_per_frame`
**Type:** int **Default:** 0 (adaptive)
How many layers the progressive cache build draws per frame. `0` hands the
decision to the adaptive controller, which is almost always what you want —
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
