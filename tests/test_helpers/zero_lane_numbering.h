// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "display_numbering.h"

/// Counts positions from 0 for one scope, then restores the default.
struct ScopedZeroLaneNumbering {
    ScopedZeroLaneNumbering() {
        helix::ui::set_lane_numbering_from_zero(true);
    }
    ~ScopedZeroLaneNumbering() {
        helix::ui::set_lane_numbering_from_zero(false);
    }
};
