// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace helix {

/**
 * @brief Toolhead-position activity inference for silent pre-print windows.
 *
 * Some firmwares (Creality K1-class) stop echoing gcode_response for minutes
 * while they home Z, validate the loaded mesh at its corners, and sweep a
 * calibration mesh — but Klipper keeps pushing toolhead.position the whole
 * time. This classifier turns that position stream into one of the prep
 * activities so the collector can keep the status line truthful through the
 * silence.
 *
 * Input coordinates are gcode/toolhead millimetres. Bounds are the BED MESH
 * probe area (mesh_min/mesh_max from the bed_mesh status object), not the
 * axis travel: Z-probing happens at the mesh centre, mesh validation at the
 * mesh corners, and the K1-class wipe strip sits BEYOND mesh_max at the bed
 * rear. Until bounds arrive, classification returns NONE.
 *
 * Pure logic: no subjects, no threads, no LVGL. The collector owns one and
 * feeds it from the position observers.
 */
enum class PositionActivity {
    NONE,         ///< Not enough evidence yet (or bounds unknown)
    WIPE,         ///< Rear-strip oscillation — nozzle cleaning/wiping
    CENTER_PROBE, ///< Repeated Z descents near the mesh centre — Z homing/probing
    CORNER_PROBE, ///< Touches at ≥3 distinct mesh corners — bed-mesh validation
    RASTER,       ///< Monotonic grid march — bed-mesh calibration sweep
};

class PrintStartPositionClassifier {
  public:
    /// Feed one position sample (mm) with a monotonic millisecond clock
    /// (collector uptime works; corpus tests feed capture-relative times).
    /// Near-duplicate positions are dropped.
    void note_position(float x_mm, float y_mm, float z_mm, uint64_t now_ms);

    /// Mesh probe-area bounds (gcode mm) from the bed_mesh status object.
    void set_bounds(float x_min, float x_max, float y_min, float y_max);

    /// Current inference over the recent-sample window.
    PositionActivity activity() const {
        return activity_;
    }

    /// Test/diagnostic access to the retained window size.
    size_t window_size() const {
        return samples_.size();
    }

    /// Drop all evidence (new print start).
    void reset();

  private:
    struct Sample {
        float x;
        float y;
        float z;
        uint64_t t;
    };

    void reclassify();

    bool have_bounds_ = false;
    float x_min_ = 0.0f, x_max_ = 0.0f, y_min_ = 0.0f, y_max_ = 0.0f;

    std::deque<Sample> samples_;
    PositionActivity activity_ = PositionActivity::NONE;
};

} // namespace helix
