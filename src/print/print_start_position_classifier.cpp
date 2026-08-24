// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_position_classifier.h"

#include <cmath>

namespace helix {

namespace {

/// Evidence older than this stops counting. Long enough to span a K1-class
/// corner validation (4 corners at ~7.5 s apart, measured on hardware);
/// short enough that one activity's evidence retires once the chain moves on.
constexpr uint64_t EVIDENCE_WINDOW_MS = 45'000;

/// A wipe verdict requires every sample this recent to sit in the wipe zone.
constexpr uint64_t WIPE_RECENCY_MS = 8'000;

/// Zone sizes as fractions of the mesh extent.
constexpr float CENTER_ZONE_FRACTION = 0.12f;
constexpr float CORNER_ZONE_FRACTION = 0.10f;

/// Minimum X spacing between consecutive raster-march samples (mm). Mesh
/// rows step tens of mm; corner-to-corner jumps break the row tolerance and
/// wipe oscillation reverses direction long before a run of three forms.
constexpr float RASTER_MIN_STEP_MM = 8.0f;

/// Y tolerance for "same row" while marching (mm).
constexpr float RASTER_ROW_TOLERANCE_MM = 5.0f;

/// How far past mesh_max the toolhead must sit before it reads as the wipe
/// strip (mm) — skips travel noise right at the mesh edge.
constexpr float WIPE_MARGIN_MM = 2.0f;

/// Distinct-corner count that says "validation tour", not "sweep row ends"
/// (a sweep row brushes at most two corners).
constexpr int CORNER_DISTINCT_MIN = 3;

/// Minimum centre-zone samples before CENTER_PROBE is declared, and the Z
/// descent (mm) those samples must show somewhere — a parked toolhead at the
/// centre is not probing.
constexpr int CENTER_SAMPLE_MIN = 3;
constexpr float CENTER_DESCENT_Z_MM = 4.0f;

int corner_index(float x, float y, float x_min, float x_max, float y_min, float y_max) {
    const float w_x = x_max - x_min;
    const float w_y = y_max - y_min;
    if (w_x <= 0.0f || w_y <= 0.0f) {
        return -1;
    }
    const bool near_x_min = (x - x_min) <= w_x * CORNER_ZONE_FRACTION;
    const bool near_x_max = (x_max - x) <= w_x * CORNER_ZONE_FRACTION;
    const bool near_y_min = (y - y_min) <= w_y * CORNER_ZONE_FRACTION;
    const bool near_y_max = (y_max - y) <= w_y * CORNER_ZONE_FRACTION;
    if (near_x_min && near_y_min)
        return 0;
    if (near_x_min && near_y_max)
        return 1;
    if (near_x_max && near_y_min)
        return 2;
    if (near_x_max && near_y_max)
        return 3;
    return -1;
}

} // namespace

void PrintStartPositionClassifier::set_bounds(float x_min, float x_max, float y_min, float y_max) {
    if (x_max <= x_min || y_max <= y_min) {
        return; // nonsense bounds — keep whatever we had
    }
    x_min_ = x_min;
    x_max_ = x_max;
    y_min_ = y_min;
    y_max_ = y_max;
    have_bounds_ = true;
    reclassify();
}

void PrintStartPositionClassifier::reset() {
    samples_.clear();
    activity_ = PositionActivity::NONE;
}

void PrintStartPositionClassifier::note_position(float x_mm, float y_mm, float z_mm,
                                                 uint64_t now_ms) {
    if (!samples_.empty()) {
        const Sample& last = samples_.back();
        const bool duplicate = std::fabs(last.x - x_mm) < 0.05f &&
                               std::fabs(last.y - y_mm) < 0.05f && std::fabs(last.z - z_mm) < 0.05f;
        if (duplicate) {
            return; // same resting position — nothing new to say
        }
    }
    samples_.push_back({x_mm, y_mm, z_mm, now_ms});
    while (!samples_.empty() && now_ms - samples_.front().t > EVIDENCE_WINDOW_MS) {
        samples_.pop_front();
    }
    reclassify();
}

void PrintStartPositionClassifier::reclassify() {
    if (!have_bounds_ || samples_.empty()) {
        activity_ = PositionActivity::NONE;
        return;
    }

    const uint64_t now = samples_.back().t;

    // --- RASTER: three time-consecutive samples marching the same direction
    // in X along a row of near-constant Y, all inside the mesh area.
    for (size_t i = 0; i + 2 < samples_.size(); ++i) {
        const Sample& a = samples_[i];
        const Sample& b = samples_[i + 1];
        const Sample& c = samples_[i + 2];
        if (now - a.t > EVIDENCE_WINDOW_MS) {
            continue;
        }
        if (a.y < y_min_ || a.y > y_max_) {
            continue;
        }
        if (std::fabs(a.y - b.y) > RASTER_ROW_TOLERANCE_MM ||
            std::fabs(b.y - c.y) > RASTER_ROW_TOLERANCE_MM) {
            continue;
        }
        const float dx1 = b.x - a.x;
        const float dx2 = c.x - b.x;
        if (std::fabs(dx1) < RASTER_MIN_STEP_MM || std::fabs(dx2) < RASTER_MIN_STEP_MM) {
            continue;
        }
        if ((dx1 > 0.0f) != (dx2 > 0.0f)) {
            continue; // direction reversed — oscillation, not a sweep
        }
        activity_ = PositionActivity::RASTER;
        return;
    }

    // --- WIPE: every sample in the recent slice sits beyond mesh_max at the
    // bed rear (the K1-class wipe strip). Current position outranks stale
    // evidence from earlier activities, so this is checked before corner and
    // centre.
    {
        const float wipe_y = y_max_ + WIPE_MARGIN_MM;
        size_t recent = 0;
        bool all_wipe = true;
        for (auto it = samples_.rbegin(); it != samples_.rend() && now - it->t <= WIPE_RECENCY_MS;
             ++it) {
            ++recent;
            if (it->y <= wipe_y) {
                all_wipe = false;
                break;
            }
        }
        if (all_wipe && recent >= 2) {
            activity_ = PositionActivity::WIPE;
            return;
        }
    }

    // --- CORNER: touches at enough DISTINCT mesh corners to be a validation
    // tour. A calibration sweep's first/last rows brush two corners at most.
    {
        bool seen[4] = {false, false, false, false};
        int distinct = 0;
        for (const auto& s : samples_) {
            const int idx = corner_index(s.x, s.y, x_min_, x_max_, y_min_, y_max_);
            if (idx >= 0 && !seen[idx]) {
                seen[idx] = true;
                ++distinct;
            }
        }
        if (distinct >= CORNER_DISTINCT_MIN) {
            activity_ = PositionActivity::CORNER_PROBE;
            return;
        }
    }

    // --- CENTER: several samples near the mesh centre with a Z descent
    // among them (ACCURATE_G28-style probing hovers and dips).
    {
        const float cx = (x_min_ + x_max_) / 2.0f;
        const float cy = (y_min_ + y_max_) / 2.0f;
        const float tol_x = (x_max_ - x_min_) * CENTER_ZONE_FRACTION;
        const float tol_y = (y_max_ - y_min_) * CENTER_ZONE_FRACTION;
        int count = 0;
        float min_z = 1e9f;
        for (const auto& s : samples_) {
            if (std::fabs(s.x - cx) <= tol_x && std::fabs(s.y - cy) <= tol_y) {
                ++count;
                min_z = std::fmin(min_z, s.z);
            }
        }
        if (count >= CENTER_SAMPLE_MIN && min_z <= CENTER_DESCENT_Z_MM) {
            activity_ = PositionActivity::CENTER_PROBE;
            return;
        }
    }

    activity_ = PositionActivity::NONE;
}

} // namespace helix
