# G-code Viewer Configuration

## Shading Model

The G-code 3D viewer supports three shading models for rendering extrusion paths. Configure via `helixconfig.json`:

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

- **Template**: `config/helixconfig.json.template`
- **Runtime**: Config loaded from path specified at startup (typically `/tmp/helixconfig.json` in development)
- **Code**: Setting is read in `src/gcode_tinygl_renderer.cpp` initialization

### Example

To switch to flat shading for debugging:

1. Edit your helixconfig.json:
   ```json
   {
     "gcode_viewer": {
       "shading_model": "flat"
     }
   }
   ```

2. Restart the application (config is loaded once at startup)

3. The G-code viewer will now use flat shading with clearly visible face boundaries
