// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The three linear axes, shared by everything that is "per axis" — homing
// state, per-tool offsets. One enum so an X here is the same X there and a
// caller never has to translate between two spellings of the same thing.

namespace helix {

enum class Axis { X, Y, Z };

/// 0, 1, 2 — for indexing a per-axis array.
constexpr int axis_index(Axis a) {
    return static_cast<int>(a);
}

/// 'x', 'y', 'z' — the letter Klipper uses in homed_axes and parameter names.
constexpr char axis_letter(Axis a) {
    return "xyz"[axis_index(a)];
}

/// Every axis, in X/Y/Z order, for range-for loops.
inline constexpr Axis kAllAxes[] = {Axis::X, Axis::Y, Axis::Z};

} // namespace helix
