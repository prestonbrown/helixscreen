# HelixScreen Memory Usage Analysis
*Last Updated: 2026-03-04*

Paths in this analysis name the tree as it stood on 2026-03-04.

## Executive Summary

**Current Pattern:** Create-once, toggle visibility (all panels loaded at startup)
**Memory Footprint:** ~32-35 MB physical, ~68 MB RSS
**Heap Usage:** ~12.6 MB of actual allocations
**Leaks:** None detected
**Recommendation:** ✅ **KEEP CURRENT APPROACH** - memory usage is excellent

---

## Memory Profile (2025-11-30)

### Normal Mode (All Panels Pre-Created)
```
Physical Footprint:  32.6 MB
Physical Peak:       36.2 MB
RSS (Resident):      68 MB
VSZ (Virtual):       ~35 GB (virtual address space, not real)
Heap Allocations:    40,143 nodes
Allocated Data:      12.6 MB
```

### Wizard Mode (On-Demand Panel Creation)
```
Physical Footprint:  32.3 MB
Physical Peak:       34.9 MB
RSS (Resident):      62 MB
Heap Allocations:    ~38,000 nodes (estimated)
```

### Memory Comparison (Normal vs Wizard)
```
Physical Footprint:  -0.3 MB (negligible)
RSS:                 -6 MB (wizard uses slightly less)
```

**KEY FINDING:** Both modes use approximately the same memory. The create-once pattern has no meaningful memory penalty.

---

## Memory Breakdown

### Where the Memory Goes

**Framework Overhead (Majority):**
- LVGL runtime: ~10-15 MB
- SDL2 graphics: ~10-15 MB
- System libraries: shared, not counted in physical footprint
- Total Framework: ~25-30 MB

**UI Panels:**
- All XML panels + overlays + wizard: ~12.6 MB heap
- Individual panel estimate: ~400-800 KB each

### Heap Statistics Detail (2025-11-30)

```
Allocation Size Distribution:
- 32-byte objects: 13,124 instances (LVGL widgets)
- 48-byte objects: 6,617 instances
- 64-byte objects: 5,454 instances
- 80-byte objects: 3,773 instances
- 128-byte objects: 1,419 instances
- Larger objects: Sparse (fonts, images, buffers)

Top Memory Consumers:
- Non-object allocations: 9.3 MB (LVGL internal state)
- CFString objects: 158 KB (3,076 instances)
```

---

## LVGL 9 Documentation Findings

### Official Best Practices (from docs.lvgl.io & forum)

**Pattern 1: Create Once, Keep in Memory**
> "Just loads an already existing screen. When the screen is left, it remains in memory, so all states are preserved."

**Pattern 2: Dynamic Create/Delete**
> "The screen is created dynamically, and when it's left, it is deleted, so all changes are lost (unless they are saved in subjects)."

**Memory Optimization Quote:**
> "To work with lower LV_MEM_SIZE you can create objects only when required and delete them when they are not needed. This allows for the creation of a screen just when a button is clicked to open it, and for deletion of screens when a new screen is loaded."

**Auto-Delete Feature:**
> "By using `lv_screen_load_anim(scr, transition_type, time, delay, auto_del)` ... if `auto_del` is true the previous screen is automatically deleted when any transition animation finishes."

### When to Use Each Pattern

**Create-Once (Current HelixScreen approach):**
- ✅ Moderate number of screens (~10 main + overlays + wizard)
- ✅ State preservation is critical (temps, positions, settings)
- ✅ Instant panel switching matters
- ✅ Known memory ceiling (~35 MB physical)
- ✅ Target hardware has adequate RAM (>256 MB)

**Dynamic Create/Delete (Use When):**
- ❌ Dozens of panels (we have ~15 total)
- ❌ Panels with heavy resources (we use lightweight XML)
- ❌ Panels rarely accessed (all panels are frequently used)
- ❌ Running on <64 MB RAM (Raspberry Pi 3+ has 1 GB+)

---

## Recommendations

### 1. KEEP Current Create-Once Pattern ✅

**Rationale:**
- Memory usage is excellent (~35 MB physical)
- Instant panel switching (0ms vs 50-100ms)
- State preserved automatically
- No serialization complexity
- Predictable memory usage
- Zero risk of allocation failures at runtime

### 2. Future: Profile on Target Hardware 📊

macOS SDL2 simulator uses different memory patterns than Linux framebuffer:
- SDL2 uses GPU acceleration
- Framebuffer uses CPU rendering (different overhead)
- Test on actual Raspberry Pi for real-world numbers

### 3. Future: Consider Lazy Image Loading 🖼️

If print thumbnails become numerous:
```cpp
// Instead of loading 100 thumbnails at once:
// Load thumbnails on-demand when scrolled into view
// Unload when scrolled out (image cache)
```

---

## G-code Layer Cache Tiering (2025-12-24)

**Problem**: The layer cache for G-code visualization was consuming 32 MB on the AD5M (108 MB total RAM), leaving only ~15 MB free and causing severe performance issues.

### Tiered Cache Budgets

| Device Tier | Total RAM | Cache Budget | Adaptive Mode | Examples |
|-------------|-----------|--------------|---------------|----------|
| **Constrained** | < 256 MB | **4 MB** | Yes | AD5M (108 MB), embedded devices |
| **Normal** | 256-512 MB | **16 MB** | Yes | Pi 3, low-end Pi 4 |
| **Good** | > 512 MB | **32 MB** | No | Desktop, Pi 4 2GB+ |

### Implementation Details

- **Detection**: Uses `MemoryInfo::is_constrained_device()`, `is_normal_device()`, `is_good_device()` (see `include/memory_utils.h`)
- **Adaptive Mode**: Constrained/normal devices use adaptive mode that responds to memory pressure
- **Pressure Response**: `check_memory_pressure()` is called on every cache access to dynamically adjust budget

### AD5M Reality Check (Measured 2025-12-24)

| Metric | Before Fix | After Fix | Notes |
|--------|------------|-----------|-------|
| HelixScreen RSS | ~40 MB | ~16 MB | Still tight |
| System Available | 15 MB | 37 MB | Marginal safety |
| Layer Cache | 31.9 MB | < 4 MB | Trade-off: fewer cached layers |

**Caveats**:
- The 4 MB cache means more frequent layer re-parsing during G-code preview navigation
- Large G-code files (10 MB+) may still cause memory pressure during initial loading
- Adaptive mode can shrink cache to 1 MB under pressure, degrading preview performance
- These numbers were measured at idle; actual usage during printing may differ

---

## Conclusion

The current architecture is **well-optimized** for the use case. Physical memory footprint is ~35 MB on desktop, ~16 MB on constrained devices like AD5M.

**Don't refactor unless:**
- Profiling shows OOM crashes on target hardware
- Need to support 50+ panels

---

## Proactive Memory Monitoring (2026-03-04)

### Overview

`MemoryMonitor` now evaluates device-tier-aware thresholds every 5 seconds and fires `memory_warning` telemetry events on breach. This provides production visibility into memory issues without manual intervention.

### Components

| Component | File | Role |
|-----------|------|------|
| `MemoryMonitor` | `include/memory_monitor.h` | Background thread: 5s sampling, threshold evaluation, growth tracking |
| `SmapsRollup` | `include/memory_utils.h` | Reads `/proc/self/smaps_rollup` for heap vs shared lib vs file-backed breakdown |
| `TelemetryManager` | `src/system/telemetry_manager.cpp` | `memory_warning` event (on breach) + enriched `memory_snapshot` (hourly) |
| `MemoryProfiler` | `src/system/memory_profiling.cpp` | SIGUSR1/periodic snapshots now include PSS, shared, swap breakdown |
| `MemoryStatsOverlay` | `src/ui/ui_panel_memory_stats.cpp` | Dev overlay shows pressure level: OK/ELEVATED/WARNING/CRITICAL |

### Threshold Tiers

| Tier | Total RAM | Warn RSS | Critical RSS | Warn Available | Critical Available | Growth/5min |
|------|-----------|----------|--------------|----------------|--------------------|-------------|
| Constrained | <256 MB | 15 MB | 20 MB | 15 MB | 8 MB | 1 MB |
| Normal | 256-512 MB | 120 MB | 180 MB | 32 MB | 16 MB | 3 MB |
| Good | >512 MB | 180 MB | 230 MB | 48 MB | 24 MB | 5 MB |

Thresholds are hardcoded (not configurable) — these are operational guardrails, not user preferences. Tier detection uses `MemoryInfo::is_constrained_device()` / `is_normal_device()` / `is_good_device()`.

### How It Works

1. `MemoryMonitor::start(5000)` launches a background thread sampling `/proc/self/status` every 5s
2. Every sample: RSS and system available memory are checked against thresholds
3. Every 30s (6th tick): RSS is recorded in a 10-sample circular buffer for growth rate tracking
4. On breach: `evaluate_thresholds()` determines the highest pressure level and fires `WarningCallback`
5. Callback is rate-limited to 1 per level per 5 minutes to avoid telemetry spam
6. `TelemetryManager::record_memory_warning()` builds and enqueues the telemetry event
7. `MemoryStatsOverlay` (M key) reads `MemoryMonitor::pressure_level()` for real-time display

### Performance Impact

| Operation | Frequency | Cost |
|-----------|-----------|------|
| `/proc/self/status` read | 5s | ~50 μs |
| `/proc/meminfo` read | 5s | ~50 μs |
| `/proc/self/smaps_rollup` | On warning only | ~100 μs |
| Telemetry JSON build | ≤1 per 5min | Negligible |

### Verification

```bash
# Check threshold log at startup
./build/bin/helix-screen --test -vv 2>&1 | grep "Thresholds:"

# SIGUSR1 snapshot with smaps breakdown
kill -USR1 $(pidof helix-screen)

# Memory overlay (press M in running app)
# Shows: RSS, Peak, Private, Delta, Pressure level
```

---

## Test Commands Used

```bash
# Build app
make -j

# Profile normal mode (all panels)
./build/bin/helix-screen -s small --test &
PID=$!
./build/bin/helix-screen ctl navigate home
sleep 4
ps -o pid,rss,vsz -p $PID
heap $PID | head -35
vmmap --summary $PID | grep "Physical footprint"
kill $PID

# Profile wizard mode
./build/bin/helix-screen -s small --wizard --test &
PID=$!
sleep 4
# ... same profiling commands
```
