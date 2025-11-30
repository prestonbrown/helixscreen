# Bed Mesh Phase 4: Moonraker Integration Plan

**Status:** In Progress
**Goal:** Replace test mesh data with real bed mesh from Moonraker API
**Dependencies:** Phases 1-3 complete (settings panel, renderer, visualization UI)

---

## Overview

Phase 4 integrates the bed mesh visualization with Moonraker's real-time printer state. The bed mesh data comes from Klipper via Moonraker's `printer.objects.subscribe` API, providing reactive updates when mesh data changes (e.g., after `BED_MESH_CALIBRATE`).

---

## Moonraker Bed Mesh Data Structure

### JSON Path in Printer State

```
/printer_state/bed_mesh
```

### Complete Structure

```json
{
  "bed_mesh": {
    "profile_name": "default",           // Currently active profile
    "mesh_min": [0.0, 0.0],             // Min X,Y coordinates
    "mesh_max": [200.0, 200.0],         // Max X,Y coordinates
    "probed_matrix": [                  // 2D array of Z heights (row-major)
      [0.1, 0.15, 0.12, ...],           // First row (Y=min)
      [0.09, 0.11, 0.10, ...],          // Second row
      ...
    ],
    "profiles": {                        // Available mesh profiles
      "default": {
        "points": [[0.1, 0.15], ...],   // Raw probe points
        "mesh_params": {
          "min_x": 0.0,
          "max_x": 200.0,
          "min_y": 0.0,
          "max_y": 200.0,
          "x_count": 5,                  // Probes per row
          "y_count": 5,                  // Number of rows
          "algo": "lagrange",            // Interpolation algorithm
          "tension": 0.2,
          "mesh_x_pps": 2,               // Points per segment (X)
          "mesh_y_pps": 2                // Points per segment (Y)
        }
      },
      "adaptive": { ... },
      "calibration": { ... }
    }
  }
}
```

### Key Fields

| Field | Type | Description |
|-------|------|-------------|
| `profile_name` | string | Active mesh profile |
| `mesh_min` | [float, float] | Minimum X,Y coordinates of mesh |
| `mesh_max` | [float, float] | Maximum X,Y coordinates of mesh |
| `probed_matrix` | float[][] | 2D array of Z heights (row-major order) |
| `profiles` | object | Dictionary of available mesh profiles |

---

## Implementation Tasks

### Task 1: Add Bed Mesh Data to MoonrakerClient

**File:** `include/moonraker_client.h`, `src/moonraker_client.cpp`

**Changes:**

1. **Add bed mesh data structure** (header):
```cpp
/**
 * @brief Bed mesh profile data from Klipper
 */
struct BedMeshProfile {
    std::string name;                              // Profile name
    std::vector<std::vector<float>> probed_matrix; // Z height grid (row-major)
    float mesh_min[2];                             // Min X,Y coordinates
    float mesh_max[2];                             // Max X,Y coordinates
    int x_count;                                   // Probes per row
    int y_count;                                   // Number of rows
    std::string algo;                              // Interpolation algorithm

    BedMeshProfile() : mesh_min{0, 0}, mesh_max{0, 0}, x_count(0), y_count(0) {}
};
```

2. **Add bed mesh accessors** (header):
```cpp
/**
 * @brief Get currently active bed mesh profile
 * @return Active mesh profile, or empty profile if none loaded
 */
const BedMeshProfile& get_active_bed_mesh() const {
    return active_bed_mesh_;
}

/**
 * @brief Get list of available mesh profile names
 * @return Vector of profile names (e.g., "default", "adaptive")
 */
const std::vector<std::string>& get_bed_mesh_profiles() const {
    return bed_mesh_profiles_;
}

/**
 * @brief Check if bed mesh data is available
 * @return true if mesh has been loaded
 */
bool has_bed_mesh() const {
    return !active_bed_mesh_.probed_matrix.empty();
}
```

3. **Add protected member variables**:
```cpp
protected:
    BedMeshProfile active_bed_mesh_;            // Currently active mesh
    std::vector<std::string> bed_mesh_profiles_; // Available profile names
```

4. **Parse bed mesh in notify_status_update callback** (implementation):
```cpp
// In onMessage() or notify handler:
if (result.contains("bed_mesh")) {
    parse_bed_mesh(result["bed_mesh"]);
}
```

5. **Implement parse_bed_mesh()** (new method):
```cpp
void MoonrakerClient::parse_bed_mesh(const json& bed_mesh) {
    // Parse active profile name
    if (bed_mesh.contains("profile_name") && !bed_mesh["profile_name"].is_null()) {
        active_bed_mesh_.name = bed_mesh["profile_name"].get<std::string>();
    }

    // Parse probed_matrix
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        for (const auto& row : bed_mesh["probed_matrix"]) {
            std::vector<float> row_vec;
            for (const auto& val : row) {
                row_vec.push_back(val.get<float>());
            }
            active_bed_mesh_.probed_matrix.push_back(row_vec);
        }

        // Update dimensions
        active_bed_mesh_.y_count = active_bed_mesh_.probed_matrix.size();
        active_bed_mesh_.x_count = active_bed_mesh_.probed_matrix.empty() ?
            0 : active_bed_mesh_.probed_matrix[0].size();
    }

    // Parse mesh bounds
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array()) {
        active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].get<float>();
        active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].get<float>();
    }

    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array()) {
        active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].get<float>();
        active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].get<float>();
    }

    // Parse available profiles
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        for (auto& [profile_name, profile_data] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(profile_name);
        }
    }

    spdlog::info("[Moonraker] Bed mesh updated: {} ({}x{} points)",
                 active_bed_mesh_.name,
                 active_bed_mesh_.x_count,
                 active_bed_mesh_.y_count);
}
```

6. **Subscribe to bed_mesh in discover_printer()**:
```cpp
// In discover_printer(), add "bed_mesh" to subscription objects:
json subscribe_params = {
    {"objects", {
        {"toolhead", json::array({"position", "print_time"})},
        {"extruder", json::array({"temperature", "target"})},
        {"heater_bed", json::array({"temperature", "target"})},
        {"bed_mesh", nullptr}  // Subscribe to all bed_mesh fields
    }}
};
```

---

### Task 2: Add Bed Mesh Subjects for Reactive UI

**File:** `src/ui_panel_bed_mesh.cpp`

**Changes:**

1. **Create subjects** (after existing static variables):
```cpp
// Reactive subjects for bed mesh data
static lv_subject_t bed_mesh_available;        // 0 = no mesh, 1 = mesh loaded
static lv_subject_t bed_mesh_profile_name;     // String: active profile name
static lv_subject_t bed_mesh_dimensions;       // String: "10x10 points"
static lv_subject_t bed_mesh_z_range;          // String: "Z: 0.05 to 0.35mm"
```

2. **Initialize subjects in ui_panel_bed_mesh_init_subjects()**:
```cpp
void ui_panel_bed_mesh_init_subjects() {
    lv_subject_init_int(&bed_mesh_available, 0);
    lv_subject_init_string(&bed_mesh_profile_name, "");
    lv_subject_init_string(&bed_mesh_dimensions, "No mesh data");
    lv_subject_init_string(&bed_mesh_z_range, "");

    spdlog::debug("[BedMesh] Subjects initialized");
}
```

3. **Register Moonraker callback in ui_panel_bed_mesh_setup()**:
```cpp
// Register callback for bed mesh updates
MoonrakerClient* client = get_moonraker_client();
if (client) {
    client->register_notify_update([](json notification) {
        if (notification.contains("bed_mesh")) {
            on_bed_mesh_update(client->get_active_bed_mesh());
        }
    });
}
```

4. **Implement on_bed_mesh_update()** (new function):
```cpp
static void on_bed_mesh_update(const MoonrakerClient::BedMeshProfile& mesh) {
    if (mesh.probed_matrix.empty()) {
        lv_subject_set_int(&bed_mesh_available, 0);
        lv_subject_set_string(&bed_mesh_dimensions, "No mesh data");
        lv_subject_set_string(&bed_mesh_z_range, "");
        spdlog::warn("[BedMesh] No mesh data available");
        return;
    }

    // Update subjects
    lv_subject_set_int(&bed_mesh_available, 1);
    lv_subject_set_string(&bed_mesh_profile_name, mesh.name.c_str());

    // Format dimensions
    char dim_buf[64];
    snprintf(dim_buf, sizeof(dim_buf), "%dx%d points", mesh.x_count, mesh.y_count);
    lv_subject_set_string(&bed_mesh_dimensions, dim_buf);

    // Calculate Z range
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();
    for (const auto& row : mesh.probed_matrix) {
        for (float z : row) {
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
    }

    // Format Z range
    char z_buf[64];
    snprintf(z_buf, sizeof(z_buf), "Z: %.3f to %.3f mm", min_z, max_z);
    lv_subject_set_string(&bed_mesh_z_range, z_buf);

    // Update renderer with new mesh data
    ui_panel_bed_mesh_set_data(mesh.probed_matrix);

    spdlog::info("[BedMesh] Mesh updated: {} ({}x{}, Z: {:.3f} to {:.3f})",
                 mesh.name, mesh.x_count, mesh.y_count, min_z, max_z);
}
```

5. **Load mesh on panel setup** (in ui_panel_bed_mesh_setup()):
```cpp
// Load initial mesh data from MoonrakerClient
MoonrakerClient* client = get_moonraker_client();
if (client && client->has_bed_mesh()) {
    on_bed_mesh_update(client->get_active_bed_mesh());
} else {
    // Fall back to test mesh if no real data
    spdlog::warn("[BedMesh] No mesh data from Moonraker, using test mesh");
    std::vector<std::vector<float>> test_mesh;
    create_test_mesh_data(test_mesh);
    ui_panel_bed_mesh_set_data(test_mesh);
}
```

---

### Task 3: Update UI to Display Mesh Metadata

**File:** `ui_xml/bed_mesh_panel.xml`

**Changes:**

1. **Add profile selector dropdown** (after mesh_info_label):
```xml
<!-- Profile selector -->
<lv_dropdown name="mesh_profile_dropdown"
             width="150"
             style_text_font="lv_font_montserrat_14"
             style_pad_all="8">
  <lv_dropdown-bind_text subject="bed_mesh_profile_name"/>
</lv_dropdown>
```

2. **Add Z range label** (after mesh_info_label):
```xml
<!-- Z range label -->
<lv_label name="mesh_z_range_label"
          text=""
          style_text_font="lv_font_montserrat_14"
          style_text_color="lv_xml_get_const('text_secondary')">
  <lv_label-bind_text subject="bed_mesh_z_range"/>
</lv_label>
```

3. **Update mesh_info_label to use subject**:
```xml
<lv_label name="mesh_info_label"
          text="No mesh data"
          style_text_font="lv_font_montserrat_16"
          style_text_color="lv_xml_get_const('text_secondary')">
  <lv_label-bind_text subject="bed_mesh_dimensions"/>
</lv_label>
```

4. **Add "no mesh" placeholder** (overlay on canvas):
```xml
<!-- No mesh placeholder (shown when bed_mesh_available == 0) -->
<lv_obj name="no_mesh_placeholder"
        width="lv_pct(100)"
        height="lv_pct(100)"
        style_bg_opa="0"
        style_align="center">
  <lv_obj-bind_flag_if_eq subject="bed_mesh_available" flag="hidden" ref_value="1"/>

  <lv_label text="No bed mesh data available\nRun BED_MESH_CALIBRATE to generate mesh"
            style_text_align="center"
            style_text_font="lv_font_montserrat_18"
            style_text_color="lv_xml_get_const('text_secondary')"
            style_align="center"/>
</lv_obj>
```

---

### Task 4: Mock Backend Support

**File:** `src/moonraker_client_mock.cpp`

**Changes:**

1. **Generate synthetic mesh in discover_printer()**:
```cpp
void MoonrakerClientMock::discover_printer(std::function<void()> on_complete) {
    // ... existing mock hardware discovery ...

    // Generate synthetic bed mesh
    generate_mock_bed_mesh();

    if (on_complete) {
        on_complete();
    }
}
```

2. **Implement generate_mock_bed_mesh()** (new method):
```cpp
void MoonrakerClientMock::generate_mock_bed_mesh() {
    active_bed_mesh_.name = "default";
    active_bed_mesh_.mesh_min[0] = 0.0f;
    active_bed_mesh_.mesh_min[1] = 0.0f;
    active_bed_mesh_.mesh_max[0] = 200.0f;
    active_bed_mesh_.mesh_max[1] = 200.0f;
    active_bed_mesh_.x_count = 7;
    active_bed_mesh_.y_count = 7;
    active_bed_mesh_.algo = "lagrange";

    // Generate dome shape (matches test mesh from Phase 3)
    active_bed_mesh_.probed_matrix.clear();
    float center_x = active_bed_mesh_.x_count / 2.0f;
    float center_y = active_bed_mesh_.y_count / 2.0f;
    float max_radius = std::min(center_x, center_y);

    for (int row = 0; row < active_bed_mesh_.y_count; row++) {
        std::vector<float> row_vec;
        for (int col = 0; col < active_bed_mesh_.x_count; col++) {
            float dx = col - center_x;
            float dy = row - center_y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float normalized_dist = dist / max_radius;
            float height = 0.3f * (1.0f - normalized_dist * normalized_dist);
            row_vec.push_back(height);
        }
        active_bed_mesh_.probed_matrix.push_back(row_vec);
    }

    bed_mesh_profiles_ = {"default", "adaptive"};

    spdlog::info("[MockClient] Generated synthetic bed mesh: {}x{} dome",
                 active_bed_mesh_.x_count, active_bed_mesh_.y_count);
}
```

---

## Testing Plan

### Unit Testing

1. **Test MoonrakerClient::parse_bed_mesh()**
   - Valid mesh JSON → correct BedMeshProfile
   - Empty mesh → empty probed_matrix
   - Missing fields → graceful degradation
   - Multiple profiles → correct profile list

2. **Test subject updates**
   - Mesh update → subjects change
   - No mesh → subjects show "No mesh data"
   - Profile switch → profile name updates

### Integration Testing

1. **Test with mock backend**
   ```bash
   ./build/bin/helix-ui-proto --test -p settings
   ```
   - Click Bed Mesh card
   - Verify synthetic dome mesh renders
   - Verify dimensions label shows "7x7 points"
   - Verify Z range label shows correct min/max

2. **Test with real printer**
   ```bash
   ./build/bin/helix-ui-proto
   ```
   - Complete wizard setup
   - Navigate to Settings → Bed Mesh
   - If no mesh: verify "No mesh data" placeholder shows
   - Run `BED_MESH_CALIBRATE` in console
   - Verify mesh updates in real-time
   - Verify rotation controls work with real mesh

3. **Test profile switching** (future enhancement)
   - Load multiple profiles via `BED_MESH_PROFILE LOAD=adaptive`
   - Verify dropdown populates with profile names
   - Click profile → mesh updates

---

## Success Criteria

- ✅ MoonrakerClient parses bed_mesh from printer state
- ✅ Bed mesh subjects update reactively
- ✅ UI displays mesh dimensions and Z range
- ✅ "No mesh data" placeholder shows when mesh unavailable
- ✅ Real mesh data renders correctly (not just test dome)
- ✅ Mock backend generates synthetic mesh
- ✅ Real-time updates work during BED_MESH_CALIBRATE
- ✅ Zero compilation warnings
- ✅ No memory leaks (renderer cleanup on panel delete)

---

## Future Enhancements (Phase 5)

- **Profile Management UI**
  - Profile selector dropdown (switch between default, adaptive, calibration)
  - Save/load profiles via Moonraker API
  - Delete profiles
  - Export profiles to file

- **Mesh Calibration Controls**
  - "Run Calibration" button → execute BED_MESH_CALIBRATE
  - Progress indicator during calibration
  - Real-time mesh updates as probing occurs

- **Advanced Visualization**
  - Toggle wireframe mode
  - Toggle color gradient band
  - Adjustable color mapping (rainbow, plasma, etc.)
  - Display probe points as markers

---

## References

- **GuppyScreen Implementation:** `/Users/pbrown/code/guppyscreen/src/bedmesh_panel.cpp` (lines 356-569)
- **Moonraker API:** WebSocket `printer.objects.subscribe` with `bed_mesh` object
- **Klipper Docs:** [Bed Mesh Module](https://www.klipper3d.org/Bed_Mesh.html)
- **Phase 2 Renderer:** `include/bed_mesh_renderer.h`, `src/bed_mesh_renderer.cpp`
- **Phase 3 UI:** `ui_xml/bed_mesh_panel.xml`, `src/ui_panel_bed_mesh.cpp`
