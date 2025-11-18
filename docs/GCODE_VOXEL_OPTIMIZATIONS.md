# G-code Voxel Preview - Optimization Guide

## Current Performance Baseline

| Metric | Value | Target | Status |
|--------|-------|--------|--------|
| Peak Memory | 11.8 MB | <50 MB | ✅ 4.2x under target |
| Processing Time | 30s | <60s | ✅ 2x under target |
| Voxel Count | 76,718 | - | Efficient |
| File Size | 1.4 MB | - | Typical |

## Optimization Priorities

### 🔴 Critical (3-5x speedup, minimal complexity)

#### 1. Separable Gaussian Blur
**Current**: Single 3D convolution (7×7×7 kernel = 343 operations per voxel)
**Optimized**: Three 1D convolutions (7+7+7 = 21 operations per voxel)

**Implementation**:
```cpp
void SparseVoxelGrid::applyGaussianBlurSeparable(float sigma, int chunk_size) {
    // Build 1D kernel
    int radius = static_cast<int>(std::ceil(sigma * 3.0f));
    std::vector<float> kernel_1d(2 * radius + 1);

    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        kernel_1d[i + radius] = std::exp(-(i * i) / (2.0f * sigma * sigma));
        sum += kernel_1d[i + radius];
    }

    // Normalize
    for (auto& k : kernel_1d) k /= sum;

    // Apply X pass
    auto temp1 = applyBlur1D(voxels_, kernel_1d, 0); // X direction
    // Apply Y pass
    auto temp2 = applyBlur1D(temp1, kernel_1d, 1);   // Y direction
    // Apply Z pass
    voxels_ = applyBlur1D(temp2, kernel_1d, 2);      // Z direction
}

std::unordered_map<glm::ivec3, float, IVec3Hash>
SparseVoxelGrid::applyBlur1D(
    const std::unordered_map<glm::ivec3, float, IVec3Hash>& input,
    const std::vector<float>& kernel,
    int axis // 0=X, 1=Y, 2=Z
) {
    std::unordered_map<glm::ivec3, float, IVec3Hash> output;
    int radius = kernel.size() / 2;

    for (const auto& [pos, val] : input) {
        float sum = 0.0f;

        for (int d = -radius; d <= radius; ++d) {
            glm::ivec3 neighbor = pos;
            neighbor[axis] += d;

            sum += getVoxel(neighbor) * kernel[d + radius];
        }

        if (sum > 0.01f) {
            output[pos] = sum;
        }
    }

    return output;
}
```

**Expected Improvement**: 3-5x faster blur (30s → 6-10s total)
**Memory Impact**: +50% peak during blur (temporary buffers)
**Effort**: 4-6 hours

---

#### 2. Multi-threaded Voxelization
**Current**: Single-threaded segment processing
**Optimized**: Partition segments across CPU cores

**Implementation**:
```cpp
void SparseVoxelGrid::addSegmentsParallel(
    const std::vector<std::pair<glm::vec3, glm::vec3>>& segments,
    float thickness,
    int num_threads = std::thread::hardware_concurrency()
) {
    std::vector<std::unordered_map<glm::ivec3, float, IVec3Hash>> thread_grids(num_threads);
    std::vector<std::thread> threads;

    size_t chunk_size = segments.size() / num_threads;

    for (int i = 0; i < num_threads; ++i) {
        size_t start = i * chunk_size;
        size_t end = (i == num_threads - 1) ? segments.size() : start + chunk_size;

        threads.emplace_back([&, i, start, end]() {
            for (size_t j = start; j < end; ++j) {
                addSegmentToGrid(segments[j].first, segments[j].second,
                                thickness, thread_grids[i]);
            }
        });
    }

    // Wait for all threads
    for (auto& t : threads) t.join();

    // Merge grids (take max value at each position)
    for (const auto& grid : thread_grids) {
        for (const auto& [pos, val] : grid) {
            voxels_[pos] = std::max(voxels_[pos], val);
        }
    }
}
```

**Expected Improvement**: 2-4x faster voxelization on 4-core CPU
**Memory Impact**: +200% during voxelization (per-thread grids)
**Effort**: 6-8 hours

---

### 🟡 High Value (2-3x speedup, moderate complexity)

#### 3. SIMD Vector Operations
**Current**: Scalar floating-point math
**Optimized**: SSE/AVX vector instructions via GLM

**Implementation**:
```cpp
// Enable GLM SIMD
#define GLM_FORCE_SIMD_SSE2
#include <glm/glm.hpp>
#include <glm/gtc/type_aligned.hpp>

// Use aligned vector types
using vec3 = glm::aligned_vec3;
using ivec3 = glm::aligned_ivec3;

// Batch process multiple voxels
void SparseVoxelGrid::processBatch(const std::vector<glm::vec3>& points) {
    // GLM will automatically use SIMD for vector operations
    for (size_t i = 0; i + 4 <= points.size(); i += 4) {
        // Process 4 points at once using SIMD
        // GLM handles this automatically when using aligned types
    }
}
```

**Expected Improvement**: 1.5-2x overall speedup
**Memory Impact**: None (compile-time optimization)
**Effort**: 2-3 days (requires refactoring vec3/ivec3 types)

---

#### 4. Octree Spatial Acceleration
**Current**: Flat hash map
**Optimized**: Hierarchical octree for neighbor queries

**Use Cases**: Faster blur, faster rendering, better cache locality

**Implementation**:
```cpp
class OctreeVoxelGrid {
    struct OctreeNode {
        std::array<std::unique_ptr<OctreeNode>, 8> children;
        float value = 0.0f;
        bool is_leaf = true;
    };

    std::unique_ptr<OctreeNode> root_;

    float getVoxel(const glm::ivec3& pos) {
        // Traverse octree (O(log n) instead of O(1))
        // But better cache locality for neighbor access
    }
};
```

**Expected Improvement**: 2-3x faster blur (better cache locality)
**Memory Impact**: -30% (more compact storage)
**Effort**: 1-2 weeks (major refactoring)

---

### 🟢 Nice to Have (polish, minor speedup)

#### 5. Incremental Preview Updates
**Current**: Generate entire preview at once
**Optimized**: Progressive rendering with UI updates

```cpp
void SparseVoxelGrid::renderProgressive(
    const std::string& output_path,
    std::function<void(int)> progress_callback
) {
    // Phase 1: Low-res preview (64x64)
    renderToImage(output_path, angle, 64, 64);
    progress_callback(33);

    // Phase 2: Medium-res (256x256)
    renderToImage(output_path, angle, 256, 256);
    progress_callback(66);

    // Phase 3: Full-res (512x512)
    renderToImage(output_path, angle, 512, 512);
    progress_callback(100);
}
```

**Expected Improvement**: Better UX (faster first preview)
**Memory Impact**: None
**Effort**: 1-2 days

---

#### 6. G-code Parser Optimization
**Current**: String operations on every line
**Optimized**: Lookup table for common commands

```cpp
class GCodeParserOptimized {
    // Pre-compiled regex for common patterns
    std::regex move_regex_{"G[01]\\s+"};

    // Fast path for most common case (G1 X Y Z E)
    bool parseFastPath(const std::string& line) {
        if (line[0] != 'G' || line[1] != '1') return false;
        // Custom parser for G1 format
        // 3-5x faster than general parser
    }
};
```

**Expected Improvement**: 1.5x faster parsing
**Memory Impact**: None
**Effort**: 1 day

---

### 🔵 Long-term (transformative, high effort)

#### 7. GPU Acceleration
**Technology**: OpenGL Compute Shaders
**Expected Improvement**: 10-100x speedup

**High-level approach**:
```glsl
// voxelize.comp - GPU voxelization shader
#version 430

layout(local_size_x = 256) in;

struct Segment {
    vec3 p1;
    vec3 p2;
    float thickness;
};

layout(std430, binding = 0) buffer Segments {
    Segment segments[];
};

layout(std430, binding = 1) buffer VoxelGrid {
    float voxels[];
};

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= segments.length()) return;

    // DDA voxelization on GPU
    voxelize_segment(segments[idx]);
}
```

**Pros**:
- Massive parallelism (1000+ cores)
- Fast blur (separable 3D convolution on GPU)
- Real-time preview generation

**Cons**:
- Requires OpenGL 4.3+
- Complex debugging
- Not all embedded targets have GPU

**Effort**: 2-4 weeks

---

#### 8. Adaptive LOD (Level of Detail)
**Current**: Fixed resolution everywhere
**Optimized**: Fine detail where needed, coarse elsewhere

```cpp
float SparseVoxelGrid::computeLocalComplexity(const glm::vec3& pos) {
    // Count segments nearby
    int nearby_segments = 0;
    for (const auto& seg : segments_near(pos, 5.0f)) {
        nearby_segments++;
    }

    // High complexity -> high resolution
    if (nearby_segments > 20) return 4.0f; // 4 voxels/mm
    if (nearby_segments > 5)  return 2.0f; // 2 voxels/mm
    return 1.0f; // 1 voxel/mm
}
```

**Expected Improvement**: 3-5x fewer voxels for same visual quality
**Memory Impact**: -60% (fewer voxels stored)
**Effort**: 2-3 weeks

---

## Recommended Implementation Order

### Phase 1: Quick Wins (1 week)
1. ✅ Separable Gaussian blur
2. ✅ Multi-threaded voxelization
3. ✅ G-code parser optimization

**Expected Result**: 5-10x total speedup (30s → 3-6s)

### Phase 2: Quality Improvements (2 weeks)
4. ✅ Progressive rendering
5. ✅ SIMD vector operations
6. ✅ Better error handling

**Expected Result**: Smoother UI, more robust

### Phase 3: Advanced Features (1 month)
7. ✅ Octree spatial structure
8. ✅ Adaptive LOD
9. ✅ GPU compute shaders (if supported)

**Expected Result**: Real-time preview generation

---

## Memory vs Speed Tradeoffs

| Optimization | Speed Gain | Memory Impact | Recommended |
|--------------|------------|---------------|-------------|
| Separable blur | 3-5x | +50% peak | ✅ Yes |
| Multi-threading | 2-4x | +200% peak | ✅ Yes |
| SIMD | 1.5-2x | 0% | ✅ Yes |
| Octree | 2-3x | -30% | ⚠️ Major refactor |
| GPU compute | 10-100x | 0% | ⚠️ Platform dependent |
| Adaptive LOD | 1x | -60% | ✅ Yes (long-term) |

---

## Platform Considerations

### Embedded Linux (Target Platform)
- **CPU**: ARM Cortex-A series (4 cores typical)
- **RAM**: 512MB - 2GB
- **GPU**: Mali-400 or similar (OpenGL ES 2.0)

**Recommended**:
- ✅ Separable blur (works everywhere)
- ✅ Multi-threading (4 cores available)
- ❌ GPU compute (need OpenGL 4.3+)
- ✅ SIMD (NEON on ARM)

### Development (SDL2 Simulator)
- **CPU**: x86_64 (8+ cores typical)
- **RAM**: 16GB+
- **GPU**: Full OpenGL 4.5+

**Recommended**:
- ✅ All optimizations
- ✅ GPU compute for testing
- ✅ Profiling tools available

---

## Profiling Tools

### Memory Profiling
```bash
# Valgrind massif
valgrind --tool=massif ./build/bin/gcode_voxel_preview_test model.gcode
ms_print massif.out.* | less

# macOS Instruments
instruments -t Allocations ./build/bin/gcode_voxel_preview_test

# Custom profiling
# Already built into test program:
# [info] Peak memory: 11.75 MB
```

### Performance Profiling
```bash
# Linux perf
perf record -g ./build/bin/gcode_voxel_preview_test model.gcode
perf report

# macOS Instruments
instruments -t "Time Profiler" ./build/bin/gcode_voxel_preview_test

# Simple timing (built-in)
# [info] Duration: 30.09 seconds
```

### Hotspot Analysis
Expected hotspots (in order):
1. **Gaussian blur** (95% of time)
2. **Voxelization** (4% of time)
3. **Rendering** (1% of time)

---

## Testing Methodology

### Benchmark Suite
```bash
# Small (baseline)
./build/bin/gcode_voxel_preview_test build/test_gcode.gcode

# Medium (typical print)
./build/bin/gcode_voxel_preview_test build/large_test.gcode

# Large (stress test)
./build/bin/gcode_voxel_preview_test build/huge_test.gcode

# Real-world samples
./build/bin/gcode_voxel_preview_test samples/benchy.gcode
./build/bin/gcode_voxel_preview_test samples/calibration_cube.gcode
```

### Regression Testing
- Track memory usage over time
- Alert if >50MB peak
- Alert if >2x slowdown
- Visual diff of preview images

---

## Conclusion

**Current implementation is production-ready** with excellent memory efficiency. The most impactful optimizations are:

1. **Separable blur** (highest ROI, lowest effort)
2. **Multi-threading** (2nd highest ROI, low effort)
3. **SIMD** (good ROI, medium effort)

These three optimizations combined would reduce processing time from 30s → ~5s while staying well under the 50MB memory limit.

---

**Last Updated**: 2025-11-18
**Performance Target**: <5s for typical prints
**Memory Target**: <50MB peak
