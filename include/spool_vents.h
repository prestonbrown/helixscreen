// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix::ui {

/// How much vent a spool flange of a given size can show and still read.
enum class SpoolVentDetail : uint8_t { None, Slots, SlotsAndDots };

inline constexpr int32_t kSpoolVentSlotsMinPx = 32;
inline constexpr int32_t kSpoolVentDotsMinPx = 96;

/// The detail a spool canvas @p size_px square draws on its flange.
SpoolVentDetail spool_vent_detail(int32_t size_px);

/// One flange face in canvas pixels: centre, the canvas size the geometry is a
/// fraction of, and the horizontal squash of the projection (tray::kDepthSkew
/// for the side view, 1.0 face-on).
struct SpoolVentFace {
    float cx = 0.0f;
    float cy = 0.0f;
    float size = 0.0f;
    float skew = 1.0f;
    SpoolVentDetail detail = SpoolVentDetail::None;
    float rotation_cos = 1.0f; ///< the face's rotation, set by spool_vent_face_set_phase()
    float rotation_sin = 0.0f;
};

/// Vent phases a turning spool steps through before its vents repeat, 1 when
/// there are no vents to turn. A phase is 7.5 degrees of rotation.
int spool_vent_phase_count(SpoolVentDetail detail);

/// Turn @p face to rotation @p phase; phases wrap at spool_vent_phase_count().
void spool_vent_face_set_phase(SpoolVentFace& face, int phase);

/// How much of the pixel centred on (@p x, @p y) is open vent, 0..1, with an
/// anti-aliased edge. 0 everywhere when the face's detail is None.
float spool_vent_coverage(const SpoolVentFace& face, float x, float y);

/// Distance from the face centre in unskewed face units (pixels of size S),
/// for compositing what lies behind a vent at that radius.
float spool_vent_face_radius(const SpoolVentFace& face, float x, float y);

} // namespace helix::ui
