// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file guided_load_flow.h
 * @brief The decisions of a guided filament load, with no UI and no I/O.
 *
 * Some stock load macros are one step of a wizard the printer's own screen
 * drives (filament_macros::guided_load()). This class decides what the next
 * step sends; the modal that shows it and the code that sends it stay thin.
 *
 * Every event returns the gcode to send now, or an empty string for nothing.
 * At most one script is in flight: events that would send while one is running
 * are ignored, and command_finished() reports its outcome.
 */

#pragma once

#include "filament_macro_profiles.h"

#include <string>

namespace helix {

/// A hand-pushed feed: from the extruder gears through the melt zone to the nozzle.
inline constexpr int kFeedMm = 60;
inline constexpr int kFeedSpeedMmMin = 300;
/// Extrude more: nothing came out of the nozzle yet.
inline constexpr int kPurgeMoreMm = 20;
inline constexpr int kPurgeSpeedMmMin = 300;

/// The steps for filament the user pushed into the extruder by hand: a slow feed to
/// the nozzle, then a shorter extrusion for each Extrude more. Nothing to flush,
/// release or clean up, because no printer wizard is waiting.
[[nodiscard]] filament_macros::GuidedLoad hand_feed_steps();

enum class GuidedLoadStage {
    Heating,       ///< The nozzle is heating for the load; nothing is sent until it is hot.
    AwaitPush,     ///< The macro stopped; the user pushes filament into the extruder.
    Feeding,       ///< Extruding to the nozzle (feed or retry in flight).
    ConfirmOutput, ///< Asking whether new filament comes out of the nozzle.
    Finishing,     ///< Flush and/or cleanup in flight.
    Finished,      ///< Done; nothing more is sent.
    Cancelled,     ///< Stopped by the user or by a failed step; cleanup sent.
};

class GuidedLoadFlow {
  public:
    /// @param waits_for_push The printer reported it stopped for a hand push.
    ///        False means the load macro already extruded, so the flow opens on
    ///        the question and has no feed, flush or release step.
    GuidedLoadFlow(filament_macros::GuidedLoad steps, bool waits_for_push);

    /// A load whose nozzle the screen heats first: opens on Heating, and heated()
    /// moves it on to the hand push.
    [[nodiscard]] static GuidedLoadFlow after_preheat(filament_macros::GuidedLoad steps);

    [[nodiscard]] GuidedLoadStage stage() const {
        return stage_;
    }
    [[nodiscard]] bool busy() const {
        return stage_ == GuidedLoadStage::Feeding || stage_ == GuidedLoadStage::Finishing;
    }
    [[nodiscard]] bool ended() const {
        return stage_ == GuidedLoadStage::Finished || stage_ == GuidedLoadStage::Cancelled;
    }

    /// The nozzle reached the load temperature. Moves Heating on to AwaitPush.
    void heated();
    /// The filament sensor went from empty to detected. Feeds from AwaitPush only.
    [[nodiscard]] std::string filament_detected();
    /// The user asked to extrude: the feed from AwaitPush, a retry from ConfirmOutput.
    [[nodiscard]] std::string extrude();
    /// The user saw new filament at the nozzle.
    [[nodiscard]] std::string done();
    /// The user backed out, or the dialog went away. Sends cleanup unless ended.
    [[nodiscard]] std::string cancel();
    /// The in-flight script returned. A failure cancels and returns the cleanup.
    [[nodiscard]] std::string command_finished(bool ok);

  private:
    [[nodiscard]] std::string feed();
    [[nodiscard]] std::string cleanup_script() const;

    filament_macros::GuidedLoad steps_;
    bool waits_for_push_;
    GuidedLoadStage stage_;
};

} // namespace helix
