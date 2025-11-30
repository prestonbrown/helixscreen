# GuppyScreen Bed Mesh Renderer - Comprehensive Analysis

## Executive Summary

GuppyScreen implements a complete 3D bed mesh visualization system using LVGL 9 canvas drawing primitives. The renderer uses classic computer graphics algorithms: perspective projection, painter's algorithm (depth sorting), scanline triangle rasterization with gradient interpolation, and a scientific heat-map color scheme. All 3D math is hand-coded using double-precision floating-point with no external graphics library dependency.

**Key Achievement:** Enables interactive 3D visualization of bed mesh height maps on embedded framebuffer displays without GPU acceleration.

---

## File Locations & Line Ranges

### Main Implementation Files
- **Header**: `/Users/pbrown/code/guppyscreen/src/bedmesh_panel.h` (lines 1-284)
- **Implementation**: `/Users/pbrown/code/guppyscreen/src/bedmesh_panel.cpp` (2250 lines total)

### Key Algorithm Locations in bedmesh_panel.cpp:

| Algorithm | Lines | Purpose |
|-----------|-------|---------|
| Perspective Projection | 1016-1063 | Convert 3D world coords → 2D screen coords with rotation |
| Triangle Rasterization (Solid) | 1066-1127 | Scanline fill algorithm for flat-colored triangles |
| Triangle Rasterization (Gradient) | 1130-1267 | Scanline fill with RGB color interpolation |
| Quad Generation | 1270-1330 | Create 3D quads from mesh grid with colors |
| Depth Sorting (Z-Buffer) | 1333-1339 | Sort quads back-to-front for painter's algorithm |
| Quad Rendering | 1342-1392 | Project & render single quad with dual triangles |
| Main 3D Render Loop | 1394-1491 | Core orchestration: clear → project → sort → render |
| Wireframe Grid | 794-832 | Draw mesh grid lines after projection |
| Axes & Labels | 834-1013 | Draw coordinate system + grid reference planes |
| Color Gradient Band | 1733-1810 | Draw left-side height→color legend |
| FOV Scale Calculation | 1869-1891 | Compute perspective scale to fit mesh in canvas |
| Mesh Callback (Table) | 685-704 | Color the table view cells by height |
| Canvas Drag Handling | 1893+ | Interactive rotation control |
| Color Mapping (Heat Map) | 1578-1678 | Map height values to purple→blue→cyan→yellow→red spectrum |

---

## Data Structures

### 3D Mesh Representation

#### Point3D (header lines 43-47)
```cpp
struct Point3D {
  double x, y, z;           // 3D world coordinates
  int screen_x, screen_y;   // 2D screen coordinates after perspective projection
  double depth;             // Z-depth for sorting (closer objects rendered last)
};
```
**Usage**: Represents a single mesh vertex after projection to screen space

#### Vertex3D (header lines 50-53)
```cpp
struct Vertex3D {
  double x, y, z;           // 3D position in world space
  lv_color_t color;         // Color at this vertex (for gradient blending)
};
```
**Usage**: 3D vertex with pre-computed color for gradient interpolation

#### Quad3D (header lines 56-60)
```cpp
struct Quad3D {
  Vertex3D vertices[4];     // Four corners: [0]=BL, [1]=BR, [2]=TL, [3]=TR
  double avg_depth;         // Average depth for back-to-front sorting
  lv_color_t center_color;  // Fallback solid color during fast rendering
};
```
**Usage**: Represents one mesh cell (4 vertices) with color information

#### Mesh Data Storage (header line 258)
```cpp
std::vector<std::vector<double>> mesh;  // mesh[row][col] = Z height value
```
**Layout**: 
- Rows = Y-axis (front to back)
- Cols = X-axis (left to right)
- Values are absolute Z heights from printer bed

---

## 3D Rendering Pipeline Flow

### Stage 1: Data Initialization
```
Input: JSON mesh data from Moonraker
  ↓
Parser extracts: dimensions, Z min/max, 2D mesh array
  ↓
Store in: BedMeshPanel::mesh (std::vector<std::vector<double>>)
```

### Stage 2: Projection Setup (draw_3d_mesh, line 1394)
```
1. Clear canvas (line 1416)
2. Find Z min/max for color mapping (lines 1419-1425)
3. Compute dynamic Z scaling (lines 1427-1433)
   - Z-scale = DEFAULT_Z_TARGET_HEIGHT / z_range
   - Clamp to [35.0, 120.0] to prevent extreme projections
4. Compute FOV scale (line 1436)
   - Scale perspective view to fit mesh in canvas
   - Formula: fov_scale = (available_pixels × CAMERA_DISTANCE) / mesh_diagonal
```

### Stage 3: 3D Quad Generation (generate_mesh_quads, lines 1270-1330)
```
For each mesh cell (row, col):
  1. Calculate world position:
     - base_x = (col - cols/2) × MESH_SCALE
     - base_y = (rows-1-row - rows/2) × MESH_SCALE
     - (Centered around origin, with inverted Y for row indexing)
     
  2. Create 4 vertices with Z values:
     - quad.vertices[i].z = (mesh[row][col] - z_center) × z_scale
     - (Centered around z=0 for rotation)
     
  3. Compute vertex colors:
     - color = color_gradient_enhanced(height_value, min_z, max_z)
     - Uses scientific heat-map: purple(low) → red(high)
     
  4. Set fallback center color for fast rendering
  
  Result: std::vector<Quad3D> with all mesh cells
```

### Stage 4: Depth Calculation
```
For each quad:
  For each vertex (i=0 to 3):
    projected[i] = project_3d_to_2d(vertex position)
    total_depth += projected[i].depth
  
  quad.avg_depth = total_depth / 4.0
  
(Prepares for back-to-front sorting)
```

### Stage 5: Point Grid Creation
```
Create matching 2D point_grid[row][col] with all vertices projected
(Used for wireframe + axis rendering)
```

### Stage 6: Rendering Layers (bottom to top)
```
1. Draw axes & reference planes (line 1472)
   - 3D coordinate system (X, Y, Z axes)
   - XY plane grid at bottom
   - XZ & YZ vertical planes

2. Sort quads by depth (line 1475)
   - Painter's algorithm: furthest first (largest avg_depth first)

3. Render quads in sorted order (lines 1478-1480)
   - For each quad: draw_3d_quad()
   - Switches between solid color (during drag) and gradient (static)

4. Draw wireframe grid (line 1483)
   - Mesh connectivity lines on top of surface

5. Draw color gradient legend (line 1486)
   - Vertical color bar showing height → color mapping
```

---

## 3D Math Primitives

### Perspective Projection (lines 1016-1063)

**Input**: (x, y, z) in world space
**Output**: Point3D with screen coordinates

**Algorithm**:
```
Step 1: Z-axis rotation (user controlled)
  z_angle = current_view_angle × π/180
  rotated_x = x×cos(z_angle) - y×sin(z_angle)
  rotated_y = x×sin(z_angle) + y×cos(z_angle)
  rotated_z = z × z_display_scale

Step 2: X-axis rotation (tilt up/down)
  x_angle = current_x_angle × π/180
  final_x = rotated_x
  final_y = rotated_y×cos(x_angle) + rotated_z×sin(x_angle)
  final_z = rotated_y×sin(x_angle) - rotated_z×cos(x_angle)

Step 3: Translate camera back
  final_z += camera_distance  (typically 450.0)

Step 4: Perspective projection (similar triangles)
  perspective_x = (final_x × fov_scale) / final_z
  perspective_y = (final_y × fov_scale) / final_z

Step 5: Convert to screen coordinates
  screen_x = (canvas_width - GRADIENT_BAND_TOTAL_WIDTH)/2 + perspective_x
  screen_y = canvas_height × Z_ORIGIN_VERTICAL_POSITION + perspective_y

Result: Point3D with screen_x, screen_y, and depth = final_z
```

**Constants** (header lines 120-124):
- `CAMERA_DISTANCE = 450.0` — Distance of camera from origin
- `VIEW_ANGLE_X_DEGREES = -85.0` — Initial tilt angle
- `VIEW_ANGLE_Z_DEGREES = 10.0` — Initial spin angle
- `Z_ORIGIN_VERTICAL_POSITION = 0.4` — Canvas Y position for Z=0 plane

### Rotation Matrices

Uses explicit Euler angle rotation (Z then X):
- **Z rotation**: Spin around vertical axis (controls horizontal panning)
- **X rotation**: Tilt front-to-back (controls vertical perspective)
- No Y rotation (keeps bed upright)

---

## Triangle Rasterization

### Solid Color Triangle (lines 1066-1127)

**Algorithm**: Scanline fill
```
Input: 3 vertices (x1,y1), (x2,y2), (x3,y3) + color

Step 1: Sort vertices by Y coordinate (top to bottom)
  y1 ≤ y2 ≤ y3

Step 2: For each scanline from y1 to y3:
  Calculate left & right X edges using linear interpolation:
  
  If y < y2 (upper half):
    x_left = x1 + (x2-x1) × (y-y1)/(y2-y1)     [edge v0→v1]
    x_right = x1 + (x3-x1) × (y-y1)/(y3-y1)    [edge v0→v2]
  
  Else (lower half):
    x_left = x2 + (x3-x2) × (y-y2)/(y3-y2)     [edge v1→v2]
    x_right = x1 + (x3-x1) × (y-y1)/(y3-y1)    [edge v0→v2]

Step 3: Draw horizontal line from x_left to x_right
  Uses LVGL's lv_canvas_draw_rect() with 1-pixel height
  Color: solid with 90% opacity
```

**Complexity**: O(height × width) pixels scanned
**Degenerate case**: Skip if y1 == y3 (zero height)

### Gradient Triangle (lines 1130-1267)

**Algorithm**: Scanline fill with per-pixel color interpolation

**Extra features**:
1. **Per-vertex color storage** — Each vertex has RGB color
2. **Barycentric interpolation** — Color interpolated along edges
3. **Performance optimization** — Segments long lines to reduce color computation

```
Step 1: Sort vertices by Y, extract RGB components

Step 2: For each scanline y:
  Calculate left/right edges with parameter t ∈ [0,1]
  
  Interpolate RGB at left/right:
    r_left = r1 + t1 × (r2 - r1)  [along v0→v1]
    r_right = r1 + t2 × (r3 - r1) [along v0→v2]
  
  (Same for green, blue components)

Step 3: For each pixel on scanline:
  If line_width ≤ GRADIENT_MIN_LINE_WIDTH (3px):
    Use average color (fast path)
  Else:
    Divide into GRADIENT_MAX_SEGMENTS (6) segments
    For each segment:
      Interpolate color at segment center
      Draw rectangle with interpolated color

Step 4: Clamp to [0,255] and draw
```

**Key optimization** (line 1233):
```cpp
int segments = std::min(GRADIENT_MAX_SEGMENTS, line_width / 4);
```
- Limits gradient computation to 6 segments max
- Prevents performance bottleneck on large triangles
- Uses division by 4 to scale segment count with line width

---

## Depth Sorting (Z-Buffer / Painter's Algorithm)

### Sorting (lines 1333-1339)

```cpp
std::sort(quads.begin(), quads.end(), [](const Quad3D& a, const Quad3D& b) {
  return a.avg_depth > b.avg_depth;  // Sort DESCENDING (furthest first)
});
```

**Key principle**: Furthest objects rendered first (painter's algorithm)
- Larger depth value = further away
- No per-pixel Z-buffer (expensive on embedded)
- Average depth of 4 vertices used as proxy

### Depth Calculation (lines 1444-1453)

```
For each quad vertex:
  projected = project_3d_to_2d(x, y, z, canvas_width, canvas_height)
  total_depth += projected.depth

quad.avg_depth = total_depth / 4.0
```

Depth = final_z (camera distance component after rotation)

### Performance Impact

- O(n log n) sort for n quads (typically 10-100 quads)
- Prevents transparent areas from showing through
- Essential for visual correctness when mesh rotates

---

## Color Mapping Strategy (Height → Color Gradient)

### Heat Map Specification (lines 1593-1678)

**Function**: `color_gradient_enhanced(offset, min_val, max_val)`

**Color Spectrum** (purple → red):
```
Value Range          Color Transition
─────────────────────────────────────
0.000 - 0.125        Purple (128,0,255) → Blue (0,128,255)
0.125 - 0.375        Blue (0,128,255) → Cyan (0,255,255)
0.375 - 0.625        Cyan (0,255,255) → Yellow (255,255,0)
0.625 - 0.875        Yellow (255,255,0) → Orange-Red (255,64,0)
0.875 - 1.000        Deep Red (255,0,0)
```

### Normalization Process

```
Step 1: Calculate data range
  data_range = max_val - min_val
  
Step 2: Apply compression for enhanced contrast
  adjusted_range = data_range × COLOR_COMPRESSION_FACTOR (0.8)
  (Smaller factor = more dramatic colors for small variations)

Step 3: Center the color scale
  data_center = (min_val + max_val) / 2.0
  color_scale_min = data_center - (adjusted_range / 2.0)
  
Step 4: Normalize to [0,1]
  normalized = (offset - color_scale_min) / adjusted_range
  normalized = clamp(0, 1)

Step 5: Apply color transformation using normalized value

Step 6: Desaturate by 35%
  gray = (r + g + b) / 3
  r = r × 0.65 + gray × 0.35  (blend with gray)
  g = g × 0.65 + gray × 0.35
  b = b × 0.65 + gray × 0.35
  (Reduces saturation for more muted appearance)

Result: lv_color_t (24-bit RGB)
```

### Gradient Band Legend (lines 1733-1810)

Draws vertical color bar on left side:
- Width: 20px
- Height: 80% of canvas height
- Margin: 10px from left edge
- Pixel-by-pixel color mapping from min_z (bottom) to max_z (top)
- Labels show extended color scale range

---

## LVGL Canvas Integration

### Canvas Initialization (lines 124-135)

```cpp
auto canvas_width = 400;
auto canvas_height = 400;

// Allocate true-color buffer (3 bytes per pixel: R,G,B)
canvas_draw_buf = lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(canvas_width, canvas_height));

// Attach buffer to canvas object
lv_canvas_set_buffer(mesh_canvas, canvas_draw_buf, canvas_width, canvas_height, LV_IMG_CF_TRUE_COLOR);

// Fill with background color
lv_canvas_fill_bg(mesh_canvas, lv_color_make(40, 40, 40), LV_OPA_COVER);
```

### Drawing Primitives Used

| LVGL Function | Purpose | Usage |
|---------------|---------|-------|
| `lv_canvas_draw_rect()` | Draw filled rectangle | Scanline pixels in triangle |
| `lv_canvas_draw_line()` | Draw line between 2 points | Wireframe, axes, grid |
| `lv_canvas_draw_text()` | Draw text label | Axis labels, Z range info |
| `lv_canvas_fill_bg()` | Clear entire canvas | Background fill |
| `lv_obj_invalidate()` | Mark for redraw | Force canvas update |

### Event Handling

**Draw callback** (header line 28):
```cpp
static void _mesh_draw_cb(lv_event_t *e) {
  BedMeshPanel *panel = (BedMeshPanel*)e->user_data;
  panel->mesh_draw_cb(e);
};
```

Attached to table cells for colorization during table view.

### Buffer Management

**Memory allocation** (line 706-729):
```cpp
void BedMeshPanel::resize_canvas()
{
  // Free old buffer
  lv_mem_free(canvas_draw_buf);
  
  // Allocate new buffer with new size
  canvas_draw_buf = lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(width, height));
  
  // Set new buffer on canvas object
  lv_canvas_set_buffer(mesh_canvas, canvas_draw_buf, width, height, LV_IMG_CF_TRUE_COLOR);
}
```

Canvas is resized when display container changes (line 140-143).

---

## Public API Surface

### Main Entry Points

#### Constructor
```cpp
BedMeshPanel(KWebSocketClient &c, std::mutex &l);
```
Initializes UI layout, canvas, and controls.

#### Data Update
```cpp
void consume(json &j);              // Parse Moonraker JSON update
void refresh_views_with_lock(json &);  // Update with lock held
void refresh_views(json &);         // Process JSON into mesh data
```

#### Rendering
```cpp
void draw_3d_mesh();                // Main render function
void mesh_draw_cb(lv_event_t *e);   // LVGL callback for table coloring
```

#### Interactive Controls
```cpp
void handle_z_zoom_in(lv_event_t *event);      // Increase Z height scale
void handle_z_zoom_out(lv_event_t *event);     // Decrease Z height scale
void handle_fov_zoom_in(lv_event_t *event);    // Increase perspective zoom
void handle_fov_zoom_out(lv_event_t *event);   // Decrease perspective zoom
void handle_canvas_drag(lv_event_t *event);    // Rotate via drag
void handle_canvas_scroll(lv_event_t *event);  // Scroll wheel zoom
void handle_canvas_gesture(lv_event_t *event); // Multi-touch pinch zoom
void handle_toggle_view(lv_event_t *event);    // Switch 3D ↔ table view
```

#### UI Management
```cpp
void foreground();                  // Called when panel becomes visible
void refresh_profile_info(std::string pname);  // Update profile display
```

### Low-Level Rendering Functions (Private)

```cpp
Point3D project_3d_to_2d(double x, double y, double z, 
                         int canvas_width, int canvas_height);

void fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3, 
                   lv_color_t color, int canvas_width, int canvas_height);

void fill_triangle_gradient(int x1, int y1, lv_color_t c1,
                            int x2, int y2, lv_color_t c2,
                            int x3, int y3, lv_color_t c3,
                            int canvas_width, int canvas_height);

std::vector<Quad3D> generate_mesh_quads(int rows, int cols, 
                                         double min_z, double max_z);

void sort_quads_by_depth(std::vector<Quad3D>& quads);

void draw_3d_quad(const Quad3D& quad, 
                  int canvas_width, int canvas_height);

void draw_mesh_wireframe(const std::vector<std::vector<Point3D>>& points,
                         int rows, int cols, int canvas_width, int canvas_height);

void draw_axes_and_labels(const std::vector<std::vector<Point3D>>& points,
                          int rows, int cols, int canvas_width, int canvas_height,
                          double min_z, double max_z, double z_scale);

void draw_color_gradient_band(int canvas_width, int canvas_height, 
                              double min_z, double max_z);
```

---

## External Dependencies

### Only LVGL + Standard C++ (NO GPU/Graphics Library)

| Dependency | Purpose | Version |
|------------|---------|---------|
| LVGL | Canvas drawing, UI framework | 9.4 |
| C++ Standard Library | Containers, math | C++17 |
| spdlog | Logging | Header-only |
| cmath | sin, cos, sqrt, etc. | Standard |
| algorithm | std::sort, std::swap | Standard |
| vector | std::vector mesh storage | Standard |

### NOT Used (Significantly Different from GuppyScreen's Simplicity)
- OpenGL / Vulkan / Metal (no GPU)
- GLM / Eigen / Glm (no math library)
- Assimp (no model loading)
- Software rasterizers (custom implementation)

---

## Critical Constants

### Rendering Constants (header lines 109-152)

| Constant | Value | Purpose |
|----------|-------|---------|
| CULLING_MARGIN | 50 px | Margin for off-screen triangle inclusion |
| MESH_SCALE | 50.0 units | Base spacing between mesh points in world coords |
| DEFAULT_Z_SCALE | 60.0 | Height amplification factor |
| DEFAULT_Z_TARGET_HEIGHT | 80.0 units | Target projected height range |
| DEFAULT_Z_MIN_SCALE | 35.0 | Min Z amplification to prevent flatness |
| DEFAULT_Z_MAX_SCALE | 120.0 | Max Z amplification to prevent extreme projections |
| CAMERA_DISTANCE | 450.0 units | Distance of virtual camera from origin |
| CANVAS_PADDING_RATIO | 0.1 | 10% padding on each side |
| Z_ORIGIN_VERTICAL_POSITION | 0.4 | Canvas Y position for Z=0 plane (0=top, 1=bottom) |
| Z_AXIS_EXTENSION | 0.75 | Extend Z-axis beyond data range for visualization |
| COLOR_COMPRESSION_FACTOR | 0.8 | Fraction of data range for color mapping (smaller = more contrast) |
| GRADIENT_BAND_WIDTH | 20 px | Width of color legend bar |
| GRADIENT_BAND_MARGIN | 10 px | Space from left edge |
| GRADIENT_MAX_SEGMENTS | 6 | Max segments per scanline (gradient performance) |
| GRADIENT_MIN_LINE_WIDTH | 3 px | Use solid color for narrower lines |

---

## Gotchas & Important Patterns

### 1. **Y-Axis Coordinate Inversion**
```cpp
// Row 0 is FRONT (min Y), last row is BACK (max Y)
double base_y = ((rows - 1 - row) - rows / 2.0) * MESH_SCALE;
```
This inverts because mesh[0] = front edge, but standard graphics use top=first.

### 2. **Z Centering for Rotation**
```cpp
// Subtract z_center BEFORE multiplying by z_scale
quad.vertices[0].z = (mesh[row][col] - z_center) * z_scale;
```
This centers the mesh around Z=0 for rotation. Without this, rotation would orbit around the bed instead of rotating it in place.

### 3. **Depth Sorting is DESCENDING**
```cpp
// Furthest away (largest depth) rendered FIRST
return a.avg_depth > b.avg_depth;
```
Common mistake: Using ascending sort would reverse draw order.

### 4. **Performance Trade-off: Gradient Quality vs Speed**
```cpp
// During drag: solid colors (fast)
if (is_dragging) {
  fill_triangle(..., quad.center_color, ...);
} else {
  // Static: gradient colors (beautiful but slower)
  fill_triangle_gradient(..., quad.vertices[0].color, ...);
}
```
Adaptive rendering for smooth interaction.

### 5. **FOV Scale Adjustment After Rotation**
```cpp
fov_scale = calculate_dynamic_fov_scale(mesh_rows, mesh_cols, canvas_width, canvas_height);
```
Recalculated every frame to ensure mesh stays centered when user rotates.

### 6. **Canvas Resize Hysteresis**
```cpp
// Only resize if change is >10 pixels (avoid excessive reallocation)
if (abs(canvas_width - current_width) > 10 || ...)
```
Prevents thrashing during small layout changes.

### 7. **Color Compression Factor**
```cpp
// 0.8 means only use 80% of data range for colors
// This makes small variations more colorful
adjusted_range = data_range * COLOR_COMPRESSION_FACTOR;
```
For a flat 0.1mm mesh, this creates dramatic colors. For a 2mm mesh, it's more subtle.

### 8. **Table View Uses Different Color Function**
```cpp
// Table uses simpler linear gradient (color_gradient)
lv_color_t color = color_gradient(offset);

// 3D view uses enhanced heat-map (color_gradient_enhanced)
lv_color_t color = color_gradient_enhanced(mesh[row][col], min_z, max_z);
```
Two separate color functions with different ranges/compression.

### 9. **Canvas Draw Buffer Type**
```cpp
// MUST use LV_IMG_CF_TRUE_COLOR (3 bytes/pixel)
// Can also use LV_IMG_CF_RGB565 (2 bytes/pixel) for memory savings
lv_canvas_set_buffer(..., ..., LV_IMG_CF_TRUE_COLOR);
```
The format affects memory usage and color precision.

### 10. **Multi-touch Detection**
```cpp
#ifdef SIMULATOR
  use_gesture_zoom = true;  // Mouse wheel zoom
#else
  has_multitouch = detect_multitouch_support();  // Real hardware check
#endif
```
Different input handling for simulator vs real hardware.

---

## Recommended Porting Patterns for HelixScreen

### 1. **Separate Data from Rendering**
```cpp
// Good: Keep mesh data independent
struct MeshData {
  std::vector<std::vector<double>> points;
  double min_z, max_z;
};

// Pass to rendering function
void render_mesh(const MeshData& mesh, lv_obj_t *canvas);
```

### 2. **Use Motion State Object**
```cpp
struct MeshViewState {
  double x_angle;
  double z_angle;
  double z_scale;
  double fov_scale;
  bool is_dragging;
};
```
Makes rotation state testable without coupling to UI.

### 3. **Adapter Pattern for Color Mapping**
```cpp
class ColorMapper {
public:
  virtual lv_color_t map_height(double value, double min_z, double max_z) = 0;
};

class HeatMapColorMapper : public ColorMapper {
  // Implement color_gradient_enhanced logic
};

class LinearColorMapper : public ColorMapper {
  // Simpler gradient for performance
};
```
Allows swapping color schemes without changing renderer.

### 4. **Strategy Pattern for Rasterization**
```cpp
class RasterStrategy {
public:
  virtual void fill_triangle(...) = 0;
};

class FastRasterizer : public RasterStrategy {
  // Solid color during drag
};

class QualityRasterizer : public RasterStrategy {
  // Gradient interpolation when static
};
```

### 5. **Keep Math Testable**
```cpp
// Testable: No LVGL dependency
Point3D project_point(double x, double y, double z,
                     double camera_distance, double fov_scale,
                     double x_angle, double z_angle);

// Hard to test: Depends on LVGL
Point3D project_3d_to_2d(double x, double y, double z,
                        int canvas_width, int canvas_height);
```

### 6. **Reduce Constants Magic**
Replace hardcoded constants with configuration:
```cpp
struct RenderConfig {
  double camera_distance = 450.0;
  double default_z_scale = 60.0;
  double color_compression = 0.8;
  // ... enable testing different values
};
```

### 7. **Avoid Tight LVGL Coupling**
```cpp
// BAD: Direct LVGL in projection code
lv_color_t color = color_gradient_enhanced(...);

// GOOD: Return normalized [0,1] value, map to color separately
double normalized = normalize_height(...);
lv_color_t color = heat_map_table[normalized * 255];
```

### 8. **Profile Critical Paths**
```cpp
// The render loop is called ~60Hz, profile for:
// - Triangle rasterization (largest bottleneck)
// - Sorting (O(n log n) for quads)
// - Color interpolation (avoid in inner loop)
```

### 9. **Implement Incremental Rendering**
```cpp
// Instead of re-rendering entire mesh every frame,
// track which quads changed and only re-render those
std::vector<int> dirty_quads;
// ... update with delta rendering logic
```

### 10. **Memory Layout for Cache Efficiency**
```cpp
// Better: Structure of Arrays (SoA)
struct QuadBatch {
  std::vector<Vertex3D> vertices;  // Packed vertex data
  std::vector<double> depths;      // For sorting
  std::vector<int> colors;         // Pre-packed colors
};

// Less cache-friendly: Array of Structures (AoS)
std::vector<Quad3D> quads;  // Current approach
```

---

## Performance Characteristics

### Rendering Complexity

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Perspective projection | O(n) | n = num vertices |
| Quad generation | O(rows × cols) | Mesh cells |
| Depth sorting | O(n log n) | n quads ≈ (rows-1)×(cols-1) |
| Triangle rasterization | O(pixels) | Per-scanline |
| Color interpolation | O(pixels × segments) | Max 6 segments |

### Typical Frame Rates (Estimated)

- **10×10 mesh** (81 quads): ~60 FPS (SDL2 desktop)
- **20×20 mesh** (361 quads): ~30-45 FPS (depends on gradient detail)
- **50×50 mesh** (2401 quads): ~10-15 FPS (CPU-bound on embedded)

Optimization opportunities:
1. Reduce triangle segment count during drag
2. Implement quad culling (only render visible quads)
3. Use vertex buffer caching
4. Profile on actual hardware

---

## Summary: Architecture Strengths & Weaknesses

### Strengths
✓ Zero external graphics library dependency
✓ Runs on any LVGL canvas (SDL2, framebuffer, etc.)
✓ Correct 3D math with proper depth sorting
✓ Smooth gradient interpolation for visual quality
✓ Adaptive rendering (fast drag, quality static)
✓ Clear separation of math and LVGL drawing
✓ Comprehensive axis/grid visualization

### Weaknesses
✗ Software rasterization is slow on CPU (can't use GPU)
✗ No normal calculation (lighting not possible)
✗ Limited to small meshes (< 50×50) for interactive frame rates
✗ Gradient computation with segments is approximation, not true Gouraud shading
✗ No occlusion detection (overlapping triangles blend unnecessarily)
✗ Hard-coded rotation axes (can't rotate around Y)

### When to Use GuppyScreen's Approach
- Embedded devices without GPU
- Simple mesh visualization (< 30×30 points)
- Code simplicity / no external dependencies
- LVGL-only target platforms

### When to Consider Alternatives
- Large meshes (> 50×50 requiring 60 FPS)
- High-quality rendering needed (per-pixel lighting, shadows)
- Multiple 3D objects (lighting becomes essential)

