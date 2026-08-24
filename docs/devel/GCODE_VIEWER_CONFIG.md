# G-code Viewer Configuration

Configuration knobs for the G-code viewer. For the pipeline these settings sit inside — render-mode selection and precedence, streaming vs full parse, downloads and caches — see [the G-code pipeline chapter](architecture/16-gcode-pipeline.md); for runtime env overrides (`HELIX_GCODE_MODE`, `HELIX_GCODE_STREAMING`) see [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md).

## Shading Model

> **Status note (v0.99.115):** this key is currently **inert**. It is written into the config defaults (`src/system/config.cpp` — default `"phong"`) and the settings template, but no code reads it: the GLES renderer always renders per-pixel Phong with a camera-following light (`src/rendering/gcode_gles_renderer.cpp`), and `set_smooth_shading()` is an uncalled stub. Editing `shading_model` in `settings.json` has no effect on the current build. The section below is kept as the reference for what the values mean, for whenever the switch is wired up again.

The G-code 3D viewer supports three shading models for rendering extrusion paths. Configure via `settings.json`:

```json
{
  "gcode_viewer": {
    "shading_model": "phong"
  }
}
```

### Shading Options

| Model | Description | Visual Quality | Performance |
|-------|-------------|----------------|-------------|
| `flat` | Uniform lighting per face, faceted appearance | Low (sharp edges visible) | Best (lowest cost) |
| `smooth` | Gouraud shading - per-vertex lighting interpolated across faces | Medium | Good |
| `phong` | Per-pixel lighting with smooth gradients | High (default) | Good (negligible overhead) |

### Recommendations

- **Default: `phong`** - Provides the best visual quality with smooth gradients that clearly show the 3D tube geometry. Performance impact is negligible on modern hardware.

- **Use `flat`** - For debugging geometry (clearly shows face boundaries) or on extremely constrained hardware.

- **Use `smooth`** - Not recommended for current diamond tube implementation. Due to per-face vertex structure, smooth shading produces identical results to flat shading.

### Technical Notes

The current diamond cross-section implementation uses separate vertices for each face (not shared) to enable per-face coloring in debug mode. This means:

- Each face has 4 vertices with identical normals
- `smooth` (Gouraud) shading interpolates between identical normals, producing the same result as `flat`
- `phong` (per-pixel) shading provides gradients across each face for better depth perception

### Configuration Location

- **Template**: `config/settings.json.template` (note: the template ships `"smooth"` while the code default in `src/system/config.cpp` is `"phong"` — an unrelated template inconsistency)
- **Runtime**: the config directory's `settings.json` (on device `~/helixscreen/config/`; `--test` runs use `settings-test.json` from the same directory)
- **Code**: defaults are seeded in `src/system/config.cpp`

### Example

To switch to flat shading for debugging:

1. Edit your settings.json:
   ```json
   {
     "gcode_viewer": {
       "shading_model": "flat"
     }
   }
   ```

2. Restart the application (config is loaded once at startup)

3. The G-code viewer will now use flat shading with clearly visible face boundaries
