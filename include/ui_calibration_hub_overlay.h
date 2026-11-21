// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Calibration screen: a tile per calibration step this printer has, with
// what each last measured and when. Each tile opens the step's own screen.

#include "calibration_catalog.h"
#include "core/subject.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>

namespace helix::calibration {
struct HistoryEntry;
}

namespace helix::ui {

/// The color of a tile's status line; the values are calibration_hub_tile.xml's.
enum class TileTone : int { Idle = 0, Due = 1, Done = 2 };

/// What a tile's status line says about a step: its last result, else the
/// result the printer's config holds (@p in_config, no date), else its tier.
struct TileStatus {
    std::string text;
    TileTone tone;
};
[[nodiscard]] TileStatus tile_status(const calibration::StepInfo& step,
                                     const calibration::HistoryEntry* last,
                                     const std::string* in_config);

class CalibrationHubOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Calibration";
    }
    const char* xml_component() const override {
        return "calibration_hub_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    /// Per step id: cal_shown_<id>, cal_tone_<id>, cal_status_<id>.
    void init_subjects() override;

    /// Back from a step's screen: its result may have just landed.
    void on_activate() override;

  protected:
    void before_show() override;

  private:
    /// Publish every step's tile from the catalog and the history.
    void refresh();

    static constexpr size_t kStatusLen = 96;
    SubjectManager subjects_;
    std::array<std::string, calibration::kStepCount * 3> names_;
    std::array<helix::Subject, calibration::kStepCount> shown_{};
    std::array<helix::Subject, calibration::kStepCount> tone_{};
    std::array<helix::Subject, calibration::kStepCount> status_{};
    char status_bufs_[calibration::kStepCount][kStatusLen]{};
};

CalibrationHubOverlay& get_calibration_hub_overlay();

/// Registers the Advanced panel's Calibration row callback. Startup, before XML.
void init_calibration_hub_row_handler();

} // namespace helix::ui
