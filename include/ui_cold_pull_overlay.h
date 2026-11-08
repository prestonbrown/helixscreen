// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::ui {

/**
 * @brief The guided cold pull's view (prestonbrown/helixscreen#2011).
 *
 * A view over ColdPullRunner's subjects: the run belongs to the runner, so a
 * screensaver or a view pushed on top changes nothing, and closing the overlay
 * mid-run is the abort.
 *
 * @threading Main thread only
 */
class ColdPullOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Cold Pull";
    }
    const char* xml_component() const override {
        return "cold_pull_overlay";
    }
    /// Freed on close; each opening builds from the runner's current state.
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void on_deactivating(DeactivateReason reason) override;
    void on_dismissed() override;
};

ColdPullOverlay& get_cold_pull_overlay();

} // namespace helix::ui
