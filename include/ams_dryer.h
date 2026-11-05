// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_dryer.h
 * @brief Capability of a filament system that can drive a dryer
 *
 * Reached with AmsBackend::as<IDryer>(). ACE Pro, MultiACE, OpenAMS, the QIDI box, Happy Hare
 * and the mock implement it. Having the interface does not mean this machine has the hardware:
 * get_dryer_info(unit).supported answers that, per unit. dryer_info() asks that of any backend,
 * so a caller reading state needs no branch of its own.
 *
 * A unit's environment zones stay on AmsBackend, since a passive sensor makes a zone with no
 * dryer behind it.
 */

#pragma once

#include "ams_drying_types.h"
#include "ams_error.h"

#include <vector>

namespace helix {

class AmsBackend;

class IDryer {
  public:
    /**
     * @brief Get dryer state and capabilities
     *
     * Returns current dryer state including temperature, duration, and
     * hardware capabilities. Check DryerInfo::supported before showing dryer UI.
     *
     * @return DryerInfo struct (supported=false if this unit has no dryer)
     */
    [[nodiscard]] virtual DryerInfo get_dryer_info(int unit = 0) const = 0;

    /**
     * @brief Start drying operation
     *
     * Initiates filament drying at specified temperature and duration.
     *
     * @param temp_c Target temperature in Celsius (within min_temp_c..max_temp_c)
     * @param duration_min Drying duration in minutes (positive, capped at max_duration_min)
     * @param fan_pct Fan speed percentage (0-100, -1 = use backend default)
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1,
                                  int unit = 0) = 0;

    /**
     * @brief Start drying that also ends once the box reaches a humidity target
     *
     * The cycle runs at most @p duration_min and ends sooner once the box's relative
     * humidity reads at or below @p humidity_pct. Only a backend whose
     * get_dryer_info(unit).supports_humidity_target is true implements this; callers
     * with no target use start_drying(), which leaves any target to the dryer's own default.
     *
     * @param humidity_pct Target relative humidity, % (positive)
     */
    virtual AmsError start_drying_to_humidity(float temp_c, int duration_min, float humidity_pct,
                                              int fan_pct = -1, int unit = 0) {
        (void)temp_c;
        (void)duration_min;
        (void)humidity_pct;
        (void)fan_pct;
        (void)unit;
        return AmsErrorHelper::not_supported("Dryer humidity target");
    }

    /**
     * @brief Stop drying operation
     *
     * Stops any active drying and turns off heater/fan.
     *
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError stop_drying(int unit = 0) = 0;

    /**
     * @brief Start drying as @p request describes
     *
     * The one entry point a surface needs whatever the dryer can do. With no gates and
     * no rotation it is exactly start_drying(), or start_drying_to_humidity() when a
     * target is set, so a backend overrides it only to take a gate selection
     * (DryerInfo::supports_gate_selection) or spool rotation (supports_rotation).
     */
    virtual AmsError start_drying_request(const DryingRequest& request) {
        if (!request.gates.empty() || request.rotate) {
            return AmsErrorHelper::not_supported("Drying selected gates");
        }
        if (request.humidity_pct) {
            return start_drying_to_humidity(request.temp_c, request.duration_min,
                                            *request.humidity_pct, request.fan_pct, request.unit);
        }
        return start_drying(request.temp_c, request.duration_min, request.fan_pct, request.unit);
    }

    /**
     * @brief Stop drying on @p gates only, or everything on @p unit when empty
     *
     * Empty @p gates is exactly stop_drying(). Gates need
     * DryerInfo::supports_gate_selection; the rest of the cycle keeps running.
     */
    virtual AmsError stop_drying_gates(int unit, const std::vector<int>& gates) {
        if (!gates.empty()) {
            return AmsErrorHelper::not_supported("Drying selected gates");
        }
        return stop_drying(unit);
    }

    /**
     * @brief Update drying parameters while running
     *
     * Adjusts temperature, duration, or fan speed during an active dry cycle.
     * Pass -1 to keep current value for any parameter.
     *
     * @param temp_c New target temperature (-1 = no change)
     * @param duration_min New duration (-1 = no change)
     * @param fan_pct New fan speed (-1 = no change)
     * @return AmsError with SUCCESS result on success, or error with reason
     */
    virtual AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1,
                                   int unit = 0) {
        (void)temp_c;
        (void)duration_min;
        (void)fan_pct;
        (void)unit;
        return AmsErrorHelper::not_supported("Dryer");
    }

    /**
     * @brief Get available drying presets
     *
     * Preset profiles for common filament materials. A backend overrides this to
     * offer hardware-specific presets.
     */
    [[nodiscard]] virtual std::vector<DryingPreset> get_drying_presets() const {
        return get_default_drying_presets();
    }

  protected:
    ~IDryer() = default;
};

/// @p backend's dryer state for @p unit; an unsupported DryerInfo when it has no IDryer.
[[nodiscard]] DryerInfo dryer_info(const AmsBackend& backend, int unit = 0);

} // namespace helix
