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

#include "json_fwd.h"
#include "pa_model.h"
#include "pa_tuning.h"

#include <optional>
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

/// The firmware tower's height (Kalico's [pa_test] default).
inline constexpr float kTowerHeightMm = 50.0f;
/// The firmware tower's layer height (Kalico's [pa_test] default).
inline constexpr float kTowerLayerMm = 0.2f;

/// Where a printing tower is, from the nozzle height alone: every layer above
/// the first is the same shape, so height is the tower's real progress.
struct TowerLayerProgress {
    int layer = 1;
    int layers = 1;
    float fraction = 0.0f; ///< 0..1 of the tower's height
};
TowerLayerProgress tower_layer_progress(float z_mm, float height_mm = kTowerHeightMm,
                                        float layer_mm = kTowerLayerMm);

/// Time left on a tower. The first layer carries the brim and runs slow, so
/// the rate is measured from the second layer on; until the nozzle has climbed
/// kTowerRateClimbMm past that, it is the expected print time less what has
/// printed.
class TowerTimeLeft {
  public:
    static constexpr float kTowerRateClimbMm = 1.0f;
    explicit TowerTimeLeft(int expected_print_s, float height_mm = kTowerHeightMm,
                           float layer_mm = kTowerLayerMm);
    /// The nozzle at @p z_mm, @p t_s seconds into the print.
    void sample(float z_mm, double t_s);
    [[nodiscard]] int seconds_left() const;

  private:
    int expected_s_;
    float height_mm_;
    float layer_mm_;
    float z_ = 0.0f;
    double t_ = 0.0;
    double first_t_ = -1.0;
    float climb_z_ = -1.0f;
    double climb_t_ = 0.0;
};

/// Left and front heights closer than this count as the same.
inline constexpr float kSameHeightMm = 1.0f;

/// Where left and front looked best, as a verdict: Same within kSameHeightMm.
SideVerdict verdict_from_heights(float left_mm, float front_mm);

/// A face of the firmware tower, seen from outside it.
enum class TowerFace : int { Left = 0, Front = 1 };
enum class SpeedKind : int { Fast = 0, Medium = 1, Slow = 2, VerySlow = 3 };
/// SlowDown and SpeedUp are the edges of the slow middle; a slow-down with too
/// little advance leaves a bulge, a speed-up a thin spot.
enum class SpotKind : int { SlowDown = 0, SpeedUp = 1, Pause = 2, VerySlowSpot = 3 };

struct SpeedSegment {
    float from = 0.0f; ///< fraction across the face, in print order
    float to = 0.0f;
    SpeedKind speed = SpeedKind::Fast;
};
struct TowerSpot {
    float at = 0.0f; ///< fraction across the face
    SpotKind kind = SpotKind::SlowDown;
};
/// How the nozzle crosses one face, left to right as seen from outside the
/// tower (Kalico's klippy/extras/pa_test.py, gen_tower).
struct FaceLayout {
    float width_mm = 0.0f;
    std::vector<SpeedSegment> segments;
    std::vector<TowerSpot> spots; ///< in order across the face
};
/// The tower's default footprint, 50 x 40 mm.
inline constexpr float kTowerSizeXMm = 50.0f;
inline constexpr float kTowerSizeYMm = 40.0f;
FaceLayout tower_face_layout(TowerFace face, float size_x_mm = kTowerSizeXMm,
                             float size_y_mm = kTowerSizeYMm);
/// The faces a pass's user looks at, left face first.
std::vector<TowerFace> judged_faces(TowerSide side);

/// What a two-sided verdict changes before the pass prints again.
struct Correction {
    NlParam param = NlParam::Offset;
    bool raise = false; ///< about 10% up; otherwise about 10% down
    float from = 0.0f;  ///< the value the next tower corrects
    float to = 0.0f;    ///< what the next tower prints with
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
    /// Pass @p i of the order, 0-based; @p i < pass_count().
    const NonlinearPass& pass_at(size_t i) const {
        return passes_[i];
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
    /// A two-sided pass judged from where each side looked best: the verdict
    /// comes from verdict_from_heights() and the height taken is the middle.
    /// Both heights are remembered, so going back re-opens them as entered.
    void record_heights(float left_mm, float front_mm);
    /// What record_verdict(@p verdict) would change for the next tower, or
    /// nullopt when it takes the height instead (Same, or the repeat limit).
    std::optional<Correction> correction(SideVerdict verdict) const;

    /// A judgement has been recorded that go_back() can take back.
    bool can_go_back() const {
        return !history_.empty();
    }
    /// Take back the latest judgement: the procedure is on the tower it judged
    /// again, with the values that tower printed with. Returns the height that
    /// was judged, or nullopt when there is nothing to take back.
    std::optional<float> go_back();
    /// The heights a judgement re-opened by go_back_judgement() was made with:
    /// the one height, or for a two-sided pass the left's and the front's.
    struct Reopened {
        float height_mm = 0.0f;
        std::optional<float> left_mm;
        std::optional<float> front_mm;
    };
    /// go_back(), with both of a two-sided pass's heights.
    std::optional<Reopened> go_back_judgement();

    ExtruderKind kind() const {
        return kind_;
    }

    /// Everything a restart would lose, for from_json().
    nlohmann::json to_json() const;
    /// The procedure @p j describes, or nullopt when it describes none this
    /// build can continue (another pass order, an index out of range).
    static std::optional<NonlinearProcedure> from_json(const nlohmann::json& j);

  private:
    /// The state before one judgement, so it can be put back.
    struct Judged {
        size_t index = 0;
        int repeats = 0;
        NonlinearValues values;
        float height_mm = 0.0f;
        std::optional<float> left_mm;
        std::optional<float> front_mm;
    };

    void apply(NlParam param, float value);
    void judge_two_sided(SideVerdict verdict, float height_mm, std::optional<float> left_mm,
                         std::optional<float> front_mm);
    void advance();
    void remember(float height_mm, std::optional<float> left_mm = std::nullopt,
                  std::optional<float> front_mm = std::nullopt);

    ExtruderKind kind_;
    std::vector<NonlinearPass> passes_;
    size_t index_ = 0;
    int repeats_ = 0;
    NonlinearValues values_;
    std::vector<Judged> history_;
};

} // namespace helix::pa
