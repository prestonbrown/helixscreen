// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_clog_detection.h
 * @brief Capability of a filament system with a configurable clog detection mode
 *
 * Reached with AmsBackend::as<IClogDetectionConfig>(). Happy Hare implements it
 * with MMU_TEST_CONFIG. The clog widget is offered on backends without it too
 * (AFC buffer fault detection); there the mode and length controls hide.
 */

#pragma once

#include <optional>
#include <string>

namespace helix {

class IClogDetectionConfig {
  public:
    /**
     * @brief G-code that sets clog detection mode, if this machine has the setting.
     *
     * @param mode       0 off, 1 manual (fixed detection length), 2 automatic
     * @param det_length Detection length in mm, sent in manual mode when positive
     * @return The command, or nullopt when this machine has no such setting
     */
    [[nodiscard]] virtual std::optional<std::string>
    clog_detection_mode_gcode(int mode, float det_length) const = 0;

    /**
     * @brief The detection length (mm) a manual clog mode write sets, as the
     *        printer is configured.
     *
     * @return nullopt when the printer reports none; callers then show the
     *         live encoder length
     */
    [[nodiscard]] virtual std::optional<float> clog_detection_length_setting() const = 0;

  protected:
    ~IClogDetectionConfig() = default;
};

} // namespace helix
