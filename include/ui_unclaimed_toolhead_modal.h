// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

namespace helix::ui {

/**
 * @brief Asks what the filament at the toolhead is when no lane claims it.
 *
 * AmsState decides when to ask (ams::UnclaimedToolheadPromptGate) and when the
 * question has gone away. Use as bypass engages the bypass, Unload runs the
 * toolhead unload, Not now closes. Each action re-checks the state at tap time
 * and does nothing once it no longer holds. Every close tells AmsState.
 */
class UnclaimedToolheadModal : public Modal {
  public:
    /// Show the prompt, unless one is already up.
    static void offer();
    /// Close the prompt if it is up.
    static void dismiss();

    [[nodiscard]] const char* get_name() const override {
        return "Unclaimed Toolhead Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "unclaimed_toolhead_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;
    void on_ok() override;
    void on_tertiary() override;
};

} // namespace helix::ui
