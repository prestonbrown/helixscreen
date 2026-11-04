// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if HELIX_HAS_SNAPMAKER

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "snapmaker_nozzle_offset.h"
#include "subject_managed_panel.h"

#include <string>

namespace helix::ui {

/**
 * @brief Guided multi-toolhead offset calibration for the Snapmaker U1
 *
 * Renders snapmaker::nozzle_offset::Runner, which owns the command sequence:
 * this overlay asks the user for the two human steps (brushing each parked
 * nozzle, taking the PEI sheet off), shows which head is on which step, and
 * reads the saved offsets back at the end. Leaving the overlay mid-run cancels
 * it, which sends the firmware's exit so no heater is left on.
 *
 * @pattern Overlay (lazy init, singleton via get_snapmaker_nozzle_offset_overlay())
 * @threading Main thread only
 */
class SnapmakerNozzleOffsetOverlay : public OverlayBase {
  public:
    SnapmakerNozzleOffsetOverlay();
    ~SnapmakerNozzleOffsetOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Toolhead Offset Calibration";
    }
    const char* xml_component() const override {
        return "snapmaker_nozzle_offset_overlay";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

    /// Opens on the intro, unless a run is still going: then it shows that run.
    void open(lv_obj_t* parent);

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

    /**
     * @brief Asks whether to end a calibration state no run here owns
     *
     * Another screen may be mid-run, so only "End calibration" sends EXIT.
     * One prompt at a time; repeated reconnects do not stack them.
     */
    void prompt_end_held_calibration();

    /// The run, for tests that drive the flow without a printer.
    snapmaker::nozzle_offset::Runner& runner() {
        return runner_;
    }

    /// Screen published on the state subject. The XML shows one block per value.
    enum class Screen {
        Intro = 0,
        Working = 1,
        CleanNozzle = 2,
        RemovePlate = 3,
        Done = 4,
        Failed = 5,
    };

  private:
    snapmaker::nozzle_offset::Runner::Hooks make_hooks();
    void send(const std::string& gcode, uint32_t timeout_ms,
              snapmaker::nozzle_offset::Runner::Completion done);
    void abort_probe();
    /// Publishes the runner's phase and step on the subjects.
    void refresh();
    void read_offsets();

    snapmaker::nozzle_offset::Runner runner_;
    SubjectManager subjects_;
    bool held_prompt_open_ = false;
};

inline SnapmakerNozzleOffsetOverlay& get_snapmaker_nozzle_offset_overlay() {
    return lazy_global<SnapmakerNozzleOffsetOverlay>("SnapmakerNozzleOffsetOverlay");
}

/// Pure mapping from the runner's phase and prompt to the screen shown.
[[nodiscard]] SnapmakerNozzleOffsetOverlay::Screen
nozzle_offset_screen(snapmaker::nozzle_offset::Runner::Phase phase,
                     snapmaker::nozzle_offset::Prompt prompt);

} // namespace helix::ui

#endif // HELIX_HAS_SNAPMAKER
