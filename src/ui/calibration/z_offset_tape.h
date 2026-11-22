// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Z offset step's tape: a ruler that scrolls past a fixed nozzle, zoomed by
// the selected step, with the bed estimated from the last calibration. Pure
// geometry and wording, so the indicator draws it and tests pin it without LVGL.

#include <climits>
#include <optional>
#include <string>
#include <vector>

namespace helix::ztape {

/// An estimate subject's value when there is no estimate.
inline constexpr int kNoEstimate = INT_MIN;

/// The four step sizes, in microns, in the order the step buttons show them.
inline constexpr int kStepMicrons[] = {10, 50, 100, 1000};
inline constexpr int kStepCount = 4;
/// The step selected when the screen opens (0.10 mm).
inline constexpr int kDefaultStepIndex = 2;
/// The step from which the screen warns that a press can reach the bed.
inline constexpr int kRiskyStepIndex = 3;

/// Microns of the step at @p index; an index out of range is the default step.
[[nodiscard]] int step_microns(int index);

/// How much Z the ruler shows and how it is ticked, for one step size.
struct Zoom {
    int span_microns;  ///< Z covered from the ruler's top to its bottom
    int every_microns; ///< Z between ticks: a quarter of the span
};

/// The zoom follows the step: 1 mm shows 4 mm (±2), 0.1 shows 1 mm, 0.05 shows
/// 0.4 mm and 0.01 shows 0.2 mm (±0.1).
[[nodiscard]] Zoom zoom_for_step(int step_microns);

struct Tick {
    int y;       ///< pixel row, in the same space as the tip and ruler bounds
    int microns; ///< the Z it marks
};

/// Ticks on the multiples of the zoom's tick spacing whose rows fall between
/// @p ruler_top and @p ruler_bottom, inclusive. The nozzle tip sits at @p tip_y
/// and marks @p value_microns; higher Z draws higher up.
[[nodiscard]] std::vector<Tick> ticks(int value_microns, Zoom zoom, int tip_y, int ruler_top,
                                      int ruler_bottom);

/// A tick's label: one decimal where that is exact ("1.5", "0.8" never stands
/// for 0.75), otherwise two ("1.25"); whole millimeters at the 1 mm zoom ("2").
[[nodiscard]] std::string tick_label(int microns, Zoom zoom);

/// The range note: "±2 mm", "±0.5 mm", "±0.2 mm", "±0.1 mm".
[[nodiscard]] std::string range_text(Zoom zoom);

/// Where the bed is drawn.
enum class BedView {
    Unknown,     ///< no estimate: the bed below a break, distance unknown
    FarBelow,    ///< estimate further down than the diagram reaches
    Estimated,   ///< the estimated bed, dashed, rising toward the nozzle
    PastLastTime ///< Z is below the estimate: the nozzle rests on it, amber
};

struct BedPlacement {
    BedView view = BedView::Unknown;
    int paper_top = 0; ///< row of the paper's top edge (Estimated, PastLastTime)
};

/// The bed for Z @p value_microns against the estimated drag point
/// @p estimate_microns (kNoEstimate for none). @p static_paper_top is the row
/// the paper sits on when the bed is drawn at the bottom of the diagram; an
/// estimate whose paper would fall below that row draws as FarBelow.
[[nodiscard]] BedPlacement place_bed(int value_microns, int estimate_microns, Zoom zoom, int tip_y,
                                     int ruler_top, int ruler_bottom, int static_paper_top);

/// Where the paper should drag this time, in this run's Z: the Z position the
/// saved offset puts the bed at. PROBE_CALIBRATE starts its manual probe 5 mm
/// over the point the probe triggered, so the drag point is
/// start - 5 mm - saved z_offset. Z_ENDSTOP_CALIBRATE's position_endstop puts
/// the bed at Z=0. Nothing for any other sequence, or when the start is not
/// the probe's own (a session resumed after it moved).
enum class EstimateSource { None, ProbeCalibrate, Endstop };
/// The source for the calibrate command the panel sent: a scanning probe's
/// own calibrate command starts wherever it chooses, so it has none.
[[nodiscard]] EstimateSource estimate_source_for(const std::string& calibrate_gcode);
[[nodiscard]] int expected_drag_microns(EstimateSource source, std::optional<int> start_z_microns,
                                        int saved_offset_microns);

/// The line under the Z readout and its tone.
enum class GuideTone {
    Neutral, ///< no estimate: travel from the start
    Above,   ///< at or above the estimate's drag point
    Past     ///< below it, amber
};
struct Guide {
    std::string text;
    GuideTone tone = GuideTone::Neutral;
};

/// "about 0.765 mm above where the paper dragged last time" against an estimate,
/// "down 0.300 mm from start" without one. @p compact is the micro screen's
/// shorter wording ("0.765 mm above last time"). Texts are translated.
[[nodiscard]] Guide guide(int value_microns, int estimate_microns, int start_microns, bool compact);

} // namespace helix::ztape
