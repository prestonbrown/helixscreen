// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_firmware_prompt_dedup.h
 * @brief Capability of a filament system whose firmware raises its own dialog
 *        for a fault the recovery popup already shows
 *
 * Reached with AmsBackend::as<IFirmwarePromptDedup>(). Happy Hare implements it
 * for its pause and error notices. The recovery presenter closes such a prompt
 * when it raises the popup, so one fault reads as one dialog.
 */

#pragma once

#include <string>

namespace helix {

class IFirmwarePromptDedup {
  public:
    /// Whether the firmware prompt titled @p title is this backend's own dialog
    /// for a fault the recovery popup already shows.
    [[nodiscard]] virtual bool duplicates_firmware_prompt(const std::string& title) const = 0;

    /// Whether a button sending @p gcode on a prompt duplicates_firmware_prompt()
    /// claims is one of that dialog's own actions, which the recovery popup
    /// covers. A firmware dialog can carry buttons a user's macros added, and
    /// closing it would take those away, so the presenter keeps a prompt with
    /// any button this answers false for.
    [[nodiscard]] virtual bool firmware_prompt_button_is_own(const std::string& gcode) const = 0;

  protected:
    ~IFirmwarePromptDedup() = default;
};

} // namespace helix
