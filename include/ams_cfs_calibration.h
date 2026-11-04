// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_cfs_calibration.h
 * @brief Capability of a filament system that calibrates its cutter and purge chute
 *
 * Reached with AmsBackend::as<ICfsCalibration>(). The step-by-step contract (which
 * callbacks fire when, and on which thread) is documented on AmsBackendCfs.
 */

#pragma once

#include "ams_error.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace helix {

class ICfsCalibration {
  public:
    /// Sweep the cutter to find its position. @p on_result fires once with the outcome.
    virtual AmsError
    calibrate_cutter(std::function<void(bool ok, const std::string& line)> on_result = nullptr) = 0;

    /// Home and park for chute calibration; @p on_ready fires when jogging may start.
    virtual AmsError
    start_chute_calibration(std::function<void()> on_ready = nullptr,
                            std::function<void(const std::string& klipper_msg)> on_failed = nullptr,
                            std::shared_ptr<std::atomic<bool>> cancel_requested = nullptr) = 0;

    /// Jog the toolhead's Y by @p delta_mm during chute calibration.
    virtual AmsError jog_chute_y(float delta_mm) = 0;

    /// Jog the toolhead's X by @p delta_mm during chute calibration. Sent only
    /// after the caller's ChuteJogGuard has allowed the move.
    virtual AmsError jog_chute_x(float delta_mm) = 0;

    /// Save the current toolhead position as the chute's extrude position.
    virtual AmsError save_chute_position(
        std::function<void()> on_saved = nullptr,
        std::function<void(const std::string& klipper_msg)> on_failed = nullptr) = 0;

    /// Abort path: re-park the toolhead once chute calibration has moved it.
    virtual AmsError exit_chute_calibration() = 0;

    /// The position pair the last chute save reported; false when none was captured.
    [[nodiscard]] virtual bool last_chute_saved_position(double& x_mm, double& y_mm) const = 0;

  protected:
    ~ICfsCalibration() = default;
};

} // namespace helix
