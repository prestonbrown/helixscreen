// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The guided procedure that tunes a nonlinear pressure-advance model by eye:
// a series of towers, each sweeping one parameter while the others hold.
//
// The tower prints slow-to-fast transitions on its left side and fast ones on
// its front. Direct drive tunes the offset on the left first, then the advance
// until the left and front look best at the same height (moving the offset 10%
// when they disagree), then the time offset on the front. Bowden tunes the
// advance on the front first and keeps 80% of it, then the offset until left
// and front agree (moving the advance 10%), then the time offset, then the
// offset again.
//
// Pure: no LVGL, no network. The overlay prints what current() asks for and
// reports what the user judged.

#include "pa_model.h"
#include "pa_tuning.h"

#include <vector>

namespace helix::pa {

/// Which side of the tower the user judges.
enum class TowerSide : int {
    Left = 0,        ///< slow-to-fast transitions
    Front = 1,       ///< fast transitions
    LeftVsFront = 2, ///< where each side looks best, compared
};

/// Where the left side looked best, relative to the front.
enum class SideVerdict : int { LeftLower = 0, Same = 1, LeftHigher = 2 };

struct NonlinearPass {
    TowerSweep sweep;
    TowerSide judge = TowerSide::Left;
};

class NonlinearProcedure {
  public:
    /// A pass repeated this often without left and front agreeing ends on the
    /// user's height anyway: the 10% steps no longer converge.
    static constexpr int kMaxRepeats = 4;

    /// @p kind sets the order and the sweeps; @p model the model to tune.
    NonlinearProcedure(ExtruderKind kind, ModelKind model);

    /// The pass to print next. Undefined once done().
    const NonlinearPass& current() const {
        return passes_[index_];
    }
    bool done() const {
        return index_ >= passes_.size();
    }
    /// 1-based number of the pass now, and how many there are.
    int pass_number() const {
        return static_cast<int>(index_) + 1;
    }
    int pass_count() const {
        return static_cast<int>(passes_.size());
    }
    /// How often the current pass has been reprinted.
    int repeats() const {
        return repeats_;
    }

    /// The values to run the next tower with, and the result once done().
    const NonlinearValues& values() const {
        return values_;
    }

    /// A one-sided pass: the height where that side looked best. Advances.
    void record_height(float height_mm);
    /// A two-sided pass: Same takes @p height_mm and advances; otherwise the
    /// other parameter moves 10% and the pass repeats, until kMaxRepeats, when
    /// @p height_mm is taken anyway.
    void record_verdict(SideVerdict verdict, float height_mm);

  private:
    void apply(NlParam param, float value);
    void advance();

    ExtruderKind kind_;
    std::vector<NonlinearPass> passes_;
    size_t index_ = 0;
    int repeats_ = 0;
    NonlinearValues values_;
};

} // namespace helix::pa
