# G-code Voxel Preview Generator

## Overview

Memory-efficient voxel-based G-code preview generator optimized for embedded displays. Uses sparse voxel storage and chunked Gaussian blur to generate high-quality preview images with minimal RAM usage.

## Features

- **Sparse voxel storage**: Only stores non-zero voxels using `std::unordered_map`
- **Memory efficient**: Peak usage <12MB for typical prints, <50MB target
- **Fast parsing**: Handles G90/G91, M82/M83, detects retractions
- **Gaussian blur**: Smooths voxels to create organic-looking previews
- **Multiple camera angles**: Front, top, side, and isometric views
- **PNG output**: 512x512 images with lighting/shading

## Architecture

```
┌─────────────────┐
│  G-code File    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  GCodeParser    │  Parse extrusions, track E position
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ SparseVoxelGrid │  Voxelize line segments
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Gaussian Blur   │  Smooth voxels (chunked)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Depth Render  │  Raycast to depth buffer
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   PNG Output    │  Write 512x512 image
└─────────────────┘
```

## Implementation

### Core Classes

#### `GCodeParser`
Parses G-code and extracts extrusion paths:
- Handles absolute/relative positioning (G90/G91)
- Tracks extrusion axis (M82/M83)
- Filters out retractions (E < 0)
- Returns line segments with 3D coordinates

#### `SparseVoxelGrid`
Sparse voxel storage and processing:
- `std::unordered_map<ivec3, float>` for O(1) voxel access
- DDA-style line rasterization
- Gaussian blur with 3D kernel
- Depth buffer rendering with normal estimation

#### `generateGCodePreview()`
High-level convenience function combining all steps.

### Memory Optimization Techniques

1. **Sparse storage**: Only allocate voxels that are filled
2. **No full mesh generation**: Direct depth buffer rendering
3. **Chunked blur**: Process in 32³ chunks to limit peak memory
4. **Early bounds culling**: Track min/max bounds during voxelization
5. **Value threshold**: Discard voxels below 0.01 after blur

### Performance

| Test Case | G-code Size | Segments | Peak Memory | Duration |
|-----------|-------------|----------|-------------|----------|
| Simple cube | 1.5 KB | 40 | 5.8 MB | 0.04s |
| Spiral vase | 1.4 MB | 32,500 | 11.8 MB | 30s |
| Dense infill | 13 MB | 500,000 | ~45 MB* | ~5 min* |

*Estimated based on scaling factors

### Comparison to Python Prototype

| Metric | Python (NumPy) | C++ (Sparse) | Improvement |
|--------|----------------|--------------|-------------|
| Memory | ~256 MB | <12 MB | **21x less** |
| Speed | 45s | 30s | 1.5x faster |
| Dependencies | NumPy, scikit-image | GLM, stb_image_write | Lighter |

## Usage

### Basic Example

```cpp
#include "gcode_voxel_preview.h"

VoxelPreviewConfig config;
config.resolution = 2.0f;      // 2 voxels/mm
config.blur_sigma = 2.0f;      // Moderate smoothing
config.output_width = 512;     // 512x512 output

bool success = generateGCodePreview(
    "model.gcode",
    "preview.png",
    config
);
```

### Advanced Usage

```cpp
GCodeParser parser;
auto segments = parser.parseExtrusions("model.gcode");

SparseVoxelGrid grid(2.0f); // 2 voxels/mm
for (const auto& seg : segments) {
    grid.addSegment(seg.first, seg.second, 0.4f); // 0.4mm nozzle
}

grid.applyGaussianBlur(2.0f, 32); // sigma=2.0, 32x32x32 chunks

// Generate multiple views
grid.renderToImage("front.png", CameraAngle::FRONT, 512, 512);
grid.renderToImage("top.png", CameraAngle::TOP, 512, 512);
grid.renderToImage("iso.png", CameraAngle::ISOMETRIC, 512, 512);
```

### Configuration Options

```cpp
struct VoxelPreviewConfig {
    float resolution = 2.0f;         // Voxels per millimeter (1.0 - 4.0)
    float blur_sigma = 2.0f;         // Gaussian blur sigma (1.0 - 3.0)
    int chunk_size = 32;             // Blur chunk size (16 - 64)
    int output_width = 512;          // Output image width
    int output_height = 512;         // Output image height
    float iso_threshold = 0.3f;      // Surface detection threshold
    bool adaptive_resolution = true; // Use adaptive resolution (future)
};
```

## Testing

### Unit Test

```bash
make test-gcode-preview
```

### Generate Test Files

```bash
# Small test (auto-generated)
./build/bin/gcode_voxel_preview_test

# Large test
./build/bin/generate_large_gcode build/my_test.gcode
./build/bin/gcode_voxel_preview_test build/my_test.gcode build/preview.png
```

### Memory Profiling

The test program reports peak memory usage:

```
[info] Peak memory:  11.75 MB
[info] Memory usage is within 50MB target
```

## Limitations

1. **No color information**: All previews are single color with lighting
2. **No support detection**: Multi-material/support not differentiated
3. **Gaussian blur is slow**: O(n × k³) where k is kernel size
4. **Fixed nozzle size**: Currently hardcoded to 0.4mm

## Future Optimizations

### Immediate (< 1 week)
- [ ] Separable Gaussian blur (3 passes instead of 1)
- [ ] Multi-threaded voxelization
- [ ] Configurable nozzle diameter
- [ ] Color support from G-code comments

### Medium-term (1-4 weeks)
- [ ] Adaptive resolution based on local complexity
- [ ] GPU acceleration (OpenGL compute shaders)
- [ ] Progressive rendering for UI feedback
- [ ] Cache voxel grid for multiple view angles

### Long-term (> 1 month)
- [ ] Real-time preview during slicing
- [ ] Support structure detection
- [ ] Multi-material visualization
- [ ] Integration with Moonraker file browser

## Integration with HelixScreen

### Moonraker File Browser

The preview generator can be integrated with the file browser panel to show G-code previews:

```cpp
// In ui_panel_file_browser.cpp

void FileBrowserPanel::load_gcode_preview(const std::string& filepath) {
    // Generate preview in background thread
    std::thread([this, filepath]() {
        VoxelPreviewConfig config;
        config.resolution = 1.5f; // Lower res for faster generation
        config.output_width = 300;
        config.output_height = 300;

        std::string preview_path = "/tmp/gcode_preview.png";
        if (generateGCodePreview(filepath, preview_path, config)) {
            // Update UI on main thread
            lv_async_call([this, preview_path]() {
                display_preview_image(preview_path);
            });
        }
    }).detach();
}
```

### Thumbnail Cache

For better performance, cache previews by file hash:

```cpp
class GCodePreviewCache {
    std::unordered_map<std::string, std::string> cache_; // hash -> png path

    std::string get_or_generate(const std::string& gcode_path) {
        std::string hash = compute_file_hash(gcode_path);
        if (cache_.count(hash)) {
            return cache_[hash];
        }

        std::string preview_path = "/tmp/preview_" + hash + ".png";
        if (generateGCodePreview(gcode_path, preview_path)) {
            cache_[hash] = preview_path;
            return preview_path;
        }
        return "";
    }
};
```

## Dependencies

- **GLM**: 3D math library (header-only)
- **stb_image_write.h**: PNG writing (single header)
- **spdlog**: Logging (header-only)
- **C++17**: `std::unordered_map`, filesystem

## References

- [Voxelization Algorithms](https://www.researchgate.net/publication/220789995_A_Fast_Voxelization_Algorithm_for_Trilinear_Filtering)
- [Gaussian Blur Optimization](https://www.intel.com/content/www/us/en/developer/articles/technical/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html)
- [G-code Reference](https://reprap.org/wiki/G-code)

## License

Copyright 2025 HelixScreen
SPDX-License-Identifier: GPL-3.0-or-later
