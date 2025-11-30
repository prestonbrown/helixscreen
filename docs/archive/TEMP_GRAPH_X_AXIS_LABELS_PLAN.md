# Temperature Graph X-Axis Time Labels Plan

**Status:** Implemented (2025-11-29)
**Created:** 2025-11-26
**Priority:** Enhancement for temperature monitoring UX

## Summary

Add scrolling absolute time labels ("HH:MM") below temperature graphs, with 6 labels distributed evenly across the chart width. Labels update as new data arrives.

## Design Decisions

- **Time format:** Absolute time (e.g., "14:30", "14:31")
- **Label count:** 6 labels (~1 minute intervals for 5-minute graph)
- **Position:** New 20px row below chart
- **Approach:** Panel-level implementation (matches existing Y-axis label pattern)

## Current State

- Temperature graphs use LVGL chart with `LV_CHART_UPDATE_MODE_SHIFT`
- 300 data points = ~5 minutes at 1-second updates
- Y-axis labels: Custom flex column with `UI_FONT_SMALL` (montserrat_12)
- X-axis labels: **Currently missing entirely**
- No timestamp tracking - just implicit indices

## Implementation

### Phase 1: XML Layout Changes

**Files:** `ui_xml/nozzle_temp_panel.xml`, `ui_xml/bed_temp_panel.xml`

Wrap existing `graph_container` in outer flex column, add X-axis row:

```xml
<lv_obj name="graph_outer_container" width="66%" height="100%"
        style_bg_opa="0" style_pad_all="0" scrollable="false"
        flex_flow="column" style_pad_gap="0">

  <!-- Existing graph area (Y-axis + chart) - now flex_grow -->
  <lv_obj name="graph_container" width="100%" flex_grow="1" ...>
    <lv_obj name="y_axis_labels" width="40" height="100%">...</lv_obj>
    <lv_obj name="chart_area" flex_grow="1" height="100%">...</lv_obj>
  </lv_obj>

  <!-- NEW: X-axis time labels -->
  <lv_obj name="x_axis_labels" width="100%" height="20"
          style_bg_opa="0" style_border_width="0" scrollable="false"
          style_pad_left="48" style_pad_right="8"
          flex_flow="row" style_flex_main_place="space_between">
    <!-- Labels created programmatically -->
  </lv_obj>
</lv_obj>
```

**Key:** `style_pad_left="48"` = Y-axis width (40) + gap (8) to align with chart area.

### Phase 2: Header Changes

**File:** `include/ui_temp_control_panel.h`

Add member variables:
```cpp
static constexpr int X_AXIS_LABEL_COUNT = 6;

// X-axis label storage
std::array<lv_obj_t*, X_AXIS_LABEL_COUNT> nozzle_x_labels_{};
std::array<lv_obj_t*, X_AXIS_LABEL_COUNT> bed_x_labels_{};

// Timestamp tracking (start time + count, not per-point)
int64_t nozzle_start_time_ms_ = 0;
int64_t bed_start_time_ms_ = 0;
int nozzle_point_count_ = 0;
int bed_point_count_ = 0;
```

Add method declarations:
```cpp
void create_x_axis_labels(lv_obj_t* container,
                          std::array<lv_obj_t*, X_AXIS_LABEL_COUNT>& labels);
void update_x_axis_labels(std::array<lv_obj_t*, X_AXIS_LABEL_COUNT>& labels,
                          int64_t start_time_ms, int point_count);
```

### Phase 3: Implementation

**File:** `src/ui_temp_control_panel.cpp`

**3.1 Add includes:**
```cpp
#include <chrono>
```

**3.2 Create labels (similar to Y-axis pattern):**
```cpp
void TempControlPanel::create_x_axis_labels(
    lv_obj_t* container,
    std::array<lv_obj_t*, X_AXIS_LABEL_COUNT>& labels) {
    if (!container) return;

    for (int i = 0; i < X_AXIS_LABEL_COUNT; i++) {
        lv_obj_t* label = lv_label_create(container);
        lv_label_set_text(label, "--:--");
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        labels[i] = label;
    }
}
```

**3.3 Update labels on data arrival:**
```cpp
void TempControlPanel::update_x_axis_labels(
    std::array<lv_obj_t*, X_AXIS_LABEL_COUNT>& labels,
    int64_t start_time_ms, int point_count) {

    if (start_time_ms == 0 || point_count == 0) return;

    // Graph shows 300 points at 1-second intervals = 5 minutes
    constexpr int MAX_POINTS = 300;
    constexpr int64_t UPDATE_INTERVAL_MS = 1000;

    // Calculate visible time span
    int visible_points = std::min(point_count, MAX_POINTS);
    int64_t visible_duration_ms = visible_points * UPDATE_INTERVAL_MS;

    // Current time (rightmost point)
    int64_t now_ms = start_time_ms + (point_count * UPDATE_INTERVAL_MS);

    // Oldest visible time (leftmost point)
    int64_t oldest_ms = now_ms - visible_duration_ms;

    // Interval between labels
    int64_t label_interval_ms = visible_duration_ms / (X_AXIS_LABEL_COUNT - 1);

    // Update labels left-to-right (oldest to newest)
    for (int i = 0; i < X_AXIS_LABEL_COUNT; i++) {
        int64_t label_time_ms = oldest_ms + (i * label_interval_ms);
        time_t label_time = static_cast<time_t>(label_time_ms / 1000);

        struct tm* tm_info = localtime(&label_time);
        char buf[8];
        strftime(buf, sizeof(buf), "%H:%M", tm_info);
        lv_label_set_text(labels[i], buf);
    }
}
```

**3.4 Hook into temperature callbacks:**
```cpp
void TempControlPanel::on_nozzle_temp_changed(int temp) {
    nozzle_current_ = temp;
    update_nozzle_display();

    // Track timestamp on first point
    if (nozzle_start_time_ms_ == 0) {
        nozzle_start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    nozzle_point_count_++;

    if (nozzle_graph_ && nozzle_series_id_ >= 0) {
        ui_temp_graph_update_series(nozzle_graph_, nozzle_series_id_,
                                    static_cast<float>(temp));
        update_x_axis_labels(nozzle_x_labels_, nozzle_start_time_ms_,
                            nozzle_point_count_);
    }
}
```

**3.5 Wire up in panel setup:**

In `setup_nozzle_panel()` and `setup_bed_panel()`:
```cpp
lv_obj_t* x_axis_labels = lv_obj_find_by_name(overlay_content, "x_axis_labels");
if (x_axis_labels) {
    create_x_axis_labels(x_axis_labels, nozzle_x_labels_);
}
```

## Files to Modify

| File | Changes |
|------|---------|
| `ui_xml/nozzle_temp_panel.xml` | Add outer container + x_axis_labels row |
| `ui_xml/bed_temp_panel.xml` | Same changes |
| `include/ui_temp_control_panel.h` | Add label arrays + timestamp tracking |
| `src/ui_temp_control_panel.cpp` | Implement create/update methods, hook callbacks |

## Testing Checklist

- [ ] Labels appear below chart, aligned with chart area
- [ ] Initial state shows "--:--" placeholders
- [ ] Labels update with each temperature reading
- [ ] Times scroll left as new data arrives
- [ ] Rightmost label shows current time
- [ ] Works on both nozzle and bed panels
- [ ] Font matches Y-axis labels (montserrat_12)

## Estimate

**Effort:** 2-3 hours
**Risk:** Low (follows existing Y-axis pattern)

## Key Insight

The scrolling math works because we track `start_time_ms` (when first point arrived) and `point_count` (how many points added). As `point_count` increases beyond 300, the `oldest_ms` calculation shifts forward, causing all labels to update with newer times while maintaining the same visual positions.
