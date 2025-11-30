# Bed Mesh Renderer - Implementation Patterns & Code Examples

Quick reference for implementing/adapting bed mesh visualization in HelixScreen.

## 1. Complete Rendering Pipeline (High-Level)

```cpp
// Main orchestration function
void render_bed_mesh(const BedMeshData& mesh_data, lv_obj_t *canvas) {
  // Stage 1: Prepare
  clear_canvas(canvas);
  int width = lv_obj_get_width(canvas);
  int height = lv_obj_get_height(canvas);
  
  // Stage 2: Compute render parameters
  double min_z = mesh_data.min_height;
  double max_z = mesh_data.max_height;
  double z_scale = compute_z_scale(min_z, max_z);  // Dynamic scaling
  double fov_scale = compute_fov_scale(mesh_data.rows, mesh_data.cols, width, height);
  
  // Stage 3: Generate geometry
  std::vector<Quad3D> quads = generate_mesh_quads(mesh_data, z_scale);
  
  // Stage 4: Compute depth for sorting
  for (auto& quad : quads) {
    quad.avg_depth = compute_quad_depth(quad, fov_scale);
  }
  
  // Stage 5: Sort and render
  std::sort(quads.begin(), quads.end(), 
    [](const Quad3D& a, const Quad3D& b) { return a.avg_depth > b.avg_depth; });
  
  // Stage 6: Draw layers (back to front)
  render_axes_and_grid(canvas, width, height);
  for (const auto& quad : quads) {
    render_quad(quad, canvas, width, height);
  }
  render_wireframe(canvas, width, height);
  render_legend(canvas, width, height, min_z, max_z);
  
  lv_obj_invalidate(canvas);  // Force LVGL redraw
}
```

## 2. Perspective Projection Template

```cpp
// Generic perspective projection (no LVGL dependency)
struct Vec3 { double x, y, z; };
struct Mat4 { double m[4][4]; };  // Homogeneous transformation matrix

Vec3 project_perspective(Vec3 world_pos, const ProjectionParams& params) {
  // Apply rotation matrices
  Vec3 rotated = rotate_euler_zx(world_pos, params.angle_z, params.angle_x);
  
  // Translate camera
  rotated.z += params.camera_distance;
  
  // Perspective divide (similar triangles)
  Vec3 screen;
  screen.x = (rotated.x * params.focal_length) / rotated.z;
  screen.y = (rotated.y * params.focal_length) / rotated.z;
  screen.z = rotated.z;  // Depth for sorting
  
  // Convert to screen coordinates
  screen.x += params.canvas_center_x;
  screen.y += params.canvas_center_y;
  
  return screen;
}

// Euler angle rotation (Z then X)
Vec3 rotate_euler_zx(Vec3 p, double angle_z, double angle_x) {
  // Z rotation (spin)
  double cos_z = cos(angle_z), sin_z = sin(angle_z);
  Vec3 p1 = {
    p.x * cos_z - p.y * sin_z,
    p.x * sin_z + p.y * cos_z,
    p.z
  };
  
  // X rotation (tilt)
  double cos_x = cos(angle_x), sin_x = sin(angle_x);
  Vec3 p2 = {
    p1.x,
    p1.y * cos_x + p1.z * sin_x,
    p1.y * sin_x - p1.z * cos_x
  };
  
  return p2;
}
```

## 3. Scanline Triangle Rasterization (Optimized)

```cpp
// Fill triangle with gradient interpolation
void rasterize_triangle_gradient(
    int x0, int y0, RGB c0,
    int x1, int y1, RGB c1,
    int x2, int y2, RGB c2,
    Canvas& canvas)
{
  // Sort vertices by Y coordinate
  struct Vertex { int x, y; RGB color; };
  Vertex v[3] = {{x0, y0, c0}, {x1, y1, c1}, {x2, y2, c2}};
  
  // Bubble sort (only 3 elements)
  if (v[0].y > v[1].y) std::swap(v[0], v[1]);
  if (v[1].y > v[2].y) std::swap(v[1], v[2]);
  if (v[0].y > v[1].y) std::swap(v[0], v[1]);
  
  // Skip degenerate triangles
  if (v[0].y == v[2].y) return;
  
  // Rasterize by scanline
  for (int y = v[0].y; y <= v[2].y; y++) {
    // Compute left/right edges + colors
    double t1 = (y - v[0].y) / (double)(v[1].y - v[0].y);
    double t2 = (y - v[0].y) / (double)(v[2].y - v[0].y);
    
    int x_left = v[0].x + (int)(t1 * (v[1].x - v[0].x));
    int x_right = v[0].x + (int)(t2 * (v[2].x - v[0].x));
    RGB c_left = lerp_color(v[0].color, v[1].color, t1);
    RGB c_right = lerp_color(v[0].color, v[2].color, t2);
    
    if (x_left > x_right) {
      std::swap(x_left, x_right);
      std::swap(c_left, c_right);
    }
    
    // Draw horizontal line with interpolated colors
    draw_line_gradient(canvas, y, x_left, x_right, c_left, c_right);
  }
}

// Helper: Linear interpolation of colors
RGB lerp_color(RGB a, RGB b, double t) {
  return RGB {
    (uint8_t)(a.r + t * (b.r - a.r)),
    (uint8_t)(a.g + t * (b.g - a.g)),
    (uint8_t)(a.b + t * (b.b - a.b))
  };
}

// Helper: Draw horizontal line with gradient
void draw_line_gradient(Canvas& canvas, int y, int x_left, int x_right,
                        RGB c_left, RGB c_right) {
  int width = x_right - x_left + 1;
  
  // Performance: use solid color for thin lines
  if (width < 4) {
    RGB avg_color = lerp_color(c_left, c_right, 0.5);
    canvas.draw_rect(x_left, y, width, 1, avg_color);
  } else {
    // Gradient: divide into segments
    int segments = std::min(6, width / 4);
    for (int seg = 0; seg < segments; seg++) {
      int x_start = x_left + (seg * width) / segments;
      int x_end = x_left + ((seg + 1) * width) / segments;
      double t = (seg + 0.5) / segments;
      RGB seg_color = lerp_color(c_left, c_right, t);
      canvas.draw_rect(x_start, y, x_end - x_start, 1, seg_color);
    }
  }
}
```

## 4. Depth Sorting (Painter's Algorithm)

```cpp
// Data structure for sorting
struct RenderQuad {
  Point3D vertices[4];
  double avg_depth;
  RGB color;
};

// Compute average depth for quad
double compute_quad_depth(const RenderQuad& quad) {
  double total = 0;
  for (int i = 0; i < 4; i++) {
    total += quad.vertices[i].z;  // Z after camera transformation
  }
  return total / 4.0;  // Average
}

// Sort by depth (furthest first)
void sort_by_depth(std::vector<RenderQuad>& quads) {
  std::sort(quads.begin(), quads.end(),
    [](const RenderQuad& a, const RenderQuad& b) {
      return a.avg_depth > b.avg_depth;  // Descending = furthest first
    });
}

// Render in sorted order
void render_sorted_quads(const std::vector<RenderQuad>& quads, Canvas& canvas) {
  for (const auto& quad : quads) {
    // Project and rasterize quad (split into 2 triangles)
    render_triangle(quad.vertices[0], quad.vertices[1], quad.vertices[2], quad.color, canvas);
    render_triangle(quad.vertices[1], quad.vertices[2], quad.vertices[3], quad.color, canvas);
  }
}
```

## 5. Color Mapping: Heat Map

```cpp
// Convert Z height to RGB color using scientific heat map
// Purple (low) → Blue → Cyan → Yellow → Red (high)
RGB height_to_color(double value, double min_val, double max_val) {
  // Compute data range and apply compression
  double range = max_val - min_val;
  double adjusted_range = range * 0.8;  // Compression factor
  double data_center = (min_val + max_val) / 2.0;
  double color_min = data_center - (adjusted_range / 2.0);
  
  // Normalize to [0, 1]
  double normalized = (value - color_min) / adjusted_range;
  normalized = std::max(0.0, std::min(1.0, normalized));
  
  // Compute RGB using segmented mapping
  uint8_t r, g, b;
  if (normalized < 0.125) {
    // Purple to Blue
    double t = normalized / 0.125;
    r = (uint8_t)(128 * (1.0 - t));
    g = (uint8_t)(128 * t);
    b = 255;
  } else if (normalized < 0.375) {
    // Blue to Cyan
    double t = (normalized - 0.125) / 0.25;
    r = 0;
    g = (uint8_t)(128 + 127 * t);
    b = 255;
  } else if (normalized < 0.625) {
    // Cyan to Yellow
    double t = (normalized - 0.375) / 0.25;
    r = (uint8_t)(255 * t);
    g = 255;
    b = (uint8_t)(255 * (1.0 - t));
  } else if (normalized < 0.875) {
    // Yellow to Red
    double t = (normalized - 0.625) / 0.25;
    r = 255;
    g = (uint8_t)(255 * (1.0 - t));
    b = 0;
  } else {
    // Deep Red
    r = 255;
    g = 0;
    b = 0;
  }
  
  // Desaturate by 35% for muted appearance
  uint8_t gray = (r + g + b) / 3;
  r = (uint8_t)(r * 0.65 + gray * 0.35);
  g = (uint8_t)(g * 0.65 + gray * 0.35);
  b = (uint8_t)(b * 0.65 + gray * 0.35);
  
  return RGB{r, g, b};
}

// Alternative: Pre-compute lookup table for performance
void build_color_lut(std::vector<RGB>& lut, double min_val, double max_val) {
  for (int i = 0; i < 256; i++) {
    double normalized = i / 255.0;
    double value = min_val + normalized * (max_val - min_val);
    lut[i] = height_to_color(value, min_val, max_val);
  }
}

// Fast path: look up pre-computed color
RGB get_height_color_fast(double value, double min_val, double max_val,
                          const std::vector<RGB>& lut) {
  double normalized = (value - min_val) / (max_val - min_val);
  int idx = std::clamp((int)(normalized * 255), 0, 255);
  return lut[idx];
}
```

## 6. Adaptive Rendering (Quality vs Performance)

```cpp
class AdaptiveRenderer {
private:
  bool is_dragging = false;
  bool use_high_quality = true;
  
public:
  void set_dragging(bool dragging) { is_dragging = dragging; }
  
  void render_quad(const RenderQuad& quad, Canvas& canvas) {
    if (is_dragging) {
      // Fast path: solid color only
      render_triangle_solid(quad.vertices[0], quad.vertices[1], quad.vertices[2],
                           quad.color, canvas);
      render_triangle_solid(quad.vertices[1], quad.vertices[2], quad.vertices[3],
                           quad.color, canvas);
    } else {
      // Quality path: gradient interpolation
      render_triangle_gradient(quad.vertices[0], quad.vertices[1], quad.vertices[2],
                              quad.color, canvas);
      render_triangle_gradient(quad.vertices[1], quad.vertices[2], quad.vertices[3],
                              quad.color, canvas);
    }
  }
};
```

## 7. Interactive Rotation Control

```cpp
struct ViewState {
  double angle_x = -85.0;  // Tilt (up/down)
  double angle_z = 10.0;   // Spin (horizontal)
  double z_scale = 60.0;   // Height amplification
  double fov_scale = 100.0;  // Perspective zoom
};

class InteractiveViewer {
private:
  ViewState view;
  bool is_dragging = false;
  int drag_start_x = 0, drag_start_y = 0;
  int last_x = 0, last_y = 0;
  
public:
  void handle_drag_start(int x, int y) {
    is_dragging = true;
    drag_start_x = x;
    last_x = x;
    drag_start_y = y;
    last_y = y;
  }
  
  void handle_drag_motion(int x, int y) {
    if (!is_dragging) return;
    
    // Horizontal drag = spin around Z axis
    int dx = x - last_x;
    view.angle_z += dx * 0.5;  // Sensitivity: 0.5 deg per pixel
    
    // Vertical drag = tilt around X axis
    int dy = y - last_y;
    view.angle_x += dy * 0.5;
    
    // Clamp angles to valid ranges
    view.angle_x = std::clamp(view.angle_x, -85.0, -10.0);  // Don't flip over
    
    last_x = x;
    last_y = y;
  }
  
  void handle_drag_end() {
    is_dragging = false;
  }
  
  void handle_z_zoom(double delta) {
    view.z_scale *= (1.0 + delta);  // delta = +0.1 for zoom in, -0.1 for out
    view.z_scale = std::clamp(view.z_scale, 0.1, 5.0);
  }
  
  void handle_fov_zoom(double delta) {
    view.fov_scale *= (1.0 + delta);
    view.fov_scale = std::clamp(view.fov_scale, 50.0, 200.0);
  }
  
  const ViewState& get_view() const { return view; }
};
```

## 8. Data Structure: Mesh Representation

```cpp
// Input format from Moonraker
struct BedMeshData {
  std::vector<std::vector<double>> points;  // Height map
  int rows, cols;
  double min_height, max_height;
  
  // Computed metrics
  double get_z_range() const { return max_height - min_height; }
};

// Intermediate: Projected geometry
struct Point3D {
  double x, y, z;        // World 3D coordinates
  int screen_x, screen_y;  // Screen 2D after projection
  double depth;          // Z-distance from camera (for sorting)
};

// Rendering: Colored vertices
struct Vertex {
  Point3D position;
  RGB color;
};

// Rendering: Quad surface
struct RenderQuad {
  Vertex vertices[4];    // 4 corners
  double avg_depth;      // For sorting
  RGB center_color;      // Fallback solid color
};
```

## 9. Bounding Box Culling (Performance)

```cpp
// Skip triangles completely off-screen
bool is_visible(const Point3D& p, int canvas_width, int canvas_height) {
  const int MARGIN = 50;  // Pixels beyond canvas to include
  return p.screen_x >= -MARGIN && p.screen_x < canvas_width + MARGIN &&
         p.screen_y >= -MARGIN && p.screen_y < canvas_height + MARGIN;
}

// Check if triangle has any visible vertex
bool triangle_visible(const Point3D& v0, const Point3D& v1, const Point3D& v2,
                     int canvas_width, int canvas_height) {
  return is_visible(v0, canvas_width, canvas_height) ||
         is_visible(v1, canvas_width, canvas_height) ||
         is_visible(v2, canvas_width, canvas_height);
}

// Fast culling in render loop
for (const auto& quad : quads) {
  if (!triangle_visible(quad.vertices[0], quad.vertices[1], quad.vertices[2],
                       canvas_width, canvas_height)) {
    continue;  // Skip rendering
  }
  render_quad(quad, canvas);
}
```

## 10. Canvas Integration (LVGL)

```cpp
// Initialize canvas buffer
void init_3d_canvas(lv_obj_t *canvas, int width, int height) {
  // Allocate buffer (3 bytes per pixel for true color)
  void *buffer = lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(width, height));
  
  // Attach to canvas
  lv_canvas_set_buffer(canvas, buffer, width, height, LV_IMG_CF_TRUE_COLOR);
  
  // Clear background
  lv_canvas_fill_bg(canvas, lv_color_make(40, 40, 40), LV_OPA_COVER);
}

// Draw primitives via LVGL
void draw_rect(lv_obj_t *canvas, int x, int y, int w, int h, RGB color) {
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = lv_color_make(color.r, color.g, color.b);
  dsc.bg_opa = LV_OPA_90;
  dsc.border_width = 0;
  lv_canvas_draw_rect(canvas, x, y, w, h, &dsc);
}

void draw_line(lv_obj_t *canvas, int x1, int y1, int x2, int y2, RGB color) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = lv_color_make(color.r, color.g, color.b);
  dsc.width = 1;
  dsc.opa = LV_OPA_70;
  
  lv_point_t points[2] = {{x1, y1}, {x2, y2}};
  lv_canvas_draw_line(canvas, points, 2, &dsc);
}

// Force redraw after rendering
lv_obj_invalidate(canvas);
```

---

## Quick Formulas Reference

### Perspective Projection
```
screen_x = (world_x * fov_scale) / camera_depth
screen_y = (world_y * fov_scale) / camera_depth
```

### Z-Scale Calculation
```
z_scale = target_height_range / data_height_range
z_scale = clamp(z_scale, min_scale, max_scale)
```

### Color Normalization
```
normalized = (value - center - range/2) / (adjusted_range)
normalized = clamp(normalized, 0, 1)
```

### FOV Scale
```
fov_scale = (canvas_size * camera_distance) / mesh_diagonal
```

---

## Common Implementation Gotchas

| Issue | Solution |
|-------|----------|
| Triangles inverted | Check vertex winding order (clockwise vs CCW) |
| Mesh not rotating | Verify Z is centered at 0 before scaling |
| Colors all flat | Check color_gradient_enhanced is using compressed range |
| Flickering | Call lv_obj_invalidate() only once per frame |
| Memory leak | Free canvas_draw_buf before reallocating |
| Slow on drag | Use solid colors (not gradients) during interaction |
| Mesh too small/large | Adjust MESH_SCALE constant or canvas size |

