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
#include "chute_jog_guard.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct MoonrakerError; // NAMESPACE_OK: MoonrakerError itself is global (moonraker_error.h)

namespace helix {

class ICfsCalibration {
  public:
    /// Sweep the cutter to find its position. @p on_result fires once with the outcome.
    virtual AmsError
    calibrate_cutter(std::function<void(bool ok, const std::string& line)> on_result = nullptr) = 0;

    /// Run the printer's motorized belt tensioning. @p on_result fires once with the
    /// outcome. Not every CFS printer has motorized tensioners.
    virtual AmsError
    tension_belts(std::function<void(bool ok, const std::string& detail)> on_result = nullptr) {
        (void)on_result;
        return AmsErrorHelper::not_supported("Belt tensioning");
    }

    /// True when the cutter calibration is the K1 box sweep, which takes about a
    /// minute. Other firmware's cutter command has no measured duration.
    [[nodiscard]] virtual bool cutter_calibration_takes_a_minute() const {
        return false;
    }

    /// A chute calibration step failed with @p err. When err.raised_by_klipper()
    /// GcodeErrorRouter has reported it already, from Klipper's `!!` copy.
    using ChuteStepFailed = std::function<void(const MoonrakerError& err)>;
    /// A chute save failed. @p position_saved: the save itself succeeded and
    /// the park after it failed, so the toolhead may still be at the chute.
    using ChuteSaveFailed = std::function<void(const MoonrakerError& err, bool position_saved)>;

    /// Home and park for chute calibration; @p on_ready fires when jogging may start.
    virtual AmsError
    start_chute_calibration(std::function<void()> on_ready = nullptr,
                            ChuteStepFailed on_failed = nullptr,
                            std::shared_ptr<std::atomic<bool>> cancel_requested = nullptr) = 0;

    /// Jog the toolhead's Y by @p delta_mm during chute calibration.
    virtual AmsError jog_chute_y(float delta_mm) = 0;

    /// Jog the toolhead's X by @p delta_mm during chute calibration. Sent only
    /// after the caller's ChuteJogGuard has allowed the move.
    virtual AmsError jog_chute_x(float delta_mm) = 0;

    /// Save the current toolhead position as the chute's extrude position.
    virtual AmsError save_chute_position(std::function<void()> on_saved = nullptr,
                                         ChuteSaveFailed on_failed = nullptr) = 0;

    /// Abort path: re-park the toolhead once chute calibration has moved it.
    virtual AmsError exit_chute_calibration() = 0;

    /// The position pair the last chute save reported; false when none was captured.
    [[nodiscard]] virtual bool last_chute_saved_position(double& x_mm, double& y_mm) const = 0;

    /// The nozzle wiper behind the chute, when the printer's config places one;
    /// the jog guard stops short of it. Main thread.
    [[nodiscard]] virtual std::optional<ChuteWiperLine> chute_wiper() const {
        return std::nullopt;
    }

  protected:
    ~ICfsCalibration() = default;
};

} // namespace helix
